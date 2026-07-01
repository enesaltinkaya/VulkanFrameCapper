// =============================================================================
// VulkanFrameCapper — a Vulkan layer that implements the two modes described in
// scanline-sync.md:
//
//   Mode 1: low-latency frame limiter (FPS=N)
//           paces the start of each frame at acquire so no queue builds up.
//
//   Mode 2: experimental scanline sync (SCANLINE=N), RTSS-like.
//           forces VK_PRESENT_MODE_IMMEDIATE_KHR, pins the tear line to a chosen
//           vertical position by timing vkQueuePresentKHR against the display's
//           scanout phase. Uses VK_EXT_present_timing first-pixel-out feedback
//           to phase-lock when available; falls back to manual tuning otherwise.
//
// Core algorithm (per scanline-sync.md):
//
//   refresh_ns            = 1e9 / refresh_hz   (measured via present_timing)
//   vtotal                = visible_height * vtotal_scale   (includes blanking)
//   scanline_offset_ns    = refresh_ns * scanline / vtotal  (wrapped to [0,T))
//   target_flip_time      = phase_base + scanline_offset_ns     (first-pixel-out)
//   desired_present_call  = target_flip_time - present_latency  (call->first-pixel)
//   desired_acquire_wake  = desired_present_call - render_lead - acquire_margin
//
//   sleep_until(desired_acquire_wake - sleep_margin)
//   busy_wait_until(desired_acquire_wake)          // at vkAcquireNextImageKHR
//   ... render ...
//   busy_wait_until(desired_present_call)          // at vkQueuePresentKHR (capped)
//
// phase_base and present_latency are corrected by a PLL using first-pixel-out
// feedback from VK_EXT_present_timing.
// =============================================================================

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include "key_input.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <strings.h>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

namespace {

using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;
using Sec = std::chrono::seconds;

// =============================================================================
// Section 1 — environment / configuration
// =============================================================================

bool env_bool(const char *name, bool fallback) {
  const char *v = std::getenv(name);
  if (!v || !*v)
    return fallback;
  return std::strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0 &&
         strcasecmp(v, "no") != 0 && strcasecmp(v, "off") != 0;
}

double env_double(const char *name, double fallback) {
  const char *v = std::getenv(name);
  if (!v || !*v)
    return fallback;
  char *end = nullptr;
  double d = std::strtod(v, &end);
  if (end == v)
    return fallback;
  return d;
}

int64_t env_int(const char *name, int64_t fallback) {
  const char *v = std::getenv(name);
  if (!v || !*v)
    return fallback;
  char *end = nullptr;
  long long ll = std::strtoll(v, &end, 10);
  if (end == v)
    return fallback;
  return static_cast<int64_t>(ll);
}

// Parse a duration in microseconds, clamped to [lo, hi], returned in ns.
Ns env_us(const char *name, double fallback_us, double lo_us, double hi_us) {
  double v = std::clamp(env_double(name, fallback_us), lo_us, hi_us);
  return std::chrono::nanoseconds(static_cast<int64_t>(v * 1000.0));
}

double env_clamped(const char *name, double fallback, double lo, double hi) {
  return std::clamp(env_double(name, fallback), lo, hi);
}

uint64_t hz_to_ns(double hz) {
  if (hz <= 0.0 || !std::isnormal(hz))
    return 0;
  return static_cast<uint64_t>(1.0e9 / hz);
}

enum class Mode { Off, FpsLimit, ScanlineSync };

struct Config {
  Mode mode = Mode::Off;

  // Mode 1
  double target_fps = 0.0;

  // Mode 2
  int64_t scanline = 0;
  Ns manual_offset = Ns::zero();
  Ns fallback_refresh = Ns::zero();
  double vtotal_scale = 1.05;
  bool force_immediate = true;

  // shared / pacing
  bool want_present_timing = false;   // attempt VK_EXT_present_timing feedback
  bool want_present_wait = false;     // opt into VK_KHR_present_wait
  Ns acquire_margin = Ns::zero();     // slack between acquire wake and present
  Ns max_present_wait = Ns::zero();   // cap on the final present-time busy wait
  Ns sleep_margin = Ns::zero();       // handoff from nanosleep to busy spin
  Ns busy_window = Ns::zero();        // final busy-spin duration
  double pll_strength = 0.0;          // PLL correction gain on phase_base
  Ns pll_max_step = Ns::zero();       // clamp on a single PLL step
  double latency_alpha = 0.0;         // EWMA gain for present_latency
  double render_alpha = 0.0;          // EWMA gain for render_lead

  // guards
  bool disable_when_vrr = true;
  Ns disable_jitter = Ns::zero();

  bool log = false;
};

Config parse_config() {
  Config c;
  c.log = env_bool("VFC_LOG", false);

  int64_t scanline = env_int("SCANLINE", 0);
  const char *sv = std::getenv("SCANLINE");
  bool scanline_set = sv && *sv;
  double fps = env_double("FPS", 0.0);

  if (scanline_set)
    c.mode = Mode::ScanlineSync;
  else if (fps > 0.0 && std::isnormal(fps))
    c.mode = Mode::FpsLimit;
  else
    c.mode = Mode::Off;

  c.target_fps = fps;
  c.scanline = scanline;
  c.manual_offset = env_us("VFC_SCANLINE_OFFSET_US", 0.0, -1'000'000.0, 1'000'000.0);
  c.fallback_refresh = Ns(static_cast<int64_t>(hz_to_ns(
      env_clamped("VFC_SCANLINE_REFRESH_HZ", 60.0, 1.0, 1000.0))));
  c.vtotal_scale = env_clamped("VFC_SCANLINE_VTOTAL_SCALE", 1.05, 1.0, 1.30);
  c.force_immediate = env_bool("VFC_FORCE_IMMEDIATE", true);

  // present_timing feedback is opt-in: the doc warns it can freeze some drivers
  // at launch. Manual scanline pacing (phase model + manual offset) works
  // without it.
  // present_timing feedback defaults to ON in scanline mode. The layer
  // auto-disables it where the driver's create flag crashes (RADV), so it's
  // safe to attempt unconditionally.
  c.want_present_timing = c.mode == Mode::ScanlineSync;
  c.want_present_wait = env_bool("VFC_PRESENT_WAIT",
                                 c.mode != Mode::Off);

  c.acquire_margin = env_us("VFC_SCANLINE_ACQUIRE_MARGIN_US", 750.0, 0.0, 100'000.0);
  c.max_present_wait = env_us("VFC_SCANLINE_MAX_PRESENT_WAIT_US", 4000.0, 0.0, 100'000.0);
  c.sleep_margin = env_us("VFC_SLEEP_MARGIN_US", 1000.0, 0.0, 100'000.0);
  c.busy_window = env_us("VFC_BUSY_WAIT_US", 200.0, 0.0, 100'000.0);
  c.pll_strength = env_clamped("VFC_SCANLINE_PLL_STRENGTH", 0.05, 0.0, 1.0);
  c.pll_max_step = env_us("VFC_SCANLINE_MAX_PLL_STEP_US", 200.0, 0.0, 1'000'000.0);
  c.latency_alpha = env_clamped("VFC_SCANLINE_LATENCY_ALPHA", 0.10, 0.0, 1.0);
  c.render_alpha = env_clamped("VFC_SCANLINE_RENDER_ALPHA", 0.02, 0.0, 1.0);

  c.disable_when_vrr = env_bool("VFC_DISABLE_WHEN_VRR", true);
  c.disable_jitter = env_us("VFC_DISABLE_WHEN_JITTER_ABOVE_US", 500.0, 0.0, 1'000'000.0);

  return c;
}

Config g_cfg = parse_config();

// Live FPS cap (adjustable via hotkeys). Inactive in scanline mode.
std::atomic<double> g_live_fps{g_cfg.mode == Mode::FpsLimit ? g_cfg.target_fps : 0.0};
// Live scanline target and offset calibration (adjustable via hotkeys).
std::atomic<int64_t> g_live_scanline{g_cfg.scanline};
std::atomic<int64_t> g_live_offset_ns{g_cfg.manual_offset.count()};

// =============================================================================
// Section 2 — logging
// =============================================================================

void log(const char *fmt, ...) {
  if (!g_cfg.log)
    return;
  va_list args;
  va_start(args, fmt);
  std::fputs("[VulkanFrameCapper] ", stderr);
  std::vfprintf(stderr, fmt, args);
  std::fputc('\n', stderr);
  va_end(args);
  std::fflush(stderr);
}

// =============================================================================
// Section 3 — DXVK-style sleep (coarse nanosleep + final busy spin)
//
// Mirrors the doc's:
//   sleep_until(target_present_time - safety_margin);
//   busy_wait_until(target_present_time);
// =============================================================================

class Sleeper {
public:
  // Sleep from t0 until target. A coarse clock_nanosleep runs up to
  // (target - busy_window); the remainder is a tight spin loop for accuracy.
  static void wait_until(Clock::time_point target, Ns busy_window) {
    auto now = Clock::now();
    if (target <= now)
      return;

    Clock::time_point handoff = target - busy_window;
    if (handoff > now) {
      // Coarse sleep in slices; clock_nanosleep may return early on signal.
      timespec ts{};
      while (true) {
        now = Clock::now();
        if (now >= handoff)
          break;
        Ns remaining = std::chrono::duration_cast<Ns>(handoff - now);
        Ns slice = std::min<Ns>(remaining, std::chrono::milliseconds(4));
        ts.tv_sec = static_cast<time_t>(
            std::chrono::duration_cast<Sec>(slice).count());
        ts.tv_nsec = static_cast<long>(slice.count() % 1'000'000'000LL);
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
      }
    }

    // Final busy spin.
    while (Clock::now() < target)
      std::this_thread::yield();
  }
};

// =============================================================================
// Section 4 — layer dispatch tables & swapchain state
// =============================================================================

struct SwapchainState {
  // geometry / mode
  uint32_t width = 0;
  uint32_t height = 0;
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
  bool immediate_active = false;
  int64_t scanline = 0; // requested tearline position

  // refresh
  uint64_t refresh_ns = 0;     // measured or fallback
  bool refresh_measured = false;
  bool vrr_like = false;       // refreshInterval != refreshDuration

  // phase model (scanline mode)
  // phase_base: estimated absolute time the display last started scanning a
  // frame from the top (a top-of-refresh anchor).
  Clock::time_point phase_base{};
  bool phase_seeded = false;
  Ns present_latency = std::chrono::microseconds(2000); // call -> first-pixel-out
  Ns render_lead = std::chrono::milliseconds(3);        // acquire-wake -> present-call
  Ns scanline_offset_ns = Ns::zero();                   // computed from scanline value

  // acquire pacing
  Clock::time_point next_acquire_target{};
  Clock::time_point last_acquire_wake{};
  bool acquire_seeded = false;

  // fps-limit pacing
  Ns fps_period = Ns::zero();
  Clock::time_point next_frame_start{};
  bool fps_seeded = false;

  // present-timing feedback
  bool timing_enabled = false;        // create flag set on swapchain
  bool first_pixel_out_supported = false;
  bool feedback_active = false;       // timing + calibration + stages available
  uint64_t next_present_id = 1;
  struct Pending {
    Clock::time_point present_call;
    Clock::time_point target; // desired first-pixel-out time
  };
  std::unordered_map<uint64_t, Pending> pending;

  // time-domain mapping (present-timing timestamps -> CLOCK_MONOTONIC)
  VkTimeDomainKHR timing_domain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
  uint64_t timing_domain_id = 0;
  bool domain_known = false;
  bool domain_is_monotonic = true;
  Ns domain_to_monotonic = Ns::zero(); // monotonic = domain_time + offset
  Clock::time_point last_domain_cal{};

  // jitter tracking for the disable guard
  Ns last_phase_err = Ns::zero();
  bool jitter_high = false;
  uint64_t feedback_count = 0;

  bool logged_setup = false;
};

struct InstanceLayer {
  VkInstance instance = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
  PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
  PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR GetPhysicalDeviceSurfaceCapabilities2KHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
  PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
  bool has_surface_caps2 = false;
};

struct DeviceLayer {
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  InstanceLayer *instance = nullptr;

  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
  PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
  PFN_vkGetDeviceQueue2 GetDeviceQueue2 = nullptr;
  PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
  PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
  PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
  PFN_vkAcquireNextImage2KHR AcquireNextImage2KHR = nullptr;
  PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
  PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;

  // present_id / present_wait
  bool present_id_enabled = false;
  bool present_wait_enabled = false;
  PFN_vkWaitForPresentKHR WaitForPresentKHR = nullptr;

  // present_timing
  bool present_timing_enabled = false;
  bool present_timing_caps_queried = false;
  bool caps_present_timing = false;
  bool caps_first_pixel_out = false;
  PFN_vkSetSwapchainPresentTimingQueueSizeEXT SetSwapchainPresentTimingQueueSizeEXT = nullptr;
  PFN_vkGetSwapchainTimingPropertiesEXT GetSwapchainTimingPropertiesEXT = nullptr;
  PFN_vkGetSwapchainTimeDomainPropertiesEXT GetSwapchainTimeDomainPropertiesEXT = nullptr;
  PFN_vkGetPastPresentationTimingEXT GetPastPresentationTimingEXT = nullptr;
  PFN_vkGetCalibratedTimestampsKHR GetCalibratedTimestampsKHR = nullptr;

  std::mutex mutex;
  std::unordered_map<VkSwapchainKHR, SwapchainState> swapchains;
};

std::mutex g_instance_mutex;
std::unordered_map<VkInstance, InstanceLayer *> g_instances;
std::mutex g_device_mutex;
std::unordered_map<VkDevice, DeviceLayer *> g_devices;
std::mutex g_queue_mutex;
std::unordered_map<VkQueue, DeviceLayer *> g_queues;
std::unordered_map<VkPhysicalDevice, InstanceLayer *> g_phys_devices;

InstanceLayer *get_instance(VkInstance i) {
  std::lock_guard<std::mutex> lk(g_instance_mutex);
  auto it = g_instances.find(i);
  return it == g_instances.end() ? nullptr : it->second;
}

DeviceLayer *get_device(VkDevice d) {
  std::lock_guard<std::mutex> lk(g_device_mutex);
  auto it = g_devices.find(d);
  return it == g_devices.end() ? nullptr : it->second;
}

DeviceLayer *get_device_by_queue(VkQueue q) {
  std::lock_guard<std::mutex> lk(g_queue_mutex);
  auto it = g_queues.find(q);
  return it == g_queues.end() ? nullptr : it->second;
}

// =============================================================================
// Section 5 — present-timing helpers
// =============================================================================

// Detect drivers where VK_EXT_present_timing's swapchain create flag is
// broken. On RADV (Mesa AMD), setting VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT
// segfaults inside the driver's vkQueuePresentKHR, so present-timing feedback
// cannot be used there. We auto-disable it so present-timing degrades
// gracefully to manual scanline pacing instead of crashing the process.
bool driver_crashes_present_timing(DeviceLayer *dev) {
  if (!dev || !dev->instance || !dev->instance->GetPhysicalDeviceProperties2)
    return false;
  VkPhysicalDeviceDriverProperties drv{};
  drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &drv;
  dev->instance->GetPhysicalDeviceProperties2(dev->physicalDevice, &props2);
  return drv.driverID == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
         strcasestr(drv.driverName, "radv") != nullptr;
}

// Query VK_EXT_present_timing surface capabilities (stage support). Called from
// vkCreateSwapchainKHR so we know whether first-pixel-out feedback is possible.
void query_present_timing_caps(DeviceLayer *dev, VkSurfaceKHR surface) {
  if (dev->present_timing_caps_queried || !dev->instance ||
      !dev->instance->GetPhysicalDeviceSurfaceCapabilities2KHR ||
      !dev->instance->has_surface_caps2 || surface == VK_NULL_HANDLE)
    return;
  dev->present_timing_caps_queried = true;

  VkPresentTimingSurfaceCapabilitiesEXT caps{};
  caps.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;

  VkSurfaceCapabilities2KHR surf2{};
  surf2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
  surf2.pNext = &caps;

  VkPhysicalDeviceSurfaceInfo2KHR sinfo{};
  sinfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
  sinfo.surface = surface;

  VkResult r = dev->instance->GetPhysicalDeviceSurfaceCapabilities2KHR(
      dev->physicalDevice, &sinfo, &surf2);
  if (r != VK_SUCCESS) {
    log("present_timing: surface caps query failed %d", r);
    return;
  }
  dev->caps_present_timing = !!caps.presentTimingSupported;
  dev->caps_first_pixel_out =
      !!(caps.presentStageQueries & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
  log("present_timing caps: supported=%d first_pixel_out=%d",
      dev->caps_present_timing, dev->caps_first_pixel_out);
}

// Read refreshDuration (and detect VRR via refreshInterval) from the swapchain.
bool query_refresh(DeviceLayer *dev, VkSwapchainKHR sw, SwapchainState &st) {
  if (!dev->present_timing_enabled || !dev->GetSwapchainTimingPropertiesEXT)
    return false;
  VkSwapchainTimingPropertiesEXT t{};
  t.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
  uint64_t counter = 0;
  VkResult r = dev->GetSwapchainTimingPropertiesEXT(dev->device, sw, &t, &counter);
  if (r != VK_SUCCESS || t.refreshDuration == 0)
    return false;
  st.refresh_ns = t.refreshDuration;
  st.refresh_measured = true;
  // refreshInterval == UINT64_MAX (or != refreshDuration) signals VRR.
  st.vrr_like = (t.refreshInterval == 0 || t.refreshInterval == UINT64_MAX ||
                 t.refreshInterval != t.refreshDuration);
  return true;
}

// Enumerate the swapchain's supported time domains and pick one. The opaque
// id we store MUST come from here: passing a stale/zero id in
// VkPresentTimingInfoEXT.timeDomainId makes the driver dereference an invalid
// entry (RADV crashes inside vkQueuePresentKHR).
//
// Preference order: CLOCK_MONOTONIC > CLOCK_MONOTONIC_RAW > the present-stage
// local domain > whatever the driver offers first.
bool query_time_domains(DeviceLayer *dev, VkSwapchainKHR sw, SwapchainState &st) {
  if (!dev->present_timing_enabled || !dev->GetSwapchainTimeDomainPropertiesEXT)
    return false;

  VkSwapchainTimeDomainPropertiesEXT props{};
  props.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT;
  props.timeDomainCount = 0;
  VkResult r = dev->GetSwapchainTimeDomainPropertiesEXT(
      dev->device, sw, &props, nullptr);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE)
    return false;
  if (props.timeDomainCount == 0)
    return false;

  uint32_t count = props.timeDomainCount;
  std::vector<VkTimeDomainKHR> domains(count);
  std::vector<uint64_t> ids(count);
  props.timeDomainCount = count;
  props.pTimeDomains = domains.data();
  props.pTimeDomainIds = ids.data();
  r = dev->GetSwapchainTimeDomainPropertiesEXT(dev->device, sw, &props, nullptr);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE)
    return false;
  count = std::min<uint32_t>(count, props.timeDomainCount);

  static const VkTimeDomainKHR pref[] = {
      VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT,
      VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT,
      VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT,
  };
  uint32_t chosen = UINT32_MAX;
  for (VkTimeDomainKHR want : pref) {
    for (uint32_t i = 0; i < count; i++) {
      if (domains[i] == want) {
        chosen = i;
        break;
      }
    }
    if (chosen != UINT32_MAX)
      break;
  }
  if (chosen == UINT32_MAX)
    chosen = 0; // fall back to the first offered domain

  st.timing_domain = domains[chosen];
  st.timing_domain_id = ids[chosen];
  st.domain_is_monotonic =
      (st.timing_domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT ||
       st.timing_domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR ||
       st.timing_domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT ||
       st.timing_domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR);
  st.domain_known = true;
  log("present_timing: time domain %u id=%llu monotonic=%d (of %u)",
      static_cast<unsigned>(st.timing_domain),
      static_cast<unsigned long long>(st.timing_domain_id),
      st.domain_is_monotonic ? 1 : 0, count);
  return true;
}

// Calibrate the present-timing time domain to CLOCK_MONOTONIC.
//
// past-presentation stage timestamps are expressed in some VkTimeDomainKHR. On
// Linux it is usually CLOCK_MONOTONIC already; otherwise we measure the offset
// with vkGetCalibratedTimestampsKHR (CLOCK_MONOTONIC vs the timing domain).
bool calibrate_time_domain(DeviceLayer *dev, SwapchainState &st) {
  if (!dev || !dev->GetCalibratedTimestampsKHR)
    return false;

  VkTimeDomainKHR doms[2] = {VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR, st.timing_domain};
  if (st.domain_is_monotonic) {
    st.domain_to_monotonic = Ns::zero();
    return true;
  }

  VkCalibratedTimestampInfoKHR infos[2]{};
  infos[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
  infos[0].timeDomain = doms[0];
  infos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
  infos[1].timeDomain = doms[1];

  uint64_t ts[2] = {};
  uint64_t devi = 0;
  VkResult r =
      dev->GetCalibratedTimestampsKHR(dev->device, 2, infos, ts, &devi);
  if (r != VK_SUCCESS) {
    log("present_timing: calibrated timestamps failed %d", r);
    return false;
  }
  // monotonic = domain_time + (ts[0] - ts[1])
  st.domain_to_monotonic = Ns(static_cast<int64_t>(ts[0]) - static_cast<int64_t>(ts[1]));
  return true;
}

// Poll past presentation timing, match presentIds, and run the PLL that keeps
// phase_base (display scanout phase) and present_latency aligned to reality.
void poll_feedback(DeviceLayer *dev, VkSwapchainKHR sw, SwapchainState &st) {
  if (!dev->present_timing_enabled || !dev->GetPastPresentationTimingEXT ||
      !st.feedback_active || st.pending.empty())
    return;

  auto now = Clock::now();
  // Re-calibrate the time domain periodically.
  if (!st.domain_is_monotonic &&
      (st.last_domain_cal == Clock::time_point() ||
       now - st.last_domain_cal > std::chrono::seconds(1))) {
    if (calibrate_time_domain(dev, st))
      st.last_domain_cal = now;
  }

  VkPastPresentationTimingInfoEXT info{};
  info.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
  info.flags = VK_PAST_PRESENTATION_TIMING_ALLOW_PARTIAL_RESULTS_BIT_EXT |
               VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT;
  info.swapchain = sw;

  VkPastPresentationTimingPropertiesEXT props{};
  props.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;

  VkResult r = dev->GetPastPresentationTimingEXT(dev->device, &info, &props);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE && r != VK_NOT_READY)
    return;
  if (!props.presentationTimingCount)
    return;

  std::vector<VkPresentStageTimeEXT> stages(props.presentationTimingCount);
  std::vector<VkPastPresentationTimingEXT> timings(props.presentationTimingCount);
  for (uint32_t i = 0; i < props.presentationTimingCount; i++) {
    timings[i].sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
    timings[i].presentStageCount = 1;
    timings[i].pPresentStages = &stages[i];
  }
  props.pPresentationTimings = timings.data();
  r = dev->GetPastPresentationTimingEXT(dev->device, &info, &props);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE && r != VK_NOT_READY)
    return;

  int64_t refresh = st.refresh_ns ? static_cast<int64_t>(st.refresh_ns)
                                  : static_cast<int64_t>(g_cfg.fallback_refresh.count());

  for (uint32_t i = 0; i < props.presentationTimingCount; i++) {
    const auto &tm = timings[i];
    if (!tm.reportComplete || !tm.presentStageCount)
      continue;

    const VkPresentStageTimeEXT *chosen = nullptr;
    for (uint32_t s = 0; s < tm.presentStageCount; s++) {
      if (tm.pPresentStages[s].stage &
          VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) {
        chosen = &tm.pPresentStages[s];
        break;
      }
    }
    if (!chosen)
      continue;

    // Learn the time domain from the first report, and calibrate to
    // CLOCK_MONOTONIC if it isn't already.
    if (!st.domain_known) {
      st.timing_domain = tm.timeDomain;
      st.timing_domain_id = tm.timeDomainId;
      st.domain_is_monotonic =
          (tm.timeDomain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT ||
           tm.timeDomain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR);
      st.domain_known = true;
      log("present_timing: time domain = %u (monotonic=%d)",
          static_cast<unsigned>(st.timing_domain),
          st.domain_is_monotonic ? 1 : 0);
    } else if (tm.timeDomain != st.timing_domain ||
               tm.timeDomainId != st.timing_domain_id) {
      // Domain changed underneath us; drop this entry.
      st.pending.erase(st.pending.find(tm.presentId));
      continue;
    }

    auto pit = st.pending.find(tm.presentId);
    if (pit == st.pending.end())
      continue;

    Clock::time_point actual =
        Clock::time_point(Ns(static_cast<int64_t>(chosen->time))) +
        st.domain_to_monotonic;

    // Phase error: how far the actual first-pixel-out was from our target.
    // Wrap to [-T/2, T/2] so the PLL corrects the short way.
    auto raw = std::chrono::duration_cast<Ns>(actual - pit->second.target);
    int64_t e = raw.count();
    e = ((e + refresh / 2) % refresh + refresh) % refresh - refresh / 2;
    Ns err(e);

    // present_latency = present-call -> first-pixel-out (EWMA).
    Ns measured_latency =
        std::chrono::duration_cast<Ns>(actual - pit->second.present_call);
    st.present_latency =
        Ns(static_cast<int64_t>(st.present_latency.count() *
                                    (1.0 - g_cfg.latency_alpha) +
                                measured_latency.count() * g_cfg.latency_alpha));

    // PLL on phase_base.
    int64_t step = static_cast<int64_t>(std::llround(err.count() * g_cfg.pll_strength));
    step = std::clamp(step, -g_cfg.pll_max_step.count(), g_cfg.pll_max_step.count());
    st.phase_base += Ns(step);

    // jitter tracking for the disable guard
    Ns derr = err - st.last_phase_err;
    st.last_phase_err = err;
    if (st.feedback_count > 4 && std::abs(derr.count()) > g_cfg.disable_jitter.count())
      st.jitter_high = true;
    st.feedback_count++;

    st.pending.erase(pit);
  }

  // Bound the pending map so a runaway driver can't leak memory.
  if (st.pending.size() > 64)
    st.pending.clear();
}

// =============================================================================
// Section 6 — pacing algorithms
// =============================================================================

// Wrap a signed ns offset into [0, refresh).
int64_t wrap_period(int64_t v, int64_t refresh) {
  if (refresh <= 0)
    return 0;
  v %= refresh;
  if (v < 0)
    v += refresh;
  return v;
}

// Compute scanline_offset_ns from the requested scanline value and geometry.
void recompute_scanline_offset(SwapchainState &st) {
  int64_t refresh = st.refresh_ns ? static_cast<int64_t>(st.refresh_ns)
                                  : static_cast<int64_t>(g_cfg.fallback_refresh.count());
  double vtotal = std::max(1.0, static_cast<double>(st.height) * g_cfg.vtotal_scale);
  double frac = static_cast<double>(st.scanline) / vtotal; // may be negative
  int64_t off = static_cast<int64_t>(std::llround(frac * static_cast<double>(refresh)));
  st.scanline_offset_ns = Ns(wrap_period(off, refresh));
}

void sync_live_scanline(SwapchainState &st) {
  st.scanline = g_live_scanline.load(std::memory_order_relaxed);
  recompute_scanline_offset(st);
}

// The desired first-pixel-out time for THIS frame. Each call advances the
// phase anchor by exactly one refresh (one frame per refresh, like RTSS), and
// skips forward only when a slot was missed because rendering fell behind.
// This is what caps the frame rate to the refresh rate with even spacing.
Clock::time_point scanline_target(SwapchainState &st, Clock::time_point now) {
  int64_t refresh = st.refresh_ns ? static_cast<int64_t>(st.refresh_ns)
                                  : static_cast<int64_t>(g_cfg.fallback_refresh.count());
  if (!st.phase_seeded) {
    // Seed phase_base roughly now; the PLL refines it from feedback.
    st.phase_base = now;
    st.phase_seeded = true;
  }
  Clock::time_point target = st.phase_base + st.scanline_offset_ns;
  // Skip only slots whose target time is actually already gone. The old logic
  // skipped as soon as the *wake* time was missed, which made scanline mode
  // drop whole refreshes from tiny scheduler/render-lead jitter. It is smoother
  // to keep the current slot if the target itself is still ahead; the final
  // present-side wait can still hit the desired phase.
  while (target <= now) {
    st.phase_base += Ns(refresh);
    target += Ns(refresh);
  }
  // Consume this slot: advance the anchor by one refresh so the next frame
  // targets the next refresh. Without this, multiple frames would pile onto
  // the same slot and burst past the refresh rate.
  st.phase_base += Ns(refresh);
  return target;
}

// Forward declaration: hotkey handling lives in a later section but the
// pacing loop polls it here.
void apply_hotkeys();

// Mode 2 — pace the frame start at acquire for low input lag.
void scanline_acquire_pace(DeviceLayer *dev, VkSwapchainKHR sw) {
  if (g_cfg.mode != Mode::ScanlineSync)
    return;
  std::lock_guard<std::mutex> lk(dev->mutex);
  auto it = dev->swapchains.find(sw);
  if (it == dev->swapchains.end())
    return;
  SwapchainState &st = it->second;

  // Lazily re-query refresh; some drivers only populate it after warmup.
  if (dev->present_timing_enabled && !st.refresh_measured)
    query_refresh(dev, sw, st);
  if (!st.refresh_measured)
    st.refresh_ns = static_cast<uint64_t>(g_cfg.fallback_refresh.count());
  sync_live_scanline(st);

  if (g_cfg.disable_when_vrr && st.vrr_like) {
    static bool warned = false;
    if (!warned) {
      log("scanline: VRR-like refresh detected; phase may drift (feedback off)");
      warned = true;
    }
  }

  // Hotkey adjustment.
  apply_hotkeys();

  auto now = Clock::now();
  Clock::time_point target = scanline_target(st, now);
  Ns live_offset = Ns(g_live_offset_ns.load(std::memory_order_relaxed));
  // Wake early enough to finish rendering before the desired present call.
  Clock::time_point wake =
      target - st.present_latency - st.render_lead - g_cfg.acquire_margin +
      live_offset;

  st.next_acquire_target = target;
  st.last_acquire_wake = std::max(wake, now);

  if (wake > now)
    Sleeper::wait_until(wake, g_cfg.busy_window);
}

void scanline_record_acquire(DeviceLayer *dev, VkSwapchainKHR sw, VkResult res) {
  if (g_cfg.mode != Mode::ScanlineSync)
    return;
  if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
    return;
  std::lock_guard<std::mutex> lk(dev->mutex);
  auto it = dev->swapchains.find(sw);
  if (it != dev->swapchains.end())
    it->second.last_acquire_wake = Clock::now();
}

// Mode 2 — small final correction before calling down-chain QueuePresent.
Clock::time_point scanline_present_pace(DeviceLayer *dev, VkSwapchainKHR sw) {
  if (g_cfg.mode != Mode::ScanlineSync)
    return Clock::time_point();
  std::lock_guard<std::mutex> lk(dev->mutex);
  auto it = dev->swapchains.find(sw);
  if (it == dev->swapchains.end())
    return Clock::time_point();
  SwapchainState &st = it->second;

  poll_feedback(dev, sw, st);

  auto now = Clock::now();
  Clock::time_point target = st.next_acquire_target; // set during acquire pace
  // If acquire pacing was skipped (e.g. first frame), compute it now.
  if (target == Clock::time_point()) {
    sync_live_scanline(st);
    target = scanline_target(st, now);
  }

  Ns live_offset = Ns(g_live_offset_ns.load(std::memory_order_relaxed));
  Clock::time_point desired_present_call = target - st.present_latency + live_offset;

  if (desired_present_call > now) {
    Ns remaining = std::chrono::duration_cast<Ns>(desired_present_call - now);
    if (remaining <= g_cfg.max_present_wait)
      Sleeper::wait_until(desired_present_call, g_cfg.busy_window);
  }

  // Update render_lead EWMA from the real acquire-wake -> present-call span.
  if (st.last_acquire_wake != Clock::time_point()) {
    Ns measured =
        std::chrono::duration_cast<Ns>(Clock::now() - st.last_acquire_wake);
    if (measured.count() > 0)
      st.render_lead =
          Ns(static_cast<int64_t>(st.render_lead.count() *
                                      (1.0 - g_cfg.render_alpha) +
                                  measured.count() * g_cfg.render_alpha));
    // Clamp render_lead to a sane window.
    int64_t refresh = st.refresh_ns ? static_cast<int64_t>(st.refresh_ns)
                                    : static_cast<int64_t>(g_cfg.fallback_refresh.count());
    // Keep render_lead within the range the final present wait can correct.
    // If render_lead grows too large after one slow frame, the next fast frame
    // wakes very early, final correction refuses to wait (remaining > max), and
    // the present happens early -> visible frametime jitter. Default max:
    // max_present_wait(4ms) - acquire_margin(0.75ms) ~= 3.25ms.
    Ns max_correctable = g_cfg.max_present_wait > g_cfg.acquire_margin
                             ? g_cfg.max_present_wait - g_cfg.acquire_margin
                             : g_cfg.max_present_wait;
    max_correctable = std::max(max_correctable, Ns(std::chrono::microseconds(500)));
    st.render_lead = std::clamp<Ns>(
        st.render_lead, Ns(std::chrono::microseconds(200)),
        std::min(Ns(refresh / 2), max_correctable));
  }

  if (!st.logged_setup) {
    st.logged_setup = true;
    log("scanline: mode=%s immediate=%d refresh=%lluns offset=%+lldus "
        "feedback=%d latency=%lldus lead=%lldus",
        "scanline-sync", st.immediate_active ? 1 : 0,
        static_cast<unsigned long long>(st.refresh_ns),
        static_cast<long long>(st.scanline_offset_ns.count() / 1000),
        st.feedback_active ? 1 : 0,
        static_cast<long long>(st.present_latency.count() / 1000),
        static_cast<long long>(st.render_lead.count() / 1000));
  }

  return target;
}

// Mode 1 — pace frame start at acquire.
void fps_acquire_pace(DeviceLayer *dev, VkSwapchainKHR sw) {
  if (g_cfg.mode != Mode::FpsLimit)
    return;
  double fps = g_live_fps.load(std::memory_order_relaxed);
  if (fps <= 0.0)
    return;
  std::lock_guard<std::mutex> lk(dev->mutex);
  auto it = dev->swapchains.find(sw);
  if (it == dev->swapchains.end())
    return;
  SwapchainState &st = it->second;

  apply_hotkeys();

  if (st.fps_period.count() == 0)
    st.fps_period = Ns(static_cast<int64_t>(1e9 / fps));

  auto now = Clock::now();
  if (!st.fps_seeded) {
    st.next_frame_start = now;
    st.fps_seeded = true;
  }
  // Re-derive period from the live cap each frame.
  st.fps_period = Ns(static_cast<int64_t>(1e9 / fps));

  if (st.next_frame_start > now)
    Sleeper::wait_until(st.next_frame_start, g_cfg.busy_window);

  // Advance by whole periods; if we fell behind, skip ahead to avoid bursts.
  now = Clock::now();
  while (st.next_frame_start <= now)
    st.next_frame_start += st.fps_period;
}

// =============================================================================
// Section 7 — hotkeys
//
// FPS mode: Shift+/- steps 0.01, Ctrl+/- steps 0.10.
// Scanline mode: Shift+/- steps the phase offset by 1ms,
//                Ctrl+/- moves the tearline target by 10 scanlines.
// =============================================================================

enum class HotkeyCombo {
  NoCombo,
  IncreaseSlow,
  DecreaseSlow,
  IncreaseFast,
  DecreaseFast,
};

std::mutex g_hotkey_mutex;
Clock::time_point g_last_hotkey_check;
HotkeyCombo g_held_hotkey = HotkeyCombo::NoCombo;

HotkeyCombo poll_hotkey_combo() {
  // Ctrl combos first, so Ctrl+Shift+Plus still means the fast action.
  if (vfc::key_input::increase_fast_pressed())
    return HotkeyCombo::IncreaseFast;
  if (vfc::key_input::decrease_fast_pressed())
    return HotkeyCombo::DecreaseFast;
  if (vfc::key_input::increase_pressed())
    return HotkeyCombo::IncreaseSlow;
  if (vfc::key_input::decrease_pressed())
    return HotkeyCombo::DecreaseSlow;
  return HotkeyCombo::NoCombo;
}

void apply_hotkey_action(HotkeyCombo combo) {
  if (combo == HotkeyCombo::NoCombo)
    return;

  if (g_cfg.mode == Mode::FpsLimit) {
    double f = g_live_fps.load(std::memory_order_relaxed);
    switch (combo) {
    case HotkeyCombo::IncreaseSlow:
      f += 0.01;
      break;
    case HotkeyCombo::DecreaseSlow:
      f -= 0.01;
      break;
    case HotkeyCombo::IncreaseFast:
      f += 0.10;
      break;
    case HotkeyCombo::DecreaseFast:
      f -= 0.10;
      break;
    case HotkeyCombo::NoCombo:
      break;
    }
    f = std::clamp(f, 1.0, 1000.0);
    g_live_fps.store(f, std::memory_order_relaxed);
    log("fps cap -> %.2f", f);
  } else if (g_cfg.mode == Mode::ScanlineSync) {
    if (combo == HotkeyCombo::IncreaseFast || combo == HotkeyCombo::DecreaseFast) {
      int64_t scanline = g_live_scanline.load(std::memory_order_relaxed);
      constexpr int64_t line_step = 100;
      scanline += (combo == HotkeyCombo::IncreaseFast) ? line_step : -line_step;
      scanline = std::clamp<int64_t>(scanline, -1'000'000LL, 1'000'000LL);
      g_live_scanline.store(scanline, std::memory_order_relaxed);
      log("scanline target -> %+lld", static_cast<long long>(scanline));
    } else {
      int64_t off = g_live_offset_ns.load(std::memory_order_relaxed);
      constexpr int64_t coarse = 1'000'000; // 1 ms
      switch (combo) {
      case HotkeyCombo::IncreaseSlow:
        off += coarse;
        break;
      case HotkeyCombo::DecreaseSlow:
        off -= coarse;
        break;
      case HotkeyCombo::IncreaseFast:
      case HotkeyCombo::DecreaseFast:
      case HotkeyCombo::NoCombo:
        break;
      }
      off = std::clamp<int64_t>(off, -1'000'000'000LL, 1'000'000'000LL);
      g_live_offset_ns.store(off, std::memory_order_relaxed);
      log("scanline offset -> %+lld us", static_cast<long long>(off / 1000));
    }
  }
}

void apply_hotkeys() {
  std::lock_guard<std::mutex> lk(g_hotkey_mutex);

  auto now = Clock::now();
  // Throttle polling (XQueryKeymap / wayland dispatch) to ~50 Hz.
  if (now - g_last_hotkey_check < std::chrono::milliseconds(20))
    return;
  g_last_hotkey_check = now;

  HotkeyCombo current = poll_hotkey_combo();

  if (current != HotkeyCombo::NoCombo) {
    // Record the combo while held, but do not repeat. If the user changes from
    // one combo to another without releasing, track the new combo.
    g_held_hotkey = current;
    return;
  }

  // Fire exactly once when the combo is released.
  HotkeyCombo released = g_held_hotkey;
  g_held_hotkey = HotkeyCombo::NoCombo;
  apply_hotkey_action(released);
}

// =============================================================================
// Section 8 — pNext-chain helpers
// =============================================================================

const void *find_in_chain(const void *pNext, VkStructureType sType) {
  while (pNext) {
    auto *s = static_cast<const VkBaseInStructure *>(pNext);
    if (s->sType == sType)
      return pNext;
    pNext = s->pNext;
  }
  return nullptr;
}

bool device_has_extension(InstanceLayer *inst, VkPhysicalDevice pd,
                          const char *name) {
  if (!inst || !inst->EnumerateDeviceExtensionProperties)
    return false;
  uint32_t count = 0;
  if (inst->EnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr) !=
      VK_SUCCESS)
    return false;
  std::vector<VkExtensionProperties> exts(count);
  if (inst->EnumerateDeviceExtensionProperties(pd, nullptr, &count, exts.data()) !=
      VK_SUCCESS)
    return false;
  for (auto &e : exts)
    if (std::strcmp(e.extensionName, name) == 0)
      return true;
  return false;
}

} // namespace

// =============================================================================
// Section 9 — layer entry points (extern "C", global linkage so the loader can
// resolve vkNegotiateLoaderLayerInterfaceVersion by name via dlsym).
// =============================================================================

extern "C" {

// ---- Instance ----

// Pull the loader's layer link info out of the pNext chain. The loader inserts
// a VkLayerInstanceCreateInfo / VkLayerDeviceCreateInfo (function ==
// VK_LAYER_LINK_INFO) whose pLayerInfo points at the next layer's proc-addr.
static VkLayerInstanceCreateInfo *
get_instance_link(const void *pNext) {
  while (pNext) {
    auto *s = static_cast<const VkBaseInStructure *>(pNext);
    if (s->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
      auto *lic = const_cast<VkLayerInstanceCreateInfo *>(
          static_cast<const VkLayerInstanceCreateInfo *>(pNext));
      if (lic->function == VK_LAYER_LINK_INFO)
        return lic;
    }
    pNext = s->pNext;
  }
  return nullptr;
}

static VkLayerDeviceCreateInfo *
get_device_link(const void *pNext) {
  while (pNext) {
    auto *s = static_cast<const VkBaseInStructure *>(pNext);
    if (s->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
      auto *ldc = const_cast<VkLayerDeviceCreateInfo *>(
          static_cast<const VkLayerDeviceCreateInfo *>(pNext));
      if (ldc->function == VK_LAYER_LINK_INFO)
        return ldc;
    }
    pNext = s->pNext;
  }
  return nullptr;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
  auto *layer = new InstanceLayer();

  // Resolve the down-chain gipa from the loader's layer link info.
  VkLayerInstanceCreateInfo *link = get_instance_link(pCreateInfo->pNext);
  if (!link || !link->u.pLayerInfo) {
    delete layer;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkLayerInstanceLink *layer_link = link->u.pLayerInfo;
  PFN_vkGetInstanceProcAddr next_gipa =
      layer_link->pfnNextGetInstanceProcAddr;
  if (!next_gipa) {
    delete layer;
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkInstanceCreateInfo ci = *pCreateInfo;

  // Ensure VK_KHR_get_surface_capabilities2 is enabled so we can query
  // present-timing surface caps (only when scanline + present_timing wanted).
  std::vector<const char *> exts;
  bool need_surface_caps2 =
      g_cfg.want_present_timing && g_cfg.mode == Mode::ScanlineSync;
  if (need_surface_caps2) {
    bool present = false;
    if (ci.ppEnabledExtensionNames) {
      for (uint32_t i = 0; i < ci.enabledExtensionCount; i++) {
        if (std::strcmp(ci.ppEnabledExtensionNames[i],
                        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0)
          present = true;
      }
    }
    if (!present) {
      exts.assign(ci.ppEnabledExtensionNames,
                  ci.ppEnabledExtensionNames + ci.enabledExtensionCount);
      exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
      ci.ppEnabledExtensionNames = exts.data();
      ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    }
  }

  PFN_vkCreateInstance chain_create = reinterpret_cast<PFN_vkCreateInstance>(
      next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));

  // Advance the link so the next layer in the chain sees its own link info.
  link->u.pLayerInfo = layer_link->pNext;

  VkResult res = chain_create(&ci, pAllocator, pInstance);
  if (res != VK_SUCCESS) {
    delete layer;
    return res;
  }

  layer->instance = *pInstance;
  layer->GetInstanceProcAddr = next_gipa;
    layer->GetPhysicalDeviceFeatures2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            next_gipa(layer->instance, "vkGetPhysicalDeviceFeatures2"));
  if (!layer->GetPhysicalDeviceFeatures2)
    layer->GetPhysicalDeviceFeatures2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            next_gipa(layer->instance, "vkGetPhysicalDeviceFeatures2KHR"));
  layer->GetPhysicalDeviceProperties2 =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
          next_gipa(layer->instance, "vkGetPhysicalDeviceProperties2"));
  if (!layer->GetPhysicalDeviceProperties2)
    layer->GetPhysicalDeviceProperties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            next_gipa(layer->instance,
                     "vkGetPhysicalDeviceProperties2KHR"));
  layer->EnumerateDeviceExtensionProperties =
      reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
          next_gipa(layer->instance, "vkEnumerateDeviceExtensionProperties"));
  layer->GetPhysicalDeviceSurfaceCapabilities2KHR =
      reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
          next_gipa(layer->instance,
                    "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
  layer->GetPhysicalDeviceSurfacePresentModesKHR =
      reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
          next_gipa(layer->instance,
                    "vkGetPhysicalDeviceSurfacePresentModesKHR"));
  layer->DestroySurfaceKHR = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
      next_gipa(layer->instance, "vkDestroySurfaceKHR"));
  layer->has_surface_caps2 =
      need_surface_caps2 &&
      layer->GetPhysicalDeviceSurfaceCapabilities2KHR != nullptr;

  {
    std::lock_guard<std::mutex> lk(g_instance_mutex);
    g_instances[layer->instance] = layer;
  }
  log("instance created; mode=%d", static_cast<int>(g_cfg.mode));
  return res;
}

// Map physical devices back to their InstanceLayer so CreateDevice can resolve
// instance-level helpers (extension enum, features, surface caps).
VK_LAYER_EXPORT VkResult VKAPI_CALL
vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                           VkPhysicalDevice *pPhysicalDevices) {
  InstanceLayer *layer = get_instance(instance);
  if (!layer || !layer->GetInstanceProcAddr)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto *fp = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      layer->GetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
  if (!fp)
    return VK_ERROR_INITIALIZATION_FAILED;
  VkResult res = fp(instance, pPhysicalDeviceCount, pPhysicalDevices);
  if ((res == VK_SUCCESS || res == VK_INCOMPLETE) && pPhysicalDevices) {
    std::lock_guard<std::mutex> lk(g_instance_mutex);
    for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++)
      g_phys_devices[pPhysicalDevices[i]] = layer;
  }
  return res;
}

InstanceLayer *instance_for_physical_device(VkPhysicalDevice pd) {
  std::lock_guard<std::mutex> lk(g_instance_mutex);
  auto it = g_phys_devices.find(pd);
  return it == g_phys_devices.end() ? nullptr : it->second;
}

VK_LAYER_EXPORT void VKAPI_CALL
vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
  InstanceLayer *layer = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_instance_mutex);
    auto it = g_instances.find(instance);
    if (it != g_instances.end()) {
      layer = it->second;
      g_instances.erase(it);
    }
  }
  if (layer) {
    if (layer->GetInstanceProcAddr) {
      auto *fp = reinterpret_cast<PFN_vkDestroyInstance>(
          layer->GetInstanceProcAddr(instance, "vkDestroyInstance"));
      if (fp)
        fp(instance, pAllocator);
    }
    delete layer;
  }
}

// ---- Device ----

VK_LAYER_EXPORT VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physicalDevice,
               const VkDeviceCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
  // Resolve the down-chain gipa/dgpa from the loader's device layer link.
  VkLayerDeviceCreateInfo *link = get_device_link(pCreateInfo->pNext);
  if (!link || !link->u.pLayerInfo) {
    log("vkCreateDevice: no loader device link in chain");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkLayerDeviceLink *layer_link = link->u.pLayerInfo;
  PFN_vkGetInstanceProcAddr chain_gipa =
      layer_link->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr next_dgpa =
      layer_link->pfnNextGetDeviceProcAddr;
  if (!chain_gipa || !next_dgpa) {
    log("vkCreateDevice: missing chain proc-addrs");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  PFN_vkCreateDevice chain_create = reinterpret_cast<PFN_vkCreateDevice>(
      chain_gipa(VK_NULL_HANDLE, "vkCreateDevice"));

  // The instance that owns this physical device (for extension/feature/caps
  // queries). Falls back to null gracefully.
  InstanceLayer *inst = instance_for_physical_device(physicalDevice);

  auto *dev = new DeviceLayer();
  dev->physicalDevice = physicalDevice;
  dev->instance = inst;

  // Decide which of our extensions to enable, based on availability + config.
  bool want_pid = g_cfg.want_present_wait || g_cfg.want_present_timing;
  bool want_pwait = g_cfg.want_present_wait;
  bool want_ptiming = g_cfg.want_present_timing;
  bool want_calib = g_cfg.want_present_timing;

  bool have_pid = want_pid && device_has_extension(inst, physicalDevice,
                                                   VK_KHR_PRESENT_ID_EXTENSION_NAME);
  bool have_pwait =
      want_pwait && device_has_extension(inst, physicalDevice,
                                         VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
  bool have_ptiming =
      want_ptiming && device_has_extension(inst, physicalDevice,
                                           VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
  bool have_calib =
      want_calib && device_has_extension(inst, physicalDevice,
                                         VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

  // Query feature support before enabling feature structs.
  bool feat_pid = false, feat_pwait = false, feat_ptiming = false;
  if (inst && inst->GetPhysicalDeviceFeatures2) {
    VkPhysicalDevicePresentIdFeaturesKHR f_pid{};
    f_pid.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    VkPhysicalDevicePresentWaitFeaturesKHR f_pwait{};
    f_pwait.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    f_pwait.pNext = &f_pid;
    VkPhysicalDevicePresentTimingFeaturesEXT f_ptiming{};
    f_ptiming.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;
    f_ptiming.pNext = &f_pwait;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f_ptiming;
    inst->GetPhysicalDeviceFeatures2(physicalDevice, &f2);
    feat_pid = have_pid && f_pid.presentId;
    feat_pwait = have_pwait && f_pwait.presentWait;
    feat_ptiming = have_ptiming && f_ptiming.presentTiming;
  } else {
    // No Features2: assume supported if the extension is present.
    feat_pid = have_pid;
    feat_pwait = have_pwait;
    feat_ptiming = have_ptiming;
  }

  // Build a new extension list.
  std::vector<const char *> exts;
  if (pCreateInfo->ppEnabledExtensionNames) {
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++)
      exts.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
  }
  auto add_ext = [&](bool cond, const char *name) {
    if (!cond)
      return;
    for (auto *e : exts)
      if (std::strcmp(e, name) == 0)
        return;
    exts.push_back(name);
  };
  add_ext(feat_pid, VK_KHR_PRESENT_ID_EXTENSION_NAME);
  add_ext(feat_pwait, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
  add_ext(feat_ptiming, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
  add_ext(have_calib, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

  // Build a feature pNext chain to enable them.
  VkPhysicalDevicePresentIdFeaturesKHR en_pid{};
  en_pid.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
  VkPhysicalDevicePresentWaitFeaturesKHR en_pwait{};
  en_pwait.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
  VkPhysicalDevicePresentTimingFeaturesEXT en_ptiming{};
  en_ptiming.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;

  void *feat_head = const_cast<void *>(pCreateInfo->pNext);
  if (feat_ptiming) {
    en_ptiming.presentTiming = VK_TRUE;
    en_ptiming.presentAtAbsoluteTime = VK_TRUE;
    en_ptiming.pNext = feat_head;
    feat_head = &en_ptiming;
  }
  if (feat_pwait) {
    en_pwait.presentWait = VK_TRUE;
    en_pwait.pNext = feat_head;
    feat_head = &en_pwait;
  }
  if (feat_pid) {
    en_pid.presentId = VK_TRUE;
    en_pid.pNext = feat_head;
    feat_head = &en_pid;
  }

  VkDeviceCreateInfo ci = *pCreateInfo;
  ci.ppEnabledExtensionNames = exts.data();
  ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
  ci.pNext = feat_head;

  // Advance the link so the next layer in the chain sees its own link info.
  link->u.pLayerInfo = layer_link->pNext;

  VkResult res = chain_create(physicalDevice, &ci, pAllocator, pDevice);
  if (res != VK_SUCCESS) {
    delete dev;
    return res;
  }

  dev->device = *pDevice;
  dev->GetDeviceProcAddr = next_dgpa;

  if (dev->GetDeviceProcAddr) {
    auto *d = dev->GetDeviceProcAddr;
    dev->GetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
        d(*pDevice, "vkGetDeviceQueue"));
    dev->GetDeviceQueue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(
        d(*pDevice, "vkGetDeviceQueue2"));
    dev->CreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        d(*pDevice, "vkCreateSwapchainKHR"));
    dev->DestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
        d(*pDevice, "vkDestroySwapchainKHR"));
    dev->AcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
        d(*pDevice, "vkAcquireNextImageKHR"));
    dev->AcquireNextImage2KHR = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(
        d(*pDevice, "vkAcquireNextImage2KHR"));
    dev->QueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
        d(*pDevice, "vkQueuePresentKHR"));
    dev->DeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
        d(*pDevice, "vkDeviceWaitIdle"));
    dev->WaitForPresentKHR = reinterpret_cast<PFN_vkWaitForPresentKHR>(
        d(*pDevice, "vkWaitForPresentKHR"));
    dev->SetSwapchainPresentTimingQueueSizeEXT =
        reinterpret_cast<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(
            d(*pDevice, "vkSetSwapchainPresentTimingQueueSizeEXT"));
    dev->GetSwapchainTimingPropertiesEXT =
        reinterpret_cast<PFN_vkGetSwapchainTimingPropertiesEXT>(
            d(*pDevice, "vkGetSwapchainTimingPropertiesEXT"));
    dev->GetSwapchainTimeDomainPropertiesEXT =
        reinterpret_cast<PFN_vkGetSwapchainTimeDomainPropertiesEXT>(
            d(*pDevice, "vkGetSwapchainTimeDomainPropertiesEXT"));
    dev->GetPastPresentationTimingEXT =
        reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(
            d(*pDevice, "vkGetPastPresentationTimingEXT"));
    dev->GetCalibratedTimestampsKHR =
        reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
            d(*pDevice, "vkGetCalibratedTimestampsKHR"));
  }

  dev->present_id_enabled = feat_pid;
  dev->present_wait_enabled = feat_pwait;
  dev->present_timing_enabled = feat_ptiming;

  {
    std::lock_guard<std::mutex> lk(g_device_mutex);
    g_devices[dev->device] = dev;
  }
  log("device created; pid=%d pwait=%d ptiming=%d calib=%d",
      feat_pid ? 1 : 0, feat_pwait ? 1 : 0, feat_ptiming ? 1 : 0,
      have_calib ? 1 : 0);
  return res;
}

VK_LAYER_EXPORT void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
  DeviceLayer *dev = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_device_mutex);
    auto it = g_devices.find(device);
    if (it != g_devices.end()) {
      dev = it->second;
      g_devices.erase(it);
    }
  }
  if (dev) {
    // Clean up queue map entries for this device.
    {
      std::lock_guard<std::mutex> lk(g_queue_mutex);
      for (auto it = g_queues.begin(); it != g_queues.end();) {
        if (it->second == dev)
          it = g_queues.erase(it);
        else
          ++it;
      }
    }
    if (dev->DeviceWaitIdle)
      dev->DeviceWaitIdle(device);
    if (dev->GetDeviceProcAddr) {
      auto *fp = reinterpret_cast<PFN_vkDestroyDevice>(
          dev->GetDeviceProcAddr(device, "vkDestroyDevice"));
      if (fp)
        fp(device, pAllocator);
    }
    delete dev;
  }
}

// ---- Queues ----

VK_LAYER_EXPORT void VKAPI_CALL
vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex,
                 VkQueue *pQueue) {
  DeviceLayer *dev = get_device(device);
  if (!dev || !dev->GetDeviceQueue) {
    // Without our dispatch this device was not created through us.
    return;
  }
  dev->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
  if (pQueue && *pQueue) {
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    g_queues[*pQueue] = dev;
  }
}

VK_LAYER_EXPORT void VKAPI_CALL
vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo,
                  VkQueue *pQueue) {
  DeviceLayer *dev = get_device(device);
  if (!dev || !dev->GetDeviceQueue2)
    return;
  dev->GetDeviceQueue2(device, pQueueInfo, pQueue);
  if (pQueue && *pQueue) {
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    g_queues[*pQueue] = dev;
  }
}

// ---- Swapchain ----

VK_LAYER_EXPORT VkResult VKAPI_CALL
vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkSwapchainKHR *pSwapchain) {
  DeviceLayer *dev = get_device(device);
  if (!dev || !dev->CreateSwapchainKHR)
    return VK_ERROR_INITIALIZATION_FAILED;

  VkSwapchainCreateInfoKHR ci = *pCreateInfo;

  // Force IMMEDIATE present mode in scanline mode (so the tear line is movable).
  bool forced_immediate = false;
  if (g_cfg.mode == Mode::ScanlineSync && g_cfg.force_immediate &&
      ci.presentMode != VK_PRESENT_MODE_IMMEDIATE_KHR &&
      dev->instance && dev->instance->GetPhysicalDeviceSurfacePresentModesKHR) {
    uint32_t count = 0;
    VkResult r = dev->instance->GetPhysicalDeviceSurfacePresentModesKHR(
        dev->physicalDevice, ci.surface, &count, nullptr);
    if (r == VK_SUCCESS && count) {
      std::vector<VkPresentModeKHR> modes(count);
      r = dev->instance->GetPhysicalDeviceSurfacePresentModesKHR(
          dev->physicalDevice, ci.surface, &count, modes.data());
      if (r == VK_SUCCESS) {
        for (auto m : modes) {
          if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            forced_immediate = true;
            break;
          }
        }
      }
    }
    if (!forced_immediate)
      log("scanline: VK_PRESENT_MODE_IMMEDIATE_KHR not available; "
          "tearline cannot be moved");
  }

  // Query present-timing caps for this surface.
  if (g_cfg.want_present_timing)
    query_present_timing_caps(dev, ci.surface);

  // Add the present-timing create flag so the swapchain records timing.
  bool timing_flag = false;
  if (g_cfg.mode == Mode::ScanlineSync && dev->present_timing_enabled &&
      dev->caps_present_timing) {
    if (driver_crashes_present_timing(dev)) {
      static bool warned = false;
      if (!warned) {
        log("present_timing: VK_EXT_present_timing create flag crashes RADV; "
            "disabling timing feedback. Manual scanline pacing still active.");
        warned = true;
      }
      dev->present_timing_enabled = false;
    } else {
      ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
      timing_flag = true;
    }
  }

  VkResult res =
      dev->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
  if (res != VK_SUCCESS)
    return res;

  // Initialize swapchain state.
  {
    std::lock_guard<std::mutex> lk(dev->mutex);
    SwapchainState &st = dev->swapchains[*pSwapchain];
    st.width = ci.imageExtent.width;
    st.height = ci.imageExtent.height;
    st.present_mode = ci.presentMode;
    st.immediate_active = (ci.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR);
    st.scanline = g_live_scanline.load(std::memory_order_relaxed);
    st.refresh_ns = static_cast<uint64_t>(g_cfg.fallback_refresh.count());
    recompute_scanline_offset(st);

    if (timing_flag) {
      st.timing_enabled = true;
      st.first_pixel_out_supported = dev->caps_first_pixel_out;
      if (dev->SetSwapchainPresentTimingQueueSizeEXT) {
        VkResult qr = dev->SetSwapchainPresentTimingQueueSizeEXT(
            dev->device, *pSwapchain, 8);
        if (qr != VK_SUCCESS)
          log("present_timing: SetSwapchainPresentTimingQueueSizeEXT %d", qr);
      }
      // Try to read refresh immediately.
      query_refresh(dev, *pSwapchain, st);
      recompute_scanline_offset(st);
      // Resolve a valid time-domain id up front. Without this the present-time
      // VkPresentTimingInfoEXT.timeDomainId would be 0 and crash the driver.
      query_time_domains(dev, *pSwapchain, st);
      st.feedback_active =
          dev->caps_first_pixel_out && dev->GetPastPresentationTimingEXT != nullptr
          && st.domain_known;
      if (st.vrr_like)
        log("scanline: VRR-like refresh reported (refreshInterval!=Duration)");
    }
    log("swapchain created %ux%u mode=%u immediate=%d timing=%d feedback=%d",
        st.width, st.height, static_cast<unsigned>(st.present_mode),
        st.immediate_active ? 1 : 0, st.timing_enabled ? 1 : 0,
        st.feedback_active ? 1 : 0);
  }
  return res;
}

VK_LAYER_EXPORT void VKAPI_CALL
vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                      const VkAllocationCallbacks *pAllocator) {
  DeviceLayer *dev = get_device(device);
  if (!dev || !dev->DestroySwapchainKHR)
    return;
  {
    std::lock_guard<std::mutex> lk(dev->mutex);
    dev->swapchains.erase(swapchain);
  }
  if (dev->DestroySwapchainKHR)
    dev->DestroySwapchainKHR(device, swapchain, pAllocator);
}

// ---- Acquire ----

VK_LAYER_EXPORT VkResult VKAPI_CALL
vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                      VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex) {
  DeviceLayer *dev = get_device(device);
  if (dev) {
    if (g_cfg.mode == Mode::ScanlineSync)
      scanline_acquire_pace(dev, swapchain);
    else if (g_cfg.mode == Mode::FpsLimit)
      fps_acquire_pace(dev, swapchain);
  }
  VkResult res = dev && dev->AcquireNextImageKHR
                     ? dev->AcquireNextImageKHR(device, swapchain, timeout,
                                                semaphore, fence, pImageIndex)
                     : VK_ERROR_INITIALIZATION_FAILED;
  if (dev)
    scanline_record_acquire(dev, swapchain, res);
  return res;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *pInfo,
                       uint32_t *pImageIndex) {
  DeviceLayer *dev = get_device(device);
  if (dev) {
    if (g_cfg.mode == Mode::ScanlineSync)
      scanline_acquire_pace(dev, pInfo->swapchain);
    else if (g_cfg.mode == Mode::FpsLimit)
      fps_acquire_pace(dev, pInfo->swapchain);
  }
  VkResult res = dev && dev->AcquireNextImage2KHR
                     ? dev->AcquireNextImage2KHR(device, pInfo, pImageIndex)
                     : VK_ERROR_INITIALIZATION_FAILED;
  if (dev)
    scanline_record_acquire(dev, pInfo->swapchain, res);
  return res;
}

// ---- Present ----

VK_LAYER_EXPORT VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
  DeviceLayer *dev = get_device_by_queue(queue);
  if (!dev || !dev->QueuePresentKHR)
    return VK_ERROR_INITIALIZATION_FAILED;

  // Scanline: small final wait before present, then chain our timing request.
  VkSwapchainKHR primary = (pPresentInfo && pPresentInfo->swapchainCount)
                               ? pPresentInfo->pSwapchains[0]
                               : VK_NULL_HANDLE;
  Clock::time_point target{};
  if (g_cfg.mode == Mode::ScanlineSync && primary != VK_NULL_HANDLE)
    target = scanline_present_pace(dev, primary);

  apply_hotkeys();

  // Build a present-info copy with our pNext chain (presentId + stage query),
  // without disturbing the app's existing chain.
  VkPresentInfoKHR info = *pPresentInfo;
  std::vector<uint64_t> our_ids;
  std::vector<VkPresentTimingInfoEXT> tinfo;
  VkPresentIdKHR pid{};
  VkPresentTimingsInfoEXT timings{};
  void *our_head = const_cast<void *>(pPresentInfo->pNext);

  bool inject_pid = dev->present_id_enabled && g_cfg.mode == Mode::ScanlineSync;
  bool inject_timing =
      dev->present_timing_enabled && dev->caps_present_timing &&
      dev->caps_first_pixel_out && g_cfg.mode == Mode::ScanlineSync;

  // Resolve a valid time-domain id per swapchain. We can only safely chain
  // VkPresentTimingInfoEXT when every swapchain in this present has a known
  // domain id (timeDomainId is an opaque driver handle; 0 crashes RADV).
  std::vector<uint64_t> domain_ids;
  if (inject_timing && info.swapchainCount) {
    domain_ids.resize(info.swapchainCount, 0);
    std::lock_guard<std::mutex> lk(dev->mutex);
    for (uint32_t i = 0; i < info.swapchainCount; i++) {
      auto it = dev->swapchains.find(info.pSwapchains[i]);
      if (it == dev->swapchains.end() || !it->second.feedback_active) {
        inject_timing = false;
        break;
      }
      domain_ids[i] = it->second.timing_domain_id;
    }
  }

  if (inject_timing) {
    timings.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;
    // Per-swapchain timing request: ask for first-pixel-out feedback.
    tinfo.resize(info.swapchainCount);
    for (uint32_t i = 0; i < info.swapchainCount; i++) {
      tinfo[i].sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
      tinfo[i].flags = 0;
      tinfo[i].targetTime = 0; // no scheduling; we self-pace by timing the call
      tinfo[i].timeDomainId = domain_ids[i];
      tinfo[i].presentStageQueries =
          VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
      tinfo[i].targetTimeDomainPresentStage = 0;
    }
    timings.swapchainCount = info.swapchainCount;
    timings.pTimingInfos = tinfo.data();
    timings.pNext = our_head;
    our_head = &timings;
  }

  if (inject_pid) {
    pid.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    // Use the app's ids if it already supplied VkPresentIdKHR; else our own.
    const auto *app_pid = static_cast<const VkPresentIdKHR *>(
        find_in_chain(pPresentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR));
    if (app_pid && app_pid->pPresentIds) {
      // Record a pending entry keyed by the first id.
      pid = *app_pid;
    } else {
      our_ids.resize(info.swapchainCount);
      uint64_t base = 0;
      {
        std::lock_guard<std::mutex> lk(dev->mutex);
        auto it = dev->swapchains.find(primary);
        if (it != dev->swapchains.end()) {
          base = it->second.next_present_id;
          it->second.next_present_id += info.swapchainCount;
        }
      }
      for (uint32_t i = 0; i < info.swapchainCount; i++)
        our_ids[i] = base + i;
      pid.swapchainCount = info.swapchainCount;
      pid.pPresentIds = our_ids.data();
      pid.pNext = our_head;
      our_head = &pid;
    }
  }

  info.pNext = our_head;

  // Record the pending target for each swapchain (present call time + target).
  if (inject_pid) {
    std::lock_guard<std::mutex> lk(dev->mutex);
    Clock::time_point call = Clock::now();
    for (uint32_t i = 0; i < info.swapchainCount; i++) {
      auto it = dev->swapchains.find(info.pSwapchains[i]);
      if (it == dev->swapchains.end())
        continue;
      uint64_t id = pid.pPresentIds[i];
      Clock::time_point tgt = (i == 0) ? target : it->second.next_acquire_target;
      it->second.pending[id] = {call, tgt};
    }
  }

  VkResult res = dev->QueuePresentKHR(queue, &info);

  // Optionally wait for the present to complete (helps keep queue depth low).
  if (dev->present_wait_enabled && dev->WaitForPresentKHR &&
      inject_pid && res == VK_SUCCESS) {
    dev->WaitForPresentKHR(dev->device, primary, pid.pPresentIds[0],
                           50'000'000ULL);
  }

  return res;
}

// ---- Surface creation (Wayland key-input init) ----

VK_LAYER_EXPORT VkResult VKAPI_CALL
vkCreateWaylandSurfaceKHR(VkInstance instance,
                          const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkSurfaceKHR *pSurface) {
  InstanceLayer *layer = get_instance(instance);
  PFN_vkCreateWaylandSurfaceKHR fp = nullptr;
  if (layer && layer->GetInstanceProcAddr)
    fp = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
        layer->GetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR"));
  if (!fp)
    return VK_ERROR_INITIALIZATION_FAILED;
  VkResult res = fp(instance, pCreateInfo, pAllocator, pSurface);
  if (res == VK_SUCCESS && pCreateInfo && pCreateInfo->display) {
    vfc::key_input::init_wayland_display(pCreateInfo->display,
                                         pCreateInfo->surface);
  }
  return res;
}

VK_LAYER_EXPORT void VKAPI_CALL
vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                    const VkAllocationCallbacks *pAllocator) {
  InstanceLayer *layer = get_instance(instance);
  if (layer && layer->DestroySurfaceKHR)
    layer->DestroySurfaceKHR(instance, surface, pAllocator);
  // We don't track Wayland surface->wl_surface mapping for unref here; the
  // key_input display is ref-counted by create and torn down at instance loss.
}

// =============================================================================
// Section 10 — proc-addr resolution & layer negotiation
// =============================================================================

#define NAME_EQ(x) (std::strcmp(pName, x) == 0)

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName) {
  if (!pName)
    return nullptr;
  if (NAME_EQ("vkGetDeviceProcAddr"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
  if (NAME_EQ("vkCreateSwapchainKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR);
  if (NAME_EQ("vkDestroySwapchainKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkDestroySwapchainKHR);
  if (NAME_EQ("vkAcquireNextImageKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImageKHR);
  if (NAME_EQ("vkAcquireNextImage2KHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImage2KHR);
  if (NAME_EQ("vkQueuePresentKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR);
  if (NAME_EQ("vkGetDeviceQueue"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue);
  if (NAME_EQ("vkGetDeviceQueue2"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue2);

  DeviceLayer *dev = get_device(device);
  if (dev && dev->GetDeviceProcAddr)
    return dev->GetDeviceProcAddr(device, pName);
  return nullptr;
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
  if (!pName)
    return nullptr;
  if (NAME_EQ("vkGetInstanceProcAddr"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
  if (NAME_EQ("vkGetDeviceProcAddr"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
  if (NAME_EQ("vkCreateInstance"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance);
  if (NAME_EQ("vkDestroyInstance"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance);
  if (NAME_EQ("vkEnumeratePhysicalDevices"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkEnumeratePhysicalDevices);
  if (NAME_EQ("vkCreateDevice"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice);
  if (NAME_EQ("vkDestroyDevice"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice);
  if (NAME_EQ("vkCreateWaylandSurfaceKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkCreateWaylandSurfaceKHR);
  if (NAME_EQ("vkDestroySurfaceKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkDestroySurfaceKHR);
  // Device-level hooks also queryable via instance gpa:
  if (NAME_EQ("vkCreateSwapchainKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR);
  if (NAME_EQ("vkDestroySwapchainKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkDestroySwapchainKHR);
  if (NAME_EQ("vkAcquireNextImageKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImageKHR);
  if (NAME_EQ("vkAcquireNextImage2KHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImage2KHR);
  if (NAME_EQ("vkQueuePresentKHR"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR);
  if (NAME_EQ("vkGetDeviceQueue"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue);
  if (NAME_EQ("vkGetDeviceQueue2"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue2);

  InstanceLayer *layer = get_instance(instance);
  if (layer && layer->GetInstanceProcAddr)
    return layer->GetInstanceProcAddr(instance, pName);
  return nullptr;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct) {
  if (!pVersionStruct || pVersionStruct->loaderLayerInterfaceVersion < 1)
    return VK_ERROR_INITIALIZATION_FAILED;
  pVersionStruct->loaderLayerInterfaceVersion = 2;
  pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
  // pfnGetPhysicalDeviceProcAddr is optional; leave null.
  return VK_SUCCESS;
}

} // extern "C"

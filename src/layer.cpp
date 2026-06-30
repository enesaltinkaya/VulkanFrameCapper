#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "key_input.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <strings.h>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName);
extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName);
extern "C" VK_LAYER_EXPORT VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct);

namespace {

using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;

bool env_bool(const char* name, bool fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v)
    return fallback;

  return std::strcmp(v, "0") != 0
      && strcasecmp(v, "false") != 0
      && strcasecmp(v, "no") != 0
      && strcasecmp(v, "off") != 0;
}

double env_double(const char* name, double fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v)
    return fallback;

  char* end = nullptr;
  double d = std::strtod(v, &end);
  return end && end != v ? d : fallback;
}

struct ScanlineConfig {
  bool active = false;
  int64_t line = 0;
};

uint64_t refresh_ns_from_hz(double hz) {
  if (!std::isfinite(hz) || hz <= 1.0)
    return 16'666'666;

  return static_cast<uint64_t>(std::llround(1'000'000'000.0 / hz));
}

uint64_t env_refresh_ns() {
  // Optional manual refresh override for scanline mode. Without present-timing
  // feedback Vulkan does not expose the physical scanout cadence to us.
  double hz = env_double("VFC_SCANLINE_REFRESH_HZ", 0.0);
  if (hz <= 0.0)
    hz = env_double("VFC_REFRESH_HZ", 0.0);
  return refresh_ns_from_hz(hz);
}

double env_clamped_double(const char* name, double fallback, double min_value, double max_value) {
  double value = env_double(name, fallback);
  if (!std::isfinite(value))
    return fallback;
  return std::clamp(value, min_value, max_value);
}

Ns env_microseconds(const char* name, double fallback_us) {
  return std::chrono::duration_cast<Ns>(std::chrono::duration<double, std::micro>(
      env_double(name, fallback_us)));
}

ScanlineConfig env_scanline() {
  const char* v = std::getenv("SCANLINE");
  if (!v || !*v)
    return {};

  errno = 0;
  char* end = nullptr;
  long long line = std::strtoll(v, &end, 10);
  if (errno || end == v || (end && *end))
    return {};

  return {true, static_cast<int64_t>(line)};
}

bool g_log = env_bool("VFC_LOG", false);
ScanlineConfig g_scanline = env_scanline();
uint64_t g_scanline_fallback_refresh_ns = env_refresh_ns();
double g_scanline_vtotal_scale = env_clamped_double("VFC_SCANLINE_VTOTAL_SCALE", 1.05, 1.0, 1.30);
Ns g_scanline_offset = env_microseconds("VFC_SCANLINE_OFFSET_US", 0.0);
Ns g_scanline_acquire_margin = env_microseconds("VFC_SCANLINE_ACQUIRE_MARGIN_US", 750.0);
Ns g_scanline_max_present_wait = env_microseconds("VFC_SCANLINE_MAX_PRESENT_WAIT_US", 4000.0);
double g_target_fps = g_scanline.active ? 0.0 : env_double("FPS", 0.0);
bool g_cap_active = !g_scanline.active && g_target_fps > 0.0 && std::isnormal(g_target_fps);
// VK_EXT_present_timing feedback can freeze some games at launch (swapchain
// timing flag / queue size). Opt-in only; manual scanline pacing works without it.
bool g_want_present_timing = g_scanline.active && env_bool("VFC_PRESENT_TIMING", false);

void log(const char* fmt, ...) {
  if (!g_log)
    return;

  va_list args;
  va_start(args, fmt);
  std::fprintf(stderr, "[VulkanFrameCapper] ");
  std::vfprintf(stderr, fmt, args);
  std::fprintf(stderr, "\n");
  va_end(args);
}

class DxvkStyleSleep {
public:
  static Clock::time_point sleep_until(Clock::time_point t0, Clock::time_point t1) {
    return sleep_for(t0, std::chrono::duration_cast<Ns>(t1 - t0));
  }

private:
  static Clock::time_point sleep_for(Clock::time_point t0, Ns duration) {
    if (duration <= Ns::zero())
      return t0;

    // DXVK assumes 0.5 ms granularity on non-Windows and busy-waits
    // the final part for better timing accuracy.
    constexpr Ns sleep_granularity = std::chrono::duration_cast<Ns>(std::chrono::microseconds(500));
    Ns sleep_threshold = 4 * sleep_granularity;
    sleep_threshold += duration / 6;

    Ns remaining = duration;
    Clock::time_point t1 = t0;

    while (remaining > sleep_threshold) {
      std::this_thread::sleep_for(remaining - sleep_threshold);

      t1 = Clock::now();
      remaining -= std::chrono::duration_cast<Ns>(t1 - t0);
      t0 = t1;
    }

    while (remaining > Ns::zero()) {
      t1 = Clock::now();
      remaining -= std::chrono::duration_cast<Ns>(t1 - t0);
      t0 = t1;
    }

    return t1;
  }
};

class FpsLimiter {
public:
  FpsLimiter() {
    set_fps(g_target_fps);
  }

  void set_fps(double fps) {
    std::lock_guard<std::mutex> lock(m_mutex);
    set_fps_locked(fps);
  }

  void adjust(double delta) {
    std::lock_guard<std::mutex> lock(m_mutex);
    set_fps_locked(std::max(0.0, m_fps + delta));
  }

  bool enabled() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_interval != Ns::zero();
  }

  void delay() {
    std::unique_lock<std::mutex> lock(m_mutex);
    auto interval = m_interval;

    if (interval == Ns::zero()) {
      m_next_frame = Clock::time_point();
      return;
    }

    auto t1 = Clock::now();
    auto next_frame = m_next_frame;

    // Same structure as DXVK: unlock while sleeping, then update the
    // absolute next-frame schedule based on the pre-sleep timestamp.
    lock.unlock();

    if (t1 < next_frame)
      DxvkStyleSleep::sleep_until(t1, next_frame);

    lock.lock();
    m_next_frame = (t1 < m_next_frame + interval)
      ? m_next_frame + interval
      : t1 + interval;
  }

private:
  void set_fps_locked(double fps) {
    m_fps = fps > 0.0 && std::isnormal(fps) ? fps : 0.0;

    if (m_fps > 0.0)
      m_interval = Ns(int64_t(1'000'000'000.0 / m_fps));
    else
      m_interval = Ns::zero();

    m_next_frame = Clock::time_point();
    log("FPS limit %.2f (%lld ns)", m_fps, static_cast<long long>(m_interval.count()));
  }

  std::mutex m_mutex;
  double m_fps = 0.0;
  Ns m_interval = Ns::zero();
  Clock::time_point m_next_frame = Clock::time_point();
};

FpsLimiter g_limiter;
bool g_want_present_wait = g_cap_active && env_bool("VFC_PRESENT_WAIT", true);

struct HotkeyState {
  bool held = false;
  Clock::time_point first_press = Clock::time_point();
  Clock::time_point last_action = Clock::time_point();
};

bool hotkey_should_fire(bool pressed, HotkeyState& state, Clock::time_point now) {
  using namespace std::chrono_literals;

  if (!pressed) {
    state.held = false;
    return false;
  }

  if (!state.held) {
    state.held = true;
    state.first_press = now;
    state.last_action = now;
    return true;
  }

  if (now - state.first_press >= 400ms && now - state.last_action >= 100ms) {
    state.last_action = now;
    return true;
  }

  return false;
}

void check_hotkeys() {
  using namespace std::chrono_literals;

  static Clock::time_point last_poll = Clock::time_point();
  static HotkeyState increase_state;
  static HotkeyState decrease_state;
  static HotkeyState increase_fast_state;
  static HotkeyState decrease_fast_state;

  auto now = Clock::now();
  if (now - last_poll < 50ms)
    return;
  last_poll = now;

  bool increase_fast = vfc::key_input::increase_fast_pressed();
  bool decrease_fast = vfc::key_input::decrease_fast_pressed();

  if (hotkey_should_fire(increase_fast, increase_fast_state, now))
    g_limiter.adjust(+0.10);

  if (hotkey_should_fire(decrease_fast, decrease_fast_state, now))
    g_limiter.adjust(-0.10);

  // Ctrl+Plus on many layouts is physically Ctrl+Shift+=, so suppress
  // the Shift+Plus 0.01 hotkey while the Ctrl hotkey is active.
  if (hotkey_should_fire(!increase_fast && vfc::key_input::increase_pressed(), increase_state, now))
    g_limiter.adjust(+0.01);

  if (hotkey_should_fire(!decrease_fast && vfc::key_input::decrease_pressed(), decrease_state, now))
    g_limiter.adjust(-0.01);
}

template <typename T>
const T* find_pnext(const void* pNext, VkStructureType sType) {
  auto* base = reinterpret_cast<const VkBaseInStructure*>(pNext);
  while (base) {
    if (base->sType == sType)
      return reinterpret_cast<const T*>(base);
    base = base->pNext;
  }
  return nullptr;
}

bool has_extension(const std::vector<VkExtensionProperties>& exts, const char* name) {
  return std::any_of(exts.begin(), exts.end(), [name](const VkExtensionProperties& e) {
    return std::strcmp(e.extensionName, name) == 0;
  });
}

bool name_in_list(const char* name, uint32_t count, const char* const* names) {
  for (uint32_t i = 0; i < count; i++)
    if (names[i] && std::strcmp(names[i], name) == 0)
      return true;
  return false;
}

template <typename T>
T get_layer_link_info(const void* pNext, VkLayerFunction func, VkStructureType type) {
  auto* chain = reinterpret_cast<const VkLayerInstanceCreateInfo*>(pNext);
  while (chain) {
    if (chain->sType == type && chain->function == func)
      return reinterpret_cast<T>(const_cast<VkLayerInstanceCreateInfo*>(chain));
    chain = reinterpret_cast<const VkLayerInstanceCreateInfo*>(chain->pNext);
  }
  return nullptr;
}

struct InstanceDispatch {
  VkInstance instance = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
  PFN_vkDestroyInstance DestroyInstance = nullptr;
  PFN_vkCreateDevice CreateDevice = nullptr;
  PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
  PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
  PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR GetPhysicalDeviceSurfaceCapabilities2KHR = nullptr;
  PFN_vkCreateWaylandSurfaceKHR CreateWaylandSurfaceKHR = nullptr;
  PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
};

struct SwapchainState {
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_MAX_ENUM_KHR;
  VkExtent2D extent = {};
  uint64_t refresh_ns = 16'666'666;
  bool present_timing_requested = false;
  bool first_pixel_out_supported = false;
  bool refresh_measured = false;
  bool scanline_logged = false;
  bool non_immediate_logged = false;
  Clock::time_point phase_base = Clock::time_point();
  Clock::time_point next_present_target = Clock::time_point();
  Clock::time_point last_acquire_done = Clock::time_point();
  Clock::time_point last_refresh_query = Clock::time_point();
  Ns render_lead = std::chrono::milliseconds(3);
};

struct DeviceDispatch {
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  InstanceDispatch* instance = nullptr;
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
  PFN_vkDestroyDevice DestroyDevice = nullptr;
  PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
  PFN_vkGetDeviceQueue2 GetDeviceQueue2 = nullptr;
  PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
  PFN_vkWaitForPresentKHR WaitForPresentKHR = nullptr;
  PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
  PFN_vkAcquireNextImage2KHR AcquireNextImage2KHR = nullptr;
  PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
  PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
  PFN_vkSetSwapchainPresentTimingQueueSizeEXT SetSwapchainPresentTimingQueueSizeEXT = nullptr;
  PFN_vkGetSwapchainTimingPropertiesEXT GetSwapchainTimingPropertiesEXT = nullptr;
  PFN_vkGetPastPresentationTimingEXT GetPastPresentationTimingEXT = nullptr;

  bool present_id_enabled = false;
  bool present_wait_enabled = false;
  bool present_timing_enabled = false;

  std::mutex present_mutex;
  std::unordered_map<VkSwapchainKHR, uint64_t> present_ids;
  std::unordered_map<VkSwapchainKHR, VkPresentModeKHR> present_modes;
  std::unordered_map<VkSwapchainKHR, SwapchainState> swapchains;
};

std::mutex g_mutex;
std::unordered_map<VkInstance, InstanceDispatch> g_instances;
std::unordered_map<VkPhysicalDevice, InstanceDispatch*> g_phys_instances;
std::unordered_map<VkDevice, std::unique_ptr<DeviceDispatch>> g_devices;
std::unordered_map<VkQueue, DeviceDispatch*> g_queues;

InstanceDispatch* get_instance_dispatch(VkInstance instance) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_instances.find(instance);
  return it == g_instances.end() ? nullptr : &it->second;
}

DeviceDispatch* get_device_dispatch(VkDevice device) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_devices.find(device);
  return it == g_devices.end() ? nullptr : it->second.get();
}

DeviceDispatch* get_queue_dispatch(VkQueue queue) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_queues.find(queue);
  return it == g_queues.end() ? nullptr : it->second;
}

bool should_wait_for_present_mode(VkPresentModeKHR mode) {
  // Match DXVK: only wait for actual FIFO vsync modes. Waiting for
  // MAILBOX/IMMEDIATE can effectively clamp to monitor refresh on some WSI
  // paths, especially XWayland.
  return mode == VK_PRESENT_MODE_FIFO_KHR
      || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
}

VkPresentModeKHR get_present_mode(DeviceDispatch* dev,
                                  VkSwapchainKHR swapchain,
                                  const VkSwapchainPresentModeInfoEXT* mode_info,
                                  uint32_t index) {
  if (mode_info && index < mode_info->swapchainCount && mode_info->pPresentModes)
    return mode_info->pPresentModes[index];

  std::lock_guard<std::mutex> lock(dev->present_mutex);
  auto it = dev->present_modes.find(swapchain);
  return it == dev->present_modes.end() ? VK_PRESENT_MODE_MAX_ENUM_KHR : it->second;
}

bool surface_supports_present_mode(DeviceDispatch* dev, VkSurfaceKHR surface, VkPresentModeKHR wanted) {
  if (!dev || !dev->instance || !dev->instance->GetPhysicalDeviceSurfacePresentModesKHR)
    return false;

  uint32_t count = 0;
  VkResult r = dev->instance->GetPhysicalDeviceSurfacePresentModesKHR(dev->physical_device, surface, &count, nullptr);
  if (r != VK_SUCCESS || !count)
    return false;

  std::vector<VkPresentModeKHR> modes(count);
  r = dev->instance->GetPhysicalDeviceSurfacePresentModesKHR(dev->physical_device, surface, &count, modes.data());
  if (r != VK_SUCCESS && r != VK_INCOMPLETE)
    return false;

  return std::find(modes.begin(), modes.begin() + count, wanted) != modes.begin() + count;
}

struct SurfaceTimingCaps {
  bool present_timing = false;
  bool first_pixel_out = false;
};

SurfaceTimingCaps query_surface_timing_caps(DeviceDispatch* dev, VkSurfaceKHR surface) {
  SurfaceTimingCaps result{};

  if (!dev || !dev->instance || !dev->instance->GetPhysicalDeviceSurfaceCapabilities2KHR)
    return result;

  VkPresentTimingSurfaceCapabilitiesEXT timing_caps{};
  timing_caps.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;

  VkSurfaceCapabilities2KHR caps{};
  caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
  caps.pNext = &timing_caps;

  VkPhysicalDeviceSurfaceInfo2KHR surface_info{};
  surface_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
  surface_info.surface = surface;

  VkResult r = dev->instance->GetPhysicalDeviceSurfaceCapabilities2KHR(
      dev->physical_device, &surface_info, &caps);
  if (r != VK_SUCCESS)
    return result;

  result.present_timing = timing_caps.presentTimingSupported;
  result.first_pixel_out = !!(timing_caps.presentStageQueries & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
  return result;
}

uint64_t query_swapchain_refresh_ns(DeviceDispatch* dev, VkSwapchainKHR swapchain, bool* measured = nullptr) {
  if (measured)
    *measured = false;

  if (!dev || !dev->present_timing_enabled || !dev->GetSwapchainTimingPropertiesEXT)
    return g_scanline_fallback_refresh_ns;

  VkSwapchainTimingPropertiesEXT timing{};
  timing.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
  uint64_t counter = 0;

  VkResult r = dev->GetSwapchainTimingPropertiesEXT(dev->device, swapchain, &timing, &counter);
  if (r != VK_SUCCESS || !timing.refreshDuration)
    return g_scanline_fallback_refresh_ns;

  if (timing.refreshInterval == UINT64_MAX)
    log("scanline: VRR-like refresh interval reported; fixed scanline sync may be unstable");

  if (measured)
    *measured = true;
  return timing.refreshDuration;
}

Ns scanline_default_render_lead(uint64_t refresh_ns) {
  Ns refresh(static_cast<int64_t>(refresh_ns ? refresh_ns : g_scanline_fallback_refresh_ns));
  return std::clamp(std::chrono::duration_cast<Ns>(std::chrono::milliseconds(3)),
      std::chrono::duration_cast<Ns>(std::chrono::microseconds(250)), refresh / 2);
}

Ns scanline_max_render_lead(uint64_t refresh_ns) {
  Ns refresh(static_cast<int64_t>(refresh_ns ? refresh_ns : g_scanline_fallback_refresh_ns));
  Ns max_lead = refresh - std::chrono::microseconds(1000);
  return std::max(max_lead, std::chrono::duration_cast<Ns>(std::chrono::microseconds(250)));
}

Ns positive_mod(Ns value, Ns period) {
  if (period <= Ns::zero())
    return Ns::zero();

  int64_t rem = value.count() % period.count();
  if (rem < 0)
    rem += period.count();
  return Ns(rem);
}

Ns scanline_offset_for_state(const SwapchainState& state) {
  uint32_t height = std::max(1u, state.extent.height);
  Ns refresh(static_cast<int64_t>(state.refresh_ns ? state.refresh_ns : g_scanline_fallback_refresh_ns));
  long double vtotal = std::max<long double>(1.0L,
      static_cast<long double>(height) * static_cast<long double>(g_scanline_vtotal_scale));
  auto scanline_ns = Ns(static_cast<int64_t>(std::llround(
      static_cast<long double>(refresh.count()) * static_cast<long double>(g_scanline.line) / vtotal)));

  // Normalize to one refresh. Negative scanlines intentionally wrap to the end
  // of the previous refresh, which is how users hide the tearline near vblank.
  return positive_mod(scanline_ns + g_scanline_offset, refresh);
}

Clock::time_point scanline_compute_target_locked(SwapchainState& state, Clock::time_point now) {
  uint32_t height = std::max(1u, state.extent.height);
  Ns refresh(static_cast<int64_t>(state.refresh_ns ? state.refresh_ns : g_scanline_fallback_refresh_ns));
  Ns offset = scanline_offset_for_state(state);

  if (!state.scanline_logged) {
    log("scanline sync active: line=%lld height=%u vtotal_scale=%.3f offset=%lld us refresh=%llu ns measured=%d present_mode=%d timing=%d first_pixel_out=%d",
        static_cast<long long>(g_scanline.line), height, g_scanline_vtotal_scale,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(offset).count()),
        static_cast<unsigned long long>(state.refresh_ns), state.refresh_measured,
        state.present_mode, state.present_timing_requested, state.first_pixel_out_supported);
    state.scanline_logged = true;
  }

  if (state.present_mode != VK_PRESENT_MODE_IMMEDIATE_KHR && !state.non_immediate_logged) {
    log("scanline: swapchain is not IMMEDIATE present mode (%d); visible tearline control is unlikely",
        state.present_mode);
    state.non_immediate_logged = true;
  }

  if (state.phase_base == Clock::time_point())
    state.phase_base = now;

  Clock::time_point target = state.phase_base + offset;
  if (target <= now) {
    auto late = std::chrono::duration_cast<Ns>(now - target);
    int64_t periods = late.count() / refresh.count() + 1;
    Ns advance(refresh.count() * periods);
    state.phase_base += advance;
    target += advance;
  }

  state.next_present_target = target;
  return target;
}

void scanline_acquire_delay(DeviceDispatch* dev, VkSwapchainKHR swapchain) {
  if (!g_scanline.active || !swapchain)
    return;

  auto now = Clock::now();
  Clock::time_point wake = now;

  {
    std::lock_guard<std::mutex> lock(dev->present_mutex);
    auto it = dev->swapchains.find(swapchain);
    if (it == dev->swapchains.end())
      return;

    SwapchainState& state = it->second;
    // Lazily re-query refresh duration: some drivers only populate it after
    // the first few presents, so the initial value may still be the fallback.
    if (state.present_timing_requested && !state.refresh_measured &&
        (state.last_refresh_query == Clock::time_point() || now - state.last_refresh_query > std::chrono::seconds(1))) {
      state.last_refresh_query = now;
      bool measured = false;
      uint64_t refresh_ns = query_swapchain_refresh_ns(dev, swapchain, &measured);
      if (measured) {
        state.refresh_ns = refresh_ns;
        state.refresh_measured = true;
        state.render_lead = std::min(state.render_lead, scanline_max_render_lead(state.refresh_ns));
        state.scanline_logged = false;
        log("scanline: measured refresh %llu ns", static_cast<unsigned long long>(refresh_ns));
      }
    }
    Clock::time_point target = scanline_compute_target_locked(state, now);
    // Start rendering based on measured acquire->present time. This keeps the
    // input-lag win of frame-start pacing while adapting enough lead time to
    // avoid missing the target scanline and causing jumps.
    wake = target - state.render_lead - g_scanline_acquire_margin;
  }

  if (wake > now)
    DxvkStyleSleep::sleep_until(now, wake);
}

void scanline_mark_acquire_done(DeviceDispatch* dev, VkSwapchainKHR swapchain, VkResult result) {
  if (!g_scanline.active || !swapchain || (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR))
    return;

  std::lock_guard<std::mutex> lock(dev->present_mutex);
  auto it = dev->swapchains.find(swapchain);
  if (it != dev->swapchains.end())
    it->second.last_acquire_done = Clock::now();
}

// Present-timing feedback polling. Currently unused while the feedback path is
// disabled to avoid launch freezes; retained for the future PLL implementation.
[[maybe_unused]]
void scanline_poll_feedback(DeviceDispatch* dev, VkSwapchainKHR swapchain) {
  if (!g_scanline.active || !dev || !dev->present_timing_enabled || !dev->GetPastPresentationTimingEXT || !swapchain)
    return;

  // Throttle to ~2 Hz to avoid log spam and overhead.
  static std::mutex poll_mutex;
  static std::unordered_map<VkSwapchainKHR, Clock::time_point> last_poll;
  {
    std::lock_guard<std::mutex> lock(poll_mutex);
    auto& t = last_poll[swapchain];
    auto now = Clock::now();
    if (now - t < std::chrono::milliseconds(500))
      return;
    t = now;
  }

  VkPastPresentationTimingInfoEXT info{};
  info.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
  info.swapchain = swapchain;

  VkPastPresentationTimingPropertiesEXT props{};
  props.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;

  uint32_t count = 0;
  (void)count;
  VkResult r = dev->GetPastPresentationTimingEXT(dev->device, &info, &props);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE && r != VK_NOT_READY)
    return;
  if (!props.presentationTimingCount)
    return;

  std::vector<VkPastPresentationTimingEXT> timings(props.presentationTimingCount);
  for (auto& t : timings) {
    t.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
    t.presentStageCount = 0;
  }
  props.pPresentationTimings = timings.data();
  r = dev->GetPastPresentationTimingEXT(dev->device, &info, &props);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE)
    return;

  // Log the newest entry's stages + time domain, to confirm feedback is real
  // and discover which time domain the stage timestamps use.
  const auto& last = timings[props.presentationTimingCount - 1];
  const char* td = "?";
  switch (last.timeDomain) {
    case VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT: td = "MONOTONIC"; break;
    case VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT: td = "MONOTONIC_RAW"; break;
    case VK_TIME_DOMAIN_DEVICE_EXT: td = "DEVICE"; break;
    case VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT: td = "PRESENT_STAGE_LOCAL"; break;
    case VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT: td = "SWAPCHAIN_LOCAL"; break;
    default: break;
  }
  log("scanline feedback: id=%llu td=%s stages=%u complete=%d",
      static_cast<unsigned long long>(last.presentId), td,
      last.presentStageCount, last.reportComplete);
  for (uint32_t i = 0; i < last.presentStageCount; i++)
    log("  stage=0x%x time=%llu", last.pPresentStages[i].stage,
        static_cast<unsigned long long>(last.pPresentStages[i].time));
}

void scanline_present_delay(DeviceDispatch* dev,
                            const VkPresentInfoKHR* pPresentInfo,
                            const VkSwapchainPresentModeInfoEXT* mode_info) {
  if (!g_scanline.active || !pPresentInfo || !pPresentInfo->swapchainCount || !pPresentInfo->pSwapchains)
    return;

  VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[0];
  auto now = Clock::now();
  Clock::time_point target = now;

  {
    std::lock_guard<std::mutex> lock(dev->present_mutex);
    auto it = dev->swapchains.find(swapchain);
    if (it == dev->swapchains.end())
      return;

    SwapchainState& state = it->second;
    if (mode_info && mode_info->swapchainCount && mode_info->pPresentModes)
      state.present_mode = mode_info->pPresentModes[0];

    if (state.last_acquire_done != Clock::time_point() && now > state.last_acquire_done) {
      Ns measured = std::chrono::duration_cast<Ns>(now - state.last_acquire_done);
      measured = std::clamp(measured,
          std::chrono::duration_cast<Ns>(std::chrono::microseconds(250)),
          scanline_max_render_lead(state.refresh_ns));
      state.render_lead = Ns((state.render_lead.count() * 9 + measured.count()) / 10);
    }

    target = state.next_present_target;
    if (target == Clock::time_point() || target <= now)
      target = scanline_compute_target_locked(state, now);
  }

  // Most waiting should happen at acquire/frame start. This final wait is only
  // a correction; cap it to avoid a whole-refresh stall if the frame missed the
  // target badly.
  if (target > now && target - now <= g_scanline_max_present_wait)
    DxvkStyleSleep::sleep_until(now, target);
}

} // namespace

extern "C" {

VK_LAYER_EXPORT VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* pVersionStruct) {
  if (!pVersionStruct)
    return VK_ERROR_INITIALIZATION_FAILED;

  pVersionStruct->loaderLayerInterfaceVersion = std::min<uint32_t>(
      pVersionStruct->loaderLayerInterfaceVersion, CURRENT_LOADER_LAYER_INTERFACE_VERSION);
  pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
  pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
  return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
  auto* chain = get_layer_link_info<VkLayerInstanceCreateInfo*>(
      pCreateInfo->pNext, VK_LAYER_LINK_INFO, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
  if (!chain || !chain->u.pLayerInfo)
    return VK_ERROR_INITIALIZATION_FAILED;

  PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

  auto next_create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(nullptr, "vkCreateInstance"));
  if (!next_create)
    return VK_ERROR_INITIALIZATION_FAILED;

  VkInstanceCreateInfo create_info = *pCreateInfo;
  std::vector<const char*> enabled_instance_exts;
  bool want_surface_caps2 = g_want_present_timing && g_scanline.active
      && !name_in_list(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
          pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames);

  if (want_surface_caps2) {
    enabled_instance_exts.reserve(pCreateInfo->enabledExtensionCount + 1);
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++)
      enabled_instance_exts.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
    enabled_instance_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_instance_exts.size());
    create_info.ppEnabledExtensionNames = enabled_instance_exts.data();
  }

  VkResult r = next_create(&create_info, pAllocator, pInstance);
  if (r != VK_SUCCESS && want_surface_caps2) {
    // The ICD may not support VK_KHR_get_surface_capabilities2; fall back.
    log("scanline: %s not supported (0x%x); continuing without present-timing surface caps",
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, r);
    create_info = *pCreateInfo;
    r = next_create(&create_info, pAllocator, pInstance);
  }
  if (r != VK_SUCCESS)
    return r;

  InstanceDispatch inst{};
  inst.instance = *pInstance;
  inst.GetInstanceProcAddr = next_gipa;
  inst.GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(next_gipa(*pInstance, "vkGetDeviceProcAddr"));
  inst.DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*pInstance, "vkDestroyInstance"));
  inst.CreateDevice = reinterpret_cast<PFN_vkCreateDevice>(next_gipa(*pInstance, "vkCreateDevice"));
  inst.EnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(next_gipa(*pInstance, "vkEnumeratePhysicalDevices"));
  inst.GetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(next_gipa(*pInstance, "vkGetPhysicalDeviceFeatures2"));
  inst.EnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(next_gipa(*pInstance, "vkEnumerateDeviceExtensionProperties"));
  inst.GetPhysicalDeviceSurfacePresentModesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(next_gipa(*pInstance, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
  inst.GetPhysicalDeviceSurfaceCapabilities2KHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(next_gipa(*pInstance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
  inst.CreateWaylandSurfaceKHR = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(next_gipa(*pInstance, "vkCreateWaylandSurfaceKHR"));
  inst.DestroySurfaceKHR = reinterpret_cast<PFN_vkDestroySurfaceKHR>(next_gipa(*pInstance, "vkDestroySurfaceKHR"));

  std::lock_guard<std::mutex> lock(g_mutex);
  g_instances[*pInstance] = inst;
  log("created instance");
  return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator) {
  InstanceDispatch inst{};
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instances.find(instance);
    if (it != g_instances.end()) {
      inst = it->second;
      for (auto p = g_phys_instances.begin(); p != g_phys_instances.end();) {
        if (p->second == &it->second)
          p = g_phys_instances.erase(p);
        else
          ++p;
      }
      g_instances.erase(it);
    }
  }

  if (inst.DestroyInstance)
    inst.DestroyInstance(instance, pAllocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance instance,
    uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices) {
  InstanceDispatch* inst = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instances.find(instance);
    if (it != g_instances.end())
      inst = &it->second;
  }

  if (!inst || !inst->EnumeratePhysicalDevices)
    return VK_ERROR_INITIALIZATION_FAILED;

  VkResult r = inst->EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
  if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && pPhysicalDevices && pPhysicalDeviceCount) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instances.find(instance);
    if (it != g_instances.end()) {
      for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++)
        g_phys_instances[pPhysicalDevices[i]] = &it->second;
    }
  }

  return r;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {
  auto* chain = get_layer_link_info<VkLayerDeviceCreateInfo*>(
      pCreateInfo->pNext, VK_LAYER_LINK_INFO, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
  if (!chain || !chain->u.pLayerInfo)
    return VK_ERROR_INITIALIZATION_FAILED;

  PFN_vkGetDeviceProcAddr next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

  InstanceDispatch* inst = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_phys_instances.find(physicalDevice);
    if (it != g_phys_instances.end())
      inst = it->second;
  }

  if (!inst || !inst->CreateDevice)
    return VK_ERROR_INITIALIZATION_FAILED;

  uint32_t ext_count = 0;
  std::vector<VkExtensionProperties> exts;
  if (inst->EnumerateDeviceExtensionProperties &&
      inst->EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ext_count, nullptr) == VK_SUCCESS) {
    exts.resize(ext_count);
    inst->EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &ext_count, exts.data());
  }

  const bool swapchain_ext = name_in_list(VK_KHR_SWAPCHAIN_EXTENSION_NAME,
      pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames);
  const bool app_present_id_ext = name_in_list(VK_KHR_PRESENT_ID_EXTENSION_NAME,
      pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames);
  const bool app_present_wait_ext = name_in_list(VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
      pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames);
  const bool app_present_id2_ext = name_in_list(VK_KHR_PRESENT_ID_2_EXTENSION_NAME,
      pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames);
  const bool app_calibrated_timestamps_ext = name_in_list(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
      pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames);
  const bool app_present_timing_ext = name_in_list(VK_EXT_PRESENT_TIMING_EXTENSION_NAME,
      pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames);

  bool support_present_id = false;
  bool support_present_wait = false;
  bool support_present_id2 = false;
  bool support_present_timing = false;

  if (g_cap_active && g_want_present_wait && swapchain_ext && inst->GetPhysicalDeviceFeatures2) {
    VkPhysicalDevicePresentWaitFeaturesKHR present_wait_features{};
    present_wait_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;

    VkPhysicalDevicePresentIdFeaturesKHR present_id_features{};
    present_id_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    present_id_features.pNext = &present_wait_features;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &present_id_features;

    inst->GetPhysicalDeviceFeatures2(physicalDevice, &features2);

    support_present_id = has_extension(exts, VK_KHR_PRESENT_ID_EXTENSION_NAME) && present_id_features.presentId;
    support_present_wait = support_present_id
        && has_extension(exts, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)
        && present_wait_features.presentWait;
  }

  if (g_want_present_timing && g_scanline.active && swapchain_ext && inst->GetPhysicalDeviceFeatures2) {
    VkPhysicalDevicePresentTimingFeaturesEXT present_timing_features{};
    present_timing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;

    VkPhysicalDevicePresentId2FeaturesKHR present_id2_features{};
    present_id2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
    present_id2_features.pNext = &present_timing_features;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &present_id2_features;

    inst->GetPhysicalDeviceFeatures2(physicalDevice, &features2);

    support_present_id2 = has_extension(exts, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) && present_id2_features.presentId2;
    support_present_timing = support_present_id2
        && has_extension(exts, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)
        && has_extension(exts, VK_EXT_PRESENT_TIMING_EXTENSION_NAME)
        && present_timing_features.presentTiming;
  }

  const auto* app_present_id_features = find_pnext<VkPhysicalDevicePresentIdFeaturesKHR>(
      pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
  const auto* app_present_wait_features = find_pnext<VkPhysicalDevicePresentWaitFeaturesKHR>(
      pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);
  const auto* app_present_id2_features = find_pnext<VkPhysicalDevicePresentId2FeaturesKHR>(
      pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR);
  const auto* app_present_timing_features = find_pnext<VkPhysicalDevicePresentTimingFeaturesEXT>(
      pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT);

  // If the app explicitly provided presentId = VK_FALSE, do not force the
  // present-wait path since wait requires usable present IDs.
  if (support_present_id && app_present_id_features && !app_present_id_features->presentId) {
    support_present_id = false;
    support_present_wait = false;
  }

  if (support_present_wait && app_present_wait_features && !app_present_wait_features->presentWait)
    support_present_wait = false;

  if (support_present_id2 && app_present_id2_features && !app_present_id2_features->presentId2) {
    support_present_id2 = false;
    support_present_timing = false;
  }

  if (support_present_timing && app_present_timing_features && !app_present_timing_features->presentTiming)
    support_present_timing = false;

  std::vector<const char*> enabled_exts;
  enabled_exts.reserve(pCreateInfo->enabledExtensionCount + 5);
  for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++)
    enabled_exts.push_back(pCreateInfo->ppEnabledExtensionNames[i]);

  if (support_present_id && !app_present_id_ext)
    enabled_exts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
  if (support_present_wait && !app_present_wait_ext)
    enabled_exts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
  if (support_present_id2 && !app_present_id2_ext)
    enabled_exts.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
  if (support_present_timing && !app_calibrated_timestamps_ext)
    enabled_exts.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
  if (support_present_timing && !app_present_timing_ext)
    enabled_exts.push_back(VK_EXT_PRESENT_TIMING_EXTENSION_NAME);

  VkPhysicalDevicePresentWaitFeaturesKHR enable_present_wait{};
  enable_present_wait.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
  enable_present_wait.presentWait = VK_TRUE;

  VkPhysicalDevicePresentIdFeaturesKHR enable_present_id{};
  enable_present_id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
  enable_present_id.presentId = VK_TRUE;

  VkPhysicalDevicePresentId2FeaturesKHR enable_present_id2{};
  enable_present_id2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
  enable_present_id2.presentId2 = VK_TRUE;

  VkPhysicalDevicePresentTimingFeaturesEXT enable_present_timing{};
  enable_present_timing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;
  enable_present_timing.presentTiming = VK_TRUE;

  VkDeviceCreateInfo create_info = *pCreateInfo;
  create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_exts.size());
  create_info.ppEnabledExtensionNames = enabled_exts.data();

  bool layer_enabled_present_id_feature = false;
  bool layer_enabled_present_wait_feature = false;
  bool layer_enabled_present_id2_feature = false;
  bool layer_enabled_present_timing_feature = false;

  if (support_present_timing && !app_present_timing_features) {
    enable_present_timing.pNext = const_cast<void*>(create_info.pNext);
    create_info.pNext = &enable_present_timing;
    layer_enabled_present_timing_feature = true;
  }

  if (support_present_id2 && !app_present_id2_features) {
    enable_present_id2.pNext = const_cast<void*>(create_info.pNext);
    create_info.pNext = &enable_present_id2;
    layer_enabled_present_id2_feature = true;
  }

  if (support_present_wait && !app_present_wait_features) {
    enable_present_wait.pNext = const_cast<void*>(create_info.pNext);
    create_info.pNext = &enable_present_wait;
    layer_enabled_present_wait_feature = true;
  }

  if (support_present_id && !app_present_id_features) {
    enable_present_id.pNext = const_cast<void*>(create_info.pNext);
    create_info.pNext = &enable_present_id;
    layer_enabled_present_id_feature = true;
  }

  VkResult r = inst->CreateDevice(physicalDevice, &create_info, pAllocator, pDevice);
  if (r != VK_SUCCESS)
    return r;

  auto dev = std::make_unique<DeviceDispatch>();
  dev->device = *pDevice;
  dev->physical_device = physicalDevice;
  dev->instance = inst;
  dev->GetDeviceProcAddr = next_gdpa;
  dev->DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(next_gdpa(*pDevice, "vkDestroyDevice"));
  dev->GetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(next_gdpa(*pDevice, "vkGetDeviceQueue"));
  dev->GetDeviceQueue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(next_gdpa(*pDevice, "vkGetDeviceQueue2"));
  dev->QueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(next_gdpa(*pDevice, "vkQueuePresentKHR"));
  dev->WaitForPresentKHR = reinterpret_cast<PFN_vkWaitForPresentKHR>(next_gdpa(*pDevice, "vkWaitForPresentKHR"));
  dev->AcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(next_gdpa(*pDevice, "vkAcquireNextImageKHR"));
  dev->AcquireNextImage2KHR = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(next_gdpa(*pDevice, "vkAcquireNextImage2KHR"));
  dev->CreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(next_gdpa(*pDevice, "vkCreateSwapchainKHR"));
  dev->DestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(next_gdpa(*pDevice, "vkDestroySwapchainKHR"));
  dev->SetSwapchainPresentTimingQueueSizeEXT = reinterpret_cast<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(next_gdpa(*pDevice, "vkSetSwapchainPresentTimingQueueSizeEXT"));
  dev->GetSwapchainTimingPropertiesEXT = reinterpret_cast<PFN_vkGetSwapchainTimingPropertiesEXT>(next_gdpa(*pDevice, "vkGetSwapchainTimingPropertiesEXT"));
  dev->GetPastPresentationTimingEXT = reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(next_gdpa(*pDevice, "vkGetPastPresentationTimingEXT"));

  const bool id_feature_enabled = layer_enabled_present_id_feature
      || (app_present_id_features && app_present_id_features->presentId);
  const bool wait_feature_enabled = layer_enabled_present_wait_feature
      || (app_present_wait_features && app_present_wait_features->presentWait);
  const bool id2_feature_enabled = layer_enabled_present_id2_feature
      || (app_present_id2_features && app_present_id2_features->presentId2);
  const bool timing_feature_enabled = layer_enabled_present_timing_feature
      || (app_present_timing_features && app_present_timing_features->presentTiming);

  dev->present_id_enabled = name_in_list(VK_KHR_PRESENT_ID_EXTENSION_NAME,
      create_info.enabledExtensionCount, create_info.ppEnabledExtensionNames) && id_feature_enabled;
  dev->present_wait_enabled = name_in_list(VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
      create_info.enabledExtensionCount, create_info.ppEnabledExtensionNames)
      && wait_feature_enabled && dev->WaitForPresentKHR;
  dev->present_timing_enabled = name_in_list(VK_EXT_PRESENT_TIMING_EXTENSION_NAME,
      create_info.enabledExtensionCount, create_info.ppEnabledExtensionNames)
      && name_in_list(VK_KHR_PRESENT_ID_2_EXTENSION_NAME,
          create_info.enabledExtensionCount, create_info.ppEnabledExtensionNames)
      && id2_feature_enabled && timing_feature_enabled && dev->GetSwapchainTimingPropertiesEXT && dev->GetPastPresentationTimingEXT;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto raw = dev.get();
    g_devices.emplace(*pDevice, std::move(dev));
    log("created device: present_id=%d present_wait=%d present_timing=%d scanline=%d",
        raw->present_id_enabled, raw->present_wait_enabled,
        raw->present_timing_enabled, g_scanline.active);
  }

  return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {
  PFN_vkDestroyDevice destroy = nullptr;
  DeviceDispatch* raw = nullptr;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device);
    if (it != g_devices.end()) {
      raw = it->second.get();
      destroy = raw->DestroyDevice;

      for (auto q = g_queues.begin(); q != g_queues.end();) {
        if (q->second == raw)
          q = g_queues.erase(q);
        else
          ++q;
      }

      g_devices.erase(it);
    }
  }

  if (destroy)
    destroy(device, pAllocator);
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue) {
  DeviceDispatch* dev = get_device_dispatch(device);
  if (!dev || !dev->GetDeviceQueue)
    return;

  dev->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);

  if (pQueue && *pQueue) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queues[*pQueue] = dev;
  }
}

VK_LAYER_EXPORT void VKAPI_CALL vkGetDeviceQueue2(
    VkDevice device,
    const VkDeviceQueueInfo2* pQueueInfo,
    VkQueue* pQueue) {
  DeviceDispatch* dev = get_device_dispatch(device);
  if (!dev || !dev->GetDeviceQueue2)
    return;

  dev->GetDeviceQueue2(device, pQueueInfo, pQueue);

  if (pQueue && *pQueue) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queues[*pQueue] = dev;
  }
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateWaylandSurfaceKHR(
    VkInstance instance,
    const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface) {
  InstanceDispatch* inst = get_instance_dispatch(instance);
  if (!inst || !inst->CreateWaylandSurfaceKHR)
    return VK_ERROR_INITIALIZATION_FAILED;

  VkResult r = inst->CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
  if (g_cap_active && r == VK_SUCCESS && pCreateInfo && pSurface && *pSurface)
    vfc::key_input::init_wayland_display(pCreateInfo->display, reinterpret_cast<void*>(*pSurface));

  return r;
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroySurfaceKHR(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator) {
  InstanceDispatch* inst = get_instance_dispatch(instance);

  vfc::key_input::unref_wayland_surface(reinterpret_cast<void*>(surface));

  if (inst && inst->DestroySurfaceKHR)
    inst->DestroySurfaceKHR(instance, surface, pAllocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex) {
  DeviceDispatch* dev = get_device_dispatch(device);
  if (!dev || !dev->AcquireNextImageKHR)
    return VK_ERROR_INITIALIZATION_FAILED;

  scanline_acquire_delay(dev, swapchain);
  VkResult r = dev->AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
  scanline_mark_acquire_done(dev, swapchain, r);
  return r;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkAcquireNextImage2KHR(
    VkDevice device,
    const VkAcquireNextImageInfoKHR* pAcquireInfo,
    uint32_t* pImageIndex) {
  DeviceDispatch* dev = get_device_dispatch(device);
  if (!dev || !dev->AcquireNextImage2KHR)
    return VK_ERROR_INITIALIZATION_FAILED;

  if (pAcquireInfo)
    scanline_acquire_delay(dev, pAcquireInfo->swapchain);
  VkResult r = dev->AcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
  if (pAcquireInfo)
    scanline_mark_acquire_done(dev, pAcquireInfo->swapchain, r);
  return r;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain) {
  DeviceDispatch* dev = get_device_dispatch(device);
  if (!dev || !dev->CreateSwapchainKHR)
    return VK_ERROR_INITIALIZATION_FAILED;

  VkSwapchainCreateInfoKHR create_info = *pCreateInfo;
  SurfaceTimingCaps timing_caps{};

  if (g_scanline.active) {
    if (surface_supports_present_mode(dev, pCreateInfo->surface, VK_PRESENT_MODE_IMMEDIATE_KHR)) {
      if (create_info.presentMode != VK_PRESENT_MODE_IMMEDIATE_KHR)
        log("scanline: forcing IMMEDIATE present mode (was %d)", create_info.presentMode);
      create_info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else {
      log("scanline: IMMEDIATE present mode is not available; preserving mode %d", create_info.presentMode);
    }

    if (dev->present_timing_enabled) {
      timing_caps = query_surface_timing_caps(dev, pCreateInfo->surface);
      // If surface-capabilities2 was not enabled by the app, the caps query may
      // be unavailable even though the device/runtime supports present timing.
      // Try the swapchain flag anyway and fall back if the WSI rejects it.
      create_info.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
    }
  }

  VkResult r = dev->CreateSwapchainKHR(device, &create_info, pAllocator, pSwapchain);
  if (r != VK_SUCCESS && (create_info.flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT)) {
    log("scanline: present timing swapchain flag failed (%d), retrying without it", r);
    create_info.flags &= ~VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
    timing_caps = {};
    r = dev->CreateSwapchainKHR(device, &create_info, pAllocator, pSwapchain);
  }
  if (r == VK_SUCCESS && pCreateInfo && pSwapchain && *pSwapchain) {
    SwapchainState state{};
    state.present_mode = create_info.presentMode;
    state.extent = create_info.imageExtent;
    state.refresh_ns = g_scanline_fallback_refresh_ns;
    state.present_timing_requested = !!(create_info.flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT);
    state.first_pixel_out_supported = timing_caps.first_pixel_out;
    if (g_scanline.active && state.present_timing_requested) {
      if (dev->SetSwapchainPresentTimingQueueSizeEXT) {
        VkResult qr = dev->SetSwapchainPresentTimingQueueSizeEXT(device, *pSwapchain, 8);
        if (qr != VK_SUCCESS)
          log("scanline: vkSetSwapchainPresentTimingQueueSizeEXT returned %d", qr);
      }
      bool measured = false;
      state.refresh_ns = query_swapchain_refresh_ns(dev, *pSwapchain, &measured);
      state.refresh_measured = measured;
    }
    state.render_lead = scanline_default_render_lead(state.refresh_ns);

    std::lock_guard<std::mutex> lock(dev->present_mutex);
    dev->present_modes[*pSwapchain] = create_info.presentMode;
    dev->swapchains[*pSwapchain] = state;
    log("created swapchain: present mode %d extent=%ux%u refresh=%llu ns timing=%d",
        create_info.presentMode, create_info.imageExtent.width, create_info.imageExtent.height,
        static_cast<unsigned long long>(state.refresh_ns), state.present_timing_requested);
  }

  return r;
}

VK_LAYER_EXPORT void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator) {
  DeviceDispatch* dev = get_device_dispatch(device);
  if (!dev || !dev->DestroySwapchainKHR)
    return;

  {
    std::lock_guard<std::mutex> lock(dev->present_mutex);
    dev->present_ids.erase(swapchain);
    dev->present_modes.erase(swapchain);
    dev->swapchains.erase(swapchain);
  }

  dev->DestroySwapchainKHR(device, swapchain, pAllocator);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo) {
  DeviceDispatch* dev = get_queue_dispatch(queue);
  if (!dev || !dev->QueuePresentKHR)
    return VK_ERROR_INITIALIZATION_FAILED;

  if (g_cap_active)
    check_hotkeys();
  bool limiter_enabled = g_limiter.enabled();

  VkPresentInfoKHR info = *pPresentInfo;
  VkPresentIdKHR present_id{};
  present_id.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;

  std::vector<uint64_t> ids;
  std::vector<uint8_t> wait_swapchain(pPresentInfo->swapchainCount, 0);

  const auto* app_present_id = find_pnext<VkPresentIdKHR>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR);
  const auto* present_mode_info = find_pnext<VkSwapchainPresentModeInfoEXT>(
      pPresentInfo->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT);

  if (g_scanline.active) {
    // Conservative scanline mode: do not inject VkPresentTimingsInfoEXT yet.
    // The previous diagnostic path (timing injection + feedback polling) could
    // freeze some games at launch. Keep scanline pacing stable for now;
    // present-timing feedback PLL can be re-enabled later behind a safer
    // implementation.
    scanline_present_delay(dev, pPresentInfo, present_mode_info);
    return dev->QueuePresentKHR(queue, &info);
  }

  bool need_present_wait = limiter_enabled && g_want_present_wait && dev->present_wait_enabled;
  if (need_present_wait) {
    bool any_wait = false;

    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      VkPresentModeKHR mode = get_present_mode(dev, pPresentInfo->pSwapchains[i], present_mode_info, i);
      wait_swapchain[i] = should_wait_for_present_mode(mode) ? 1 : 0;
      any_wait |= wait_swapchain[i] != 0;
    }

    need_present_wait = any_wait;
  }

  if (need_present_wait && dev->present_id_enabled && pPresentInfo->swapchainCount && !app_present_id) {
    ids.resize(pPresentInfo->swapchainCount);

    {
      std::lock_guard<std::mutex> lock(dev->present_mutex);
      for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++)
        ids[i] = ++dev->present_ids[pPresentInfo->pSwapchains[i]];
    }

    present_id.swapchainCount = pPresentInfo->swapchainCount;
    present_id.pPresentIds = ids.data();
    present_id.pNext = info.pNext;
    info.pNext = &present_id;
  } else if (need_present_wait && app_present_id && app_present_id->swapchainCount == pPresentInfo->swapchainCount && app_present_id->pPresentIds) {
    ids.assign(app_present_id->pPresentIds, app_present_id->pPresentIds + app_present_id->swapchainCount);
  }

  VkResult r = dev->QueuePresentKHR(queue, &info);

  if (limiter_enabled && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {
    if (need_present_wait && !ids.empty()) {
      for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        if (!wait_swapchain[i])
          continue;

        VkResult wr = dev->WaitForPresentKHR(dev->device, pPresentInfo->pSwapchains[i], ids[i], UINT64_MAX);
        if (wr != VK_SUCCESS) {
          log("vkWaitForPresentKHR returned %d", wr);
          break;
        }
      }
    }

    g_limiter.delay();
  }

  return r;
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device,
    const char* pName) {
  if (!pName)
    return nullptr;

  if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
  if (std::strcmp(pName, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice);
  if (std::strcmp(pName, "vkGetDeviceQueue") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue);
  if (std::strcmp(pName, "vkGetDeviceQueue2") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue2);
  if (std::strcmp(pName, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR);
  if (std::strcmp(pName, "vkAcquireNextImageKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImageKHR);
  if (std::strcmp(pName, "vkAcquireNextImage2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImage2KHR);
  if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR);
  if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroySwapchainKHR);

  DeviceDispatch* dev = get_device_dispatch(device);
  if (dev && dev->GetDeviceProcAddr)
    return dev->GetDeviceProcAddr(device, pName);

  return nullptr;
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance,
    const char* pName) {
  if (!pName)
    return nullptr;

  if (std::strcmp(pName, "vkNegotiateLoaderLayerInterfaceVersion") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkNegotiateLoaderLayerInterfaceVersion);
  if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
  if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
  if (std::strcmp(pName, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance);
  if (std::strcmp(pName, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance);
  if (std::strcmp(pName, "vkEnumeratePhysicalDevices") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkEnumeratePhysicalDevices);
  if (std::strcmp(pName, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice);
  if (std::strcmp(pName, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice);
  if (std::strcmp(pName, "vkGetDeviceQueue") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue);
  if (std::strcmp(pName, "vkGetDeviceQueue2") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue2);
  if (std::strcmp(pName, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR);
  if (std::strcmp(pName, "vkAcquireNextImageKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImageKHR);
  if (std::strcmp(pName, "vkAcquireNextImage2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImage2KHR);
  if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR);
  if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroySwapchainKHR);
  if (std::strcmp(pName, "vkCreateWaylandSurfaceKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateWaylandSurfaceKHR);
  if (std::strcmp(pName, "vkDestroySurfaceKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroySurfaceKHR);

  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_instances.find(instance);
  if (it != g_instances.end() && it->second.GetInstanceProcAddr)
    return it->second.GetInstanceProcAddr(instance, pName);

  return nullptr;
}

} // extern C

#include "key_input.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sys/mman.h>
#include <unistd.h>

namespace vfc::key_input {
namespace {

bool env_bool(const char* name, bool fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v)
    return fallback;

  return std::strcmp(v, "0") != 0
      && strcasecmp(v, "false") != 0
      && strcasecmp(v, "no") != 0
      && strcasecmp(v, "off") != 0;
}

bool g_log = env_bool("VFC_LOG", false);

void log(const char* msg) {
  if (g_log)
    std::fprintf(stderr, "[VulkanFrameCapper] %s\n", msg);
}

// ----------------------------- X11 -----------------------------

Display* g_x11_display = nullptr;
bool g_x11_tried = false;

bool init_x11() {
  if (g_x11_tried)
    return g_x11_display != nullptr;

  g_x11_tried = true;
  g_x11_display = XOpenDisplay(nullptr);
  if (g_x11_display)
    log("X11 key input initialized");

  return g_x11_display != nullptr;
}

bool x11_keysym_pressed(const char keys[32], KeySym sym) {
  if (!g_x11_display)
    return false;

  KeyCode code = XKeysymToKeycode(g_x11_display, sym);
  if (!code)
    return false;

  return !!(keys[code >> 3] & (1 << (code & 7)));
}

bool x11_shift_pressed(const char keys[32]) {
  return x11_keysym_pressed(keys, XK_Shift_L) || x11_keysym_pressed(keys, XK_Shift_R);
}

bool x11_ctrl_pressed(const char keys[32]) {
  return x11_keysym_pressed(keys, XK_Control_L) || x11_keysym_pressed(keys, XK_Control_R);
}

bool x11_plus_pressed(const char keys[32]) {
  return x11_keysym_pressed(keys, XK_plus)
      || x11_keysym_pressed(keys, XK_equal)
      || x11_keysym_pressed(keys, XK_KP_Add);
}

bool x11_minus_pressed(const char keys[32]) {
  return x11_keysym_pressed(keys, XK_minus)
      || x11_keysym_pressed(keys, XK_underscore)
      || x11_keysym_pressed(keys, XK_KP_Subtract);
}

bool x11_increase_pressed() {
  if (!init_x11())
    return false;

  char keys[32] = {};
  XQueryKeymap(g_x11_display, keys);
  return x11_shift_pressed(keys) && x11_plus_pressed(keys);
}

bool x11_decrease_pressed() {
  if (!init_x11())
    return false;

  char keys[32] = {};
  XQueryKeymap(g_x11_display, keys);
  return x11_shift_pressed(keys) && x11_minus_pressed(keys);
}

bool x11_increase_fast_pressed() {
  if (!init_x11())
    return false;

  char keys[32] = {};
  XQueryKeymap(g_x11_display, keys);
  return x11_ctrl_pressed(keys) && x11_plus_pressed(keys);
}

bool x11_decrease_fast_pressed() {
  if (!init_x11())
    return false;

  char keys[32] = {};
  XQueryKeymap(g_x11_display, keys);
  return x11_ctrl_pressed(keys) && x11_minus_pressed(keys);
}

bool x11_cycle_limit_pressed() {
  if (!init_x11())
    return false;

  char keys[32] = {};
  XQueryKeymap(g_x11_display, keys);
  return x11_shift_pressed(keys) && x11_keysym_pressed(keys, XK_F9);
}

// --------------------------- Wayland ---------------------------

struct WaylandDisplay {
  int ref = 1;
  wl_event_queue* queue = nullptr;
  wl_seat* seat = nullptr;
  wl_keyboard* keyboard = nullptr;
  xkb_keymap* keymap = nullptr;
  xkb_state* state = nullptr;
  std::set<void*> surfaces;
  std::set<xkb_keycode_t> pressed_keycodes;

  ~WaylandDisplay() {
    pressed_keycodes.clear();
    surfaces.clear();

    if (keyboard)
      wl_keyboard_destroy(keyboard);
    if (seat)
      wl_seat_destroy(seat);
    if (queue)
      wl_event_queue_destroy(queue);
    if (state)
      xkb_state_unref(state);
    if (keymap)
      xkb_keymap_unref(keymap);
  }
};

std::mutex g_wayland_mutex;
std::map<wl_display*, WaylandDisplay> g_wayland_displays;
xkb_context* g_xkb_context = nullptr;

#ifdef wl_array_for_each
#undef wl_array_for_each
#define wl_array_for_each(pos, array)                                      \
  for (pos = (decltype(pos))((array)->data);                               \
       (array)->size != 0 &&                                               \
       (const char*)pos < ((const char*)(array)->data + (array)->size);     \
       (pos)++)
#endif

void keyboard_keymap(void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
  auto* wd = static_cast<WaylandDisplay*>(data);
  if (!wd || format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    if (fd >= 0)
      close(fd);
    return;
  }

  char* map_shm = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
  if (map_shm == MAP_FAILED) {
    close(fd);
    return;
  }

  if (!g_xkb_context)
    g_xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

  if (wd->state) {
    xkb_state_unref(wd->state);
    wd->state = nullptr;
  }
  if (wd->keymap) {
    xkb_keymap_unref(wd->keymap);
    wd->keymap = nullptr;
  }

  wd->keymap = xkb_keymap_new_from_string(
      g_xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (wd->keymap)
    wd->state = xkb_state_new(wd->keymap);

  munmap(map_shm, size);
  close(fd);
}

void keyboard_enter(void* data, wl_keyboard*, uint32_t, wl_surface*, wl_array* keys) {
  auto* wd = static_cast<WaylandDisplay*>(data);
  if (!wd)
    return;

  wd->pressed_keycodes.clear();

  uint32_t* key = nullptr;
  wl_array_for_each(key, keys)
    wd->pressed_keycodes.insert(*key + 8);
}

void keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
  auto* wd = static_cast<WaylandDisplay*>(data);
  if (wd)
    wd->pressed_keycodes.clear();
}

void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
  auto* wd = static_cast<WaylandDisplay*>(data);
  if (!wd)
    return;

  xkb_keycode_t keycode = key + 8;
  if (state)
    wd->pressed_keycodes.insert(keycode);
  else
    wd->pressed_keycodes.erase(keycode);
}

void keyboard_modifiers(void* data, wl_keyboard*, uint32_t,
                        uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
  auto* wd = static_cast<WaylandDisplay*>(data);
  if (wd && wd->state)
    xkb_state_update_mask(wd->state, depressed, latched, locked, 0, 0, group);
}

void keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}

const wl_keyboard_listener keyboard_listener = {
  keyboard_keymap,
  keyboard_enter,
  keyboard_leave,
  keyboard_key,
  keyboard_modifiers,
  keyboard_repeat_info,
};

void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
  auto* wd = static_cast<WaylandDisplay*>(data);
  if (!wd)
    return;

  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wd->keyboard) {
    wd->keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(wd->keyboard, &keyboard_listener, wd);
  }
}

void seat_name(void*, wl_seat*, const char*) {}

const wl_seat_listener seat_listener = {
  seat_capabilities,
  seat_name,
};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
  auto* wd = static_cast<WaylandDisplay*>(data);
  if (!wd)
    return;

  if (std::strcmp(interface, wl_seat_interface.name) == 0 && !wd->seat) {
    uint32_t bind_version = std::min<uint32_t>(version, 5);
    wd->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, bind_version));
    wl_seat_add_listener(wd->seat, &seat_listener, wd);
  }
}

void registry_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener registry_listener = {
  registry_global,
  registry_remove,
};

bool wayland_keysym_pressed(const WaylandDisplay& wd, xkb_keysym_t sym) {
  if (!wd.state)
    return false;

  for (xkb_keycode_t keycode : wd.pressed_keycodes) {
    if (xkb_state_key_get_one_sym(wd.state, keycode) == sym)
      return true;
  }

  return false;
}

bool wayland_shift_pressed(const WaylandDisplay& wd) {
  return wayland_keysym_pressed(wd, XKB_KEY_Shift_L)
      || wayland_keysym_pressed(wd, XKB_KEY_Shift_R);
}

bool wayland_ctrl_pressed(const WaylandDisplay& wd) {
  return wayland_keysym_pressed(wd, XKB_KEY_Control_L)
      || wayland_keysym_pressed(wd, XKB_KEY_Control_R);
}

bool wayland_plus_pressed(const WaylandDisplay& wd) {
  return wayland_keysym_pressed(wd, XKB_KEY_plus)
      || wayland_keysym_pressed(wd, XKB_KEY_equal)
      || wayland_keysym_pressed(wd, XKB_KEY_KP_Add);
}

bool wayland_minus_pressed(const WaylandDisplay& wd) {
  return wayland_keysym_pressed(wd, XKB_KEY_minus)
      || wayland_keysym_pressed(wd, XKB_KEY_underscore)
      || wayland_keysym_pressed(wd, XKB_KEY_KP_Subtract);
}

void update_wayland_queues_locked() {
  for (auto& [display, wd] : g_wayland_displays) {
    if (wd.queue)
      wl_display_dispatch_queue_pending(display, wd.queue);
  }
}

bool wayland_increase_pressed() {
  std::lock_guard<std::mutex> lock(g_wayland_mutex);
  update_wayland_queues_locked();

  for (const auto& [_, wd] : g_wayland_displays) {
    if (wayland_shift_pressed(wd) && wayland_plus_pressed(wd))
      return true;
  }

  return false;
}

bool wayland_decrease_pressed() {
  std::lock_guard<std::mutex> lock(g_wayland_mutex);
  update_wayland_queues_locked();

  for (const auto& [_, wd] : g_wayland_displays) {
    if (wayland_shift_pressed(wd) && wayland_minus_pressed(wd))
      return true;
  }

  return false;
}

bool wayland_increase_fast_pressed() {
  std::lock_guard<std::mutex> lock(g_wayland_mutex);
  update_wayland_queues_locked();

  for (const auto& [_, wd] : g_wayland_displays) {
    if (wayland_ctrl_pressed(wd) && wayland_plus_pressed(wd))
      return true;
  }

  return false;
}

bool wayland_decrease_fast_pressed() {
  std::lock_guard<std::mutex> lock(g_wayland_mutex);
  update_wayland_queues_locked();

  for (const auto& [_, wd] : g_wayland_displays) {
    if (wayland_ctrl_pressed(wd) && wayland_minus_pressed(wd))
      return true;
  }

  return false;
}

bool wayland_cycle_limit_pressed() {
  std::lock_guard<std::mutex> lock(g_wayland_mutex);
  update_wayland_queues_locked();

  for (const auto& [_, wd] : g_wayland_displays) {
    if (wayland_shift_pressed(wd) && wayland_keysym_pressed(wd, XKB_KEY_F9))
      return true;
  }

  return false;
}

bool wayland_has_displays() {
  std::lock_guard<std::mutex> lock(g_wayland_mutex);
  return !g_wayland_displays.empty();
}

} // namespace

void init_wayland_display(wl_display* display, void* surface) {
  if (!display)
    return;

  std::lock_guard<std::mutex> lock(g_wayland_mutex);

  auto it = g_wayland_displays.find(display);
  if (it != g_wayland_displays.end()) {
    it->second.ref++;
    if (surface)
      it->second.surfaces.insert(surface);
    return;
  }

  WaylandDisplay& wd = g_wayland_displays[display];
  wd.ref = 1;
  if (surface)
    wd.surfaces.insert(surface);

  wd.queue = wl_display_create_queue(display);
  if (!wd.queue)
    return;

  wl_display* wrapped = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapped), wd.queue);

  wl_registry* registry = wl_display_get_registry(wrapped);
  wl_proxy_wrapper_destroy(wrapped);

  wl_registry_add_listener(registry, &registry_listener, &wd);
  wl_display_roundtrip_queue(display, wd.queue);
  wl_display_roundtrip_queue(display, wd.queue);
  wl_registry_destroy(registry);

  log("Wayland key input initialized");
}

void unref_wayland_surface(void* surface) {
  if (!surface)
    return;

  std::lock_guard<std::mutex> lock(g_wayland_mutex);

  for (auto it = g_wayland_displays.begin(); it != g_wayland_displays.end();) {
    WaylandDisplay& wd = it->second;

    if (wd.surfaces.erase(surface))
      wd.ref--;

    if (wd.ref <= 0)
      it = g_wayland_displays.erase(it);
    else
      ++it;
  }
}

bool increase_pressed() {
  return wayland_has_displays() ? wayland_increase_pressed() : x11_increase_pressed();
}

bool decrease_pressed() {
  return wayland_has_displays() ? wayland_decrease_pressed() : x11_decrease_pressed();
}

bool increase_fast_pressed() {
  return wayland_has_displays() ? wayland_increase_fast_pressed() : x11_increase_fast_pressed();
}

bool decrease_fast_pressed() {
  return wayland_has_displays() ? wayland_decrease_fast_pressed() : x11_decrease_fast_pressed();
}

bool cycle_limit_pressed() {
  return wayland_has_displays() ? wayland_cycle_limit_pressed() : x11_cycle_limit_pressed();
}

} // namespace vfc::key_input

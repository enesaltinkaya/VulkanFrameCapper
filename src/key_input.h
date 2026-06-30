#pragma once

struct wl_display;

namespace vfc::key_input {

void init_wayland_display(wl_display* display, void* surface);
void unref_wayland_surface(void* surface);

bool increase_pressed();
bool decrease_pressed();
bool increase_fast_pressed();
bool decrease_fast_pressed();

} // namespace vfc::key_input

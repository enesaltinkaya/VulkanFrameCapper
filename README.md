# VulkanFrameCapper

Experimental Vulkan layer that caps FPS at `vkQueuePresentKHR`.

First version features:

- late limiter: present first, then throttle the app thread
- DXVK-style absolute next-frame scheduling
- DXVK-style final busy-wait sleep for better timing accuracy
- optionally auto-enables and uses `VK_KHR_present_id` + `VK_KHR_present_wait` when supported

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

## Run from build tree

From the build tree it is still an explicit layer, so use:

```sh
cd /home/enes/Projects/mein/VulkanFrameCapper
FPS=60 \
VFC_PRESENT_WAIT=1 \
VK_LAYER_PATH=$PWD/build \
VK_INSTANCE_LAYERS=VK_LAYER_MEIN_frame_capper \
vkcube
```

## Install for current user as an implicit layer

This is the MangoHud-like mode. It lets you omit `VK_INSTANCE_LAYERS`.

```sh
./scripts/install-user.sh
```

Then run with only `FPS`:

```sh
FPS=60 vkcube
```

Steam/Proton launch option example:

```sh
FPS=60 VFC_PRESENT_WAIT=1 %command%
```

Disable the implicit layer for one launch:

```sh
DISABLE_VULKAN_FRAME_CAPPER=1 %command%
```

Uninstall:

```sh
./scripts/uninstall-user.sh
```

## Environment

- `FPS=60` target FPS. Fractional values like `59.94` work. `0` or unset disables capping. Default: disabled.
- `SCANLINE=500` enables experimental RTSS-like scanline path instead of the normal FPS limiter. Integer values only. Examples: `SCANLINE=-50`, `SCANLINE=500`.
- `VFC_PRESENT_WAIT=1` use `VK_KHR_present_wait` when available. Default: `1` when FPS cap is active.
- `VFC_PRESENT_TIMING=1` (scanline mode) opt into `VK_EXT_present_timing` feedback. **Experimental, may freeze some games at launch.** Default: `0`.
- `VFC_LOG=1` print debug logs to stderr. Default: `0`.
- `DISABLE_VULKAN_FRAME_CAPPER=1` disables the implicit layer.

## Hotkeys

Hotkeys are active only when the process was started with `FPS` above 0. They are not active in `SCANLINE` mode.

- `Shift + Plus`: increase FPS cap by `0.01`
- `Shift + Minus`: decrease FPS cap by `0.01`
- `Ctrl + Plus`: increase FPS cap by `0.10`
- `Ctrl + Minus`: decrease FPS cap by `0.10`

Holding the combo repeats after a short delay. X11 and Wayland are supported.

## Experimental scanline mode

If `SCANLINE` exists and is a valid integer, the layer uses the scanline path and ignores `FPS`.

```sh
SCANLINE=500 VFC_LOG=1 <app>
SCANLINE=-50 VFC_LOG=1 <app>
```

Current prototype behavior:

- forces `VK_PRESENT_MODE_IMMEDIATE_KHR` when available
- paces most of the wait at frame start (`vkAcquireNextImageKHR`), with a small final correction before present, for low input lag
- adaptively learns the render lead time to keep the tearline stable
- maps `SCANLINE` to a phase inside the frame using the swapchain height
- negative values target a phase before the top of the visible frame
- auto-enables `VK_EXT_present_timing` (+ `present_id2`, `calibrated_timestamps`, `get_surface_capabilities2`) when supported to get real refresh + first-pixel-out feedback

This requires fullscreen/direct-scanout/tearing support to produce a stable visible tearline. On a composited window, present-timing feedback is usually unavailable.

## Notes

Do not use this together with MangoHud's FPS limiter, DXVK's limiter, driver FPS limiter, or game FPS limiter while testing. Multiple limiters can fight each other.

This first version is intentionally simple. It does not yet implement frame-in-flight limiting or present-mode-specific policy.

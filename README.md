# VulkanFrameCapper

Experimental Vulkan layer that implements two frame-pacing modes, following the
design in [`scanline-sync.md`](scanline-sync.md):

- **Mode 1 — low-latency FPS limiter** (`FPS=N`): paces the start of each frame
  at `vkAcquireNextImageKHR` so no render queue builds up.
- **Mode 2 — scanline sync** (`SCANLINE=N`): an RTSS-like mode that forces
  `VK_PRESENT_MODE_IMMEDIATE_KHR` and times `vkQueuePresentKHR` against the
  display's scanout phase, pinning the tear line to a chosen vertical position.

The pacing core (shared by both modes):

- DXVK-style coarse `clock_nanosleep` + final busy-spin for accurate timing.
- The bulk of the wait happens at frame start (`acquire`) for low input lag,
  with only a small final correction before present (better than sleeping only
  inside `vkQueuePresentKHR`, which fires too late if the GPU is busy).
- Optionally auto-enables `VK_KHR_present_id` + `VK_KHR_present_wait`.
- Optionally auto-enables `VK_EXT_present_timing` (plus `calibrated_timestamps`
  and `get_surface_capabilities2`) for measured refresh duration and
  first-pixel-out feedback, used by a PLL that phase-locks the tear line.

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

## Run from build tree (explicit layer)

```sh
cd /home/enes/Projects/mein/VulkanFrameCapper
FPS=60 \
VK_LAYER_PATH=$PWD/build \
VK_INSTANCE_LAYERS=VK_LAYER_MEIN_frame_capper \
vkcube
```

The build-tree manifest uses a *relative* `library_path`, so on some loader
setups you may get `Requested layer ... was wrong bit-type`. If that happens,
either install as an implicit layer (below) or point `VK_LAYER_PATH` at a
directory whose manifest uses an absolute `library_path`.

## Install for current user as an implicit layer

MangoHud-like mode: no `VK_INSTANCE_LAYERS` needed.

```sh
./scripts/install-user.sh
```

Then run with only the mode variable:

```sh
FPS=60 vkcube                 # FPS limiter
SCANLINE=500 vkcube           # scanline sync
```

Steam/Proton launch option:

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

### Mode selection

- `FPS=60` — enable the FPS limiter. Fractional values like `59.94` work. `0` /
  unset disables capping. Ignored when `SCANLINE` is set.
- `SCANLINE=500` — enable scanline sync instead of the FPS limiter. The value is
  the target tearline position: `0` = top of screen, `height-1` = bottom,
  negative values wrap to near the end of the refresh (useful for hiding the
  tearline in/near vblank). Integer only.
- `DISABLE_VULKAN_FRAME_CAPPER=1` — disables the implicit layer.

### Extensions

- `VFC_PRESENT_WAIT=1` — use `VK_KHR_present_wait` when available (keeps queue
  depth low). Default: `1` when a mode is active.

Present-timing feedback (`VK_EXT_present_timing` + `calibrated_timestamps` +
`get_surface_capabilities2`) is **attempted by default** in scanline mode — no
env var needed. **Known driver bug:** on RADV (Mesa AMD) the `VK_EXT_present_timing`
swapchain create flag segfaults the driver inside `vkQueuePresentKHR`. The layer
detects RADV and automatically disables feedback (falling back to manual scanline
pacing) rather than crashing — you'll see a log line saying so. On non-RADV
drivers (e.g. NVIDIA proprietary) the PLL auto-lock engages.

- `VFC_FORCE_IMMEDIATE=1` — (scanline mode) force
  `VK_PRESENT_MODE_IMMEDIATE_KHR` when available. Default: `1`. Set `0` to keep
  the app's chosen present mode.

### Scanline tuning

- `VFC_SCANLINE_REFRESH_HZ=144` — manual fixed-refresh fallback when
  present-timing is off or unavailable. Default: `60`.
- `VFC_SCANLINE_OFFSET_US=0` — manual phase calibration. Use if the tearline is
  stable but consistently shifted from the requested line.
- `VFC_SCANLINE_VTOTAL_SCALE=1.05` — total vertical size including blanking,
  as a multiple of visible height. Default: `1.05` (≈3–8% blanking).
- `VFC_SCANLINE_ACQUIRE_MARGIN_US=750` — slack between the acquire wake point
  and the present. Default: `750`.
- `VFC_SCANLINE_MAX_PRESENT_WAIT_US=4000` — cap on the final wait inside
  `vkQueuePresentKHR` (most waiting is done at acquire). Default: `4000`.

### PLL / feedback tuning (only when present-timing feedback is active)

- `VFC_SCANLINE_PLL_STRENGTH=0.05` — PLL correction gain on the phase anchor.
  Default: `0.05`.
- `VFC_SCANLINE_MAX_PLL_STEP_US=200` — clamp on a single PLL step.
  Default: `200`.
- `VFC_SCANLINE_LATENCY_ALPHA=0.10` — EWMA gain for the learned
  present-call → first-pixel-out latency. Default: `0.10`.
- `VFC_SCANLINE_RENDER_ALPHA=0.02` — EWMA gain for the learned render lead
  time. Lower values are smoother. Default: `0.02`.
- `VFC_DISABLE_WHEN_VRR=1` — warn (and rely on manual pacing) when a VRR-like
  refresh interval is reported. Default: `1`.
- `VFC_DISABLE_WHEN_JITTER_ABOVE_US=500` — feedback jitter threshold for the
  guard. Default: `500`.

### Sleep / diagnostics

- `VFC_SLEEP_MARGIN_US=1000` — handoff point from `clock_nanosleep` to the
  busy spin. Default: `1000`.
- `VFC_BUSY_WAIT_US=200` — final busy-spin duration. Default: `200`.
- `VFC_LOG=1` — print debug logs to stderr. Default: `0`.

## Hotkeys

Active in both modes. X11 and Wayland are supported. Hotkeys fire once when the combo is released, so holding the keys does not repeat.

- **FPS mode:** `Shift + Plus/Minus` steps the cap by `0.01`;
  `Ctrl + Plus/Minus` steps by `0.10`.
- **Scanline mode:** `Shift + Plus/Minus` steps the phase offset by `1 ms`;
  `Ctrl + Plus/Minus` steps by `0.1 ms`.

## Scanline mode — how it works

When `SCANLINE` is a valid integer, the layer takes the scanline path and
ignores `FPS`.

```sh
SCANLINE=500 VFC_LOG=1 <app>      # mid-screen tearline
SCANLINE=-50  VFC_LOG=1 <app>     # tearline near the bottom / vblank
SCANLINE=-50  VFC_LOG=1 <app>                        # + auto PLL lock (when supported)
```

Core algorithm (per `scanline-sync.md`):

```
refresh_ns          = 1e9 / refresh_hz        (measured via present_timing)
vtotal              = visible_height * VFC_SCANLINE_VTOTAL_SCALE
scanline_offset_ns  = refresh_ns * SCANLINE / vtotal   (wrapped to [0, refresh))
target_flip_time    = phase_base + scanline_offset_ns         (first-pixel-out)
desired_present_call= target_flip_time - present_latency
desired_acquire_wake= desired_present_call - render_lead - acquire_margin
```

Behavior:

- forces `VK_PRESENT_MODE_IMMEDIATE_KHR` when available (unless
  `VFC_FORCE_IMMEDIATE=0`);
- paces most of the wait at frame start (`vkAcquireNextImageKHR`), with a small
  final correction before present, for low input lag;
- adaptively learns the render lead time (acquire-wake → present-call) to keep
  the tearline stable;
- with present-timing available, `VK_EXT_present_timing` first-pixel-out
  feedback (enabled by default) runs a PLL on `phase_base` (display scanout
  phase drift) and an EWMA on `present_latency`, auto-locking the tearline. The
  present-timing time domain is calibrated to `CLOCK_MONOTONIC` via
  `VK_KHR_calibrated_timestamps` when needed.

This requires fullscreen / direct-scanout / tearing support (e.g. Plasma
Wayland "allow tearing in fullscreen") to produce a stable visible tearline.
Without present-timing feedback, `SCANLINE` is manually tunable rather than
physically absolute; nudge `VFC_SCANLINE_OFFSET_US` (or the hotkeys) if the
tearline is consistently shifted.

## Notes

Do not stack limiters: disable MangoHud's FPS limiter, DXVK's limiter, the
driver's FPS limiter, and the in-game limiter while testing — multiple limiters
fight each other.

This implementation does not yet do frame-in-flight limiting or
present-mode-specific policy beyond forcing IMMEDIATE in scanline mode.

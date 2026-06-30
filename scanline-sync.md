rtss has a feature, it is called scanline i think.
moves the screen tear to bottom or top.
do you know about that?

Yes — you’re thinking of **RTSS Scanline Sync** in **RivaTuner Statistics Server**.

It is basically a special frame-limiter/sync mode that tries to **pin the tear line to a specific vertical position** on the screen. Instead of random tearing all over the image with V-Sync off, it concentrates the tear into one stable line that you can move up, down, or even just off-screen. Blur Busters describes it as focusing tearing into one controllable tearline. ([forums.blurbusters.com][1])

How it works in practice:

- **Positive values** usually place the tear line at a visible scanline position, such as near the top, middle, or bottom.
- **Negative values** are often used to push the tear line outside the visible area, commonly just above the top edge or below the bottom edge.
- It is usually used with **V-Sync off**.
- It works best when your PC can consistently render at or above your refresh rate. If frametimes spike or GPU usage is too high, the tearline can jump around. ([forums.blurbusters.com][2])

The benefit is that it can feel like a **low-latency alternative to V-Sync**: less visible tearing than normal V-Sync off, but usually less input lag than traditional V-Sync on. Blur Busters also notes that VRR displays like G-Sync/FreeSync are generally easier for low-lag smoothness, but Scanline Sync is useful especially on fixed-refresh monitors. ([Blur Busters][3])

A simple example: on a 1080p screen, scanlines roughly map from **top = 1** to **bottom = 1080**. So setting a value near the bottom moves the tear down. Setting a negative value can move it out of the visible screen. ([forums.blurbusters.com][4])

It is cool, but a little fiddly. When tuned well, it can look very smooth. When tuned badly, it can add weird latency or make tearing more obvious. Blur Busters forum users specifically warn that incorrect calibration can cause bad latency, and recommend positioning the tearline carefully. ([forums.blurbusters.com][5])

[1]: https://forums.blurbusters.com/viewtopic.php?t=4916&utm_source=chatgpt.com "RTSS Scanline Sync HOWTO"
[2]: https://forums.blurbusters.com/viewtopic.php?t=5564&utm_source=chatgpt.com "RTS Scanline not locking FPS to refresh rate in some ..."
[3]: https://blurbusters.com/howto-low-lag-vsync-on/?utm_source=chatgpt.com "HOWTO: Low-Lag VSYNC ON"
[4]: https://forums.blurbusters.com/viewtopic.php?t=4952&utm_source=chatgpt.com "Help with RTSS Scanline Sync"
[5]: https://forums.blurbusters.com/viewtopic.php?t=11585&utm_source=chatgpt.com "Does scanline sync really have more input lag than ..."

in linux i built a frame limitter vulkan layer.
i wonder if i can make it work like scanline sync.

Yes, you probably **can make your Vulkan layer behave scanline-sync-like**, but only under specific conditions.

The core idea is:

> **Do not just limit FPS. Delay `vkQueuePresentKHR` so that the buffer swap/page flip occurs while the monitor is scanning a chosen vertical region.**

That makes the tear line appear near a chosen scanline, like RTSS Scanline Sync.

For Vulkan, the important present modes are:

- `VK_PRESENT_MODE_IMMEDIATE_KHR`: presents immediately and **may tear**.
- `VK_PRESENT_MODE_FIFO_KHR`: v-sync queue; tearing is not observed and it is the only mode required by Vulkan.
- `VK_PRESENT_MODE_MAILBOX_KHR`: v-sync-like latest-frame mode; also tear-free.
  Vulkan’s docs explicitly describe FIFO and MAILBOX as updating on vblank, while IMMEDIATE is the mode associated with immediate display updates/tearing. ([Vulkan Documentation][1])

So for real scanline-sync behavior, you generally need:

1. **`VK_PRESENT_MODE_IMMEDIATE_KHR`**
2. **compositor bypass / direct scanout / fullscreen unredirect**
3. stable refresh timing
4. stable GPU frametimes below refresh interval
5. a high-precision wait before present

On Linux, the biggest problem is the compositor. Under many Wayland compositors, and sometimes X11 compositing paths, your app may not directly control the physical scanout. Then your Vulkan layer can pace frames, but it cannot reliably place the tearline.

The algorithm would be roughly:

```c
refresh_ns = 1e9 / refresh_hz;

// target_scanline:
//   0 = top
//   visible_height - 1 = bottom
//   negative or >height can mean "try to hide it in vblank/offscreen"
target_offset_ns = refresh_ns * target_scanline / total_vertical_lines;

// You need to calibrate this:
present_to_visible_offset_ns = measured_or_tuned_offset;

target_present_time =
    last_vblank_time
  + target_offset_ns
  - present_to_visible_offset_ns;

// before vkQueuePresentKHR:
sleep_until(target_present_time - safety_margin);
busy_wait_until(target_present_time);
vkQueuePresentKHR(...);
```

The practical version is less clean because Vulkan does not normally give you “current scanline.” You need to infer it.

Best sources of timing feedback:

- `VK_GOOGLE_display_timing`: gives refresh duration, past presentation timing, and lets apps request “present no earlier than” a desired time. The official docs say it can obtain presentation-engine timing and schedule a present no earlier than a desired time. ([vulkan.lunarg.com][2])
- `VK_EXT_present_timing`: newer standardized timing support for presentation feedback and scheduled presentation. ([Vulkan Documentation][3])
- `VK_KHR_present_wait` + `VK_KHR_present_id`: useful for knowing when queued presents complete, but not by itself enough to choose a scanline. `vkWaitForPresentKHR` waits for a present ID to complete or timeout. ([Vulkan Documentation][4])

A layer design could look like this:

```text
Intercept:
  vkCreateSwapchainKHR
  vkGetPhysicalDeviceSurfacePresentModesKHR
  vkQueuePresentKHR
  maybe vkAcquireNextImageKHR / vkAcquireNextImage2KHR

At swapchain creation:
  detect width/height/refresh/present mode
  optionally force or prefer IMMEDIATE if available
  initialize timing state

Every present:
  estimate next vblank/display phase
  compute target time for desired tearline
  delay until that time
  call down-chain vkQueuePresentKHR
  collect presentation timing feedback if extension is available
  adjust calibration offset slowly
```

But there is an important trap: delaying only inside `vkQueuePresentKHR` may be too late or too early depending on the app’s GPU work.

In Vulkan, `vkQueuePresentKHR` often waits on render-complete semaphores. Your layer can delay the call, but if the GPU work is not finished by then, the actual presentation still happens later. So for reliable scanline sync, your limiter should ideally pace the **start of the frame** too, not only the final present. A good structure is:

```text
Frame N:
  wait so rendering starts late enough
  render
  present just before desired scanline
```

For RTSS-like tuning, expose something like:

```text
scanline = 0        top
scanline = height/2 middle
scanline = height-1 bottom
scanline = -N       hide above/top or in vblank-ish area
```

But internally I would not use only visible height. Use estimated **total vertical lines**, including blanking, because the tear can be hidden during vertical blank. If you do not know total lines, start with:

```text
vtotal ≈ visible_height * 1.03 to 1.08
```

Then allow manual offset calibration.

The hard limitations:

- **FIFO / MAILBOX cannot do visible scanline sync** because they are designed to present on vblank, so no free-moving tearline.
- **VRR makes this concept weird** because scanout timing is not fixed in the same way. `VK_GOOGLE_display_timing` even notes that it treats VRR displays as fixed-refresh for its model. ([Vulkan Documentation][5])
- **Wayland compositors usually break direct tearline control.**
- **If the game is GPU-bound**, the tearline will jump.
- **If the app queues too many frames**, latency and timing become unstable.
- **If the compositor copies/blits**, your present timing may control compositor input timing, not physical scanout timing.

My recommendation: implement it as two modes.

**Mode 1: low-latency frame limiter**

Works everywhere:

```text
target_fps = refresh_hz
pace vkAcquireNextImageKHR or frame start
avoid queue buildup
```

**Mode 2: experimental scanline sync**

Enable only when:

```text
present mode = IMMEDIATE
compositor bypass likely
refresh timing stable
frametime < refresh period
```

Then add settings:

```text
scanline_offset
busy_wait_us
sleep_margin_us
max_gpu_usage_percent
auto_calibrate = on/off
```

The most important part is calibration. You need to measure the difference between:

```text
time you call vkQueuePresentKHR
```

and

```text
time the image actually becomes visible / presentation engine accepts it
```

If `VK_GOOGLE_display_timing` or `VK_EXT_present_timing` is available, use that. Without timing extensions, you can still make a “manual RTSS-style” version where the user adjusts the scanline until the tearline visually moves to the desired place.

So: **yes, but call it “scanline sync” only for immediate/direct-scanout cases.** Otherwise it is really just a very precise Vulkan frame pacer.

[1]: https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentModeKHR.html?utm_source=chatgpt.com "VkPresentModeKHR(3) - Vulkan Documentation"
[2]: https://vulkan.lunarg.com/doc/view/1.4.335.0/linux/antora/refpages/latest/refpages/source/VK_GOOGLE_display_timing.html?utm_source=chatgpt.com "VK_GOOGLE_display_timing(3)"
[3]: https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_present_timing.html?utm_source=chatgpt.com "VK_EXT_present_timing :: Vulkan Documentation Project"
[4]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_present_wait.html?utm_source=chatgpt.com "VK_KHR_present_wait(3) - Vulkan Documentation"
[5]: https://docs.vulkan.org/refpages/latest/refpages/source/VK_GOOGLE_display_timing.html?utm_source=chatgpt.com "VK_GOOGLE_display_timing(3) - Vulkan Documentation"

kde wayland has direct scanout mode.
my gpu 5090 supports VK_EXT_present_timing.

so it is possible?

Yes — **in your setup it is realistically possible**, not just theoretical.

But the thing you would build is not “RTSS clone” in the Windows-driver sense. It would be a **phase-locked Vulkan present scheduler**:

> use `VK_PRESENT_MODE_IMMEDIATE_KHR` / Wayland tearing, get feedback from `VK_EXT_present_timing`, then time `vkQueuePresentKHR` so the page flip happens at a chosen phase of the scanout.

KDE/KWin Wayland already has direct scanout for fullscreen views, originally added to reduce compositor overhead and latency. ([Adventures in Linux and KDE][1]) Wayland’s tearing-control protocol exists specifically so suitable clients, such as games or graphics APIs, can request asynchronous page flips where lower latency is worth possible tearing. ([wayland.app][2]) Vulkan’s `IMMEDIATE` present mode is the relevant one because it does not wait for vblank and may visibly tear; `FIFO` and `MAILBOX` are designed around vblank and should not show normal tearing. ([Khronos Registry][3])

The key requirement is this:

```text
KWin direct scanout + Vulkan IMMEDIATE/tearing + fixed refresh + EXT_present_timing feedback
```

With that, you can make the tearline **stable and movable**.

The basic control loop would be:

```c
// desired tear position:
scanline = user_value; // 0 = top, height - 1 = bottom

// from VK_EXT_present_timing:
refresh_ns = swapchainTiming.refreshDuration;

// ideally from DRM mode timing, not Vulkan:
vtotal = mode.vtotal;       // includes blanking
vactive = mode.vdisplay;

scanout_offset_ns = refresh_ns * scanline / vtotal;

// learned from feedback:
phase_base_ns = last_first_pixel_out_time;
present_latency_ns = moving_average(actual_first_pixel_out - queue_present_call_time);

// desired time when the new image should begin affecting scanout:
target_flip_time =
    next_phase_base_ns + scanout_offset_ns - present_latency_ns;

sleep_until(target_flip_time - safety_margin);
busy_wait_until(target_flip_time);

vkQueuePresentKHR(...);
```

`VK_EXT_present_timing` is useful because it can expose refresh timing, schedule presents, and report presentation stages. The spec proposal lists stages including `VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT` and `VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT`, which are exactly the kind of feedback you want for phase-locking to the real display pipeline. ([Vulkan Documentation][4]) It also exposes swapchain timing properties such as `refreshDuration` and `refreshInterval`, and it can report VRR vs fixed-refresh behavior. ([Vulkan Documentation][4])

I would implement it like this:

```text
1. Intercept vkCreateSwapchainKHR
   - Prefer/force VK_PRESENT_MODE_IMMEDIATE_KHR if available.
   - Add VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT if supported.
   - Keep FIFO/MAILBOX as fallback modes.

2. Query surface present-timing support
   - presentTimingSupported
   - presentAtAbsoluteTimeSupported
   - presentAtRelativeTimeSupported
   - supported presentStageQueries

3. Warm up
   - Present a few frames with timing queries enabled.
   - Get first-pixel-out / first-pixel-visible timestamps if available.
   - Build a stable refresh phase estimate.

4. On each present
   - Predict the next scanout phase.
   - Convert desired scanline to nanoseconds after top-of-refresh.
   - Delay vkQueuePresentKHR until that phase.
   - Request timing feedback for that present.
   - Adjust offset using a PLL-style correction.
```

The practical catch: **direct scanout is conditional**, not a permanent promise. KWin may fall back to compositing if the buffer cannot go directly to a hardware plane, if the window is obstructed, if an effect modifies it, if scaling/color/HDR paths require composition, or if the driver rejects the KMS configuration. KWin’s own plane-selection work describes this kind of “try plane, atomic-test, fall back to GPU composition” behavior. ([Xaver’s blog][5])

So your layer should detect “good enough” conditions by behavior, not assumptions:

```text
Good scanline-sync candidate:
  - present mode is IMMEDIATE
  - refreshInterval == refreshDuration, not VRR
  - first-pixel-out/visible stage is supported
  - feedback jitter is small
  - app is fullscreen
  - frame time consistently below refresh interval
  - only 1 frame queued / low swapchain latency

Bad candidate:
  - FIFO or MAILBOX
  - VRR active
  - KWin compositing path active
  - present feedback timestamps are missing or unstable
  - game is GPU-bound
  - frame queue depth > 1
```

For scanline sync specifically, I would **disable VRR** or at least treat VRR as unsupported. RTSS-style scanline sync relies on a stable fixed scanout cadence. `VK_EXT_present_timing` can indicate VRR by reporting `refreshInterval == UINT64_MAX`; in that case, the refresh phase is not the same stable target. ([Vulkan Documentation][4])

One important design note: a Vulkan layer can delay `vkQueuePresentKHR`, but if the game’s rendering semaphore is not ready, the actual present will happen late anyway. So for best results, your limiter should pace earlier than present too:

```text
Better:
  delay frame start / acquire
  let GPU render just-in-time
  delay present a tiny amount
  present at target scanline

Worse:
  let app render whenever
  only sleep inside vkQueuePresentKHR
```

For settings, I would expose something like:

```text
mode = off | fps-limit | scanline-sync
scanline = -200 ... vactive+200
present_mode = force-immediate | preserve
sleep_margin_us = 1000
busy_wait_us = 100-300
pll_strength = 0.02
max_gpu_busy = 90%
disable_when_vrr = true
disable_when_jitter_above_us = 500
```

Bottom line: **yes, possible** — especially with KDE Wayland direct scanout and `VK_EXT_present_timing`. The most realistic first prototype is:

```text
Fixed-refresh monitor
Plasma Wayland
fullscreen game
KDE “allow tearing in fullscreen” enabled
VK_PRESENT_MODE_IMMEDIATE_KHR
VK_EXT_present_timing stage feedback
manual scanline offset
```

Then add auto-calibration after the manual version works.

[1]: https://pointieststick.com/2021/02/05/this-week-in-kde-kwin-gains-direct-scan-out-and-gwenview-gets-a-lot-of-love/ "This week in KDE: KWin gains direct scan-out, and Gwenview gets a lot of love – Adventures in Linux and KDE"
[2]: https://wayland.app/protocols/tearing-control-v1?utm_source=chatgpt.com "Tearing control protocol"
[3]: https://registry.khronos.org/VulkanSC/specs/1.0-extensions/man/html/VkPresentModeKHR.html "VkPresentModeKHR(3)"
[4]: https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_present_timing.html "VK_EXT_present_timing :: Vulkan Documentation Project"
[5]: https://zamundaaa.github.io/wayland/2025/10/23/more-kms-offloading.html "More KMS offloading, with overlay planes | Xaver’s blog"

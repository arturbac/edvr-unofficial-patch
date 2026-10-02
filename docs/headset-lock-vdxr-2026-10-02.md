# Headset locks on foot while the game drops to 10 fps (Virtual Desktop, v0.18.0)

## Status

*Written 2026-10-02. Update whenever this doc changes.*

- **State:** one field log read; not reproduced; no fix. The stall is inside the vendor
  runtime's `xrEndFrame`; what it waits on is not in our logs. Waiting on the user.
- **Report:** on foot, walking to the boarding circle, the headset image locks while the
  game carries on at a much lower frame rate on the mirror. Twice since v0.18.0 (once at a
  settlement, once on a station). Quest 3 through Virtual Desktop's OpenXR runtime (VDXR
  1.0.10) at 72 Hz, RTX 4070 Ti 12 GB, HMD Quality 0.50 with DLSS Performance (1261x1196
  per eye to 2522x2392), `fix.vscreen_res_width = 2880`, the on-foot route and maps at
  their defaults, `fix.weapon_stability = 1` (deferred pacing on foot).
- **What the log shows:** from 11:14:38.1 the game ran at exactly 10.0 fps until the user
  quit about a minute later. Every frame's second Submit waited about 83 ms (6 display
  periods) inside VDXR's `xrEndFrame`, and the copy into VD's swapchain waited about 76 ms
  on the XR device. Every call returned success, EDVR's own CPU cost stayed about 0.35 ms
  a frame, and the route treated every frame. The previous session's breadcrumbs end on
  the same plateau (300 frames in 30.08 s), with no process exit.
- **Reading:** VD is holding the game back: it keeps its swapchain images, so our copy
  and `xrEndFrame` wait. The same fixed 100 ms cadence in both sessions looks more like a
  throttle than a slow GPU.
- **Ruled out:**
  - ruled out: the route or on-foot maps holding a stale panel, because the route treated
    every frame through the lock with no event at the onset, and a stale panel cannot
    lower the game's own Present rate;
  - ruled out: per-frame churn from the panel auto-fit, because the width is explicit
    (2880), every size stayed constant, and no DLSS feature was created after 10:58:35;
  - ruled out: EDVR blocking the render thread, because EDVR's CPU time stayed about
    0.35 ms a frame with no file I/O, lock or fence wait, and the sampler's samples name
    the game, the NVIDIA driver and NGX;
  - ruled out: device loss, because no DXGI, removal or XR-loss line exists and the exit
    was orderly.
- **Open:** (A) VD's stream backed up (encoder, network or headset decoder) and VD
  throttles the app; (B) GPU memory pressure on a 12 GB card in a heavy on-foot scene
  (on foot, v0.18.0 holds the route's DLAA feature and buffers beside the idle eye
  features, and VD composites a quad layer); (C) deferred pacing (no overlapped end frame)
  interacting with VDXR. Nothing logged separates them.
- **Blind spot:** the freeze diagnostics cannot see this regime. 100 ms frames sit under
  the 250 ms FREEZE line and the 150 ms stall sampler, LONG FRAME is limited to one line
  per 5 s, and `--freezes` printed PASS.
- **Instruments proposed, not built:** VRAM budget and usage (DXGI
  `QueryVideoMemoryInfo`) on the 5 s lines; the vendor runtime's session-state changes
  (none are logged today); any `xrEndFrame` of 3 periods or more on its own line; a
  sustained-slow line (for example 5 s under half the display rate) with a `--freezes`
  verdict.
- **Next:** from the user, the previous session's log pair (does its last
  `xr_end_frame` read about 83 ms too?); the VD version and settings (codec, bitrate,
  Synchronous Spacewarp), what the headset showed, and whether restarting VD's stream
  recovers it; then one flight with `nvidia-smi` logging memory, clocks and encoder load
  once a second. If it recurs on foot, an A/B with
  `experimental.temporal_aa_on_foot_world = off` and `experimental.on_foot_maps_sharp =
  off`.

## Evidence

Bundle `edvr-logs-20261002-111610.zip`, v0.18.0 (build 6ABED11D). The graphics log runs
10:50:12 to 11:15:42 local; the runtime log stamps UTC, one hour behind.

- 10:59:03.6 to 11:04:03.1, on foot: the route owned the world at 72 fps, and the boarding
  at 11:04 worked (the route TAKES, a 468 ms game-side hitch).
- 11:13:19 disembark; FREEZE lines of 337, 275 and 263 ms, all game waits. 11:13:22.098
  the route OWNS; the runtime logs `native_pacing,mode=deferred`.
- 11:13:20 to 11:14:11: VDXR's predicted display period switched to 27.78 ms (half rate)
  in 11 stretches. The 20 s frame rates read 72, 54, 45, 55, 49, 54, 33, 41 and 26 in a
  heavy scene, with the game's GPU time at 13 to 22 ms against a 13.9 ms budget.
- 11:14:37.979, the last healthy route window: `frames=343 ... treated=343`.
- 11:14:38.108, the onset: `LONG FRAME -- 100.9 ms`, 0.08 ms of it in EDVR's Present hook.
  The runtime's `native_long_cycle` at 11:14:38.116 reads 101.9 ms with
  `second_submit_roundtrip=86.8`. EDVR logs no event at the onset.
- 11:14:38 to 11:15:35: route windows of exactly 50 frames (`door-layer-only=100`), and
  twice `vScreen totals ... 200 frames in 20062 ms is 10 fps`. `native_submit_phases`
  `xr_end_frame=82.7130/83.8729/84.4452/84.7388`; before the lock, every window's p50 is
  6.3 ms or less. `native_producer_gpu` copy p50 75.6 ms (normally 0.04). The GPU census
  at 11:15:12 reads the game's GPU time at 34.8 ms with a frame gap p50 of 66 ms: the GPU
  idles two thirds of each frame and every span runs about 3x slower, which follows from
  the wait or from memory pressure. The runtime's period stays 13.889 ms with
  `shouldRender=1`.
- 11:15:35.238, a 254.6 ms FREEZE (a game wait); 11:15:35.5 the route releases
  (`on-foot-gate-lost`); 10:15:40.899Z, an orderly shutdown from the game's main thread.
  The user quit; the boarding never happened.
- Not attributed: in the slow state, every sampled frame creates a texture of about
  64.5 MiB (4096x4096 at 4 bytes). The same signature appears in the cockpit with the
  route off (11:08 to 11:11), so it is the game's.

# DLSS performance review — 2026-09-28

## Status

- **State:** latest completed Frontier flight `be70af2a`, `174242`/runtime
  `174244_204_43604`, verifies DLSS/history in dump `174605`. New reticle,
  precompile and mapped GPU events pass all full-build gates; validation below.
  Existing flight: `--expect-build be70af2a`; next capture pins the promoted
  installation. Prior good `155419`/`160518`: `8d60b453`. Exclude
  `45da6ae3`/`164641` fallback. Main separate; SteamVR/OpenXR target.
- **Finding:** guarded preservation removes three stale reseeds/eye: four→one
  seed, zero stale/failures. Parent seed median .623→.156 ms/eye; machinery
  .963→.485. Current and earlier Pimax flights differ in dimensions/runtime, so
  these are stage results, not a controlled whole-frame saving.
- **Correctness:** guarded optimization passes 2,896 WARP/RTX checks with exact
  legacy pixels/depth, four→one seed/eye for captured stencil-only writes.
  Private-write/read-only-view/late replay frames keep legacy invalidation.
- **Open:** slower GPU-frame storms remain unresolved. `160518` confirms
  diagnostics OFF/early stale 0. New `174242` has a steady texture-allocation
  wait and first-use night-vision shader hitch; loading began before F9.
  Selector details below. Exhaust capture cancelled after the user observed the
  same blur with DLSS off. Baseline remains rough.
- **Ruled out:** see Exclusions and the engine-motion/terrain arcs.
- **Baseline:** original OpenVR `v0.16.2` Steam graphics `160129`/`6AA6D371`,
  runtime `160131`/`6AA6D378`; user reports fpsVR 8.9–9.6 ms.
  Dimensions/preset/DLSS hash match current, but legacy Valve OpenVR and
  current native OpenXR over SteamVR differ. No exact regression conclusion.
- **Next:** Frontier flight with 76 precompiled variants and exact-pair
  selector; measure seed cost and GPU storms after loading. GPU completions use
  an explicit producer-sequence mapping: XR frame numbers and D3D11 query
  counters are independent, so equality is unsafe. Old CPU events must report
  correlation unavailable. Capture `frontier-cpu-gpu-20260928-174230-71d273`
  spans 23:43:55–23:46:18 UTC, zero lost events/all providers, 12,197 complete
  cycles. Saved DLLs lack CodeView, limiting private attribution. Counts are
  not GPU busy time.
- **Environment:** latest RTX 5090 flight uses SteamVR/OpenXR, 2016×1948 input
  → 4032×3896 output/eye, DLSS Performance/preset K, UI 125 5040×4870. Earlier
  Pimax Crystal Super/Pimax OpenXR 90 Hz evidence used
  2037×1969→4074×3938/UI5093×4923. Installed DLSS metadata remains 310.7.0.0;
  graphics logs do not read driver/DLSS versions. New profiling/installations
  use Frontier; the requested original baseline uses Steam.

## Pre-optimization Frontier evidence

Historical logs verify rc3-7/`538175a8`, graphics `6ABA6DF5`; changes through
`1d2e60ef` leave reviewed rendering/runtime paths unchanged.

Historical 12:36:05 census: EDVR ~4.949 ms/stereo, DLSS 3.129, UI depth .520.
Round-robin/floor-corrected overlapping scopes are not a synchronized budget;
temporal totals exclude some new hologram work.

### Red bars and actual submit work

The graph's `Submit wall` label is misleading: ABI-v5
`NativePerfHistory::cpuFigure` publishes preceding `callerWorkMs`, from pose
wait return to next pose wait entry. It includes rendering, both submits and
post-submit work; excludes the next pose wait. 0.17.0/current benchmark CPU
rows use pre-submit `applicationMs`. At 90 Hz the graph turns orange above 1.02
periods (~11.33 ms) and red above two (~22.22 ms).

Older owner Submit p50/p95/p99 .423/.592/.738 ms excludes caller roundtrips;
12,625 overlaps/zero sync failures. Rare stalls remain separate.

## Source review against 0.17.0

DLSS retains copy→MV/depth/masks→NGX→UI; transfer separate. Added
engine/hologram/diagnostics cost work, retired separation removes work.
Resolve/NGX timing predates 0.17.0; setup gated/DONOTFLUSH polling. Old luma
blocking was possible, not proven; desk probes synchronous.

## Optimizations delivered in 0229c358

`91cf805b` gates/caps hologram diagnostics. `a001f669` exact16-row luma cuts
logical traffic99.55%, with nonblocking retries/cached UAVs; no measured GPU
gain claimed. `0229c358` combines crisp `472ff122`: eight holo families, cached
coverage/timing; moved shading is not net overhead. Sphere/corona and
then-unproven reticle stay in-scene. `ui_depth.cpp` remains necessary;
`fix.ui_depth` already retired. WARP/RTX proves fallback needs stencil clear.

## 2026-09-28: combined Frontier flight and next changes

`141820`/`6ABACAED`, runtime `141822_298_35576` verify `0229c358`: 12,937
pairs/zero sync failures, HUD accepted/GPU~9.5 ms. Seed .612/.841, machinery
.962/1.197 ms/eye, four seeds/three stale. OFF/ON scopes differ. Historical
Steam/UI100 comparison is not matched; UI125 has 1.621× pixels.

HDR seed is absent from the general primary-draw timer, inside apparent game
remainder. `FrameUiLayerReissues` overlaps tonemap/coverage; never add
route/census/seed-subset prices. No luma timeout; coverage factory rate
unmeasured.

`0a31d6ba` adds gated seed census/timeline and paired GPU subprices with
health/inert idle polling; no freshness change. Empty-clear bypass preserves
active-owner/merge/unwind; 256 clears remove 4,096 heap requests. Full gates
pass UI629/observer1,766/timing2,798; clean promotion/install/push preserve
INI/DLSS. Older47reds mostly post; rare Submit14.564/outside-Present435.424
remain separate.

## 2026-09-28: verified probe flight and guarded seed optimization

Graphics `151543`/`6ABAD7B5`, runtime `151544_659_18972` verify `0a31d6ba`:
23,969 waits/23,846 pairs; only23s OFF.

All36invalidators/12complete timelines are forwarded stencil-only writes
(GEQUAL/write0; read00/write04; REPLACE), PS AFED1D4B087E18A9/
66B08F89E4926C01/A4D03619D631B186. Each has NewFrame+three stale seeds,
depth1/mask00/one pass, no clears/overflow. Healthy3,360 GPU pairs confirm1:3
NewFrame:stale; copy~.028/execution~.123/CPU~.0047 ms/seed. Parent seed
.614–.623/machinery.936–.963 ms/eye. ON GPU9.432/10.102/11.286,2,698valid.

GPU proof: stencil-only source depth stays exact, but bare preservation after
private write/raw replay changes occlusion. Raw depth-state guard covers
original read-only views; monotone watermark spans both eyes/layers/shared
sources/late replay/SEH/older sequences. Newer frames recover, shutdown resets;
admission, clears and all other freshness checks unchanged.

WARP/RTX 2,896 each: four→one copy/seed/clear, exact colour/depth/claimed
stencil and unchanged game writes; guarded/mask-growth cases retain legacy
correctness. Production warning-free; counters name matched writers/first-issue
attempts.

`8d60b453` full gates/promotion/install/push pass (log
`build/dlss-depth-seed-preservation-full.log`). Empty-clear hit rate >99.99% is
not saved CPU price; short OFF windows cannot establish cadence.

## 2026-09-28: guarded Frontier flight and original Steam baseline

Graphics `edvr_gfx_20260928_155419.log` verifies rc3-56/`8d60b453`, build
`6ABAE133`; user says it seemed better. Current SteamVR/OpenXR route:
2016×1948→4032×3896/UI5040×4870, DLSS Performance K. Diagnostics 0→1 at
15:57:18.795 MDT; debug views cycle 15:57:15.962–18.041 then return off.
Exclude mixed ending 15:57:20; steady OFF ends 15:56:50, ON ends
15:57:50/15:58:20.

OFF 2,684 frames/5,368 seeds and ON 2,698/5,396 prove one seed/eye, zero
stale/failures. ON seq15488/18186 show one NewFrame/depth1/mask00/pass1 seed,
100 subsequent writers, no invalidation/clears/private writes. Aggregate keys
overflow; timelines do not. Writer counts are not avoided seeds.

Seed median/p95 OFF .156/.189, ON .156/.166 ms/eye; machinery .485/.810 versus
.485/.802. Earlier Pimax .623/.963 has changed size/runtime/HUD work. Healthy
170/168 GPU pairs: copy~.033/execution~.136/record~.013 ms/seed, subsets of
parent prices. Machinery+moved medians .635/.621; no missing-door, refused UI
or resource/SEH failures.

Full benchmark OFF 19 / ON 22 each 2,700 valid, zero missing/invalid: GPU
p50/p95/p99 **8.841/9.582/9.912** versus **8.914/9.628/9.982 ms**; CPU
**3.117/3.775/4.169** versus **3.192/3.890/4.245**. This is no causal
diagnostics price: scene/HUD work varies. General census upscaler ~3.2–3.3
ms/stereo remains largest EDVR item; scopes overlap other prices.

Runtime 46 long/45 red rows mostly startup/exit. Steady OFF three reds over
78.8 s, ON zero over 81.2 s; post 57.013/88.890/26.227 ms with owner Submit
.319/.380/.518. Outside-Present max 88.788/raw.413/EDVR-after2.853 remains
unattributed; no fixed cadence established.

Original Steam baseline `160129`/`6AA6D371`, VR `160131`/`6AA6D378`, v0.16.2
explicitly forwards Valve OpenVR. User fpsVR **8.9–9.6 ms**; same
dimensions/Performance K/DLSS 310.7 hash as current. Old temporal 1.88/max3.00
and nested NGX 1.62/max3.35 ms/eye are accumulated, not additive medians.
Exclude mode flips 16:03:48–16:04:00. No whole-frame benchmark; different
runtime/sampling prevents an exact regression comparison.

## 2026-09-28: slower-frame storms, cause unresolved

Verified `8d60b453` Frontier gfx `160518`/runtime `160519_481_49900`,
SteamVR/OpenXR 22:05:19–22:09:21 UTC. User: lower average, initial >11.1 ms
yellow storms/green gaps, later calmer; ships unconfirmed. Diagnostics OFF
16:06:30; GPU p50/p95/p99 benchmark 21 ends 16:07:39: 9.597/12.395/13.697 ms
(220 valid/two invalid); 22 ends 16:08:07: 9.252/10.944/12.549 (2,203
valid/zero invalid). Short scope changes/repeated hash; no full 30 s window.

Early stale 0 excludes reseeding as whole cause. Late target sprites coincide
with stale 1,050→5,330/seeds 7,486→15,990, guard 0; diagnostics OFF leaves
invalidator keys unknown. After 16:09:12 multiply 98.2 MB×2/loading transition,
memory 1366.7 MB.

Owner Map wall max9.398 ms, ending16:07:38.639; factories ~64.6 MB repeatedly,
16:06:46.675 47 textures/859.6 MB. These include EDVR callers/allocations and
driver waits/scheduling; neither proves game origin or GPU cost.

Frame-end window 22:07:23: xrEndFrame median/p95/max 2.390/7.945/14.756 ms;
handoff median/p95 2.685/7.512 includes queue behind deferred finish. 20,963
overlaps/zero fallback/fail. 66 caller reds mostly post; largest
548.591/outside-Present 548.110 ms.

Discriminators: caller stacks/factories/Map for resource waits; GPU contexts
for preemption; finish/handoff/xrEndFrame for runtime queueing. Prior capture
lacked DxgKrnl. Correlation/provider presence is not cause or GPU busy time.

## SDK deployment correction

`45da6ae3` Frontier `164641`: "this build has no DLSS SDK", user confirms.
Auto-detection omitted NGX/FFX; cause unproven. Good `8d60b453` had NGX
310.9.1/FFX 3.1.2. Explicit verified `EDVR_NGX_SDK`/`EDVR_FFX_DX11` full gate
passed (`build/cpu-gpu-capture-sdk-full.log`, UI 2,899/FSR 63, DLSS runtime
carried); receipt requested/resolved paths correct. Promotion passed
(`build/cpu-gpu-capture-sdk-promotion.log`); Frontier `be70af2a` verified,
INI/DLSS hashes unchanged. Gfx `170036`: NGX initialized 880 ms, warmed eyes
93/28 ms. Both combined smokes: 60 CPU frames/zero lost, all GPU providers
observed, coverage only. Original capture cancelled before flight ETL;
corrected capture completed below.

## 2026-09-28: completed trace and visual dump

`174242`/runtime `174244_204_43604` verify `be70af2a`; `174605` has 16 paired
DLSS-success/history frames, no resets. Original/offline trace reports agree.
User started F9 while loading. Loading 23:43:55–23:44:30: 20 caller reds;
steady 23:44:30–23:46:05: three among 8,494 cycles. ETW cycle N matches log
long-cycle next-wait sequence N+1. Exclude dump/exit from steady comparison.

Loading cycle5600 before-Present451.389 ms is .889 running/.518 ready/449.983
waiting in game SleepConditionVariableSRW. Steady8385 after-Present222.029 has
a 165.496 ms game CreateTexture2D wait through proxy, system D3D11, NVIDIA and
VidMM. Nearby game allocation requests ~1.03 GiB, usage ~9–10 GiB; neither
proves budget exhaustion or GPU preemption. Steady11170 pre66.610 ms includes
52.042 running, matching night-vision compilation46 ms in a 72.5 ms frame; 43
caller samples lie in d3dcompiler_47 during the same 43.55 ms interval. UI
resolve157 ms, content22 ms and holo motion122/123 ms also compile on first
use. Working changes move 76 fixed/finite variants into embedded bytecode, with
byte-exact source/flags/macros/profile parity and stage checks. Arbitrary float
macros need a later uniform refactor; game-DXBC transformations stay dynamic.
Driver shader creation remains runtime.

Decoded steady game 3D queue submit→completion p50/p95/p99/max is
.835/5.797/7.023/15.431 ms, including queue delay, not GPU busy time. System
copy-node preemption events cannot be attributed to game 3D. No game 3D
submissions during the 165 ms allocation wait or compiler interval. Caller
ready-time tails do not support pervasive scheduler starvation. Individual
GPU-query samples are missing from ETW: exact GPU-tail attribution remains
open. Trace I/O/scene changes prevent a controlled overhead comparison.

New ETW GPU completion events require an explicit producer ID on the completed
CPU cycle: XR frame IDs and D3D11 WaitBegin query IDs can diverge while loading
or restarting. Existing native benchmark pairing already uses the shared
producer domain (`native_timing.cpp` waitSequence); it is unaffected. Focused
analyzer531/wrapper204 checks pass, including unequal XR/GPU counters and false
numeric matches. Old V1 reports mapping unavailable. The capture smoke gate
requires 60 explicit synthetic GPU completions with the known duration pattern;
this validates transport, not hardware timing. Use lighter CPU stacks plus
existing GPU-query publications first, after loading; retain optimized-build
matching PDBs. GPU queue collection is optional.

Selector: saved VS71DD/PS2D03 has no texture/screen reads; CB0 projection,
depthOFF, scaled-additive blend, stencil81/ref1/EQUAL/KEEP. This resolves the
parked screen-read uncertainty. Narrow exact-pair crisp admission passes 4,152
WARP and 4,152 RTX fixture checks; live checks remain. Unknown PS stays stock.
Stencil import adds fixture seed work: count it live. P/T are before crisp
composition; L0 is the actual compositor image.

Exhaust: eight bright cores join engine slots6/8/10/12/14/16/18/20 to
AACFDCF2/CF534B32 meshes, MV~(+.22,-.09) input pixels. All 48 captured core
draws have identical PSb2; core geometry was budget-declined, PSb1/textures
missing. Blue outside-hull pixels have camera motion; their source draw is
unknown. Known smoke geometry lies behind the eye. Absent engine motion and
animation in captured PSb2 are ruled out; PSb1, geometry and arbitrary texture
animation/other glow were not resolved. User subsequently observed the blur
with DLSS off and cancelled further exhaust capture. Leave rendering unchanged.

## Exclusions and next flight

- ruled out: persistent submit blocking as principal cause of logged large
  cycles, because owner-body p99 <.74 ms and caller rows mostly outside
  submits. Rare acquire/submit stalls remain separate candidates.
- ruled out: settings causing DLSS failure in `45da6ae3`, because the binary
  lacks its SDK. Exclude `164641` fallback from DLSS performance evidence.
- ruled out: diagnostics ON as a necessary cause of red bars, because latest
  steady OFF has three reds and ON zero. This does not prove ON prevents reds
  or identify a cadence/root cause.
- ruled out: lost overlap, because both verified flights have zero synchronous
  fallbacks/failures across >12,000 completions each.
- ruled out: DLSS-only origin of the exhaust blur, because the user observes
  the same appearance with DLSS off. Targeted capture cancelled before
  shipping.
- ruled out: removing seed stencil clear locally, because Frontier feature
  level `0xC000` uses one pass per bit; WARP/RTX lack specified-stencil-ref
  support. Fallback needs clear and already uses actual read masks.
- ruled out: accepted HDR/private reissues causing stale counts, because raw
  draws bypass owner hook. KEEP-only/read-only/fully skipped draws already
  exclude invalidation; quad skip can issue surviving ranges.
- ruled out: null-token no-claim merge bypass, because failed unwind must
  revoke late claims while clear-invalidated plans preserve newer owners.
- ruled out: bare depth-only/stencil-only seed bypass, because the 631-check
  WARP/RTX fixture changes later consumer pixels after private depth writes.
- ruled out: original-effect-only private-write guard, because an admitted
  read-only game view binds writable private depth and changes later pixels.
- ruled out: retaining depth on a stencil-only stale seed to avoid later
  growth, because WARP/RTX later consumer colour differs after private writes
  against a read-only game DSV; the guard alone does not invalidate the cache.
- ruled out: engine fetch as historical 3–8 ms prep spike; equally slow windows
  had no engine views. Removed sphere coverage caused it; see September 23 in
  [engine motion](kinematic-motion-injection-2026-09-19.md).
- ruled out: terrain residual in its Frontier arc; CPU shadow removed ~306
  copies/frame, live terrain-off was inert. Read that arc before reopening it.
- unresolved: regression versus 0.17.0. No matched scene/size/DLSS flight pair;
  graph boundary change is a confound, not proof performance is unchanged.

SteamVR/OpenXR capture must correlate sequence/Present/stacks. Periodic monitor
1 s/journal 500 ms/eager 100 ms/config 1 s/menu 250 ms/GPU polling remain
hypotheses; earlier ETW was externally disabled. Separate keyed-mutex waits
from fast producer copies.

## 2026-09-28: validated checkpoint

Full SDK/profile build `build/dlss-crisp-storm-full.log` passes production,
Python tools, all rigs and self-contained installer/resource checks. Receipt
input `6c68764e80938c3b1ab1e43458ac2bd977b8aa2a7e18006738b11e21a769e3a2`;
NGX/FFX requested and resolved paths match. Optimized DLLs now carry CodeView
records for matching PDBs. Commit this source, then use the receipt-guarded
clean DLL promotion before Frontier installation. No whole-frame saving or
storm resolution is claimed before the next flight. The graph label is now
`Application wall`, matching the measured interval. Actual hologram model
admission remains separate; see the HUD arc's offline coordinate proof.

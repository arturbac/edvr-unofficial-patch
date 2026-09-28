# DLSS performance review — 2026-09-28

## Status

- **State:** installed `45da6ae3` lacks NGX/FFX; exclude `164641` fallback.
  Rendering source unchanged; good flights `155419`/`160518` use
  `--expect-build 8d60b453`. Corrected SDK full gate passed; promotion next.
  Main separate, SteamVR/OpenXR target.
- **Finding:** guarded preservation removes three stale reseeds/eye: four→one
  seed, zero stale/failures. Parent seed median .623→.156 ms/eye; machinery
  .963→.485. Current and earlier Pimax flights differ in dimensions/runtime, so
  these are stage results, not a controlled whole-frame saving.
- **Correctness:** guarded optimization passes 2,896 WARP/RTX checks with exact
  legacy pixels/depth, four→one seed/eye for captured stencil-only writes.
  Private-write/read-only-view/late replay frames keep legacy invalidation.
- **Open:** user reports lower average GPU but storms of slower frames in new
  Frontier `160518`/runtime `160519_481_49900`, verified `8d60b453`; analysis
  confirms tails even with diagnostics OFF and early stale 0. Factory/Map
  activity and SteamVR queue timing correlate only; origin/cause unresolved.
  Baseline remains rough; no further seed fix supported.
- **Ruled out:** see Exclusions and the engine-motion/terrain arcs.
- **Baseline:** original OpenVR `v0.16.2` Steam graphics `160129`/`6AA6D371`,
  runtime `160131`/`6AA6D378`; user reports fpsVR 8.9–9.6 ms.
  Dimensions/preset/DLSS hash match current, but legacy Valve OpenVR and
  current native OpenXR over SteamVR differ. No exact regression conclusion.
- **Next:** receipt-guarded corrected promotion/Frontier verify, confirm DLSS
  engagement, then bounded `tools/cpu_profile.py --gpu` file-mode CPU
  stacks/native markers/DxgKrnl. Prior capture stopped ARMED, no flight ETL.
  Provider presence is not GPU busy time; no rendering fix without evidence.
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

`91cf805b` gates/caps hologram diagnostics (2,381 checks). `a001f669` exact
16-row luma: 288,781,416→1,303,680 logical bytes/stereo/2 s, 99.55% less;
six→96 copies, WARP extra .034 ms not hardware gain. Nonblocking retry
2,759/UAV reuse 1,729 checks; scatter factory gain conditional.

`0229c358` combines `a001f669`/`472ff122`. Eight holo families admitted;
sphere/corona/reticle stay in-scene, `ui_depth.cpp` remains necessary,
`fix.ui_depth` already retired. Coverage 768/timing 1,183 checks preserve
pixels/failures/complete-eye totals; moved shading is not net overhead. Seed
WARP/RTX 385,610: fallback needs clear. See crisp arc.

Full builds 83/84/85 jobs plus quiet gates pass config/exports/rigs/installer;
combined receipt
`b64e23a47da6c21724ccba6bf2c1cbdc7bd3b2db429b6f76d8af281d5e90172f`. Clean
promotion, sanctioned Frontier install/verify and branch push preserve
INI/DLSS; main separate.

## 2026-09-28: combined Frontier flight and next changes

Graphics `141820`/`6ABACAED`, runtime `141822_298_35576` verify
rc3-54/`0229c358`:12,937 pairs/zero sync failures, ON 14:20:26.146. HUD
accepted, GPU ~9.5 ms. OFF seed .612/.841, machinery .962/1.197 ms/eye, four
seeds/three stale/no failure. OFF/ON 9.411/9.466 scopes differ. Historical
Steam `472ff122` UI 100 seed .431/.842; UI 125 pixels 1.621×, not matched A/B.

HDR seed is absent from the general primary-draw timer, inside apparent game
remainder. `FrameUiLayerReissues` overlaps tonemap/coverage; never add
route/census/seed-subset prices. No luma timeout; coverage factory rate
unmeasured.

`0a31d6ba` adds gated census (16 keys/overflow, 64-event timeline/eye/30 s),
paired GPU sampling (stride 32, four markers, 32 slots/16 classes/512 retained
pairs), explicit health and inert idle polling. WARP/RTX UI 626/timing 2,798
include partial-begin SEH. CPU clear observer 1,766 checks preserve
active-owner/merge/unwind; 256 empty clears remove 4,096 heap requests, merge
bypass unsafe. Older runtime 47 reds mostly post; rare Submit 14.564 ms and
outside-Present 435.424 remain separate/unattributed.

`0a31d6ba` full validation: 85 jobs/four quiet gates, 264-key contract,
exports/installer, UI 629/observer 1,766/timing 2,798. RTX checks, clean
promotion, Frontier install/verify/push preserve live INI/DLSS. Receipt
`9c94e20a289bf19f430f94b99fc49055f71b919e383e82625b65d1cf7214c944`. This
instrumentation delivery did not change freshness.

## 2026-09-28: verified probe flight and guarded seed optimization

Graphics `edvr_gfx_20260928_151543.log`/runtime
`edvr_openxr_20260928_151544_659_18972.log` verify rc3-55/`0a31d6ba`, graphics
`6ABAD7B5`. Runtime 21:15:44.660–21:20:20.617 UTC: 23,969 waits/23,846 pairs.
ON→OFF 21:19:51.820, only 23 s OFF.

All 36 invalidators in 12 complete eye timelines are forwarded nonzero
indexed-instanced stencil writes (GEQUAL/write0; read00/write04; pass REPLACE),
PS AFED1D4B087E18A9/66B08F89E4926C01/A4D03619D631B186. Each timeline: NewFrame
plus three stale-draw seeds, depth1/mask00/one pass; no clears. Complete
timelines have no overflow; aggregate keys do. 3,360 healthy sampled GPU pairs
show 1:3 NewFrame:stale ratio. Per-seed medians copy~.028/execution~.123/CPU
record~.0047 ms; per-eye summed parent seed .614–.623, machinery .936–.963.
Parent no-free 916–2,906/window excludes incomplete eyes. ON benchmark GPU
9.432/10.102/11.286 ms p50/p95/p99, 2,698 valid.

GPU proof: stencil-only source depth stays exact, but bare preservation after
private write/raw replay changes occlusion. Raw depth-state guard covers
original read-only views; monotone watermark spans both eyes/layers/shared
sources/late replay/SEH/older sequences. Newer frames recover, shutdown resets;
admission, clears and all other freshness checks unchanged.

WARP/RTX 2,896 each: four→one copy/seed/clear, exact colour/depth/claimed
stencil and unchanged game writes; guarded/mask-growth cases retain legacy
correctness. Production warning-free; counters name matched writers/first-issue
attempts.

`8d60b453` full validation: 85 jobs/four quiet gates, UI 2,899, 264-key
contract,exports/installer; receipt
`c8559180f0adfbff0fe89dfb8d099b4bebc7d514a1ac9deea64bf63284915631`, log
`build/dlss-depth-seed-preservation-full.log`. Clean DLL promotion, sanctioned
Frontier install/verify and branch push preserve live INI/DLSS; main separate.

Older clear bypass >99.99%, no observer failures; hit rate is not saved CPU
price. Short OFF/red rows cannot establish cost/cadence.

## 2026-09-28: guarded Frontier flight and original Steam baseline

Graphics `edvr_gfx_20260928_155419.log` verifies rc3-56/`8d60b453`, build
`6ABAE133`; user says it seemed better. Current SteamVR/OpenXR route:
2016×1948→4032×3896/UI5040×4870, DLSS Performance K. Diagnostics 0→1 at
15:57:18.795 MDT; debug views cycle 15:57:15.962–18.041 then return off.
Exclude mixed ending 15:57:20; steady OFF ends 15:56:50, ON ends
15:57:50/15:58:20.

OFF 2,684 frames/5,368 seeds and ON 2,698/5,396 prove one seed/eye, zero
stale/failures; last ON has 5,396 treated eyes/2,700 native frames. Both ON
complete timelines (seq 15488/18186, 24/28 events per eye) contain one
successful NewFrame2/depth1/mask00/pass1 seed, 100 subsequent writer events
total, no invalidation. Aggregate overflow 56,934/87,262 loses keys but
invalidated/after-stale/seed overflow all zero; no clears. Preserved matched
writer events 112,372/112,484, potential raw-depth first-issue attempts 0.
Counts are not avoided seeds.

Parent seed median/p95 per eye-frame: OFF .156/.189, ON .156/.166 and .155/.164
ms; machinery .485/.810,.485/.802,.481/.889. Earlier Pimax seed .623→.156 (~75%
lower), machinery .963→.485 (~50%); changed dimensions/runtime/HUD counts
preclude exact whole-frame attribution. Healthy 170/168 sampled pairs, only
NewFrame class 0x2, zero loss/pending: copy means .0323/.0331, execution
.1384/.1337, CPU record .0129/.0135 ms/seed. These are subsets, not additions
to parent prices; OFF starts zero sampled queries. Complete machinery+moved
medians .635/.621 ms/eye, all issued draw queries valid. Zero
refused/late/missing-door UI, device-loss/OOM/SEH/resource-failure signatures.

Full benchmark OFF 19 / ON 22 each 2,700 valid, zero missing/invalid: GPU
p50/p95/p99 **8.841/9.582/9.912** versus **8.914/9.628/9.982 ms**; CPU
**3.117/3.775/4.169** versus **3.192/3.890/4.245**. This is no causal
diagnostics price: scene/HUD work varies. General census upscaler ~3.2–3.3
ms/stereo remains largest EDVR item; scopes overlap other prices.

Runtime 46 long/45 red rows mostly startup/exit. Steady OFF three reds over
78.8 s, ON zero over 81.2 s; post 57.013/88.890/26.227 ms with owner Submit
.319/.380/.518. Outside-Present max 88.788/raw.413/EDVR-after2.853 remains
unattributed; no fixed cadence established.

Original baseline: Steam graphics `edvr_gfx_20260928_160129.log`
v0.16.2/`6AA6D371`; `edvr_vr_20260928_160131.log`/`6AA6D378` explicitly
forwards Valve SteamVR exports at 16:01:33.744. Legacy proxy wording does not
imply native OpenXR. User fpsVR **8.9–9.6 ms**; same dimensions/Performance K
and DLSS 310.7, identical SHA256
`BE6E434A94CA32499515EB62CA0E6C274526055D568D0426E4C652DCDFB6EE6E`. Old
accumulated temporal 1.88/max3.00 and nested NGX 1.62/max3.35 ms/eye are not
medians or additive. Exclude mode changes
16:03:48.013/57.504/59.691/16:04:00.462. Baseline has no 30 s whole-frame
benchmark; fpsVR range versus current native p50 cannot establish exact
regression. No Steam changes; no further seed invalidation change supported.

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

Owner-context real Map wall stall max 9.398 ms, 5 s ending 16:07:38.639; can
include EDVR callers, driver/GPU waits/scheduling, not GPU cost. LONGFRAME
factories ~64.6 MB repeatedly; 16:06:46.675: 47 textures/859.6 MB. Same-device
totals include EDVR allocations; origin unknown.

Frame-end window 22:07:23: xrEndFrame median/p95/max 2.390/7.945/14.756 ms;
handoff median/p95 2.685/7.512 includes queue behind deferred finish. 20,963
overlaps/zero fallback/fail. 66 caller reds mostly post; largest
548.591/outside-Present 548.110 ms.

Need frame-aligned CPU/GPU evidence: resource/driver work→caller stacks plus
factory/Map/GPU intervals; preemption→other-context GPU scheduling; SteamVR
queue→finish/handoff/xrEndFrame correlation. Prior capture lacked DxgKrnl;
optional combined file-mode profiling is now available. Provider presence alone
is not busy time. Correlations do not prove causes; no seed fix without
discriminator.

## SDK deployment correction

`45da6ae3` Frontier `164641`: "this build has no DLSS SDK", user confirms.
Auto-detection omitted NGX/FFX; cause unproven. Good `8d60b453` had NGX
310.9.1/FFX 3.1.2. Explicit verified `EDVR_NGX_SDK`/`EDVR_FFX_DX11` full gate
passed (`build/cpu-gpu-capture-sdk-full.log`, UI 2,899/FSR 63, DLSS runtime
carried); receipt requested/resolved paths correct. Corrected promotion HEAD
next. Combined smoke: 60 CPU frames/zero lost, Dxg 567919/D3D11 11153/DXGI 4076
events, coverage only. Stopped ARMED; no flight ETL.

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

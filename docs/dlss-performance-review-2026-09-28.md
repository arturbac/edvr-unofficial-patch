# DLSS performance review — 2026-09-28

## Status

- **Current flight:** Frontier `7aaaf39c` gfx `033435`/runtime
  `033436_691_11608` confirms both cockpit models in final composition. CPU
  spikes worsen approaching ships; exact stall cause remains unresolved. See
  the 2026-09-29 entry for chronology, limits and discriminators. Work stays on
  the separate Frontier branch; main is unchanged.
- **Conclusion:** no controlled whole-frame performance comparison or exact
  regression conclusion. The new remap follows the earliest stalls; see
  Exclusions for ruled-out causes and the HUD arc for build/fixture evidence.
- **Baseline:** OpenVR `v0.16.2` Steam graphics `160129`/`6AA6D371`, runtime
  `160131`/`6AA6D378`; user reports 8.9–9.6 ms. Dimensions/preset/DLSS hash
  match, but legacy Valve OpenVR differs from native OpenXR over SteamVR.
- **Environment:** RTX 5090 SteamVR/OpenXR, 2016×1948 input → 4032×3896
  output/eye, DLSS Performance/preset K, UI 125 5040×4870. Earlier Pimax
  Crystal Super/Pimax OpenXR 90 Hz evidence used
  2037×1969→4074×3938/UI5093×4923. Installed DLSS metadata is 310.7.0.0;
  graphics logs omit driver/DLSS versions. Profiling uses Frontier; baseline
  uses Steam.
- **Next:** existing helper, 90 s CPU/GPU capture on installed `7aaaf39c`,
  quiet cockpit then approach ships, diagnostics OFF and no eye dump. Keep the
  window open through Saving. Earlier `1ff8c224` trace `194016-f80584` is
  recovered; do not repeat it for saving.

## Pre-optimization Frontier evidence

Historical rc3-7/`538175a8`, graphics `6ABA6DF5`, 12:36:05 census: EDVR ~4.949
ms/stereo, DLSS 3.129, UI depth .520. Reviewed paths unchanged through
`1d2e60ef`. Overlapping round-robin scopes are not a synchronized budget.

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

`91cf805b` gates/caps hologram diagnostics; `a001f669` exact16-row luma cuts
logical traffic99.55% with nonblocking retries/cached UAVs, no measured GPU
gain. `0229c358` combines crisp `472ff122` (eight families). Sphere/corona and
then-unproven reticle stayed in-scene. `ui_depth.cpp` remains necessary;
`fix.ui_depth` already retired; fallback needs stencil clear.

## 2026-09-28: combined Frontier flight and next changes

`141820`/`6ABACAED`, runtime `141822_298_35576`, verify `0229c358`: 12,937
pairs/zero sync failures, HUD accepted/GPU~9.5 ms. Seed .612/.841, machinery
.962/1.197 ms/eye, four seeds/three stale; OFF/ON scopes differ. UI125 has
1.621× UI100 pixels. HDR seed lies outside the primary-draw timer; reissue,
tonemap, coverage and seed prices overlap. `0a31d6ba` adds gated seed census
and GPU subprices, not freshness changes. Empty-clear bypass removes 4,096 heap
requests/256 clears, preserving unwind. Full gates/promotion/install pass.

## 2026-09-28: verified probe flight and guarded seed optimization

`151543`/`6ABAD7B5`, runtime `151544_659_18972`, verify `0a31d6ba`. All 36
invalidators/12 timelines are forwarded stencil-only writes, PS
AFED1D4B087E18A9/66B08F89E4926C01/A4D03619D631B186. Three stale seeds/eye;
3,360 GPU pairs price copy .028/execution .123/CPU .0047 ms/seed. Parent seed
.614–.623, machinery .936–.963 ms/eye. Guarded preservation retains legacy
private-write/raw-replay/read-only-view/late-unwind behavior across shared
sources and both eyes. WARP/RTX 2,896 each: four→one seed with exact
colour/depth/claimed stencil. `8d60b453` full gates/promotion/install/push pass
(`build/dlss-depth-seed-preservation-full.log`). Clear hit rate is not saved
CPU price; the short OFF period cannot establish cadence.

## 2026-09-28: guarded Frontier flight and original Steam baseline

`155419`/`6ABAE133` verifies `8d60b453`; user says better. Current SteamVR
dimensions/preset match Status. Exclude 15:57:15.962–20 transitions. OFF2,684
and ON2,698 frames: one seed/eye, zero stale/failures. Seed median/p95
.156/.189 versus .156/.166; machinery .485/.810 versus .485/.802 ms/eye. Full
benchmark OFF19/ON22, 2,700 valid each: GPU p50/p95/p99 8.841/9.582/9.912
versus 8.914/9.628/9.982 ms; CPU3.117/3.775/4.169 versus 3.192/3.890/4.245.
Scene varies; no causal diagnostics price. Three OFF reds, ON zero; owner
Submit .319/.380/.518, post57.013/88.890/26.227. Original Steam baseline is in
Status; exclude its 16:03:48–16:04:00 mode flips.

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

Map wall max9.398 ms; factory burst47 textures/859.6 MB. Neither proves game
origin or GPU cost. xrEndFrame median/p95/max2.390/7.945/14.756 ms, handoff
2.685/7.512 includes deferred-finish queue. 20,963 overlaps/zero failures. 66
caller reds mostly post, largest548.591 ms. Discriminators: caller stacks for
resource waits, GPU contexts for preemption, finish/handoff for runtime
queueing. Prior capture lacked DxgKrnl; correlations are not GPU busy time.

## SDK deployment correction

`45da6ae3` Frontier `164641`: "this build has no DLSS SDK"; exclude fallback.
Auto-detection omitted NGX/FFX. Explicit `EDVR_NGX_SDK`310.9.1 and
`EDVR_FFX_DX11`3.1.2 passed full/promotion gates
(`build/cpu-gpu-capture-sdk-{full,promotion}.log`). Frontier `be70af2a`
verified, INI/DLSS unchanged; gfx170036 initialized NGX. Smokes had60 CPU
frames/zero loss, provider coverage only. Corrected flight follows.

## 2026-09-28: completed trace and visual dump

`174242`/runtime `174244_204_43604` verify `be70af2a`; dump `174605` has 16
paired DLSS-success/history frames, no resets. F9 started while loading:
23:43:55–23:44:30 has 20 caller reds, steady 23:44:30–23:46:05 has three among
8,494 cycles. ETW cycle N matches next-wait log sequence N+1.

Cycle5600 waits449.983/451.389 ms in game SleepConditionVariableSRW. Steady8385
includes165.496 ms CreateTexture2D wait through D3D11/NVIDIA/VidMM in222.029 ms
post-Present. Nearby~1.03GiB allocations/9–10GiB usage do not prove budget
exhaustion. Steady11170 compiler samples corroborate46 ms nightvision compile.
Other first-use compiles reach157 ms. Checkpoint embeds 76 finite variants with
exact source/flags/macros/profile/stage checks; arbitrary-float macros,
game-DXBC transforms and driver creation remain runtime.

Game3D submit→completion .835/5.797/7.023/15.431 ms p50/p95/p99/max includes
queue delay; no game3D submissions during allocation/compiler waits. System
copy preemption is not attributed to game3D. Ready tails do not establish
pervasive scheduler starvation. Individual query samples were missing. New ETW
uses explicit producer IDs, not coincident XR/D3D query counters;
Analyzer531/wrapper204 checks and60-sample transport smoke pass. V1
unavailable.

Saved VS71DD/PS2D03 has no screen reads, depthOFF, scaled-additive blend,
stencil81/ref1/EQUAL/KEEP. Exact-pair admission passes4,152 WARP/RTX each;
unknown PS stays stock. This is world brackets, not scanner <>. P/T/L0 precede
final crisp composition in `native_sharpen.cpp`. Exhaust blur persists DLSSoff;
user cancelled investigation.

## Exclusions and next flight

- ruled out: persistent submit blocking as main cause: owner p99<.74ms; long
  caller rows mainly outside submits. Rare stalls remain.
- ruled out: settings causing `45da6ae3` DLSS failure: SDK absent; exclude
  `164641` fallback.
- ruled out: diagnostics ON required for reds: steady OFF3/ON0; no causality.
- ruled out: lost overlap: >12,000 completions/flight, zero sync failures.
- ruled out: DLSS-only exhaust blur: user sees it off; capture cancelled.
- ruled out: removing seed clear: Frontier `0xC000` needs per-bit fallback;
  WARP/RTX lack specified-stencil-ref support.
- ruled out: accepted HDR reissues causing stale counts: raw draws bypass owner
  hook; KEEP/read-only/skipped draws already excluded.
- ruled out: null-token merge bypass: failed unwind must revoke late claims.
- ruled out: bare depth/stencil seed bypass:631-check WARP/RTX later pixels
  differ after private writes.
- ruled out: original-effect-only guard: read-only game DSV becomes writable
  private DSV and alters later pixels.
- ruled out: keeping stale depth: later WARP/RTX pixels differ after private
  writes; guard alone does not invalidate cache.
- ruled out: read-only private DSV saving E508 seeds:194946/16488/402,484
  original flags0/depth disabled; matching flags changes no depth writes.
- ruled out: engine fetch causing3–8ms prep: slow windows lacked engine views;
  removed sphere coverage caused it. See September23 in [engine
  motion](kinematic-motion-injection-2026-09-19.md).
- ruled out: terrain residual: shadow removed~306 copies/frame; terrain-off
  inert. Read its arc before reopening.
- unresolved: regression versus0.17.0: no matched flight pair; graph boundary
  change does not prove unchanged performance.

Correlate exact sequence/Present/stacks before assigning periodic polling,
keyed-mutex, allocation or worker waits. Earlier externally disabled ETW is not
evidence of fast execution.

## 2026-09-28: validated checkpoint

`1ff8c224` full/promotion gates pass in
`build/dlss-crisp-storm-{full,promotion}.log`; NGX/FFX and optimized PDBs
verify. Installed/pushed separately. Graph is `Application wall`; no storm
resolution.

## 2026-09-28: recovered latest flight and corrected selector identity

Frontier `1ff8c224` gfx194044/runtime194045_548_19952 verifies. Capture
`frontier-cpu-gpu-20260928-194016-f80584` was interrupted by the user while
saving. WPR was closed; 9,694,085,120B ETL decodes successfully in134s,5.28GB
peak. `report-recovered.json` has15,382 covered/derived cycles, zero loss,
15,365 valid explicit GPU joins/17 unavailable, zero malformed/ambiguous; one
GPU witness has no completed CPU cycle at the boundary. PDBs match2/2. Original
failed status is preserved beside separate `recovery-status.json`. Smoke had
actually passed60 mapped synthetic samples. Save helper now defers Ctrl+C for
the SAME stop child within180s; window closure remains unprotected. Typed
errors/progress and220 self-tests pass; this did not alter the ETL.

Quiet01:43:01–31 UTC: GPU9.129/10.648/11.719ms p50/p95/p99,2.71%>11.111.
Target01:44:20–49:11.136/13.508/14.466,51.15%>. Rise begins01:43:55, before
first world-reticle redirect01:44:17.669. No strict alternating cadence. Worst
pre-targetGPU24.77–27.43ms includesR1 game-worker waits14–17ms; elapsed query
segments are not GPU busy time. Other tails lack large waits; worker joins are
not the whole cause. Loading from01:44:50.169 and exit are excluded. Individual
full driver stacks are not available in this report. Mixed-target HUD
seeds1→~3.73/eye, parent seed median.154→.754ms/eye; target sprites also
increase, so there is no exclusive reticle attribution. All native benchmark
windows are scope-changed partials, diagnosticsOFF. Nightvision precompiled
creation.259ms versus prior46ms compile;28 first-use CreateShader
successes.129–.655ms, without the corresponding compile hitches.

Second verified gfx194646, dump194946, is separate from this ETW flight. User
pictures specify yellow <> on the scanner rim; the admitted71DD/2D03
world-space brackets are a different element. ruled out: this exact pair as the
desired scanner selector, because the user pictures establish the distinction.
Correction: `captureEyeRun(result)` runs before `uiLayerComposite` in the
native path; P/T/L0 cannot qualify final crisp pixels. Add a matched final
capture rather than infer success/refusal from those images. Actual hologram
admission was explicitly requested. Full build and fixture results are recorded
in the HUD arc (`build/dlss-holo-final-full.log`); see its final entry for
semantic evidence, guards and capture limits. No whole-frame saving claimed.
Existing probes miss scanner centers(.5861,.5916)/(.5939,.5919) and cannot
identify private HDR; shared94D5 remains unadmitted.

Tight red-hull ROI in16487/88/89/95/502 has82–86% joined engine motion. ruled
out: wholesale missing hull motion here; joins do not prove accurate vectors or
explain blur. No sharpening.

## 2026-09-29: holograms confirmed; ship-approach CPU stalls unresolved

Frontier gfx `033435`/runtime `033436_691_11608` verify installed `7aaaf39c`
(`6ABB2662`, PID11608), SteamVR/OpenXR at the Status dimensions. User confirms
good holograms and fpsVR CPU yellow spikes >11.1 ms, worse approaching ships.
Final dump `033645` writes 34/34, scenes 12224–12239/native 10555–10570 match;
32 composition true, first right crop/overview passthrough. Both models are
absent P/T/L0 and visible Final. Scanner contacts overlap the yellow <>;
existing probes miss it. Final changes world brackets and 30–34% bright pixels
in a compact ship ROI with sharpening OFF: overlay contribution, not proof of
ship mesh admission. Exclude 03:36:45.300–49.772 capture/readback.

ruled out: new remap causing the earlier storm, because its first admission is
03:36:00.207, after 140/142/231 ms post-submit stalls at 03:35:57–58. Of 38
logged pre-dump long cycles, 35 are post-submit dominant. Pre-submit median
.645→4.118 ms and post-submit 1.232→5.533 across adjacent30 s windows. Raw
Present max 1.114 ms versus outside-Present 226.314 ms in the later window;
these are aggregates, not an exact-cycle attribution. Native benchmark omits
post-submit/submit waits, so 4–6 ms medians do not refute fpsVR spikes.

Intercepted resource bursts include 41 textures/169.5MB at 03:35:57 and 50
textures + 16 buffers/907.4MB at 03:36:03; counters include EDVR. Owner Map max
6.396 ms is also unpriced by origin. DLSS median 3.23–3.38/p95 4.26–4.48
ms/stereo stays steady. HUD window 03:36:35 has 58.78 redirects, 29.12 stock
writebacks/stereo, 2.367 seeds/eye; seed .493/1.536 and machinery .971/2.023 ms
median/p95 per eye. Four remap draws/stereo, zero refusals; two shaders
prepared once. Different workload prevents exclusive remap-cost attribution.
Diagnostics OFF, all 20 benchmark rows scope-changed: no controlled baseline.

Next 90 s trace uses the verified helper/PDBs without a rebuild. Quiet cockpit
then approach ships, diagnostics OFF throughout, no eye dump. Discriminators:
running stacks game/EDVR/driver; blocked stacks+wakers allocation/Map versus
worker joins; ready delay for scheduling; caller handoff wait paired with owner
finishPair/xrEndFrame and GPU queues for backpressure. Query spans include CPU
submission gaps; distinguish packet execution from elapsed durations. Existing
instrumentation covers these causes. No speculative rendering change.

# DLSS performance review — 2026-09-28

## Status

- **State:** target-hologram admission and FinalCrisp pass the full build;
  deployment stays Frontier-only on this separate branch. Main is unchanged.
  Preceding `1ff8c224` flights `194044`/`194646`, eye dump `194946`. Capture
  `194016-f80584` recovered with 15,382 covered cycles, zero lost events,
  15,365 valid GPU joins and matching PDBs. Expect `1ff8c224` for these logs.
  Earlier `174242`/`174605`: `be70af2a`; exclude `45da6ae3`/`164641` fallback.
- **Finding:** guarded preservation removes three stale reseeds/eye: four→one
  seed, zero stale/failures. Parent seed median .623→.156 ms/eye; machinery
  .963→.485. Current and earlier Pimax flights differ in dimensions/runtime, so
  these are stage results, not a controlled whole-frame saving.
- **Correctness:** guarded optimization passes 2,896 WARP/RTX checks with exact
  legacy pixels/depth, four→one seed/eye for captured stencil-only writes.
  Private-write/read-only-view/late replay frames keep legacy invalidation.
- **Open:** GPU storms include game-worker waits before the new world marker
  appears, plus a sustained targeting/HUD-seed rise. Elapsed queries are not
  GPU busy time. Desired selector is yellow <> on the scanner rim; current
  dumps precede final HUD composition. World brackets are different. Ship blur
  and selector need final-image checks. Sampled red hull has82–86% joined
  engine motion; this does not prove vector accuracy. Exhaust capture
  cancelled.
- **Ruled out:** see Exclusions and the engine-motion/terrain arcs.
- **Baseline:** original OpenVR `v0.16.2` Steam graphics `160129`/`6AA6D371`,
  runtime `160131`/`6AA6D378`; user reports fpsVR 8.9–9.6 ms.
  Dimensions/preset/DLSS hash match current, but legacy Valve OpenVR and
  current native OpenXR over SteamVR differ. No exact regression conclusion.
- **Next:** one Frontier flight with the promoted branch build; check target
  hologram transparency/occlusion and take one eye dump with scanner <> and
  space ship visible. Compare FinalCrisp with old stages and inspect seed
  costs. Preserve the recovered flight; no repeat is needed for saving. The
  helper now defers Ctrl+C during the same bounded stop command; window closure
  is unprotected. GPU joins use explicit producer IDs; old V1 stays
  unavailable.
- **Environment:** latest RTX 5090 flight uses SteamVR/OpenXR, 2016×1948 input
  → 4032×3896 output/eye, DLSS Performance/preset K, UI 125 5040×4870. Earlier
  Pimax Crystal Super/Pimax OpenXR 90 Hz evidence used
  2037×1969→4074×3938/UI5093×4923. Installed DLSS metadata remains 310.7.0.0;
  graphics logs do not read driver/DLSS versions. New profiling/installations
  use Frontier; the requested original baseline uses Steam.

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

`174242`/runtime `174244_204_43604` verify `be70af2a`; `174605` has 16 paired
DLSS-success/history frames, no resets. Original/offline trace reports agree.
User started F9 while loading. Loading 23:43:55–23:44:30: 20 caller reds;
steady 23:44:30–23:46:05: three among 8,494 cycles. ETW cycle N matches log
long-cycle next-wait sequence N+1. Exclude dump/exit from steady comparison.

Loading cycle5600:451.389 ms,449.983 waiting in game SleepConditionVariableSRW.
Steady8385:222.029 ms after-Present, including165.496 ms CreateTexture2D wait
through proxy/D3D11/NVIDIA/VidMM. Nearby allocations~1.03GiB/usage9–10GiB do
not prove budget exhaustion. Steady11170:66.610 ms pre-Present,52.042 running;
nightvision compile46 ms/43 caller compiler samples corroborate a hitch. Other
first-use compiles: resolve157 ms, content22 ms, holo motion122/123 ms.
Checkpoint embeds76 fixed/finite variants with exact
source/flags/macros/profile and stage checks. Arbitrary-float macros and
game-DXBC transforms remain dynamic; driver shader creation remains runtime.

Game3D submit→completion p50/p95/p99/max .835/5.797/7.023/15.431 ms includes
queue delay. No game3D submissions during allocation/compiler waits; system
copy preemption is not attributable to game3D. Ready tails do not support
pervasive scheduler starvation. Individual GPU-query samples were missing.

New ETW completions explicitly join the producer ID retained by a completed CPU
cycle; XR and D3D11 query counters diverge. Native benchmark's existing shared
producer domain is unaffected. Analyzer531/wrapper204 checks reject false
numeric matches; V1 is unavailable. Smoke requires60 synthetic mapped
durations, proving transport only. Default capture uses CPU stacks/existing
queries/matching optimized PDBs; GPU queues optional.

Selector: saved VS71DD/PS2D03 has no texture/screen reads; CB0 projection,
depthOFF, scaled-additive blend, stencil81/ref1/EQUAL/KEEP. This resolves the
parked screen-read uncertainty. Narrow exact-pair crisp admission passes 4,152
WARP and 4,152 RTX fixture checks; live checks remain. Unknown PS stays stock.
Stencil import adds fixture seed work: count it live. Correction: on the native
SteamVR/OpenXR path, P/T/L0 precede final crisp composition in
`native_sharpen.cpp`; L0 is not the final application image.

Exhaust: captured cores have engine motion; identical PSb2 does not explain
blur. Geometry/PSb1/textures and outside-hull glow remain unqualified. User
observed the blur with DLSS off and cancelled capture; leave rendering
unchanged.

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

SteamVR/OpenXR capture must correlate sequence/Present/stacks. Periodic monitor
1 s/journal 500 ms/eager 100 ms/config 1 s/menu 250 ms/GPU polling remain
hypotheses; earlier ETW was externally disabled. Separate keyed-mutex waits
from fast producer copies.

## 2026-09-28: validated checkpoint

`1ff8c224` full/promotion gates pass
(`build/dlss-crisp-storm-{full,promotion}.log`), NGX/FFX paths verified and
optimized PDBs matched. Receipt input
`6c68764e80938c3b1ab1e43458ac2bd977b8aa2a7e18006738b11e21a769e3a2`.
Installed/pushed separately. Graph is `Application wall`. Actual hologram
admission remained separate; no storm resolution was claimed.

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
admission was explicitly requested. Full SDK/profile build
`build/dlss-holo-final-full.log` passes production, all86 pooled rigs, Python
tools and installer resources; receipt input
`665ab62402e1ff2363f8b10b040fa11d7b9d5ef11f7f6aee6830f3f007a7feb1`. Remap927
WARP/RTX each, classifier4157, FinalCrisp624, capture wrapper220; independent
review clears fault/lifecycle issues. See the HUD arc's final entry for
semantic evidence, state guards and capture limits. No whole-frame saving
claimed. Existing probes miss scanner centers(.5861,.5916)/(.5939,.5919) and
cannot identify private HDR; shared94D5 remains unadmitted.

Tight red-hull ROI in16487/88/89/95/502 has82–86% joined engine motion
coverage. ruled out: wholesale missing hull motion in this dump, because most
sampled red hull pixels have engine joins. Join classification does not prove
correct vectors or explain blur. Ship/selector final checks remain; no
sharpening.

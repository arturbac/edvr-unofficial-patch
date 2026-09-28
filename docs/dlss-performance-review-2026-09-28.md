# DLSS performance review — 2026-09-28

## Status

- **State:** Frontier flight `20260928_151543` verifies `0a31d6ba`. HUD was
  visually accepted previously; GPU remains ~9.5 ms. Guarded optimization
  passes the full build; live benefit awaits another flight. Main separate.
- **Finding:** all 36 captured invalidators are stencil-only; all seeds read
  depth/mask00. Four seeds/eye cost .614–.623 ms total; sampled execution ~.123
  ms/seed dominates copy ~.028 ms and CPU recording ~.0047 ms.
- **Correctness:** guarded optimization passes 2,896 WARP/RTX checks with exact
  legacy pixels/depth, four→one seed/eye for captured stencil-only writes.
  Private-write/read-only-view/late replay frames keep legacy invalidation.
- **Graph:** `Submit wall` publishes caller work, a wider boundary than
  0.17.0's CPU benchmark; see below. No graph/configuration key was renamed.
- **Open:** live optimization benefit, matched 0.17.0 A/B, rare actual Submit
  stalls and unattributed outside-Present tails. Clear bypass hits >99.99% of
  steady calls; no direct CPU price proves a hardware gain.
- **Ruled out:** see Exclusions and the engine-motion/terrain arcs.
- **Next flight:** after guarded optimization passes full validation and
  Frontier installation, hold a fixed scene/UI 125 for two full 30 s windows
  off, then two on. Verify installed build first; measure seed count, GPU
  frametime and visual quality. Latest flight starts on and has only 23 s off.
- **Environment:** RTX 5090, Pimax Crystal Super, Pimax OpenXR, 90 Hz;
  2037×1969 input → 4074×3938 output per eye, DLSS Performance/preset K; UI
  quality 125 gives 5093×4923 per eye. Installed DLSS metadata 310.7.0.0; the
  flight does not log its own DLSS/driver versions. Use Frontier for new
  profiling/installations; crisp's Steam log is historical evidence.

## Pre-optimization Frontier evidence

The reviewed logs match `v0.18.0-rc.3-7-g538175a8` (graphics build `6ABA6DF5`),
not HEAD. Through `1d2e60ef`, intervening flat/menu/config/installer changes
leave the reviewed DLSS, hologram and native runtime paths unchanged.

At 12:36:05 the sampled GPU census estimates EDVR **4.949 ms/stereo frame**:
DLSS 3.129, preparation .196, hologram resolve/celestial .269, UI resolve .293,
menu .104, in-frame hologram .301, UI depth .520, engine preparation .044.
Round-robin calls, floor correction and overlapping totals do not form an exact
synchronized budget. Recent temporal windows report median prep .16–.25, NGX
2.66–3.23, UI resolve .27–.44 ms/stereo pair; latest p95 .46/3.99/.62. Temporal
totals exclude some new hologram work.

### Red bars and actual submit work

The graph's `Submit wall` label is misleading: ABI-v5
`NativePerfHistory::cpuFigure` publishes preceding `callerWorkMs`, from pose
wait return to next pose wait entry. It includes rendering, both submits and
post-submit work; excludes the next pose wait. 0.17.0/current benchmark CPU
rows use pre-submit `applicationMs`. At 90 Hz the graph turns orange above 1.02
periods (~11.33 ms) and red above two (~22.22 ms).

Older flight: 340 cycles >two periods, 273 capped rows; 172 caller-work red
rows. Owner-body submit p50/p95/p99 .423/.592/.738 ms does not measure caller
roundtrips. 12,625 overlaps, zero synchronous fallbacks/failures. Seq12363
post188.371/caller submits.368 ms; seq9551 pre66.676 ms; seq9444 first caller
submit24.342 ms. Outside-Present window max54.665 remains unattributed;
marginal statistics are neither additive nor sequence attribution.

## Source review against 0.17.0

DLSS retains colour copy → MV/depth/mask dispatch → NGX → UI resolve/copy;
separate-device OpenXR transfer lies outside these timers. Added engine shader
substitution/private resources, hologram reissues/resolve and diagnostics cost
work; retiring UI separation/deferred replay removes work. UI resolve/corona
hold/NGX timing predate 0.17.0; flat work is profile gated. NGX setup queries
remain size/preset gated; polling uses DONOTFLUSH. Historical luma full copies
could fall back to blocking Map despite a 16×16 grid; logs prove probe rounds,
not blocking. Explicit desk probes remain synchronous.

## Optimizations delivered in 0229c358

**Hologram diagnostics (`91cf805b`):** gate queries/readback with
`advanced.temporal_aa_diagnostics`, cap 512 results; rendering equivalence and
live toggles pass 2,381 WARP checks.

**Luminance/private scatter (`a001f669`):** exact grid pixels from 16 compact
rows: 288,781,416→1,303,680 logical bytes/stereo round (99.55% less), six→96
copies/2 s. WARP extra submission ~.034 ms/round is not hardware gain. Maps
stay nonblocking; timeout/drop/retry after 30 failures (2,759 checks). Cached
private UAVs: four scatters/one creation, alternating eight/two (1,729 checks).
Steady flight has no nonempty scatters, so factory gain is conditional.

**Crisp combination (`0229c358`, parents `a001f669`/`472ff122`):** combine
`kimi/crisp-hud-census` without merging main. Accepted HDR draws already skip
UI-depth, generic hologram and screen-motion reissues. Keep `ui_depth.cpp`: its
classifier/eye table, smoke, reactive/edit masks, fallback UI, FSS and
planet/solar motion remain necessary. `fix.ui_depth` is already a retired key.
Eight generic hologram families are admitted; sphere, corona and world reticle
stay in-scene for resource/depth safety. See the crisp HUD design arc.

Cached coverage keeps `ExecuteCommandList(TRUE)`, dynamic HDR pixels and
state/lifecycle/failure equivalence: four executions/one recording, alternating
eight/two (768 checks). HDR draw timing directly gates on diagnostics;
machinery/moved totals reject incomplete eyes (1,183 checks). Moved shading is
not net overhead. Seed WARP/RTX pass 385,610 checks each; fallback needs clear.

Full builds passed respectively 83, 84 and 85 jobs plus quiet gates, including
config, exports, test rigs and self-contained installer. Combined receipt
fingerprint:
`b64e23a47da6c21724ccba6bf2c1cbdc7bd3b2db429b6f76d8af281d5e90172f`. Clean DLL
promotion, sanctioned Frontier install/verify and branch push passed; live INI
and DLSS runtime preserved. No main merge.

## 2026-09-28: combined Frontier flight and next changes

Verified graphics `edvr_gfx_20260928_141820.log` (build `6ABACAED`, 1,725
lines) and runtime `edvr_openxr_20260928_141822_298_35576.log` (509 lines) both
match `v0.18.0-rc.3-54-g0229c358`. Graphics MDT is runtime UTC minus six hours.
Run 20:18:22.299–20:21:05.807 UTC, 12,939 waits/12,937 pairs. Diagnostics
toggle 14:20:26.146 local. User says HUD looked good, empty-space GPU still
~9.5 ms.

14:20:21 full OFF report: per-eye seed sum.612/.841, tonemap.098/.100,
coverage.055/.055, write-back.045/.051, machinery.962/1.197 ms median/p95.
14:20:51 (~5 s off/~25 s on): machinery.949/1.169, moved.126/.137,
machinery-plus-moved1.081/1.290 ms. All4,524 armed eyes complete; off has zero
moved intervals. 21,488 seeds/2,686 stereo frames=four/eye; stale16,116=three.
No seed failures/tonemap misses/declines; memory1195.4 MB.
Redirected46.54/frame and24 write-backs/frame; changing HUD counts prevent
exact scene A/B.

Benchmark GPU p50 off/on **9.411/9.466 ms** differs in duration/scope; .055 ms
is not a demonstrated diagnostic penalty. General census off application 9.395,
upscaler 3.332, hologram .078, UI depth .041 ms/stereo. HDR seed precedes the
general primary-draw timer, so ~1.224 ms/stereo is excluded from census and
inside its 4.653 ms apparent game remainder (mixed window: 4.579 ms). General
GPU census `FrameUiLayerReissues` includes tonemap reissue and coverage; crisp
route timers price those separately. Never add census figures to route totals,
or sampled seed subprices to the parent HDR seed interval. Sparse draw-hook
means .552/.422/.406 ms from different windows exclude forwarded game draws and
are not a matched benchmark. No luminance timeout occurred; live coverage
factory rate is unmeasured.

Historical Steam `472ff122`, UI 100: seed .431/.842, machinery .701/1.140
ms/eye. UI 125 has 1.621× pixels; this is not a controlled regression test.

**Seed discriminator delivered in 0a31d6ba:** tested draws ~30.54/stereo:
panels 22, HUD 8.54. Depth function 7 is GEQUAL, not ALWAYS (8); writes off.
Panels stencil ALWAYS/read00/write04. A gated census retains 16 prioritized
keys and explicit overflow counts, plus one 64-event complete seed-to-door
timeline/eye/30 s. Totals count matched eye/layer observations, not unique game
commands. GPU pairs sample every 32nd frame using four timestamps in the
existing clock scope: 32 slots, 16 classes, 512 pairs/statistic. Copy and
execution are separately priced; CPU recording lies between them. Paired-only
results expose loss/expiry/reset/clock health and do not add to parent prices.
Idle polling avoids clock/owner/ring work. WARP/RTX UI fixtures pass 626
checks; timing passes 2,798, including a real partial-begin SEH failure/retry.

**CPU observer (`0a31d6ba`):** skip clear traversal under original lock only
when source claims/detached plans both empty. Preserve GPU certificates/native
callback/active-owner walks. 1,766 production differential checks cover reuse,
nesting, late claims and failed unwind. 256 empty clears:4,096→zero heap
requests,256→zero nodes. Empty merges retain2,816 requests/256 nodes; bypass
unsafe. Live counters measure hits, not hardware cost.

**Runtime:** owner-body submit p50/p95/p99 .3584/.4581/.5362 ms; 12,937
overlaps, zero sync/failures. 47 logged caller-work red rows: post 37, pre
eight, submits two. Steady off seq6596 first caller/owner submit 14.584/14.564
ms; nearby acquire max14.2896 is consistent, not unique attribution. On
seq12483 caller24.408 contains post21.027/submits.356 ms. Largest post435.832
coincides with outside-Present max435.424, EDVR-after2.617 and raw.320;
unattributed wall. Producer copies ~.039 ms; startup14–16 ms maxima and
shutdown188.535 ms cycle excluded. Marginal window statistics are not additive;
capped rows cannot establish cadence.

**0a31d6ba validation:** full 85 jobs plus four quiet gates pass, including
264-key contract, exports/installer, UI 629, observer 1,766 and timing 2,798
checks. Focused RTX checks, clean DLL promotion, Frontier install/verify and
branch push pass; live INI/DLSS preserved. Receipt fingerprint:
`9c94e20a289bf19f430f94b99fc49055f71b919e383e82625b65d1cf7214c944`. Freshness
unchanged in this delivery.

## 2026-09-28: verified probe flight and guarded seed optimization

Graphics `edvr_gfx_20260928_151543.log` and runtime
`edvr_openxr_20260928_151544_659_18972.log` verify rc3-55/`0a31d6ba`, graphics
build `6ABAD7B5`. Runtime 21:15:44.660–21:20:20.617 UTC: 23,969 waits/23,846
pairs. Diagnostics start ON, switch OFF 21:19:51.820; no full OFF window.

All 36 captured invalidators across 12 complete eye timelines are forwarded
nonzero indexed-instanced draws, depth GEQUAL/write0, stencil read00/write04,
REPLACE on pass. Shader PS hashes AFED1D4B087E18A9, 66B08F89E4926C01,
A4D03619D631B186. Each timeline has new-frame seed then three stale-draw seeds,
all depth1/mask00/one pass. Sequence 18002: seed0, writer5/reseed13,
writer14/reseed30, writer31/reseed32. Aggregate key overflow prevents an
all-flight claim; complete timelines have zero overflow. No clears observed.

3,360 selected GPU pairs complete with zero health losses; new-frame:stale
ratio 1:3. Typical per-seed medians: copy ~.028 ms, execution ~.123 ms, CPU
record ~.0047 ms. Parent per-eye seed sums .614–.623 ms and machinery .936–.963
ms (ON). Parent timer no-free losses 916–2,906/window exclude incomplete eyes;
samples are qualified. A full 30 s ON benchmark gives GPU 9.432/10.102/11.286
ms median/p95/p99 (2,698 valid). General application/EDVR census 9.842/5.073 ms
in a later ON window overlaps these prices.

Hypothesis: stencil-only writes invalidate depth-only seeds unnecessarily. GPU
fixture first proves those writers leave game depth bytes exact, then tests
preservation against legacy reseeding. Bare bypass fails when a prior private
depth write/raw replay changes fine-grid occlusion: later GEQUAL consumer
pixels differ. Final guard uses raw depth-enabled/write-ALL potential from the
already-read state, including original read-only views: their writable private
binding otherwise defeats an effect-only marker. A monotone watermark covers
both eyes/layers and delayed replay/SEH before sequence publication; older
caches stay guarded, newer frames recover, shutdown resets. No admission or
render behavior changes. Clears, depth writers, mask/source/size growth and
specified-ref behavior retain legacy invalidation.

WARP/RTX each pass 2,896 checks. Actual copy/seed draw/stencil clear commands
fall four→one/eye with exact consumer colour/depth/claimed stencil bits and
unchanged three game-writer commands. Private-write, read-only and shared
source cases retain legacy commands/pixels; mask growth reads current source.
Production objects compile without new warnings. Summary counts matched
preserved layer/eye writers and conservative first-issue attempts (may retry),
not avoided seeds or unique draws. Live frametime benefit remains unmeasured.

Full validation passes 85 jobs plus four quiet gates, UI 2,899, the 264-key
contract, production exports and self-contained installer. Receipt fingerprint
`c8559180f0adfbff0fe89dfb8d099b4bebc7d514a1ac9deea64bf63284915631`; log
`build/dlss-depth-seed-preservation-full.log`. Promote the same committed
source with `--dll-only`, then sanctioned Frontier install/verify with live
INI/DLSS hashes preserved. Push only `codex/dlss-performance-review`.

Clear bypass skips 99.9937–99.9957% of steady calls, preserving active-owner
walks and all merge walks; observer failures 0. Sparse hook mean .518–.850 ms
does not price native clears. Allocation 1195.4 MB stable; seed failures 0.

58 logged red caller cycles: 47 post, eight pre, three Submit. Stable ON has
five reds: two pre, three post 51.255/27.877/80.045 ms with owner Submit
.486/.641/.472 ms. Largest stable post window: outside-Present 79.868, raw
Present .276, EDVR-after 3.709 ms maxima; wall time remains unattributed.
Steady owner Submit medians .367–.398 ms and producer copies ~.039 ms. The 23 s
OFF interval has no logged long rows; exit outliers start 21:20:15.226. Neither
this short interval nor irregular capped red rows establish diagnostic cost or
a fixed cadence. Preserve the before/after boundary distinction.

## Exclusions and next flight

- ruled out: persistent submit blocking as principal cause of logged large
  cycles, because owner-body p99 <.74 ms and caller rows mostly outside
  submits. Rare acquire/submit stalls remain separate candidates.
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

Validate guarded seed preservation at UI 125. After warmup hold scene, display
rate, dimensions, preset/runtime; two 30 s off windows then two on, exclude
toggle compilation. Separately compare UI 100/125 and matched 0.17.0 before
claiming regression/hardware gain. If red bars persist, correlate sequence with
capture/reload/transition/Present and CPU/CSwitch stacks: monitor 1 s,
journal/Status 500 ms (eager 100 ms, enumeration 4 s), config/liveness 1 s,
menu upload 250 ms and GPU-drain polling are hypotheses, not cadence proof. ETW
previously registered but externally disabled. Keep rare keyed-mutex waits
separate from fast producer copies; preserve ownership/binding checks.

# DLSS performance review — 2026-09-28

## Status

- **State:** review since `v0.17.0` (`e72c17c8`). Frontier flight
  `20260928_123334` matches `538175a8`, not HEAD. Rounds 1–2 validate census
  gating, compact luminance and cached scatter views. Crisp HUD `472ff122` is
  now combined on the optimization branch; coverage and gated HDR pricing pass
  focused tests, independent review and all 85 full-build jobs. Main stays
  separate; no optimized Frontier flight exists yet.
- **Finding:** DLSS costs about 2.7–3.2 ms/stereo pair. Most logged long cycles
  are dominated by work before or after submit; ordinary submit p99 is 0.738
  ms.
- **Graph:** `Submit wall` uses the wider caller-work boundary than 0.17.0; see
  the red-bar section. No graph or configuration key was renamed.
- **Open:** matched 0.17.0 A/B, rare actual submit stalls, Present-tail spikes,
  crisp moved-shading cost and diagnostic observer overhead.
- **Ruled out:** see Exclusions and the engine-motion/terrain arcs below.
- **Next flight:** Frontier, fixed scene; UI quality 100 and 125 separately,
  diagnostics off then on. Verify build, preserve runtime/preset/dimensions and
  compare caller-cycle tails, real submits, post-submit phases and GPU prices.
- **Environment:** RTX 5090, Pimax Crystal Super, Pimax OpenXR, 90 Hz;
  2037×1969 input → 4074×3938 output per eye; DLSS preset K. Installed DLL
  metadata is 310.7.0.0; the flight did not log the runtime's own version or
  driver version. New profiling/installations use Frontier; crisp's Steam log
  is historical evidence.

## What the current Frontier flight establishes

Both logs match `v0.18.0-rc.3-7-g538175a8` (graphics build `6ABA6DF5`), not
HEAD. Through `1d2e60ef`, intervening flat/menu/config/installer changes leave
the reviewed DLSS, hologram and native runtime paths unchanged.

The latest sampled GPU census window, at 12:36:05, estimates EDVR at **4.949 ms
per frame**, comprising door work of 4.083 ms and in-frame work of 0.866 ms.
The round-robin census samples calls and subtracts a timer floor; overlapping
totals are not additive or a synchronized per-frame regression measurement.

| Measured component | ms per stereo frame | Interpretation |
| --- | ---: | --- |
| DLSS evaluation | 3.129 | Largest EDVR item in this window |
| Motion preparation | 0.196 | Input colour, depth, vectors and masks |
| Hologram resolve plus celestial | 0.269 | New hologram work is outside the older temporal-total timer |
| UI resolve | 0.293 | Full output pass, predates 0.17.0 |
| Menu | 0.104 | Monitor/menu display also costs GPU work |
| In-frame hologram passes | 0.301 | Contribution and element-depth reissues |
| UI depth coverage | 0.520 | In-frame reissues; separate from final UI resolve |
| Engine velocity preparation | 0.044 | Does not include every game-draw shader substitution or temporal consumer |

Recent 600-pair temporal-price windows report median preparation **0.16–0.25
ms**, NGX **2.66–3.23 ms**, and UI resolve **0.27–0.44 ms** per pair. The
latest corresponding p95 values are 0.46, 3.99 and 0.62 ms. Earlier EDVR 4.387
ms came from a different scene. The temporal timer excludes some features.

### Red bars and actual submit work

The native graph's `Submit wall` label is misleading: ABI-v5
`NativePerfHistory::cpuFigure` publishes the preceding cycle's `callerWorkMs`,
from pose-wait return to next pose-wait entry. It includes rendering, both
submits and post-submit work, excluding the next pose wait. 0.17.0 and current
benchmark CPU rows use pre-submit `applicationMs`, a different boundary.

The graph marks values above 1.02 display periods orange and above two periods
red: about 11.33 and 22.22 ms at 90 Hz.

The flight counted **340** cycles over two predicted periods; **273** were
logged because the long-cycle log is rate limited. Their dominant phases were:

| Dominant phase of logged long cycle | Count |
| --- | ---: |
| After second submit, before next pose wait | 127 |
| Before first submit | 61 |
| Following pose wait | 66 |
| Combined submit roundtrips | 19 |

172 logged cycles have caller work over 22.22 ms. Pose-wait dominated cycles
need not be red graph samples. Rate limiting prevents a full histogram or
strict periodicity claim.

Ordinary combined submit roundtrips in the latest window are **p50/p95/p99
0.423/0.592/0.738 ms**. The native frame-end overlap summary has **12,625
overlapped frames, zero synchronous fallbacks and zero failures**. These
numbers rule out persistent submit blocking or broken overlap as this flight's
dominant problem, while leaving occasional submit stalls open.

Concrete discriminators from `native_frame_cycle_long`:

- Sequence 12363: 199.453 ms cycle, 188.371 ms after submit, combined submits
  0.368 ms. A nearby transition-flash entry is a correlation, not a
  demonstrated cause.
- Sequence 9551: 75.537 ms cycle, 66.676 ms before the first submit.
- Sequence 9444: a real first-submit stall of 24.342 ms. This is a separate
  tail worth retaining, even though it is not the ordinary case.

The existing `native_post_submit_phase` report further splits the tail. One
window has a post-submit gap p50/p95/p99/max of **2.786/5.250/15.131/60.585
ms**. Raw DXGI Present is **0.057/0.134/0.299/0.574 ms**; EDVR after raw
Present is **0.088/0.512/1.044/56.491 ms**; work outside Present is
**2.526/4.809/13.083/54.665 ms**. These are marginal percentiles/maxima across
a window, not additive or attribution of sequence 12363. Both outside-Present
work and EDVR's Present tail need spike attribution.

## Source review against 0.17.0

Full-frame DLSS runs colour copy → input-sized MV/depth/mask dispatch → NGX →
output UI resolve/copy (`temporal_pass.cpp:4786`, `:4980`, `:5131`). OpenXR's
separate-device transfer/compose is outside these game-device timings.

| Since 0.17.0 | Cost and qualification |
| --- | --- |
| Engine-record motion and self-marked shader channels | Game-pass substitution, private slot clears, pool/constants snapshots, append refreshes and temporal consumption. Correct moving geometry is required; optimize demonstrated redundancy, not the vectors themselves. |
| Hologram depth | Two in-frame reissues for qualified draws, a near-light compute pass and final depth resolve. GPU pixel-count queries/readbacks are diagnostic work, separate from rendering. |
| GPU census and prep copy/MV sub-timers | More timestamp commands and registered timers. Cost must be measured; nonblocking polling does not make markers free. |
| Flat temporal/camera features | Profile gated; no evidence that their rendering runs in this native VR flight. |
| UI separation and deferred replay retired | Removes some copies and GPU work. Feature growth is not uniformly additive. |
| UI resolve/corona hold, temporal total timing and NGX timing | Already present at 0.17.0; may still be expensive, but are not new costs. |

Motion diagnostic searches compile out; previous depth swaps textures; NGX
creation/mode queries are size/preset gated. The broader compute save preserves
engine-motion state and must remain.

Temporal/NGX has no explicit Flush/spinning query wait; timestamps poll with
DONOTFLUSH. Before round 2, automatic luminance readbacks switched Map to
blocking after 30 failed polls and copied full textures every two seconds,
although only a 16×16 grid was read (~288 MB/stereo round here). The log proves
rounds ran, not that blocking occurred. This probe predates 0.17.0; explicit
NGX desk probes also read synchronously.

## Optimization with a direct proof of redundant work

Hologram pixel counts feed only the log. Previously each resolved eye/marker
attempted queries even after 512 retained results; every sixteenth frame also
copied/mapped/scanned near-light staging. This is new since 0.17.0. Round 1
gates it behind `advanced.temporal_aa_diagnostics`, retaining rendering and
passive counters, explicitly labeling unavailable pixels. WARP verifies
equivalent depth, command removal and live transitions. Hardware benefit and
the red-bar cause remain unproven.

## 2026-09-28: second optimization round and crisp HUD review

**Luminance:** keep all historical 16×16 grid pixels and format bytes, but copy
16 rows into compact staging. At Frontier sizes a stereo round transfers
288,781,416 → 1,303,680 logical bytes (99.55% less), with six → 96 copy
commands once per two seconds. The software-driver submission tradeoff is about
0.034 ms extra per round; it is not a hardware frametime result. All read Maps
remain nonblocking; after 30 unresolved polls, report `unavailable(timeout)`
and retry with fresh staging after the normal throttle. The WARP rig passes
2,759 checks for exact bytes/statistics, formats, dimensions, mip/array and
failure paths.

**Private scatter:** validated Frontier counters show 108 nonempty scatters,
3,262 rows; none added in the last steady 30 s. Cache each eye/source's UAV by
held device/resource identity, keeping binding/descriptor checks and lifecycle
resets. The production rig verifies four same-pool scatters need one creation
instead of four; eight alternating-eye scatters need two. All 1,729 checks
pass. This is conditional factory work, not a demonstrated steady CPU
bottleneck.

**Crisp HUD (`kimi/crisp-hud-census`, `472ff122`):** folding it into this
branch does not require merging main. Its accepted draws already skip UI-depth,
generic hologram and screen-motion reissues. Do not remove `ui_depth.cpp`: the
layer still uses its classifier/eye table; smoke, reactive/edit masks, fallback
UI, FSS and planet/solar motion remain. `fix.ui_depth` is already a retired
key, not an independent current setting.

The latest crisp code admits eight generic hologram families, despite older
Status wording saying eleven. Sphere `5559BD94B6852E83` stays in-scene because
its pixel-position depth loads lose 84% of the WARP image at layer size; corona
`D1281DF454A153AD` shares a shader with the real sun; world reticle
`71DD8B8B09060A81` lacks resource-safety proof. These retain depth treatment.
Ownership-based gating is the safe simplification. UI history/resolve can
potentially be skipped when both current and prior UI/edit evidence are absent,
with transition invalidation and smoke-mask type 3 preserved; this needs proof.

Crisp's journal records 87 → 72 fps at UI quality 125. A newer Steam flight,
`20260928_125942`, validates against `472ff122`: at UI quality 100, 4000×3868
per eye, cockpit route median is 0.692–0.701 ms/eye; seed alone is about 0.432
ms/eye. No unavailable/invalid/late route intervals were reported. The primary
redirected HDR HUD shading is deliberately untimed, so route totals exclude
moved rendering. This is not a controlled crisp-on/off result. Before Frontier
profiling, price those draws and test coverage command-list caching with
production-path output/state equivalence. The stencil-clear candidate has no
local savings: WARP and hardware both use the fallback seed.

**Combined optimization:** cache each eye's coverage command list, retaining
the draw and `ExecuteCommandList(TRUE)`. Production-HLSL WARP passes 768 checks
for dynamic HDR contents, exact pixels, restored state, identities, lifetimes
and failure retry: four executions need one recording, eight alternating-eye
executions need two. Add gated primary HDR draw intervals and separate
`machinery_plus_moved` totals; unarmed/invalid frames are excluded. Diagnostics
off adds no primary-draw timestamps; shading cost is not net overhead. Pricing
stays bounded by the 512-interval route ring, 128 shared leases/scope and 8,192
retained sums/statistic. Missing/invalid/late fields must accompany reported
medians; incomplete eye-frame sums are discarded. Per-slot completeness keeps
losses across queued frames/config toggles; 1,183 shared GPU-timer checks pass.
WARP/RTX seed tests pass 385,610 checks each; the clear remains necessary.

## Remaining candidates, ordered by useful evidence

| Candidate | Evidence to obtain before another optimization |
| --- | --- |
| Post-submit tail | Correlate sequence with Present/capture/reload/transition events. CPU/CSwitch stacks must separate monitor `slowSample` (1 s), journal/Status I/O (500 ms, eager 100 ms; enumeration 4 s), config/liveness checks (1 s), menu upload (250 ms) and GPU-drain polling. Cadence alone does not prove a culprit. |
| Producer synchronization | Keyed-mutex p50/p95/p99/max 0.090/0.252/6.921/14.967 ms in one window; GPU copy p50 0.039 ms. Correlate rare acquire waits with submit stalls before changing ownership/fences. |
| Compact luminance probe | Correlate two-second rounds and nonblocking timeout markers with tails; measure row-copy command submission and readback. Exact samples are proved in the rig. |
| Hologram passes and UI depth | Census separates reissues, final resolve and UI coverage. Compare fixed cockpit scene with pixel census off/on. |
| Private engine scatter CPU | UAV creation is now cached. Price the remaining binding scan (128 SRVs across six stages plus UAVs) before changing safety checks. |
| Engine snapshots | Use snapshot MB/frame and clear/copy/refresh GPU timers. Optimize proven unchanged ranges; preparation census is not total feature cost. |
| Timestamp/census overhead | New copy/MV timers add four timestamp commands per eye; timer sweeps run every 100 ms. Measure sweep/poll CPU time and cadence before changing sampling. |
| DLSS preset/runtime/output area | Hold preset, DLL, dimensions and scene fixed. Quality changes or runtime upgrades are different tests. |
| UI resolve/copies | Price copy/MV/UI and CPU state save/restore separately. Skipping pixels also affects corona history; prove equivalence. |

The GPU census's corrected estimate must not be subtracted from a median
application GPU frame and presented as exact game time: windows, estimators and
substituted game shader costs differ. Keep sampling/floor/pending/invalid
fields when reading it.

## Exclusions and next flight

- ruled out: persistently slow runtime submit as the principal source of this
  flight's logged large cycles, because ordinary submit p99 is 0.738 ms and
  most logged long cycles are dominated by pre-submit or post-submit phases.
- ruled out: loss of frame-end overlap in this flight, because 12,625
  overlapping completions have zero failures and zero synchronous fallbacks.
- ruled out: removing the seed's stencil clear as a local optimization, because
  matching Frontier log reports feature level `0xC000` and `stencil written one
  pass per bit`; WARP and RTX 5090 caps also lack specified-stencil-reference
  support. This fallback needs the clear, and callers already use actual read
  masks. No production seed change is justified.
- ruled out: engine-record fetch as the previously observed 3–8 ms prep spike,
  because equally slow historical windows had no engine views; the removed
  stage-B sphere coverage pass was responsible. See
  `kinematic-motion-injection-2026-09-19.md`, the September 23 prep-cost entry.
- ruled out: terrain residual as the previously measured Frontier cost in its
  own arc; the CPU shadow removed about 306 GPU copies per frame, and the live
  terrain-off comparison was inert. See the terrain investigation before
  reopening it.
- unresolved: an actual overall performance regression from 0.17.0, because
  there is no same-scene, same-size, same-DLSS-runtime flight pair here. The
  graph's wider measurement boundary is a confound, not proof that performance
  is unchanged.

On Frontier, validate the build and hold scene, display rate, dimensions and
DLSS preset/runtime fixed. After warmup, collect two 30-second windows with
diagnostics off, then two on; exclude toggle-time compilation. Price crisp HUD
at UI quality 100 and 125 separately, retaining route health, moved-shading,
phase, census and snapshot lines. Capture CPU/CSwitch stacks if red bars
remain; the previous ETW provider was registered but externally disabled.
Hardware A/B is required for a frametime claim; match CPU boundaries when
comparing 0.17.0.

## Validation and delivery

The production WARP rig passed 2,381 checks: complete depth output equivalence,
actual command observation and pending live transitions. Independent review
found no rendering or lifetime blocker. The absolute-path full build passed all
gates, including 83 jobs, config contract, DLL/export validation and
self-contained installer checks; it wrote `build/full_build_receipt.json`.
Delivery uses clean-commit DLL promotion and the sanctioned Frontier installer,
preserving the live INI and DLSS runtime. Round 2 passes all 84 full-build
jobs, including 2,759 luminance and 1,729 engine-velocity checks. Hardware
improvement remains unflown.

The combined absolute-path full build passes 85 jobs plus four quiet gates,
including coverage (768), GPU timing (1,183) and seed (385,610) checks, config,
exports and self-contained installer validation. Receipt source fingerprint:
`b64e23a47da6c21724ccba6bf2c1cbdc7bd3b2db429b6f76d8af281d5e90172f`. Delivery
follows clean-commit DLL promotion and Frontier install/verification; no main
merge or quality/preset/runtime change is part of this pass.

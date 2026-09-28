# DLSS performance review — 2026-09-28

## Status

- **State:** review of `v0.17.0` (`e72c17c8`, September 17) through `1d2e60ef`.
  Frontier flight `20260928_123334` is validated against its own source
  revision, `538175a8`, not HEAD. Round 1 gates hologram GPU pixel census.
  Round 2 compacts luminance readbacks and caches private-scatter UAVs; focused
  rigs, independent review and all 84 full-build jobs pass. Combining latest
  crisp HUD for Frontier profiling is authorized; main stays separate. Hardware
  benefit is unflown; no new Frontier flight exists yet.
- **Finding:** DLSS is the largest measured EDVR GPU item, about 2.7–3.2 ms per
  stereo pair in the sampled flight. Most logged long caller cycles are
  dominated by game work before the first submit or after the second, rather
  than the runtime submit roundtrips.
- **Graph:** the native CPU graph includes caller work between pose waits,
  including both submit roundtrips and post-submit work. Its wider boundary
  than 0.17.0 and the `Submit wall` label can make a comparison misleading. No
  graph feature or configuration key changed in this review.
- **Open:** no controlled same-scene 0.17.0 comparison; rare true submit
  stalls; EDVR Present-tail spikes; hologram/engine feature costs; timer and
  census observer overhead. Candidates and discriminating evidence below.
- **Ruled out:** see the recorded exclusions below and the linked engine-motion
  and terrain investigations. No shader quality, motion-vector or pacing change
  is justified by these logs alone.
- **Next flight:** Frontier, fixed scene and settings, diagnostics off then on
  in one session. Verify the installed build first; compare caller-cycle tails,
  actual submit roundtrips, post-submit breakdown and existing GPU census. The
  optimization's removed diagnostic commands need a hardware timing comparison;
  image equivalence is checked in the WARP rig.
- **Environment:** RTX 5090, Pimax Crystal Super, Pimax OpenXR, 90 Hz;
  2037×1969 input → 4074×3938 output per eye; DLSS preset K. Installed DLL
  metadata is 310.7.0.0; the flight did not log the runtime's own version or
  driver version. All flight and installation work in this arc uses Frontier.

## What the current Frontier flight establishes

Both logs match `v0.18.0-rc.3-7-g538175a8` (graphics build `6ABA6DF5`), but
fail validation against HEAD. Intervening changes concern flat
camera/menu/config and installer work; the reviewed DLSS, hologram and native
runtime paths are unchanged. Measurements belong to the flown revision.

The latest sampled GPU census window, at 12:36:05, estimates EDVR at **4.949 ms
per frame**, comprising door work of 4.083 ms and in-frame work of 0.866 ms.
The named components below overlap some broader totals and must not be added
twice. This round-robin census estimates mean cost from selected calls,
subtracts a timer floor, and is not a synchronized per-frame profile or a proof
of a regression.

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
latest corresponding p95 values are 0.46, 3.99 and 0.62 ms. An earlier census
window had EDVR at 4.387 ms with fewer hologram/UI costs; the scene changed, so
that is not an A/B comparison. The full-frame price does not alone price all
new features.

### Red bars and actual submit work

The native graph's `Submit wall` label is misleading.
`NativePerfHistory::cpuFigure` uses ABI-v5 `callerWorkMs`: one pose-wait return
to the next pose-wait entry. This includes game rendering, both submits and any
waits inside them, time between eyes, and game/EDVR work after the second
submit. The following pose wait is excluded. The published cycle is the
preceding completed cycle. Older runtimes, including 0.17.0, use pre-submit
application time instead. Benchmark CPU rows still use pre-submit
`applicationMs`; they cannot be compared directly with the new graph.

The graph marks values above 1.02 display periods orange and above two periods
red. At 90 Hz those thresholds are about 11.33 and 22.22 ms. It is not a graph
of the DLSS evaluation or of the two runtime submit calls alone.

The flight counted **340** cycles over two predicted periods; **273** were
logged because the long-cycle log is rate limited. Their dominant phases were:

| Dominant phase of logged long cycle | Count |
| --- | ---: |
| After second submit, before next pose wait | 127 |
| Before first submit | 61 |
| Following pose wait | 66 |
| Combined submit roundtrips | 19 |

Of the logged cycles, 172 also have caller work over 22.22 ms, which is
consistent with a red graph sample. Following-pose-wait dominated cycles are
not automatically red caller-work samples. Rate limiting prevents deriving a
complete spike histogram or a strict periodic cadence from these rows.

Ordinary combined submit roundtrips in the latest window are **p50/p95/p99
0.423/0.592/0.738 ms**. The native frame-end overlap summary has **12,625
overlapped frames, zero synchronous fallbacks and zero failures**. These
numbers argue against persistent blocking submit or broken overlap as the
dominant problem in this flight. They do not rule out occasional submit stalls.

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
a window, not terms to sum and not a per-cycle attribution of sequence 12363.
Ordinary Present is cheap, but both game work outside it and EDVR's
Present-boundary tail deserve spike attribution.

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

Default motion compiles diagnostic counters/searches out. Previous depth swaps
textures. NGX creation/mode queries are size/preset gated. The broader compute
save (23 SRVs, seven UAVs, three CBs) preserves engine-motion state; keep it.

The core temporal/NGX evaluation has no explicit Flush or spinning query wait;
timestamp polls use DONOTFLUSH. There is an exception in the automatically
armed luminance diagnostic: `luma_probe.cpp:362` switches staging Map from
DO_NOT_WAIT to blocking after 30 unsuccessful frames. Every two seconds it
copies the game, DLSS-output and final textures for both eyes at their full
dimensions, although the CPU reads only a 16×16 grid. At this flight's RGBA8
sizes that is about 288 MB of logical image data per round before any duplicate
source/readback effects. The log confirms automatic rounds, but does not say
whether the blocking fallback occurred. This probe predates 0.17.0, so its
presence alone cannot establish a new regression. Explicit NGX desk probes also
have synchronous readbacks. Nonblocking polling still costs commands,
transfers, mapping and CPU work.

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
profiling, price those draws and test two source-proven candidates: repeated
coverage command-list recording and stencil clear before an unconditional
full-target stencil write. Preserve fallback and lifecycle behavior; prove
production-path output and state equivalence headlessly.

## Remaining candidates, ordered by useful evidence

| Candidate | Evidence to obtain before another optimization |
| --- | --- |
| Post-submit tail | Correlate cycle sequence with `native_post_submit_phase` and Present/capture/reload/transition events; collect per-cycle attribution. |
| Producer synchronization | Keyed-mutex p50/p95/p99/max 0.090/0.252/6.921/14.967 ms in one window; GPU copy p50 0.039 ms. Correlate rare acquire waits with submit stalls before changing ownership/fences. |
| Automatic luminance probe | Count forced-blocking fallbacks; price sampling/readback and correlate two-second rounds. Reducing on GPU to 16×16 needs format/sample equivalence proof. |
| Hologram passes and UI depth | Census separates reissues, final resolve and UI coverage. Compare fixed cockpit scene with pixel census off/on. |
| Private engine scatter CPU | `engine_velocity_primary_copy.h:155` scans 128 SRVs across six stages plus UAVs; `:322` creates a UAV each application. Price scope and batches before caching; preserve binding safety and pool lifetime. |
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

On Frontier, validate the build and hold scene, game settings, display rate,
dimensions, DLSS preset/runtime fixed. After warmup, collect two 30-second
windows with diagnostics off, then two with it on; exclude toggle-time shader
compilation. Retain the phase, price, census and snapshot lines named above.
Hardware comparison is required before calling the patch a frametime win. For
0.17.0 comparison, also match the CPU measurement boundary.

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

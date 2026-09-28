# DLSS performance review — 2026-09-28

## Status

- **State:** Frontier flight `20260928_141820` verifies `0229c358`. HUD looks
  good; user reports ~9.5 ms GPU in empty space. Main stays separate. Clear
  observer optimization and joint seed probes pass the full validation gate.
- **Finding:** crisp machinery .962 ms/eye, seed .612; four seeds and three
  stale transitions/eye/frame. Armed moved shading .126 ms/eye. GPU benchmark
  p50 off/on 9.411/9.466 ms has different scopes, not a diagnostic penalty.
- **Graph:** `Submit wall` publishes caller work, a wider boundary than
  0.17.0's CPU benchmark; see below. No graph/configuration key was renamed.
- **Open:** seed invalidator/plane attribution, copy vs seed execution, live
  no-claim observer frequency, matched 0.17.0 A/B and rare real submit stalls.
- **Ruled out:** see Exclusions and the engine-motion/terrain arcs.
- **Next flight:** one Frontier session after joint probes pass; fixed scene,
  UI quality 125, diagnostics off/on. Collect seed causes/subprices and clear
  bypass counts before changing freshness. Flight analyzed: 0229c358; verify
  the next flight against the new branch HEAD.
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

The older flight counted 340 cycles above two periods, logging 273 under a rate
limit. Dominant phases: post-submit 127, pre-submit 61, next pose wait 66,
combined caller submit roundtrips 19. Of these rows, 172 have caller work above
22.22 ms; next-wait-dominated cycles need not turn the graph red. Combined
**owner-body** submit p50/p95/p99 .423/.592/.738 ms is not caller-roundtrip
percentiles. Overlap: 12,625 completions, zero synchronous fallbacks/failures.
Sparse caller rows support outside-submit dominance while retaining real tails:

- Seq 12363: 199.453 ms cycle, 188.371 post-submit, .368 caller submits.
- Seq 9551: 75.537 ms cycle, 66.676 before first submit.
- Seq 9444: first caller submit 24.342 ms; distinct from usual work.

One post-submit window reports gap p50/p95/p99/max 2.786/5.250/15.131/60.585
ms; raw Present .057/.134/.299/.574; EDVR after Present .088/.512/1.044/56.491;
outside Present 2.526/4.809/13.083/54.665. Marginal statistics are not additive
or attribution of seq 12363. Outside-Present wall time remains unattributed,
not proof of pure game CPU work.

## Source review against 0.17.0

DLSS runs colour copy → input-sized MV/depth/mask dispatch → NGX → output UI
resolve/copy. OpenXR separate-device transfer/compose is outside those
same-device timers. New costs include engine shader substitution, private slot
clears/snapshots, hologram reissues/resolve and census/timestamp commands. UI
separation/deferred replay retirement also removes work. UI resolve, corona
hold, NGX/temporal total timing predate 0.17.0. Flat work is profile gated; no
evidence it runs in this native VR flight.

Motion diagnostic searches compile out; previous depth swaps textures; NGX
creation/mode queries are size/preset gated. The wider compute save preserves
engine-motion state. Temporal/NGX timestamp polling uses DONOTFLUSH without an
explicit Flush/spinning wait. Older automatic luminance probes copied full
textures every two seconds and switched Map to blocking after 30 unsuccessful
polls, despite reading only a 16×16 grid. Logs prove rounds ran, not that a
blocking Map occurred. Explicit NGX desk probes also read synchronously.

## Optimizations delivered in 0229c358

**Hologram diagnostics (`91cf805b`):** pixel counts feed only the log. Queries
previously continued after the 512 retained-result cap; near-light staging
copied/mapped/scanned every sixteenth frame. Gate with
`advanced.temporal_aa_diagnostics`, retain rendering/passive counters and label
unavailable pixels. WARP proves depth equivalence, command removal and live
transitions (2,381 checks).

**Luminance/private scatter (`a001f669`):** preserve historical grid pixels and
bytes, copying 16 rows into compact staging. At Frontier sizes, 288,781,416 →
1,303,680 logical bytes/stereo round (99.55% less), six → 96 copy commands
every two seconds. WARP additional submission cost ~.034 ms per round is not a
hardware result. All Maps stay nonblocking; after 30 failed polls report
`unavailable(timeout)`, drop staging and retry at normal throttle. Exact
bytes/statistics/failures pass 2,759 checks. Cache per-eye/source private UAVs
with identity, binding and lifecycle guards; four same-pool scatters need one
creation, eight alternating-eye scatters two (1,729 checks). Historical
Frontier adds no nonempty scatters in its final steady window: conditional
factory removal, not a demonstrated steady bottleneck.

**Crisp combination (`0229c358`, parents `a001f669`/`472ff122`):** combine
`kimi/crisp-hud-census` without merging main. Accepted HDR draws already skip
UI-depth, generic hologram and screen-motion reissues. Keep `ui_depth.cpp`: its
classifier/eye table, smoke, reactive/edit masks, fallback UI, FSS and
planet/solar motion remain necessary. `fix.ui_depth` is already a retired key.
Eight generic hologram families are admitted. Sphere `5559BD94B6852E83` stays
in-scene (pixel-position depth loads lose 84% of WARP pixels at layer size);
corona `D1281DF454A153AD` shares the real sun's shader; world reticle
`71DD8B8B09060A81` lacks resource-safety proof.

Cache each eye's coverage command list, retaining draw and
`ExecuteCommandList(TRUE)`. WARP checks dynamic HDR contents, exact pixels,
state, identity, lifetimes and failure retry: four executions need one
recording, eight alternating-eye executions two (768 checks). Gate primary HDR
draw intervals directly on diagnostics; separate machinery from
`machinery_plus_moved`, discard incomplete eye-frame sums. Route ring bounded:
512 intervals, 128 shared leases/scope, 8,192 retained sums/statistic. Missing,
invalid, late, canceled and toggled sums remain distinguishable (1,183 timer
checks); moved shading is not net overhead. Seed WARP/RTX tests pass 385,610
checks each; hardware uses the fallback requiring stencil clear.

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

14:20:21 report: complete steady off window, 14:19:51–14:20:21. 14:20:51
report: ~5 s off/~25 s on; HDR draw sums only armed eye-frames. Median/p95 of
all intervals summed per eye-frame (HDR seed includes all four seeds):

| Component | Off window (ms) | Mixed window (ms) |
| --- | ---: | ---: |
| HDR seed | .612/.841 | .605/.832 |
| Tonemap | .098/.100 | .099/.100 |
| Coverage | .055/.055 | .055/.055 |
| Write-back | .045/.051 | .042/.046 |
| Machinery | .962/1.197 | .949/1.169 |
| Moved HDR shading | unarmed | .126/.137 |
| Machinery + moved | unarmed | 1.081/1.290 |

Complete totals use 4,524 armed eye-frames. Mixed HDR eligible/disabled/issued
128,323/20,880/107,443; aborted/unavailable/invalid/late all zero. Full off
eligible 119,630, all disabled, zero moved-HDR timing intervals. 21,488
seeds/2,686 stereo frames = four/eye/frame; stale 16,116 = three/eye/frame. No
failures/refusals/missed tonemap/declines. UI allocation stable ~1,195.4 MB.
Redirected draws 46.54/frame (44.54 HDR), 24 write-backs/frame; flight HUD
counts change later, so scenes are not exact matched A/B windows.

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

Historical Steam `472ff122`, UI 100/4000×3868: seed .431/.842, machinery
.701/1.140 ms/eye, ~7.97 seeds/stereo. Current 125 has 1.621× layer pixels;
price growth is consistent with resolution, not proof of cache regression.
Earlier crisp's 87 → 72 fps report is uncontrolled.

**Seed discriminator:** tested draws ~30.54/stereo frame: panels 22, flight HUD
8.54. Logged HUD depth ALWAYS/write off/stencil disabled; panels depth
ALWAYS/write off, stencil ALWAYS/read00/write04. Seeding uses readMask, likely
mask-zero depth-only seeds: one depth pass plus stencil clear, not eight bit
passes. Probe actual mask/pass count before asserting that. Accepted
HDR/private raw reissues cannot cause stale seeds; source-matching game
draws/clears can. Add gated bounded
invalidator/plane/state/shader/verdict/work-count/seed-reason census, one
complete eye-frame timeline per eye per 30 s. Sample seed GPU copy/execution
and CPU recording wall time. Keep freshness/rendering unchanged until these
discriminate required writes, false invalidation and recording delay. Probes
retain 16 aggregate keys, prioritizing actual invalidators/seeds with explicit
overflow counts, plus 64 timeline events per eye; complete seed-to-door needs a
door and no overflow. Event totals count matched eye/layer observations, not
unique game commands. WARP/RTX fixtures pass 626 checks for state/clear/
no-work outcomes and boundaries. Copy/execution GPU pairs sample every 32nd
source frame using four timestamps and the existing frame scope: 32 slots, 16
classes, 512 retained pairs/statistic. Recording wall is CPU time from copy-end
to execute-begin. Pair loss, clock failure, expiry, reset and late completion
stay explicit; partial GPU prices are rejected. WARP timing passes 2,798
checks; idle polls return before owner lookup, clock or ring scan. Parent route
prices stay separate.

**CPU observer proof:** off window 977,857 dictionary clears, zero claims
cleared/failures; mixed 988,236, also zero. Merge plans +106,034/+107,789.
Counters alone do not prove the claim map stayed empty. Source confirms clear
traversal feeds only source claims/detached plans. Under the original lock,
skip only when both empty; preserve copied GPU certificates, native callback
and all active-owner walks. Production differential fixtures pass 1,766 checks:
future claims, reuse, nested merge/clear, late arrivals and failed unwind. For
256 empty-owner clears, 4,096 → zero heap requests, 256 → zero nodes. Empty
merges retain 2,816 requests/256 nodes: naive merge bypass unsafe. New
cumulative skip/node/owner counters measure live hit frequency; no hardware
gain claimed.

**Runtime:** latest owner-body submit p50/p95/p99 .3584/.4581/.5362 ms; 12,937
overlaps, zero sync/failures. 249 cycles >two periods, 129 logged: post 52,
next wait 58, pre 17, submits two. 47 caller-work red rows: post 37, pre eight,
submits two; 42 startup/transitions, one steady off, one on, three shutdown.
Rate limiting prevents exhaustive cadence claims.

- Steady off seq 6596, 20:19:54.739 UTC: caller 48.249 = pre 29.291 + caller
  submits 14.868 + post 4.088 + between-eye .002 ms; next wait 5.935 excluded.
  First caller submit 14.584 vs owner 14.564; nearby acquire max 14.2896 is
  consistent, not unique attribution of that sequence.
- On seq 12483, 20:21:00.251: caller 24.408 = pre 3.024 + submits .356 + post
  21.027 + between-eye .001 ms. No additional long row in the five off seconds
  before toggle.
- Biggest post tail 435.832 ms, seq 3096: window outside-Present max 435.424 vs
  EDVR-after 2.617/raw .320/handoff .789; unattributed wall time.

Mixed post window 20:20:17.877–47.876 gap p50/p95/p99/max
1.959/2.639/4.139/7.845 ms; raw Present .046/.051/.069/.251; EDVR-after
.075/.328/.836/2.740; outside Present 1.796/2.300/3.431/7.685; nested handoff
.281/.377/.587/5.993. Marginal statistics are not additive. Producer GPU copies
~.039 ms, steady maxima <.045, 2,560 eyes across five windows without
invalid/pending drops; 14–16 ms copy maxima are startup. Shutdown seq 12678
excluded from steady conclusions (188.535 ms cycle, 184.119 post).

**Validation of this round:** absolute-path full build passes 85 jobs plus four
quiet gates, config contract (264 keys), production DLL/exports and
self-contained installer resources. UI quality 629, engine observer 1,766 and
GPU timing 2,798 checks pass inside the full gate; focused UI tests also pass
on RTX 5090. Receipt source fingerprint:
`9c94e20a289bf19f430f94b99fc49055f71b919e383e82625b65d1cf7214c944`. Delivery
uses a clean commit, receipt-guarded DLL promotion, sanctioned Frontier
install/verification and branch push; preserve live INI/DLSS runtime. Freshness
remains unchanged pending the next flight's discriminators.

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
- ruled out: engine fetch as historical 3–8 ms prep spike; equally slow windows
  had no engine views. Removed sphere coverage caused it; see September 23 in
  [engine motion](kinematic-motion-injection-2026-09-19.md).
- ruled out: terrain residual in its Frontier arc; CPU shadow removed ~306
  copies/frame, live terrain-off was inert. Read that arc before reopening it.
- unresolved: regression versus 0.17.0. No matched scene/size/DLSS flight pair;
  graph boundary change is a confound, not proof performance is unchanged.

Prioritize seed causes/subprices and clear hits at UI 125. After warmup hold
scene, display rate, dimensions, preset/runtime; two 30 s off windows then two
on, exclude toggle compilation. Separately compare UI 100/125 and matched
0.17.0 before claiming regression/hardware gain. If red bars persist, correlate
sequence with capture/reload/transition/Present and CPU/CSwitch stacks: monitor
1 s, journal/Status 500 ms (eager 100 ms, enumeration 4 s), config/liveness 1
s, menu upload 250 ms and GPU-drain polling are hypotheses, not cadence proof.
ETW previously registered but externally disabled. Keep rare keyed-mutex waits
separate from fast producer copies; preserve ownership/binding checks.

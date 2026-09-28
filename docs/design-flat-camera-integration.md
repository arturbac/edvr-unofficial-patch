# Flat temporal AA through upstream camera construction

## Status

- State: producer discovery landed. The C1 producer probe named the
  camera-row writer (FUN_140596830, trap RIP `0x596A54`) and its refresh
  chain (FUN_1405921f0 <- FUN_140594d90 <- FUN_14058ef90); the C1/C2
  addendum records the verified findings with the 2026-09-28 review's
  corrections. C1 remains partial (cache helpers FUN_1404f49f0 /
  FUN_1404f2ff0 / FUN_1404f4910 undecompiled; consumer lineage unjoined;
  auxiliary composer callers unclassified) and C2 is not demonstrated —
  do not turn camera mutation on from this evidence.
- Decision: investigate jittering the game's per-view camera construction
  before it derives raster/lighting data. Preserve the existing frame
  discovery, size negotiation, temporal backends and resource isolation.
- Motivation: the build-verified 20260928_025912 user log reports zero renderer
  calls; three recurring unknown projection pairs keep every frame in
  observation. Sean reproduced non-activation with another ship; its exact
  shader correspondence remains unverified.
- Open hypotheses: a common producer covers the derived transforms; camera
  domains and recording epochs can be distinguished before mutation. The
  camera-struct hook point is a candidate, not a validated design.
- Ruled out for that session: a backend evaluation failure as the immediate
  cause, because no backend initialized. Missing shader coverage is proven;
  whether quality settings or a mod produced those variants is not.
- Next: decompile the three cache helpers and their dirty-flag rules;
  trace camera/generation through derived data, CB writes/binds and
  handoff to establish consumer lineage; then C2 reducer and WARP
  geometry/lighting tests. The producer probe itself was rebuilt per the
  review's R1-R6 (gate-only stand-down, per-thread watch ownership,
  external-thread arm/disarm, mandatory handler registration,
  module-naming fallbacks, bounded witness collection).
- Environment: Windows x64, D3D11 flat mono; headset/runtime N/A. VR
  comparison: EDVR's OpenVR/OpenXR route. Record GPU/driver, executable/build
  identities, backend versions, dimensions, formats and mod chain for
  qualification.
- This document extends the [flat AA
  design](design-flat-temporal-aa-2026-09-23.md) and [architecture
  review](review-flat-temporal-aa-2026-09-26.md). Its camera milestones below
  supplement, rather than renumber, their existing gates.

## 1. Problem and acceptance requirements

VR supplies Elite's eye projection through a runtime interface. EDVR applies
jitter there and Elite derives rendering data from that camera. See
`native_temporal.cpp`'s tangent-shift publication and
`openxr/openvr_system.cpp::GetProjectionMatrix/GetProjectionRaw`.

Flat's current adapter instead discovers a scene through D3D11 observations and
patches private constant buffers at draw/dispatch boundaries. It must recognize
each relevant projection consumer. At `418e5231`, unknown recipes call
`refuseDraw`; observation ends only after a completely covered frame. One
unfamiliar pair every frame therefore prevents all temporal treatment.

The user log shows 704/704 frames refused, `calls=0 init=0` and `partial=on
observing=1`. Retrieve its already-saved six stage bytecode files and
`flat_trace_13562.bin`, omitted by the support ZIP, before another flight.
Captures must not become a permanent requirement to authorize each ship.

The replacement must satisfy:

1. Support device/backend resolutions, including odd extents, crops and
   supersampling, using actual dimensions without hidden quality reduction.
2. Reconfigure and resume after supported quality/size changes without a
   restart. Indefinite warming is a failed qualification.
3. Support compatible EDHM/ReShade chains; name incompatible contracts.
4. Admit ships/materials through camera lineage, without per-ship allowlists.
5. Preserve disabled behavior and VR scheduling. Object-motion coverage is
   separate: camera-only motion may limit quality while valid AA continues.

This change does not automatically solve scene/depth discovery, DoF/HDR
handoffs, UI composition or moving-object motion. Those contracts remain.

## 2. Target boundary and ownership

Proposed flow; the upstream producer box is not yet located:

```mermaid
flowchart LR
  A[Original per-view camera] --> B[Qualified jitter at camera construction]
  P[Prepared render plan] --> B
  B --> C[Game derives forward and inverse transforms]
  C --> D[Scene geometry and dependent lighting]
  D --> E[Existing frame reducer and validated handoff]
  E --> F[Temporal resolve and final scaling]
  F --> G[Qualified UI / postprocessing boundary]
```

Prefer a per-view finalization boundary before projection-dependent matrices,
view rays and screen-to-world constants are derived. Change raster projection,
not world pose or simulation. Preserve the original camera and let the game
produce coherent derivatives from the modified input.

There must be one jitter owner for each scene phase group. A group contains all
passes sharing the scene's sampling grid/depth: it may include a main camera
plus cockpit or weapon cameras with different near planes. They must receive
compatible pixel offsets, not necessarily identical matrices.

Shadow, reflection, cube-map and UI cameras are not main-camera candidates
merely because their matrices or constant-buffer addresses look similar. Leave
independent domains unchanged. A secondary camera that contributes to the
shared scene must be explicitly related to its phase group or block that group.
Composite-only UI can remain outside it at a validated boundary.

Keep CPU visibility/culling stable and conservative over the jitter envelope.
If the candidate also builds culling data, prove how stable/conservative
culling coexists with jittered raster data; do not accept phase-dependent
visibility popping as AA. Preserve reversed-Z, depth range, FOV, asymmetric
frusta, viewport/crop and matrix storage conventions.

Unknown material hashes cease to be an admission barrier only for work whose
camera derivation is covered by this contract. A different shader hash is not
itself a violation; a new camera source or rewritten projection is.

## 3. Find the producer without assuming one exists

Existing anchors identify consumers, not a canonical constructor:

- `flat_temporal_model.h` observes scene b1[270..275]; `flat_runtime.cpp` and
  `vscreen.cpp` observe CPU writes before Unmap.
- `flat_camera_probe.h` samples an exact-pair conflict; `camera_view.h` reads
  external-camera mode. Neither provides an upstream projection API.
- Kinematic records are object poses. [Camera-row carry
  evidence](camera-rows-carry-2026-09-25.md) shows why latest/plausible rows
  cannot distinguish auxiliary views. The VR row layout is not a flat camera
  ABI.

Walk measured upload callers backward to their source objects and derivation
order. Compare these hypotheses in the same investigation:

| Candidate | Evidence required | Disqualifying result |
| --- | --- | --- |
| Per-view camera finalizer | Stable view identity; proposed mutation point precedes all relevant derived reads/writes | Some derivatives escape that point, or identity is ambiguous |
| Scene-constant packer | Proves every dependent product is built/rebuilt here | Merely copies already-derived matrices; other packs escape |
| Render-context/view-table builder | Joins camera domain and generation to subsequent scene traversal/uploads | It identifies only culling/auxiliary views or runs after consumers |

Record executable fingerprint, caller locations, source object/generation,
matrix values, destination range/write epoch, thread/context, view/pass and
downstream lineage. Frequency alone cannot select a producer. Do not invent
RVAs, calling conventions or a global camera pointer.

Before patching, validate the unique locator, instruction boundaries, ABI,
forwarding and lifetime against the executable and structural/callsite/store
checks. Unknown builds, ambiguous matches and conflicting patches disable the
integration. Reuse the existing hook mechanisms after this proof.

## 4. Passive evidence and bounded probes

First replay existing data and inspect exact bytecode. Then instrument all
plausible causes together. Sample still view, a deliberate pan, main/secondary
camera transitions and one scale change in a single prepared session.

Join producer entry/mutation/return, derived writes/uploads, bindings,
scene/depth writes and handoff by view, generation and execution order. Pointer
equality or Present alone is insufficient. Distinguish command-list recording
from replay, including repeated execution.

Initial capture limits: 240 metadata/small-matrix frames per segment, two
full-payload frames, four segments, 4,096 events/frame and 64 MiB total. These
are diagnostic budgets, not lifetime admission limits. Log caps, drops and
incomplete joins; overflow invalidates proof. Adjust from measured counts.

Discriminators: shared upstream sources support a common finalizer; derivatives
preceding the proposed mutation point refute that point; auxiliary views refute
a global-camera model; record/replay mismatches refute immediate-only lifetime.
Derivation inside the original constructor is valid when injection precedes it,
for example at its entry.

Emit armed/hook-hit/producer/join/overflow/completion counts, including zero.
Persist replayable evidence; include referenced trace/shaders and truncation
reasons in the support bundle. A silent hook is not a successful probe.

## 5. Proposed camera contract and early preparation

The existing `FlatFrameContract` is produced at output-copy time. It cannot
authorize an earlier camera mutation. Add a separate immutable early plan and
compare it with the completed frame contract; do not create a second scene
selector. The following are proposed records, not current APIs:

| Record | Required contents |
| --- | --- |
| Early render plan | Game/hook/device generations; view and phase-group identity; input/output resource generations and descriptors; R/E/D extents; crop/viewport mappings; backend/model; prepared-resource lease; expected derivation epoch |
| Camera application | Original camera snapshot; actual jitter in render pixels; derived-producer identities; view/recording/execution epochs; one-shot application result; reason on decline |
| Frame closure | Join to actual color/depth/handoff; all relevant camera derivations accounted for; actual sizes and phase; temporal accepted or recovery result; history commit/reset reason |

R is scene rendering, E backend evaluation, D display output. Convert jitter
using R and the viewport, never D/E. Preserve current negotiation: trained
backends normally use E=D for upscaling and E=R for supersampling, followed by
final scaling to D, subject to backend limits. Current TAA evaluates and stores
history at D; render-grid TAA is a separate deferred change. Include negotiated
E, formats, subresources and mappings in resource/history identity.

Bootstrap from zero-jitter observation and the reducer's scene/depth/handoff
selection, even while legacy shader coverage refuses. Upstream admission needs
separately counted producer/derivation proof; legacy refusal must not reset its
warm-up or history. Existing source selection also uses pool-family and
camera-row assumptions: extend it to proven producer/target association where
needed, never bypass scene selection wholesale.

On the render owner, prepare for the next eligible construction. Revalidate
identity, generations, sizes and readiness before mutation. A previous frame is
a prediction, not authority. Changed/unprepared plans remain stock.

Worker hooks must not call D3D, initialize backends or wait on GPUs. Publish
owned immutable records through a bounded thread-safe channel; overflow is a
named refusal. Never retain mapped/stack pointers. Preserve nested caller
attribution; quiesce in-flight users before reclamation without lock deadlock.

Apply jitter to an original/private camera copy with proven consumer lifetime.
Repeated construction uses the same phase without accumulating offsets.
Deferred consumers retain their generation through execution. Separate applied
phase from accepted history; failures cannot replay old evidence as fresh.

Publish both original and rendered camera records. Feed certified unjittered
rows to `FlatMonoResolveFrame.camera/previousCamera` and original scene
snapshots to engine motion's `sceneNow/scenePrev`, with actual raster jitter
carried separately. Upstream-modified uploads cannot silently become those raw
inputs. Prove provenance rather than guessing de-jitter transforms for unknown
layouts. Test normal, disabled, failed and history-reset paths for double
correction and preservation of raw motion inputs.

## 6. Transaction, transitions and recovery

Normal states are Observing -> Prepared -> Active. A violated contract moves to
Recovery and subsequent zero-jitter observation. Each state has a reason,
current generations and counters; stable supported scenes must progress.

Before first consumption, failure means no camera mutation. After camera data
is consumed, do not switch phase, turn on legacy per-draw patches or pretend
restoring a CPU matrix undoes drawn geometry.

At handoff compare resources, derivation identity, sizes and phase with the
plan. Only matching closure authorizes temporal history. Reject camera cuts,
missing/late writers, overrides and replay mismatches.

`flatMonoResolvePreflight` proves renderer/fallback resources and backend
availability, not size-specific vendor feature creation. Late backend failure
can use preallocated spatial recovery only for *proven coherent* jitter and
valid inputs, leaving history invalid. Unknown/mixed phase instead declines
EDVR replacement, invalidates history and stops subsequent jitter; acknowledge
the possible one-frame artifact. Device/allocation failure may prevent
recovery. No re-render or perfect rollback of mixed pixels is assumed.

Quality/ship/camera/size/fullscreen/device/backend/model changes invalidate
affected generations. Reprepare, reseed and release retired resources after
outstanding users finish. Prohibit ever-growing maps and steady allocations.
Exhaustion is a named unavailable state, not an endless per-draw retry.

## 7. Existing adapter and mod compatibility

Retain reducer, depth/color provenance, negotiation, motion, retirement and
state restoration. Version replay's new producer evidence; old traces cannot
certify an upstream hook they never recorded.

First run both observers without upstream writes. At activation select one
injector per group: legacy patches OR upstream construction. Suppress legacy
projection mutation under upstream ownership; retain observations and motion.
Switch only after outstanding work closes and history resets. Keep the legacy
route for environments it qualifies; never switch injectors mid-frame.

EDHM color/material variants consuming certified camera data should need no new
hashes. Camera/inverse/depth/composition changes require explicit contracts.
Detect post-construction writers by changed derivation/resource lineage.

For ReShade, verify loader/hook and AA/UI/effect order; preserve forwarding and
D3D state. Test each mod alone and together. Depth/projection replacement or
another temporal reconstruction may need integration or remain unsupported.

Keep user-facing AA settings unchanged. Report selected versus effective
treatment and a useful blocked reason instead of perpetual "warming". No
feature/config removal or rename is authorized by this design.

## 8. Delivery milestones and stop conditions

| Milestone | Required evidence before proceeding |
| --- | --- |
| C0: baseline | Reproduce logged classifier refusals; obtain saved bytes/trace; enumerate domains and candidate callers; no mutation |
| C1: producer discovery | Passive trace identifies domain/lifetime/ABI and proves ordering for all relevant derivatives; a counterexample rejects that boundary |
| C2: offline implementation proof | Pure event/reducer tests plus WARP geometry/lighting tests validate matrices, ownership, generations and recovery; existing suites remain green |
| C3: controlled activation | Full validation build; one bounded game session verifies actual producer -> derived data -> scene/depth -> accepted history with no duplicate jitter or uncovered domain |
| C4: qualification and promotion | Different ships/cameras, supported quality/size/backend changes and mod combinations work without new admission hashes; bounded resources and measured overhead; VR regression checks pass |

If no complete upstream boundary exists, publish the missing dependencies and
retain the existing adapter. A packer hook is acceptable only if it satisfies
the same complete-derivation proof, not as an undocumented partial substitute.

Desktop tests: forward/inverse/depth/ray consistency; raw motion preservation;
storage/reversed-Z/asymmetric projection; R/E/D routes and crops/odd sizes;
interleaved views; nested/worker calls; partial uploads; pointer reuse;
delayed/repeated replay; mid-job disable; late writers; duplicate jitter;
post-application backend failure; unrepairable mixed phase.

Live matrix: working/failing ships and distinct cockpits/canopies; on-foot/
weapon views; camera/menu/docking/flight transitions; presets/effects;
below/native/above-1.0 SS; resize and mod chains. Ship/camera-family diversity
is regression coverage, not a production allowlist or proof of arbitrary mods.

Counters: producer/derivation epochs, injector, applied jitter, closure,
treatment/history streaks and reset reasons. No persistent observation, mixed
phase, stale generation or hidden steady fallback. Compare CPU/GPU cost with
the existing adapter at the same scene/size. Steady state allocates nothing,
does no blocking readbacks/captures; resolve regressions before promotion.

Implementing C++ changes requires the full absolute-path `build.bat` and its
green receipt before commit. Install/verify/log operations use the sanctioned
tools. After promotion, retain an independent stand-down path for unknown
executables, conflicting hooks and unsupported camera domains.

## C0 addendum, 2026-09-28: the existing anchors, mapped

What the installed adapter already proves about the per-view camera, before
any producer probe exists:

- The mono scene constants are a VS b1 CB of 336 float4s (observed bound at
  `VSb1`, e.g. 000001EB2E751E20 in the 203919 Caspian session); the
  per-view forward camera is its rows 270..275
  (`flat_temporal_model.h`'s kFlatCameraOffset). A per-frame camera hash
  (flatCameraHash) changes every frame in flight.
- Write paths are both shadowed at the API: hookedUpdateSubresource ->
  flatRuntimeUpdate -> camera table capture (complete CPU writes), and
  Map/Unmap -> flatRuntimeMap/Unmap -> capture at Unmap. The table holds
  up to 64 camera-shaped CBs (any CB of at least 4416 bytes — rows
  270..275 need 270*16 + 6*16 — with BIND_CONSTANT_BUFFER); each draw's
  bound b1 is matched against it and contributes its rows, write epoch
  and sequence to the draw's contract.
- Auxiliary per-view cameras exist and share the block shape: the VR
  arc's carry evidence (camera-rows-carry-2026-09-25.md) convicts parked
  auxiliary passes writing into the same block as the view, so a global
  single-camera model is refuted before probing. The camera settings
  probe (camera_view.cpp, the 6ad.x records) covers FOV/planes, not
  per-frame matrices.
- The producer gap, precisely: EDVR observes the uploaded bytes, their
  epochs and their consumers, but not WHO computed rows 270..275, from
  what source object, at what point in the frame. Existing stack/owner
  utilities (captureWriterStack's guarded unwind, ownerModuleBrief's
  module+offset naming, isExecutableAddress) make that gap closable
  without inventing RVAs or a global camera pointer.

C1 probe design (bounded, passive, no mutation): the camera producer
witness. Where the existing camera table already captures a write
(UpdateSubresource full-buffer, or Unmap of a mapped camera CB), also
capture the writer's stack (bounded frames, CaptureStackBackTrace),
name each frame module+offset, and keep one stack per unique first
non-EDVR frame (the game's upload call site), deduped to 16 sites.
Every write logs buffer, width, box/full, epoch, sequence, thread and
frame; per-5s counters report writes, unique sites and dedup drops;
overflow is a named "later writers counted without stacks" line, never
silence. Discriminators it buys for the section-3 hypotheses: one or
two shared outermost call sites across camera-table writes supports a
common per-view finalizer; a generic upload helper on every write
supports a packer; distinct call sites per camera domain supports a
view-table builder; multiple call sites writing ONE buffer means shared
staging. Deferred-context writes stay tagged by thread for the
record/replay distinction the doc requires.

## C1/C2 addendum, 2026-09-28: the producer is named

The two-step producer probe (flat_camera_producer_probe.cpp, gated by
`advanced.flat_camera_producer_probe`) hooked the Ghidra-validated upload
helper and armed one hardware write watch on the staging block's camera-row
span. Two bugs cost three flights — a 40-byte relay that swallowed the
upload (fixed 57bb2138: exact 44-byte pose_reader_watch relay plus
forward-and-return), and a hardware disarm lost to the kernel's
context-restore, leaving an orphaned watch whose unclaimed single-steps
killed the process (fixed 8af85423: the VEH claims its own hits armed or
not, clears Dr6, and disarms only through ep->ContextRecord). The clean
flight then answered the finalizer question in 30 ms:

- Writer trap RIP: `EliteDangerous64.exe+0x596A54`, inside
  **FUN_140596830** (0x596830..0x596AAA). A data watchpoint traps after the
  store executes, so this is the post-access RIP, not proof of which exact
  store fired. All eight named hits came from ONE staging block through
  three call sites (0x594E13 / 0x594EAB / 0x594FE1); a second block's watch
  recorded one further hit, logged only as budget exhaustion with no RIP —
  consistent with the same writer, not proof of it.
- **FUN_140596830(viewCbSlot, poolWriteCtx, cameraStruct+0x20)** composes
  view·projection: a twelve-float camera input (camera+0x20..+0x4C read
  through the argument; origin is NOT established there — the explicit
  origin-shaped copies in the refresh read camera +0x50/+0x54/+0x58) times
  the projection 4x4 at camera **+0x1D0..+0x20C** (uint indices 0x6C..0x7B
  from camera+0x20), sixteen float products stored into staging rows
  270..273 through a pointer FUN_1404fc580 hands back into the block. No
  static writer exists because the write goes through a computed pointer,
  and the rows change every frame because this runs ~30 times a frame.
- **FUN_1405921f0(viewConstCtx, , cameraStruct)** is the view-constant
  refresh: copies camera +0x210..+0x24C into the context +0x40..+0x7C,
  camera axes and the negated origin triple (the 0x80000000 sign mask on
  +0x50/+0x54/+0x58), near/far (+0x254/+0x258) and viewport terms, then
  invokes the composer when the slot at +0x70 is present. The refresh's
  context copy and the composer's camera read are NOT the same source:
  the composer reads the original camera struct through camera+0x20, so
  mutating only the context copy can miss the composer. Called at pass
  start (camera from view+0x158), at the first rendered object (camera
  from view+0x250), and at pass end (view+0x158).
- **FUN_140594d90(viewCtx, renderItem)** is the per-view render update:
  visibility-masked walk of the object list (item+0x30 & view+0x270),
  per-object vtable +0x70/+0x78 updates, with the refresh calls at
  0x594E0E / 0x594EA6 / 0x594FDC. Its only code caller is FUN_14058ef90
  (call at 0x58F2EF), itself not yet decompiled. The composer's other
  callers (FUN_143654ff0, FUN_14365d3c0, FUN_1436db5f0, FUN_1436dd650)
  are cross-references only; that they are shadow/reflection/env domains
  sharing the staging block remains a hypothesis to prove from their
  behavior, not a finding.
- Decompiles: analysis/decomp/flash/camera/camera_producer.txt (script
  analysis/ghidra_scripts/CameraProducerName.java).

Candidate-hook hypothesis for section 5's camera contract — a hypothesis,
not a satisfaction of acceptance requirement 4: the mutable, per-view,
per-frame input is likely the camera STRUCT consumed by FUN_1405921f0,
not the uploaded bytes. Whether jittering the projection there reaches
every raster/lighting consumer is unproven, and the two candidate
mutation points are not equivalent (the refresh copies into the context;
the composer reads the original camera). The decompile also shows three
conditional re-derivation gates on the camera flag word at +0x250 —
FUN_1404f49f0 before the +0x210 copy (bit 8), FUN_1404f2ff0 before the
composer reads its matrix (bit 4), FUN_1404f4910 before another upload
consumes products at +0x190/+0x870/+0x8B0 (bit 2). Those helpers and
their invalidation rules are undecompiled; changing one matrix could
leave inverse/ray data stale. Identify the authoritative inputs, dirty
flags, regeneration order and any earlier consumers before choosing a
mutation point; whether the camera pointer distinguishes domains and
recording/mutation epochs likewise remains to be joined.

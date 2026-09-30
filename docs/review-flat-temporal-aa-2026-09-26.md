# Flat temporal AA: review and proposed architecture

## Status

- Reviewed source: `4898cef0`, 2026-09-26. This is a design review, not a
  rendering fix. No production code, configuration or game install changed.
- Conclusion: retain the projection math, private-buffer scopes, engine-record
  motion and mono resolver. Redesign admission, frame ownership and resource
  lifetime incrementally. A wholesale rewrite of the backends is unwarranted.
- Reproduced offline: the partial-AA setting is forced off by the flat profile;
  the 33rd distinct warm projection plan fails with `plan-capacity`.
- Also reproduced: legal camera-dependent PS bytecode classifies as `Clean`.
  This is a synthetic shader, not an observed Elite/EDHM failure.
- Environment: Windows D3D11 mono SDR, MSVC/WARP probes; headset/runtime N/A.
  Real backends and VR require separate hardware qualification.
- Validation: flat CPU and WARP GPU rigs pass. Full build compiled DLLs/HLSL
  but failed when child rigs could not find MSVC; no green full-build receipt.
  No flight performed. Existing flight evidence remains in the arc doc.
- Ruled-out pointer: [arc Status](design-flat-temporal-aa-2026-09-23.md).
  Preserve its motion, resize and camera-patching conclusions.
- Next: implement the staged design for Sean's resolution/supersampling,
  quality-change and EDHM/ReShade requirements. Redesign partial refusal before
  enabling it; qualify on Epic with the existing INI preserved.

## Assessment and scope

Admission is distributed across shader tables, a heuristic classifier, resource
lineage, projection caches, motion producers and backend preflight. Their
combined decision has no single contract. New scenes, mods or settings can
expose assumptions after rendering has started.

Reviewed: flat discovery/runtime, projection/classification, shadow buffers,
scopes, hooks, mono resolve, rejection, config and CPU/GPU tests. Motion
capture's flat integration is included; VR, SDK internals and the installer
were not exhaustively re-audited.

Retain actual binding verification, private camera buffers, exact-depth engine
motion ownership, bounded storage, scope restoration and separate flat/VR
scheduling. Arc section 66 demonstrates the cost of duplicate selectors: a tone
HDR-slot fix reached the diagnostic selector but missed the online model.

## Findings, in priority order

### 1. P1: partial AA cannot activate in the flat profile

`flat_runtime.cpp:1288` reads `experimental.temporal_aa_partial` with default
`on`. `runtime_profile.h:102-106` does not permit that key. Consequently
`Config::getString` (`config.cpp:375-377`) returns `off` before consulting the
stored value or the supplied default. The advertised local-refusal behavior
does not run; unknown projections retain the global refusal/reset behavior.

An MSVC probe against the real config implementation prints:

```text
flat default partial=off jitter=on allowed=0
```

This applies even to an explicit `on` value because the gate precedes lookup.
The local-refusal tests only test strings and draw kinds, so they cannot catch
it. Merely allowing the key would expose findings 4-5; that is not a complete
fix. Preserve the key and make its intended behavior explicit during repair.

### 2. P1: generic admission overstates what the classifier proves

The runtime accepts a forward-projection VS with a PS classified `Clean`
(`flat_runtime.cpp:1666-1683`). But PS analysis is not a complete proof that no
projection-dependent computation remains. For example, two separate dot
products can construct texture coordinates from two camera matrix rows:

```text
u = dot(cameraRow0, position)
v = dot(cameraRow1, position)
sample(image, float2(u, v))
```

Each scalar contains only one row, escaping per-component multirow checks
(`flat_shader_classifier.h:517-540,750-769,1263-1274`). An equivalent HLSL PS
compiled by D3DCompile and accepted by WARP returns `Clean`. Paired with the
tracked EB5234 VS, it meets the generic admission predicate. This is a legal
synthetic counterexample, not an observed Elite shader. PS camera consumers
need their own patch or an explicit unchanged-coordinate proof.

Further code-derived gaps need fixtures: arithmetic loses varying provenance;
VS coefficient checks can treat a projection-row-dependent coefficient as
independent. Section 66 records other parser defects. A PS denylist alone
cannot establish safety for unseen PSs.

### 3. P2: bounded caches become lifetime limits

`FlatProjectionRuntime::preflight` allocates only unused cached-plan slots
(`flat_projection_runtime.cpp:460-478`). Topology equality includes the
original buffer identity. Plans remain used until reset; buffers promoted by
preflight are also protected from eviction (`:284-289,527`). The limits are 32
plans and 64 tracked buffers, not 32/64 concurrently active uses.

A WARP probe preflights buffers sequentially, releasing each caller reference.
Plans 1-32 succeed; plan 33 returns `ready=0`, `branch=plan-capacity`. Cache
references retain the old identities. Live retargeting cannot recover this
zero-phase allocation path. Resource churn can leave new content refused until
runtime reset; larger constants only delay exhaustion.

The independent generic pair memo also stops classifying unseen pairs after 64
entries (`flat_runtime.cpp:870-884`). It now logs saturation, but memory
capacity still changes compatibility for the rest of the session.

### 4. P1, latent: per-draw refusal violates the shared raster phase

After finding 1 is repaired, accepted draws can use jitter while a refused draw
uses the original projection (`flat_runtime.cpp:1694-1720`). They can write the
same color/depth buffers. Their depth tests, deferred lighting and blending
happen before the mask reaches `prep` or `finish`.

An unjittered foreground edge over a jittered background can select the wrong
depth surface or blend different sample positions. Rejecting history cannot
reconstruct missing samples. One backend jitter value cannot describe that
image. The contract violation follows from pipeline order; visual severity
needs GPU tests and flight evidence. Widening the mask cannot fix it.

Local history rejection requires coherent geometry. Mixed-phase treatment needs
separately rendered/composited layers with proved transforms/ownership.

### 5. P2, latent: the stamp and recovery paths lack ownership guarantees

- Replay replaces the original PS, disables stencil, and sometimes changes the
  depth comparison to `EQUAL` (`flat_runtime.cpp:980-995,1944-1952`). It cannot
  reproduce PS discard, shader-written depth, or the original stencil decision.
  Equal-depth fragments originally rejected by `LESS` can also be stamped. The
  mask can over-cover or miss the actual draw's pixels.
- The first refused draw determines mask dimensions; the handoff attaches it
  without checking selected-color identity or extent (`:927-947,1771-1777`).
  Other eligible scene-sized intermediates can contaminate that mask.
- Spatial fallback always samples with the global phase
  (`flat_mono_shader_source.h:153-159`), unlike the partial TAA/finish paths.
  It shifts unjittered content when a backend fails.
- `finish` switches an entire output sample to unjittered UV when any of four
  neighboring mask texels is set (`:143-151`); accepted edge pixels can shift.
  Later scene writes do not retire earlier mask ownership, and previous-frame
  local phase validity is not retained with history. Multi-frame effects need a
  GPU sequence test before being characterized visually.

All are conditional on enabling partial AA. Microsoft's [pixel-shader
specification](https://learn.microsoft.com/en-us/windows/win32/direct3d11/pixel-shader-stage)
confirms discard and depth output are PS behavior, not geometry alone.

### 6. P2: tests validate pieces, leaving their composition untested

The GPU rig excludes `flat_runtime.cpp` and the hooked stamp transaction
(`build.bat:1035-1044`). Local-refusal tests assert reason names/kinds. No
fixture covers profile -> admission -> bindings -> handoff -> backend result ->
next-frame history. The PS0 tone regression escaped the same gap.

Missing classifier fixtures can `SKIP` without failing
(`flat_shader_classifier_tests.h:76-80,371-372`). All 34 tracked DXBC fixtures
are present here; this is a weak future gate.

## Proposed system: the three product requirements

The acceptance target is every supported game resolution, supersampling value
and quality setting, including live changes. EDHM and ReShade compatibility is
part of qualification, not an afterthought. Preserve the existing keys.

### 1. Separate render size, AA evaluation size and presentation size

Track three actual dimensions: game input R, temporal output E, display D.
Discover them from attachment/view/viewport lineage, including crop and integer
rounding. Stop inferring scene identity from fixed scale bands or exact aspect
ratio equality. Allocate masks, motion and depth on their declared grids;
convert coordinates explicitly. Shader dispatches handle non-multiple sizes.

| Game rendering | Temporal treatment | Final output |
| --- | --- | --- |
| R smaller than D | DLSS/FSR upscale R to D; TAA uses its declared reconstruction path | D |
| R equals D | Native temporal AA | D |
| R larger than D | Native temporal AA at R | Proper downsample R to D |

For NVIDIA, the supersampled route is DLAA at R followed by downsampling; FSR
has Native AA for the corresponding route. These are documented capabilities
([NVIDIA](https://developer.nvidia.com/rtx/dlss),
[AMD](https://gpuopen.com/fidelityfx-super-resolution-3/)); EDVR's pinned SDKs
and D3D11 adapter still need tests. Do not silently disable the requested
backend above 100% or substitute TAA merely because D is smaller than R. Show
the effective treatment, such as DLAA plus supersampling.

Avoid double scaling: select exactly one final R/E-to-D conversion and preserve
the game's postprocess contracts. A shader sampling a replacement image must
receive the dimensions/texel mapping it expects. Qualify where the game's
existing downsample can remain and where EDVR must supply the conversion.
Supersampled history deliberately costs more memory/work; never hide a lower
internal resolution behind the user's quality setting.

### 2. Make settings changes a supported state transition

Build one immutable FrameContract containing attachment generations, camera
domains, render/evaluation/display mappings, phase, projection obligations,
effective backend and history validity. One streaming reducer of CPU command
metadata produces it; diagnostic replay runs that same reducer.

Track scene -> depth/HDR -> effects/tone -> final scene -> UI/Present by
resource writes and shader semantics. Stock quality variants are certified
families, not separately maintained VS/PS pairs. DoF/bloom and other passes
that alter the depth-to-color relationship need explicit contracts; their
dimensions or fullscreen shape alone are not proof of equivalence.

At a change, invalidate the old generation, prepare correctly sized resources
and backend state, reset history once for the new stable contract, and resume
accumulation automatically. Cache retirement must release obsolete plans and
buffers; the current 32-plan and 64-pair lifetime cliffs cannot remain. An
individual-frame transition/reseed is acceptable; an endless reset storm, stale
resources or requiring a restart is a failed requirement.

Keep one raster phase across shared scene depth/color. A final mask cannot
repair unjittered geometry mixed into a jittered scene. Unexpected late work
invalidates history and returns subsequent frames to observation until its
contract is qualified; it must not be called successful partial AA. A proxy
cannot perfectly undo a frame already drawn without retaining/re-rendering it.
Supported stock settings must be qualified ahead of time so that this is an
exception path, not normal settings-change behavior.

### 3. Qualify mods by what they change

Separate projection, motion ownership and pixel shading contracts. A mod's
color-only PS change should not invalidate a proved VS projection or motion
layout. Prove stage linkage and every projection/depth consumer; absence from a
PS denylist is insufficient. Keep exact recipes for exceptional behavior.

Place AA before final UI/ReShade postprocessing where the observed chain allows
it. Preserve loader forwarding, original state and resource lifetimes. Test
EDHM alone, ReShade alone and both together in the supported chain order.
Effects that replace depth, camera transforms or temporal reconstruction need
specific integration; arbitrary changes to those inputs cannot be guaranteed
compatible by recognizing a new shader hash.

## Delivery and acceptance gates

1. Consolidate FrameContract and the online/replay reducer without changing
   output. Save representative traces for stock settings and mod combinations.
2. Implement three-size routing, generation-based resource retirement and
   backend negotiation. GPU-test sub-native, native and supersampled inputs,
   odd sizes, ultrawide/crops, resize and repeated transitions without a
   restart.
3. Replace variant tables with proved shader-family contracts. Require every
   saved fixture; add legal adversarial shaders and verify all consumers.
4. Test actual hooked transactions end to end: config, projection, motion,
   handoff, backend failure, restored state and next-frame history. Include
   opaque overlap, transparency, stencil, discard, SV_Depth and target changes.
5. After full build, one Epic qualification session covers settings presets,
   individual effect changes, scale changes and the mod matrix. Record build,
   GPU/driver, sizes, backend versions, effective treatment, history streaks,
   refusal causes and timings. Test real DLSS/FSR separately from WARP mocks;
   preserve VR's separate scheduling and regression gate.

Retain private-buffer reuse and engine-record motion. Upload only on source,
recipe or phase changes; no normal-path synchronous readback. Keep shader
analysis out of steady per-draw execution. Acceptance includes bounded memory,
observable recovery and measured CPU/GPU overhead, not merely a treated
counter.


## Verification record

Ignored `build/review` contains the three native probes and `baseline.log`. The
config probe links real config code; capacity uses current projection
sources/WARP; classifier uses D3DCompile and WARP CreatePixelShader. All
reproduced their stated behavior. Fresh `flat_temporal_test --self-test` and
`flat_mono_resolve_test --dry-run/--self-test` pass, including both generated
pixel-fixture verifiers. These passing rigs do not test the identified gaps.

Full `build.bat` ran by absolute path: production DLLs and flat HLSL compiled,
then six child rigs failed to find `cl.exe`/`ml64.exe` before the flat rigs ran
(`build/review-build.log`). No full-pass receipt was produced. The flat rigs
above were compiled directly with MSVC in a developer environment. No binaries
were installed, no live INI changed, and no rendering fix is claimed.

# The cockpit HUD after the upscale: design

## Status

- **State:** PHASE 0, flight 1 read (2026-09-27, Steam, build fc89d59d,
  EDHM active). G-A settled (all three families, the sprite's target
  included), G-B settled (measured pair + the EDHM swap, in the wild),
  G-E settled directionally. G-C and G-D measured nothing -- instrument
  gaps, not negative results (see the flight entry). G-F got no data: the
  eye-run hotkey was never pressed.
- **Goal:** composite the cockpit HUD after the upscale, at output
  resolution, out of DLSS/FSR history. That means the holo panels, the
  flight HUD and the target sprite. It should be as sharp at HMD Quality
  0.5-0.75 as at 1.0. Menus already leave the upscaler through
  `fix.ui_quality`'s layer.
- **Why:** field reports on 2026-09-27.
  - One user's HUD is soft and smears below HMD Quality 1.0. Their rc.2
    logs show a DLSS input of 65-85% with `fix.ui_quality` off; 1.0
    "mostly" fixes it.
  - Another user found "the cockpit panel is not excluded from the AA
    pass". That is correct and by design: the layer refuses these
    families as `kHdrTarget` (`ui_layer_math.h:913`).
- **Prior art:** this is crisp-ui-handoff.md's parked form of Design A
  (lines 184-191: "PARKED, not declined"), with one change. It re-issues
  the game's own tonemap draw instead of transcribing it.
- **Decisions for Sean:** see "Decisions", before Phase 1.
- **Next step:** Phase 0.1 -- type-aware G-D query guard + decline-reason
  split, a wider G-C cb0 read, then one short re-fly that also presses the
  eye-run hotkey (INSERT) with holo panels on screen. Details in the flight
  entry at the tail.

## What exists

`fix.ui_quality` (`off | 100 | 125`, default off) owns a per-eye 8-bit
layer at the door's output size x target. It takes the post-tonemap UI
(2D screen composite, menu and modal panels, loading screen, GUI-direct)
and composites it last at the door, after the upscale and RCAS, as
premultiplied-over. All MEASURED: ui-layer-2026-09-23.md "The layer" and
crisp-ui-handoff.md:611-829. The machinery a taken draw gets is:
- eye identity by (target, viewport origin);
- a viewport and scissor remap from the game's lied viewport to the
  layer's frustum;
- a jitter cancel through `temporalJitterToTangents`;
- a depth-stencil target seeded from the game's own through
  `ui_layer_seed.h`;
- the multiply second draw, and the colourless write-back of
  depth/stencil writes;
- since 2026-09-27, draws the game makes after the UI into the same eye
  (`kAfterUi`).

A draw the layer takes skips ui_depth's depth re-issue, the hologram depth
pass and screen motion. That is the `!layered` gating at
vscreen.cpp:3909-3950 and 4569-4581 (MEASURED). Measured layer memory is
37-116 MB per eye (ui-layer doc "Cost").

## The three families (as built)

| Family | Draws | Target | Blend, depth | Samples | Source |
|---|---|---|---|---|---|
| Holo panels, vs `81216C77F90DEDD6`, ps `A2965EC2931A39C8` | 22-24 a frame | `R11G11B10_FLOAT`, the lit HDR target | premultiplied ONE/INV_SRC_ALPHA; tests GEQUAL against the scene pair, no write | its interface surface at PS t2 | MEASURED, crisp-ui-handoff.md:174-176,235; ui_depth.cpp:133-137 |
| Flight HUD, vs `B7790CBFC6554097`, ps `8DEF46452FA459F5` | UNKNOWN | the same HDR target | as above | a 256x256 grain LUT at t1, and the eye-sized scene depth at t0 (NDC-derived UV) for its own cockpit fade | MEASURED, hud_grain.h:1-25; crisp-ui-handoff.md:796-803 |
| Target sprite, vs `E508648660A352B2`, ps `63ABD86359B57D01` | 1 an eye with a target | DISPUTED: the HDR target (ui_depth.cpp:116-131) or `RGB10A2_TYPELESS` (crisp-ui-handoff.md:174) | its VS forces device Z to 1 | one surface of ~0.94x0.83 render size at t0, with alpha discard and the HUD colour matrix | MEASURED, target_sharp.h:6-12 |

Jitter: BELIEVED that all three carry the eye's jittered projection (G4
was never checked per family). HDR values above 1.0: UNKNOWN. Ordering,
MEASURED (G1/G8): all three draw before the composite pass
(`953C8123AD8DC13B`, believed to add bloom) and the tonemap, which is the
last reader of the HDR target. SMAA runs after it on RGBA8.

## The post chain

- **Tonemap** (vs `2D78DC3FD2C0C543` / ps `99C21CEB7A699821`), MEASURED
  (eye_tonemap_snapshot.h:87,139,170-201):
  - a 3-vertex full-screen triangle, unblended, no DSV;
  - VS t0 is the scalar exposure (`R32`); PS t0 the colour LUT (3D); PS
    t1 the HDR source; PS b2 at least 256 bytes;
  - the output is the RGBA8 eye.
- **Variants:** under EDHM and the settings tiers the tone PS varies. The
  flat-AA arc's tone admission identifies them by VS set and per-PS HDR
  slot (design-flat-temporal-aa-2026-09-23.md sections 63-65). Reuse that
  rather than one hash.
- **Exposure:** `fix.share_exposure` copies eye 0's exposure over eye 1's
  (exposure_fix.h). A re-issue that binds the game's own exposure SRV
  inherits it.
- **Bloom:** BELIEVED baked into the HDR source by the composite pass. It
  is computed from the HDR target, so it includes the HUD. That is the
  HUD's halo, measured in the input frame of eye dump 123118.

## Ruled out (do not re-propose)

- **Taking the families into the 8-bit layer as drawn:** they write HDR
  radiance before exposure and tonemap, so the layer would lose both. This
  is the reason the code gives for kHdrTarget.
- **Rebuilding the deferred UI replay** (retired 48ad7689). It used
  parallel capture and command lists, D3DReflect on shaders with no RDEF
  chunk (reflection reports zero bindings; edhm-black-cockpit-2026-09-15.md),
  and alias-matched submits. It logged captured=0 on every flown rig.
  Use the layer's live redirect, which flies.
- **Transcribing the tonemap into EDVR HLSL:** it drifts from EDHM and the
  settings-tier variants. Re-issue the game's draw instead.
- **Drawing the HUD in both the HDR image (for bloom) and the layer:**
  additive elements would count twice.
- **Moving the upscale before the tonemap** (HDR DLSS, with the HUD
  blended in HDR at output size): exact, but it moves the whole temporal
  pass. Out of scope. Name it in the design review if parity fails.

## The design

1. **An HDR HUD layer** per eye: `R16G16B16A16_FLOAT` at the door's output
   size x the `fix.ui_quality` target (100 when that key is off). It holds
   premultiplied HUD radiance plus coverage, in the 8-bit layer's own
   alpha convention (read `uiLayerComposite` for it). At 4074x3938 that is
   128 MB per eye at 100 and 200 MB at 125; at a Quest 3's 2564x2460, 50
   MB. Clear it per eye-frame.
2. **Take the families:** with the new key on, `uiLayerDecide` routes
   kHolo, kFlightHud and kSprite to the HDR layer instead of kHdrTarget.
   They get the same remap, jitter cancel, seeded depth (they test GEQUAL
   against the scene pair), refusals and census, and the `!layered` skips
   of ui_depth, the hologram pass, the reactive mask and screen motion.
   Confirm the "tone separation" re-issue in the vscreen.cpp:3677 comment
   is covered too. The flight HUD's t0 depth read is eye-sized and
   NDC-addressed, so it survives the remap. The after-UI rule's eye-sized
   post-pass test must not see it, because it is a named family.
3. **Tonemap re-issue**, in frame at the game's own tonemap draw (tone
   admission identifies it), once per eye:
   - bind the same VS/PS, b2, LUT and exposure;
   - put the HDR layer's SRV in the admitted HDR slot;
   - render into the eye's 8-bit UI layer, RGB only, at the layer's
     viewport;
   - one EDVR pass then writes the HUD's coverage into that layer's alpha;
   - restore everything through the vScreen*Raw entry points.

   The post-tonemap menu draws then land on top, in game order. The
   existing door composite shows both. There is no new composite and no
   separate LDR HUD image.
4. **No bloom on the HUD:** its halo goes (the look changes). SMAA does
   not apply either; at output size x 100-125 the edges need less. If
   Sean wants the glow, a later phase blurs the tonemapped HUD layer.
5. **Parity:** stock computes T(F(1-a) + L); the layer gives T(F)(1-a) +
   T(L). The two agree where the HUD is opaque or the background dark.
   They differ where a translucent HUD crosses a bright background (a sun,
   a lit station). Gate G-F measures it before Phase 1 ships.

## Phase 0: census gates (no rendering change)

One flight. Cockpit in space, then a station with a target locked, at HMD
Quality 0.75 with DLSS. Each gate is a `hud layer census:` line:
- **G-A, the families:** per family per frame, the target resource and
  format, blend, depth/stencil state, SRV sizes, and the draw index
  relative to the composite pass, the tonemap and the door. This settles
  the sprite's target and gives the flight HUD's draws per frame.
- **G-B, the tonemap:** the admitted variant (VS/PS), its SRVs (exposure,
  LUT, HDR), b2's size, and whether its HDR SRV is the resource the HUD
  drew into.
- **G-C, jitter:** each family's projection rows against the scene's
  jittered ones (G4 per family).
- **G-D, occlusion:** each family's pixels rejected by its GEQUAL test
  (occlusion queries, test on against off). A large share means the
  seeded depth's input-resolution edges show.
- **G-E, exposure:** HUD display brightness against the exposure scalar,
  across a bright and a dark scene.
- **G-F, parity:** with the HDR target and the HUD's own contribution
  captured for one eye-run frame, the error |T(F(1-a)+L) -
  (T(F)(1-a)+T(L))| on HUD pixels (p50/p99, in 8-bit steps), computed
  offline or in a diagnostic pass.

## Phases

1. The holo panels only, behind the key (default off). Log lines, every
   30 s:
   - taken, left and refused per family, with the reason;
   - HDR layer size and memory;
   - tonemap re-issues and declines, with the variant.
   GPU time for the HUD draws at layer size and for the re-issue goes into
   the existing layer timing lines.

   Rig (extend `ui_quality_test`): a model of HUD over background with a
   stand-in tonemap. Opaque, additive over dark and translucent over
   bright match stock within the tolerance the gate chose, and fail with
   the take removed.

   Flight: cockpit at HMD 0.5 and 0.75 with DLSS. The panels should read
   as sharp as at 1.0, with no brightness step against the pass-off frame
   (the G-F budget) and the census clean.
2. The flight HUD and the target sprite, once G-A has settled the sprite's
   target.
3. Optional: the hologram families (radar contacts, the ship and target
   holograms, icons: the hologram pass's eleven). They are in the HDR
   target too. Taking them would retire most of that pass.
4. Optional: the glow (see Decisions).

## Risks

- A brightness step from parity (G-F).
- Occlusion edges at input resolution (G-D).
- A family without jitter, which the cancel would shift (G-C).
- The PS hashes under EDHM: families are keyed by VS, variants by the tone
  admission tables.
- Memory, above.
- GPU: the panels shade 4x the pixels at 2x per axis (measure).
- VR-specific: the layer seeds depth per eye, and the HUD must not pick up
  the wrong eye (the layer's eye check, "0 SWAPPED", covers it).

## Decisions

For Sean, before Phase 1:
1. Accept losing the HUD's bloom halo, at least initially?
2. The key. Suggested: `fix.crisp_hud = off | on`, named for what the
   player gets, which follows `fix.ui_quality`'s size target and arms the
   layer at 100 when that key is off. Or fold it into `fix.ui_quality`.
3. Scope: the three families (Phases 1-2), or the hologram families too?
4. The memory budget: +128-200 MB per eye on a Crystal-class output.
5. Default: off until flown on Pimax and Quest.

## Phase 0 built, 2026-09-27

The census is code now, on branch `kimi/crisp-hud-census` (held off main
until the feature flies and verifies on the Steam install). Full build
green, every gate including the new `hud_parity` self-test.

- **The instrument:** `advanced.hud_census = off | on` (default off, live),
  a new module `src/d3d11/hud_layer_census.{h,cpp}` hooked into vscreen's
  eye-draw branch INDEPENDENTLY of `uiLayerLive()` -- the design flight has
  `fix.ui_quality` off, and the family naming runs only with the layer
  live. Unarmed cost is one bool per eye draw. G-A family state lines on
  first-seen/change plus 30-second window lines with draws-per-frame; G-B
  tonemap variant lines (structure-first recognition, exact hashes logged,
  so an EDHM swap names itself); G-C jitter verdicts on change; G-D
  depth-rejected shares; G-E exposure + HUD-region HDR luma at 1 Hz. All
  lines prefixed `hud layer census:`.
- **G-D's shape:** an occlusion-query pair per sampled family draw, the
  game's own GEQUAL test with ALL writes masked against depth-and-stencil
  off, both re-issued with NO colour target, full OM save/restore, queries
  never waited on. It declines on predication, on PS UAVs, and -- a case
  the design missed -- while ANY game query is open on the context (a
  re-issue inside the game's own occlusion bracket would feed its counter
  and change what it draws a frame later; tracked from the Begin/End
  hooks).
- **G-F is offline:** `tools/hud_parity.py` reads one F10 eye-run ledger's
  `panels_<stamp>.bin` (EyePanelSnapshot already captures the HDR target
  before AND after each holo draw -- F and F(1-a)+L) and `tonemap_<stamp>.bin`
  (exposure, LUT, HDR/output crops), replays T empirically from the captured
  HDR->output pairs, and prints p50/p99/max of the parity error in 8-bit
  steps, with the verdict gated on the bright-translucent regime. `--self-test`
  green and gated in build.bat.
- **Corrections to this doc from the build:**
  - ruled out: "the composite pass `953C8123AD8DC13B`, believed to add bloom"
    as a cockpit anchor -- every reference in this repo names that hash the
    FSS scanner-body composite (edvr.ini, fss_probe.h, crisp-ui-handoff.md).
    The census anchors ordering on the tonemap draw and logs any cockpit
    sighting of the FSS hash to settle it.
  - The family clip rows are VS cb0 rows 4..7, not 0..3
    (flat_projection_recipes.h); G-C reads 64 bytes at offset 64 and votes on
    the centre terms (m02/m12), which are all the jitter moves.
  - The tonemap VS already varies in the wild: EDHM flies vs
    `642017A6FEDAE0E8` with the same PS (edhm-black-cockpit-2026-09-15.md).
- **Flight checklist (Steam):** install (`python tools\install_edvr.py
  --target steam`), set `advanced.hud_census = on`, `fix.ui_quality` off,
  HMD Quality 0.75 with DLSS. Cockpit in space (2 min), then a station
  with a target locked (2 min), one bright scene and one dark (G-E); press
  the eye-dump hotkey once with panels on screen (G-F). After:
  `python tools\edvr_log.py --target steam --expect-build HEAD`, then
  `--grep "hud layer census:"`, and `python tools\hud_parity.py <ledger
  dir> --verbose`.

## Phase 0, flight 1, 2026-09-27 (Steam, fc89d59d)

Short flight (~2 min: menu, cockpit, the FSS scanner mid-flight, a
HUD-present stretch of ~270 frames at 2600x2514 = HMD Quality 0.75, DLSS
on, EDHM installed and active). Build stamp verified by
`edvr_log.py --expect-build HEAD` before any counter was read. 9,596
`hud layer census:` lines harvested; the per-gate verdicts:

- **G-A SETTLED.** Holo: 22.0 draws/frame (11/eye), the lit HDR target
  (R11G11B10_FLOAT), premultiplied over, GEQUAL depth with no depth write
  -- and a STENCIL WRITE the doc's table missed (ref/write 0x04, pass
  REPLACE: Phase 1's take needs the write-back machinery for these).
  Flight HUD: 4.8 draws/frame (4..6; was UNKNOWN), same HDR target,
  GEQUAL, stencil off; t0 = the eye-sized R32_TYPELESS depth and t1 = the
  256x256 grain LUT, both as the doc's table said. Sprite: 2.0 draws/frame
  (1/eye with a target) -- the target dispute is SETTLED for the lit HDR
  target (R11G11B10_FLOAT, the same two resources as the other families;
  ui_depth.cpp:116-131 right, crisp-ui-handoff.md:174's RGB10A2_TYPELESS
  refuted on this config) -- but its state is NOT the table's GEQUAL:
  depth test off with write-all, stencil on (0x05, GREATER), consistent
  with the VS forcing device Z to 1. Ordering holds: families at ordinals
  503..2684, the tonemap at 767..2687, always after. The bloom composite
  953C8123AD8DC13B was never seen in cockpit (0 sightings): MEASURED now
  that this hash is the FSS scanner-body composite; where bloom lives in a
  cockpit frame stays BELIEVED and un-hashed.
- **G-B SETTLED, with the variant named.** The measured pair (vs
  2D78DC3FD2C0C543 / ps 99C21CEB7A699821) flew as a full structural match:
  exposure R32 at VS t0, 3D LUT at PS t0, HDR source at PS t1 = the
  families' own target resource (identity join 442/442 in the HUD window),
  b2 = 272 bytes. The EDHM swap flew too (vs 642017A6FEDAE0E8, same PS) --
  and it binds NO exposure at VS t0, so a Phase 1 re-issue must bind the
  admitted draw's own SRVs, never a remembered exposure (where EDHM's
  exposure lives is an open question, next to the G-C rework). Two
  menu-size composites named themselves shape-only and were never
  followed; the structure-first recognition did its job.
- **G-C NOT SETTLED -- instrument gap.** All three families' cb0 rows
  4..7 are a composed model-view transform (dense rotation-like rows;
  row 2 = [0 0 0 0.025]), not the bare projection, so the centre-term
  test has nothing to compare (0 jittered, 0 unjittered, n=3552
  not-scene). "All three carry the eye's jittered projection" stays
  BELIEVED. Next instrument: capture the whole cb0 for offline
  factorisation, or vote the per-eye row deltas against the per-eye
  jitter delta; the projection may not live in cb0 at all.
- **G-D NO DATA -- instrument gap.** Every selected pair declined (2,290)
  and no result ever polled. Near-certain mechanism: EDVR's own gpu_span
  TIMESTAMP_DISJOINT query is open across the frame's draw sections
  (gpu_span_d3d11.cpp:75) and the guard declines on ANY open query, though
  a disjoint/timestamp counts no samples and could not be fed by the
  re-issue; game predication is the other candidate. Next instrument:
  split the decline counter by reason, and make the guard type-aware
  (decline only for occlusion-family queries and predication).
- **G-E SETTLED directionally.** The tonemap's own VS t0 exposure tracked
  the scene: ~600 in the bright stretch down to ~44-51 in the dark one.
  The HUD-region crop luma follows the background more than the HUD
  (0.15-0.23 bright, 0.003-0.012 dark), so "HUD display brightness"
  proper stays an offline read; the gate's deliverable is that exposure
  is a per-frame scalar the re-issue inherits by binding the admitted
  draw's own SRV -- with the EDHM caveat above.
- **G-F NO DATA.** The eye-run hotkey (INSERT) was never pressed; no
  ledger armed, no panels_/tonemap_ bins. Re-fly item.

Re-fly notes: keep the cockpit HUD up for one full 30 s window (the FSS
scanner replaced it mid-flight this time), fly one bright and one dark
scene, and press INSERT once with holo panels on screen for G-F.

## Phase 0.1 instrument, 2026-09-27 (6798b6de)

Flight 1's two instrument gaps are fixed on the branch: G-D's open-query
guard is type-aware (it declines only while a sample-counting query --
occlusion, stream-out or pipeline statistics -- is open; gpu_span's
frame-wide TIMESTAMP_DISJOINT no longer trips it), declines are counted
by reason on their own window line, and a stream-out-bound decline was
added beside the PS UAV one. G-C reads the whole VS cb0 (up to 16 rows)
and votes each 4-row quad for bare-projection structure; a family whose
projection lives outside cb0 dumps every row once per eye for offline
factorisation. Installed to Steam as v0.18.0-rc.2-50-g6798b6de; awaiting
flight 2 (same profile as flight 1, plus one INSERT press with holo
panels on screen for G-F).

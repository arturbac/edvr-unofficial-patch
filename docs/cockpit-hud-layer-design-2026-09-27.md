# The cockpit HUD after the upscale: design

## Status

- **State:** PHASE 1 BUILT (2026-09-27, branch `kimi/crisp-hud-census`,
  f032e024, installed to Steam as v0.18.0-rc.2-56-gf032e024): the holo
  panels take a per-eye HDR layer and return to the picture through the
  tonemap re-issue, folded into `fix.ui_quality` (Sean's decision 2 --
  no key of its own). Phase 0's gates are all answered (flights 1-3 + the
  offline G-C). Awaiting the Phase 1 flight: fix.ui_quality = 100, HMD
  Quality 0.5 and 0.75 with DLSS, panels as sharp as at 1.0, no
  brightness step (the G-F budget), the census clean.
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
- **Next step:** the Phase 1 flight (checklist in the tail entry), then
  its log read against the Phase 1 gates (sharpness at 0.5/0.75, no
  brightness step, declines clean). Then Phase 2: the flight HUD and the
  target sprite.

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

For Sean, before Phase 1 -- ANSWERED 2026-09-27:
1. Accept losing the HUD's bloom halo, at least initially? YES (the G-F
   numbers sized it: a few dozen pixels a frame in the bright-translucent
   regime; the Phase 4 blur is the way back if the edges read wrong).
2. The key. Suggested: `fix.crisp_hud = off | on`, named for what the
   player gets, which follows `fix.ui_quality`'s size target and arms the
   layer at 100 when that key is off. Or fold it into `fix.ui_quality`.
   SEAN: folded -- the holo take is part of fix.ui_quality (no key of its
   own).
3. Scope: the three families (Phases 1-2), or the hologram families too?
   AS SUGGESTED: three families; the holograms stay Phase 3-optional.
4. The memory budget: +128-200 MB per eye on a Crystal-class output.
   ACCEPTED (the configure and layer lines report the actual numbers).
5. Default: off until flown on Pimax and Quest. STANDS -- fix.ui_quality
   defaults off, so the take defaults off with it.

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

## Phase 0, flight 2, 2026-09-27 (Steam, 6798b6de, two sessions)

Both logs verified build 6798b6de. Sessions 12:21 and 12:24; five eye-run
ledgers at 12:30:21..12:31:04. ~19.2k census lines harvested.

- **G-D: the guard fix worked; the log cap ate the results.** Zero
  declines in every window (the type-aware guard passes gpu_span's
  disjoint), all three families' game depth states were cloned and pairs
  began -- and then the gfx log hit its 4 MB cap at 12:26:50, seven
  seconds before the first window carrying G-D results would print. The
  cap's cause is the census's own G-A flood: a single last-fingerprint
  slot re-logged every panel of every frame (~12 distinct holo interface
  surfaces cycle through one family's slot), ~9.7k lines in two minutes
  of cockpit. Fixed as Phase 0.2: first-seen-per-session fingerprint sets
  (64 per family, with a table-full note). G-D data: still none; flight 3
  gets it by holding the cockpit for one 30 s window.
- **G-C: cb0 ruled out; the composed matrix is shared.** The wide read
  dumped all of cb0: rows 0..3 are constants ([1 1 1 1], [0 0 0 0],
  [16 16 0 0], [16 16 0 0] -- panel parameters, not a projection), rows
  4..7 a composed transform, and MEASURED near-identical per eye across
  all three families (row 4 agrees to ~3 decimals between holo, flight
  HUD and sprite): one shared per-eye view-projection, the
  family-specific part elsewhere. Jitter inside a composed matrix is not
  separable by row inspection; the flight-2 ledgers captured the
  families' VS b0/b1/b2 (pool\draws_<stamp>.bin), so factorisation
  against the scene camera is offline work -- no re-fly needed. "The
  families carry the eye's jittered projection" stays BELIEVED until
  then; Phase 1's jitter cancel keeps its G-C gate.
- **G-F: measured.** Five ledgers, ~440k HUD pixels pooled. err p50 0.00,
  p99 0.00 8-bit steps: stock and the layer agree except in the predicted
  regime. In it (0<a<1 over background luma > 1): 5-40 pixels a frame
  (0.0-0.1%), restricted p99 40-113 steps; the stamp with the most
  in-regime pixels (123104, 40 of 221k) fails the default budget (113 vs
  2). Caveats measured alongside: every panel draw is INSTANCED, so the
  a/L recovery used the luminance fallback (per-pixel a is heuristic; the
  cross-check is by-construction there), and the empirical T fit residual
  ran p99 1.5-2.5 steps. Physical reading: a layer-composited HUD differs
  from stock only where translucent glass crosses a bright background, a
  few dozen pixels a frame, and there stock is brighter -- the halo,
  quantified. That is Decisions 1's price with numbers; the Phase 4 blur
  is the way back if those edges read wrong in flight.
- **G-A/G-B consistent across both sessions and both render sizes flown**
  (2600x2514 and 3000x2901): holo 22.0 draws/f with the 0x04 stencil
  write, sprite 2.0/f depthless with stencil 0x05, flight HUD ~5/f with
  the eye-sized R32 depth at t0; tonemap 1.00/frame/eye in session 1,
  five variants named (the same five), bloom composite 953C8123AD8DC13B
  never sighted in cockpit again.
- **G-E:** exposure tracked 11.3..115 across the two sessions' scenes;
  the HUD-region crop still mixes scene and HUD (by design).

## Phase 0, flight 3, 2026-09-27 (Steam, 4db05397)

One session, ~4 min, build verified. The Phase 0.2 dedupe held: 143 census
lines for the whole session (flight 2 spent 9.7k in two minutes), the log
cap never approached, and the G-D windows printed.

- **G-D SETTLED: the depth test rejects nothing.** Per family per eye the
  occlusion pair's on/off sample counts were EQUAL (holo eye 0: 33,123,678
  of 33,123,678 passed, 1,989 pairs; flight HUD eye 0 across the second
  window: 1,282,658,327 of 1,282,658,328 -- ONE sample rejected in 1.28
  billion). The doc's "large share means the seeded depth's
  input-resolution edges show" is measured absent: occlusion is ~0%, so
  Phase 1's seeded depth is a correctness item (the GEQUAL test exists and
  runs), not an edge-quality one. Two qualifications, both measured: the
  sprite's own state is depth-off, so its pair A equals B by construction
  and its line carries no gate; and the flight exercised cockpit-in-space
  and station-with-target, not a panel buried behind the dashboard at an
  extreme look-down. The ring-full declines (14-19k per window) are the
  8-pair ring throttling the 12-pair/frame budget -- sampling only; the
  accumulated counts make the 0.0% robust.
- **G-A, final numbers:** holo steady at 22..24 draws/frame (11-12/eye)
  across every flight and both render sizes; the flight HUD is
  content-dependent, 4.8 draws/f in quiet flight up to 54-56/f (27/eye) in
  the busy station scene; the sprite 2..6/f with a target. The doc's
  flight-HUD UNKNOWN is a measured range now.
- **G-B:** the identity join held 100% whenever families were present
  (4,282/4,282 and 4,526/4,526 tonemaps' HDR SRV is the families' target);
  bloom composite 953C8123AD8DC13B never sighted on a third flight; the
  five variants are the same five every session.
- **G-C unchanged:** no cb0 quad is the bare projection (n=16k+ per family
  per window, all not-scene); the dumps are consistent with flight 2's and
  add that eye 0/eye 1 differ in rotation while the w column differs per
  family -- shared view rotation plus per-family translation. The
  factorisation against the scene camera stays the named offline path
  (flight 2's draws_*.bin carry the families' VS b0/b1/b2).
- **G-E:** exposure 42..625 across this flight's scenes, same behaviour.

Phase 0 closes with one open item: G-C's factorisation, which needs no
flight. Every other gate is answered; the Decisions are unblocked.

## Phase 0, G-C settled offline, 2026-09-27 (flight-2 ledgers)

The factorisation ran on the five flight-2 ledgers' EyeDrawSnapshot
captures (drawstate_<stamp>.bin; extraction and cross-checks in
analysis/gc_results.md, git-ignored scratch; the raw bins stay in the
Steam install's edvr_logs\pool). No flight was needed.

- **Verdict: jitter CARRIED -- all three families, both eyes, all five
  ledgers, every one of 7,039 draws, by construction.** Every family draw
  binds a 336-row VS b1 whose rows 270..273 are ONE global per-eye camera
  matrix (within-frame spread across draws: exactly 0.0; layout as
  object_probe.cpp:956-958 documents, eye origin at row 275). The family's
  clip transform (cb0 rows 4..7) equals that matrix in its 3x3 part
  BIT-EXACT in every sampled draw; the w column is the same matrix applied
  to a per-panel translation. So the HUD projects with the engine-wide
  per-eye jittered transform, and the doc's BELIEVED "all three carry the
  eye's jittered projection" is MEASURED. Phase 1's cancel-exactly-once is
  correct as designed.
- The camera's centre terms oscillate per frame with EDVR's exact 8-phase
  Halton jitter (m12 slope = 2/2901 exactly; m02 at 94-99% of 2/3000,
  small per-phase residuals, likely the game-side tangent round-trip),
  which is also the proof the matrix is the live jittered one and not a
  stale copy. The in-sim census's gc dumps from flights 2 and 3 land
  exactly on the ledger per-frame values -- dump == ledger == one
  transform, and the layer's eye mapping is confirmed (0 SWAPPED never
  fired).
- Caveats carried into Phase 1 (recorded, not blocking): the absolute
  jitter phase cannot be anchored from a ledger (ledger frame != temporal
  frameCounter) and the game-applied x jitter runs a few percent under
  the naive -2*jx/w slope, so a cancel driven by EDVR's own s->shift could
  leave a few-percent-of-a-pixel residual -- validate visually in the
  Phase 1 flight. "b1 is the scene camera" rests on the documented layout
  plus the jitter fingerprint; a direct scene-side confirmation, if ever
  wanted, is one re-fly with advanced.eye_depth_capture on, comparing the
  scene pair's b1 rows 270..273.

## Phase 1 built, 2026-09-27 (f032e024)

The design's Phase 1 is code, on the branch and installed to Steam
(v0.18.0-rc.2-56-gf032e024). Built per the design with the Phase 0
measurements folded in; Sean folded the key into fix.ui_quality (decision
2) -- the take arms with the layer at 100/125 and is off by default with
it (decision 5 stands).

- **The take:** a holo draw into the lit HDR target routes to the eye's
  HDR layer (R16G16B16A16_FLOAT, door size x target) through the LDR
  take's own path -- same remap, same jitter cancel (G-C: the families
  carry the jitter, so the cancel is exact), same seeded depth-stencil
  (G-D measured rejection ~0%), and the measured 0x04 stencil write keeps
  landing in the game's own buffer through the existing colourless
  write-back. The flight HUD and the sprite refuse kHdrTarget as before
  (Phase 2).
- **The re-issue:** the tonemap draw is admitted structurally
  (tonemap_admit.h, the census's G-B recognition factored out and
  shared), once per eye per frame, right after its own issue: same VS/PS,
  b2, samplers, the admitted draw's own exposure and LUT (the EDHM swap
  binds no exposure at VS t0 -- nothing is re-bound there), the HDR layer
  at the admitted per-PS HDR slot, rendering RGB-only into the 8-bit
  layer at the layer viewport; one EDVR pass then writes the HDR layer's
  coverage into the 8-bit layer's alpha. Menus after the tonemap land on
  top in game order; the door composite is untouched. The "tone
  separation" case is the once-per-eye guard (a second admitted tonemap
  for an eye is counted, never re-issued).
- **Declines, all counted and named once:** no HDR slot (unknown PS --
  never guessed), no content, layer busy (an LDR draw already holds the
  frame -- the ordering guard), second tonemap, size mismatch, state
  drift, PS UAV, layer failed. A failure stands only the HDR path down;
  the LDR take is untouched; off means exactly stock.
- **The rig:** ui_quality_test's parity block models stock T(F(1-a)+L)
  against the layer's T(F)(1-a)+T(L) through two stand-in tonemaps:
  opaque/uncovered bit-exact, dark within the G-F budget (2 steps), the
  translucent-over-bright regime bounded by flight 2's measured ceiling
  (113 steps), and proven sensitive to a take-removed mutant.
- **Phase 1 flight protocol:** fix.ui_quality = 100, HMD Quality 0.5 then
  0.75 with DLSS, cockpit with the panels up (a station with a target
  locked for the busy scene). The panels should read as sharp as at 1.0,
  with no brightness step against pass-off (the G-F budget) and no halo
  (accepted). After: `python tools\edvr_log.py --target steam
  --expect-build HEAD`, then the "crisp hud" and "ui quality" lines:
  taken 22-24/frame, re-issues 2.00/frame, declines none, the HDR layer's
  size/memory, the route GPU times. Validate the jitter cancel visually
  (the G-C caveat: absolute phase unanchored, x slope at 94-99%).

## Phase 1 flight 1, 2026-09-27 (Steam, f032e024): the menu regression

Cockpit side measured clean: 12.41 then 23.24 holo draws/frame taken,
2.00 re-issues and coverage passes a frame, 0 declines, 0 HDR content
lost, 0 composites refused. Sean: "cockpit looks good".

The regression: the ui menus. The family census says the menus were NEVER
taken this flight -- 0 redirected in every window. Two measured
populations:

- In the cockpit: the in-flight menu composite (station services, the
  escape menu) draws vs A888D51024D9798E / ps 015EF9349EC097E8 -- the
  TINTED variant, documented in ui_depth.cpp:105-115 (three variants, nine
  disassembly lines apart, none in the sampling) but MISSING from the
  family rule's pair list (kUiPanelPs had only the two main-menu PSes
  since 2026-09-23). With no learned surface bound at recognition, the
  family was never found: "no learned surface, pixel shader not known",
  ~5,200 draws a window. This gap predates the branch -- the recognition
  is identical on main -- but ui_quality defaults off, so nobody had flown
  the menu take against the tinted variant.
- The main-menu/loading untinted panels (ps 9107E72CB016CC02, in the list)
  were recognized but refused "eye unknown" (uiDepthEyeOfTarget can't name
  their target's eye there) -- unchanged behaviour, separate question.

The regression's mechanism: with the menus left in the eye and the holo
panels now composited over the finished eye at the door, an open in-flight
menu sits UNDER the cockpit panels. Stock order (panels under menus) held
before because the panels never left the eye.

The fix (43ab5364): the tinted and cheap variants join kUiPanelPs -- the
pair route exists exactly for "no learned surface bound" -- so the
in-flight menu takes, lands in the layer AFTER the re-issue (game order:
menus draw post-tonemap), and sits over the panels again, now at layer
sharpness. The rig's family-rule fixture moved to the new expectation.
Built green, installed to Steam (v0.18.0-rc.2-58-g43ab5364). VERIFY with
a docked menu open: the menu over the panels and sharp; the log's family
line should show "decided as the menu panel: N redirected" with the tinted
PS named, and the crisp hud line clean.

## The main-menu regression, 2026-09-27 evening (84690880)

Sean after the morning install: "Main menu is still being drawn before
the AA pass" -- and reported all UI menus had taken the layer before.
Tonight's log (43ab5364): every menu draw left with "eye unknown", 0
redirected in every window. The diff against rc.2 is additive in the
layer and nil in ui_depth, and the game has been build 332841 since
2026-09-19, so the mechanism was in the branch: the crisp tonemap
admission asked uiDepthEyeOfTarget for the tonemap's LDR output every
frame, and that ask REGISTERS in ui_depth's per-frame eye table (first
target of a shape = left, second = right, the third gets "no eye"). The
main menu runs at 2000x1934 with the menu composite in its own buffer, so
the tonemap's two outputs held the shape's two slots before the menu's
target ever asked -- "eye unknown" on every menu draw, forever. In-flight
the composite writes the tonemap's own output, which is why the cockpit
never noticed.

The fix: the admission names the eye from the admitted draw's own HDR
source instead -- it IS an eye's HDR target and the holo take records
which this frame, exact, no table. The census's two observer lookups get
uiDepthEyeOfTargetReadOnly (never registers); the layer's decide keeps
the registering form, as the classifier would. The tinted/cheap pair
admission from the morning stands. Built green, on Steam as
v0.18.0-rc.2-60-g84690880.

Verify (one launch): the main menu sharp (taken, not before the AA pass)
-- the log's "left in the game's frame" line should no longer list menu
panel with "eye unknown"; then docked, a menu open over the panels: menus
over the panels, both sharp; the crisp hud line clean.

## Phase 1 flight 2, 2026-09-28 morning (84690880): the tier variant

Menus fixed and sharp (last night's eye-table fix holds). But the cockpit
panels were gone at HMD Quality 0.50 + ui_quality 125: the tier flies its
own tonemap PS (D0A16B9E55BF22CC), the admission's per-PS slot table knew
only the measured 0.75-tier one, every re-issue declined kNoHdrSlot, and
every taken panel vanished (4,060/4,062 draws; the window lines named it:
"0.00 re-issues, 4,060 HDR layers' content never reached a tonemap").
Sean: the panels gone; the radar, the ship hologram and the target
hologram still present (the hologram pass's families, never taken) -- the
ship without its shields (the shield ring is a holo-panel draw, taken and
lost with them). Exactly the families split the take makes.

The fix (09baba69): the admission finds the HDR source by IDENTITY -- the
PS slot whose 2D view reads an eye's HDR target this frame, which the holo
take records -- so the tier and EDHM variants need no table entry. And the
failure shape hardened: a draw with fresh content but no readable slot
stands the crisp path down to stock (named once, counted) instead of
losing the HUD for a session. Phase 0's census never saw this PS because
flights 1-3 ran 0.75 only -- the tier gap is recorded in the G-B entry's
risks now.

On Sean's refactor question (HUD and ui_depth sharing a path): the eye
table regression (observers registering) argues for exactly that direction
-- the admission now answers its own questions by identity instead. A
fuller merge of the two passes' recognition is real work and is NOT
folded into this regression fix; noted for the Phase 2 review.

# All view motion from the world camera (design, 2026-09-30)

## Status

- **State:** design only (no code, build or flight), from tree `1e05ef59`
  and flight 1 of the VR world route (design-flat-temporal-aa-2026-09-23.md
  section 82; `edvr_gfx_20260930_161545.log`, Frontier,
  v0.18.0-rc.4-104-gbde47f81). Sean's direction: every view's temporal-AA
  motion comes from the game camera that renders it, not head heuristics.
- **Trigger (Sean's correction):** the maps smear when dragged ON FOOT,
  where the game draws them into the 2D panel. The gate keeps the panel in
  the eye route (the journal says on foot), nothing names its camera, and
  DLSS gets head-only motion (section 1). The cockpit's stereo maps are
  fine: a regression check, not a phase.
- **Recommendation:** Phase 1 is option (i): on foot the panel counts as the
  world only while a draw that reads the world camera names it; otherwise the
  UI layer composites it after the upscaler. Option (ii), the map's own
  camera, waits on Phase 0 (section 4).
- **Phases (section 6):** 0 census episodes, 1 the on-foot panel gate, 2 the
  cockpit's rows from its eye cameras, 3 the rest. The VR world route's
  stage 2 (another worktree) is untouched.
- **Open hypotheses:** H1: on-foot maps and menus never name a source
  (flight 1, both gaps: "no pool family draw went to a depth of the screen's
  size"). H2: in an on-foot world, naming never drops for 3 frames (station:
  none outside the gaps in 14,220 frames; settlements unmeasured). H3: the
  cockpit maps are fine because kind-5 eye cameras' rows drive the world
  path (code reading).
- **Ruled out:** end of section 1.
- **Next flight:** Phase 0 (section 6); Phase 1's key toggled live in a
  second on-foot leg if both ride one build (question 2).
- **Environment:** EDVR's OpenXR runtime; Pimax Crystal Super, 90 Hz, HMD
  quality 0.65 (eye 2620x2533, output 4032x3898); 2D screen 5040x2835
  (`fix.vscreen_res_width` auto); DLSS preset K. Phase 1 needs the UI layer
  live (`fix.ui_quality` above 0 and `fix.temporal_aa` on, the defaults);
  with it off, or under the Oculus native SDK, the on-foot map keeps its
  smear.

## 1. The trigger

On foot Odyssey draws the world once into the 2D screen's texture; each eye
shows it through one composite draw (VS 5C36AF05, PS CFE84157). A pool-family
or terrain draw into a screen-sized depth names the world camera (its b1)
and depth for screen motion (screen_motion.cpp:260-315), which maps the
motion to eye pixels at each composite (:387-506). The on-foot maps make no
such draw. Flight 1, gaps 16:22:41.9-16:22:59.96 and 16:23:01.3-16:23:12.0:

- 16:22:41.911 `auxiliary camera rows do not follow the head`; 16:22:43.103
  `the 2D screen showed for 90 frames and nothing named its source`.
- 30 s world-screen lines: held by the journal alone on 238 frames
  (16:22:45.8) and 1,269 (16:23:15.8); screen depth 22 draws a frame; the
  screen `left in the picture ... the temporal pass keeps it`; GuiFocus
  unknown on every on-foot frame. DLSS ran on it (2.5 ms a pair).

With no source screenMotionView is null (:507-510), and on foot the eyes have
no scene depth (16:20:05.790), so the pass runs on the head's rotation alone
(temporal_pass.cpp:3533-3545): the panel as a still picture at infinity,
while a drag moves its content. The world route released as designed (2,396
`depth-not-screen-motion-source` declines, section 82). The gate's comment
expected a map to be held like the world (ui_layer_math.h:902-907); on foot
no camera names one.

**Why the cockpit maps are fine (H3).** There kind-5 eye cameras (the census
saw the left eye's object as kind 5 in the cockpit) draw the map into eye
targets with an eye-sized depth. The pass's rows come from the block bound at
the scene's first draw (temporal_pass.cpp:6650-6680), so they carry the head
and the map's orbit; a bound block in a populated scene pins the follow score
at 30 (temporal_math.h:50); pixels past 10 m take the rows' delta
(temporal_shader_source.h:716-722). A drag moves the rows and the vectors
follow: the camera that renders the content drives its motion.

- ruled out: Status.json's GuiFocus as the on-foot map signal, because every
  on-foot frame of flight 1 read it unknown, maps included.
- ruled out: rejecting the unnamed panel's history, or a reactive mask, as
  the fix, because DLSS would still be told the content is still.
- ruled out: the screen's depth count as the world witness, because a 3D map
  draws into the same depth (22 a frame here; a busier one could pass the
  64-draw hold); only a draw that reads the world camera proves the world.

## 2. Inventory

| # | Where | Uses | When | Guards against |
|---|---|---|---|---|
| 1 | eye shift, native_temporal.cpp:349-360 | Halton tangent shift | unless the route owns | (upscaler samples) |
| 2 | head delta, native_temporal.cpp:436-461 | runtime poses | every eye | (no other source) |
| 3 | rotation alone, temporal_pass.cpp:3439-3447, 3533-3545 | item 2 | no eye depth (on foot) | translation without depth |
| 4 | head with depth, temporal_pass.cpp:3564-3579, 3787-3795 | item 2, translation | cockpit | near parallax |
| 5 | world rows, temporal_pass.cpp:6599-6648, 6650-6680, 2556-2717 | scene-block writes at float 932; continuity, bound block | cockpit | 100+ writes a frame, mostly auxiliary |
| 6 | follow score, temporal_math.h:44-53; temporal_pass.cpp:3665-3674 | rows' turn vs the head's | unbound, or under 8 draws | menu backdrops, station chains (09-04, 09-08) |
| 7 | camera gate, temporal_math.h:355-444; temporal_pass.cpp:3686-3707 | 3 deg, unturned rows, 50 m | cockpit | another camera; a parked zero (4,674-frame stays); origin jumps |
| 8 | floor, split, temporal_math.h:42; temporal_pass.cpp:3809-3819; shader :681-733 | 8 draws; 10 m | cockpit | menu backdrops; ship geometry |
| 9 | FSS interface, temporal_pass.cpp:3878-3890; shader :715 | head for UI pixels | scanner up | a panning camera, a still panel |
| 10 | screen motion, screen_motion.cpp:260-315, 387-506; shader :670-680 | naming draw's b1, depth | on foot, named | (exact; GUI panel-fixed) |
| 11 | engine records, engine_velocity.cpp:2453-2490, 2521-2576; shader :736 | poses, draws' b1 now/last | cockpit; on-foot source | (exact per object) |
| 12 | world route, vr_world_route.cpp:204-336, 482 | item 11's rows | on foot, key auto | (one resolve) |
| 13 | flat, flat_runtime.cpp:2770-2794; flat_camera_inject.cpp:404-460, 732; flat_camera_phase.h:45-60, 236-246 | b1[270..275], kind-3 injector, rowsJitter | flat | (C3) |
| 14 | transition flash, glitch_frame.cpp:170-219; native_temporal.cpp:479-491 | magnitude floor 250 | VR | composition read as a jump |

Explorer Cam's game read is gone (explorer-cam.md); external views go
through items 5-8. Items 5-8 and 14 exist because EDVR reads rows from
anonymous buffer writes and must guess the camera; the census's detour
(vr_camera_census.cpp:95-170) names it by kind, caller and place.

## 3. The world camera, view by view

- **Cockpit:** kind-5 eye cameras, known by kind at the refresh (exact over
  4.63 million calls) and by joining an eye scene draw's b1 to a call's
  rows. Correct with depth; ship geometry takes engine records where
  certified, else the split; holograms and HUD keep their paths. Unknown:
  calls and call sites per frame. Cockpit maps: presumably the same (H3).
- **On-foot maps:** some camera drawing into the panel. The 60 s census
  window over both gaps (16:23:22) implies about 30 kind-3 calls a map frame
  (28-34; the world has 73-79), no kind 1, kind 5 still 6.0: an estimate.
  Unknown: which camera, whether stars, lines and labels write depth,
  whether b1 holds a scene block.
- **On foot:** the kind-3 world camera (54 refreshes a frame, before the
  tone), named by screen motion; the helmet HUD is GUI, masked panel-fixed;
  the weapon has its own camera.
- **FSS, DSS:** a panning scanner camera, a head-locked interface, a mono
  zoomed body (fss-scanner.md). Kinds unknown.
- **Explorer Cam, external views:** stereo, presumably eye cameras.
- **Main menu, loading:** 2D screens, layer; the VR menu backdrop's camera
  ignores the head (item 6), kind unknown.
- **Flat:** kind 3 through the injector, already the world camera.

## 4. A panel that is not the world: two options

**(i) The world camera decides the gate.** On foot the panel stays in the
eye route only while screen motion names a source; otherwise the layer takes
it. A pure step in ui_layer_math.h: 2 named frames hold, 3 unnamed release (3
is the world route's grace, so both let go on one boundary). With the key on
and screen motion live it alone decides; otherwise the gate is today's
`byJournal || byDepth` (ui_layer.cpp:2118-2186). It needs recognition of the
composite whoever takes it (today only the route's re-issue,
vscreen.cpp:4735), or naming stops two frames after a take
(screen_motion.cpp:261) and the gate never holds again; the last completed
frame's verdict, whichever boundary runs first (ui_layer.cpp:4260); and a
release line with the reason.

Handover. Opening a map: 3 frames through DLSS as a cut, then the layer
composites the map after the upscaler; the route, if on, releases on the
same boundary and the eye shift returns. Closing: the world is named on the
first or second frame, the gate holds two later, the route warms 8 and owns.
The eye history under a taken panel is black; DLSS clamps it on return, as at
every disembark (16:20:05.76-.79); a visible dark fade would call for a reset
at the hold edge (a cut, not the fix). Side effects: on-foot terminal menus
go to the layer too; thin map lines may shimmer under head motion, as the
layer samples the 5040-wide texture at mip 0 (cure: the route's mipped
screen, vr_world_mips).

**(ii) The map's own camera.** A map naming rule in screen motion: the map's
scene draws into the screen-sized depth name its camera and depth, and the
panel keeps DLSS with true motion. It needs the census to name the camera,
draw hashes and b1 layout, and depth on the moving content. Depthless stars
cannot be reprojected under pan or zoom by any camera, and labels drawn as
GUI would get panel-fixed motion while their stars move: a new smear.

**Recommendation: (i).** It removes the false claim at its source (the
journal, not a camera, declared the panel the world), hands DLSS nothing it
has no vectors for, needs no census, and is a predicate plus one call site.
(ii) needs depth on moving stars and labels, for map AA nobody has asked for.

## 5. What it replaces, and the risks

- Chooser and follow score (5-6): the view's camera is the kind-5 call whose
  composed rows equal the eye scene draw's b1; kinds 0, 1 and other kind-3
  objects never are. The score becomes a logged assertion.
- Another and Parked (7): a parked camera is one not refreshed this frame;
  the refresh count says so, nothing is carried. The 50 m jump stays.
- Rotation alone (3): only frames with neither camera nor depth; (i) takes
  the unnamed panels away from it.
- Eye jitter: kind-5 projections carry EDVR's shift, the view axes (camera
  +0x20, float-932 rows) do not; composed-row consumers remove it, as the
  engine and hologram motion already do (temporal_shader_source.h:519-575).
- Stage 2's phase: the route removes it from the kind-3 rows
  (flat_mono_shader_source.h:48-51, 92-95). Cautions: in the route's grace
  frames the eye route serves a jittered world whose screen-motion rows keep
  the phase (under half a source pixel, 3 frames); a map frame refreshes
  about 30 kind-3 calls, so shut the window after an unnamed frame.

## 6. Phases

**Phase 0: census episodes.** Key `advanced.vr_camera_census` (existing);
key off unchanged (nothing installed, allocated or logged; operator-new rig).
Today it records the session's first three on-foot frames only and never an
aboard frame (vr_camera_census_core.h:240-244, 522-531): it cannot see the
maps or the cockpit. Extend: (a) an episode, one sequence and one join 30
frames after a journal on-foot flip, a naming flip held 3 frames, a GuiFocus
change or key-on, aboard frames included, 10 a session (the call-line cap
grows to fit); (b) the join: at that frame's first draw into a screen-sized
or eye-sized depth, per eye, VS/PS, depth write, b1 size, b1 rows 270..273
read back and matched to the calls, and the pass's chosen rows matched to the
calls' view axes; (c) on-foot named and unnamed run lengths (1, 2, 3, 4-8,
9-30, 31-89, 90+) in the 5 s line; (d) the detour's own CPU; (e)
`--camera-census` per episode. Rigs: vr_camera_census_test and the reader's
self-test, both in build.bat's gate.
Flight (Frontier, the environment above; live `edvr.ini` by the Edit tool):
`advanced.vr_camera_census = on`; `fix.panel_curvature = 0` (the join reads
the eye composite, which a curved screen replaces);
`experimental.temporal_aa_on_foot_world = off` (today's eye route is the
reference). Note the clock at each open and close: cockpit 30 s; its galaxy
map (5 s still, 10 s drag and rotate, 5 s zoom), close; its system map the
same; disembark; 30 s walking; the on-foot galaxy and system maps the same,
10 s of world between; a terminal menu 10 s; board. Census off after. Read,
each with
`--target frontier --expect-build HEAD`: `--version`, `--camera-census`,
`--grep "vr camera census|world screen|screen motion:|auxiliary camera"`.
Answers: H1; H2 (the longest unnamed run in the world stays under 3, else
the release count goes above it); H3; the map's camera, depth and GUI pairs
for (ii); the cockpit's kind-5 calls and where the chooser strays; the
detour's CPU.

**Phase 1: the panel is the world only when the world camera names it**
(option (i)). Key `experimental.on_foot_maps_sharp = off|on`, default off,
on the in-VR Experimental page. Key off: the gate equals a frozen copy of
today's for every input (ui_quality_test, exhaustive), recognition unchanged
(source pin), no new line. Rigs: ui_quality_test (the step; flight 1's runs
replayed, 13,044 named, 1,597 unnamed, 98 named, 929 unnamed, then named:
a hold and four flips), screen_motion_test (recognition under a take),
vr_world_route_test (release with the gate); mutants. Flight: Phase 0's
on-foot legs, key toggled off then on, one map with the world route on.
PASS: a release within 3 frames of each map or menu opening, a hold within 3
of closing, no flip in the world, the map sharp under a drag, HUD intact,
route and gate letting go together. STOP: a release in the world, flapping,
a black eye, a lasting dark fade. WATCH: map lines shimmering (mips).

**Phase 2: the cockpit's rows from its eye cameras.** Key
`experimental.temporal_aa_game_camera = off|auto`. The detour, a quiet
observer in VR, publishes each eye's kind-5 view axes once a frame (eye by
the join or the off-centre sign); the world path takes them instead of the
chooser; score and gate become logged assertions. Key off: the chooser byte
for byte (eye-run traces replayed). Rigs: replay of Phase 0's episodes and
traces, the join convention, mutants. Flight: camera-rows-carry's roll and
turn protocol, a station approach, a supercruise arrival; cockpit maps and
FSS as regressions. PASS: no carried frame; docked, rows turn with the head.

**Phase 3: the rest,** each on its census: FSS and DSS (scanner camera for
the scene, head for the interface), external views, the menu backdrop, and
(ii) if wanted.

## 7. Open questions for Sean

1. Phase 1 as (i) or (ii)? Recommend (i), for section 4's reasons.
2. Phase 0 and 1 in one build and flight, Phase 1's key live in a second
   on-foot leg? Recommend yes: Phase 1 does not depend on the census.
3. On-foot terminal and station menus to the layer too (sharp, no temporal
   AA)? Recommend yes: the gate's comment sends static screens there.
4. Later, skip the eye upscaler under a layer-held non-world panel (a black
   eye, 2.5 ms a pair for nothing)? Recommend yes, as its own change.
5. The detour in VR by default once Phase 2 flies (observe-only; 4.63
   million calls without a fault)? Recommend yes, behind its key, once
   Phase 0 has measured its CPU.

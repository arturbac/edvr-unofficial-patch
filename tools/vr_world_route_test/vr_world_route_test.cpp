// The VR on-foot world route: the pure half and the detector's glue (docs/design-flat-temporal-aa-2026-09-23.md, section 82).
//
// WHAT IS PINNED
//   1. THE KEY-OFF CONTRACT: with experimental.temporal_aa_on_foot_world off (or any value that is not "auto") the machine
//      never leaves Off whatever else the frame shows, nothing is owned, the eye shift is never suppressed, the layer is
//      never told the world is the route's, the door never runs layer-only; and the VR hooks' added lines are guarded so
//      that with the key off each costs one load (source scans).
//   2. THE OWNERSHIP MACHINE: warm-up, ownership, the grace for single refusals, release by the gate, the layer, a scene
//      reset, a run of untreated frames and the late-write latch (with its reset by the key).
//   3. THE SELECTOR: each reason in order, each alone.
//   4. THE TRIGGER on the VR on-foot chain the 2026-09-30 census retake showed (design doc section 82, census retake table).
//      THERE IS NO TRACE of that chain: the fixture below is SYNTHETIC, built from the retake's chain table (its q numbers,
//      sizes, formats, who reads what) and the 66 census lines of q 8150-8215 that were read from the log
//      (edvr_gfx_20260930_104653.log). It runs the real flatHdrObserveDraw through the route's own glue
//      (vrWorldFillObservation) and asserts the trigger is q 8203, the tone, with H at t1, and only there, with each of the
//      rules (i)-(iv) and the mid-frame copy of H and the late writes mutated in turn.
//   5. THE 5 s LINE and the take/release lines, bounded.
//   6. SOURCE PINS for the hooks (the internal-call early returns on the nine thunks, the detector's call sites, the write
//      observers, the boundary tick) and for what the route must not do (install the camera detour, jitter the world in this
//      build).
// --self-test <repo root> runs everything and prints "vr world route: PASS" only when every check holds.

#include "../../src/d3d11/vr_world_route_math.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace edvr;

int g_failures = 0;
int g_checks = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) { std::printf("  ok    %s\n", what); return; }
    ++g_failures;
    std::printf("  FAIL  %s\n", what);
}

// ---- 1. the key ------------------------------------------------------------------------------------------------------
void keyCases() {
    check(vrWorldKeyFromText("auto") == VrWorldKey::Auto, "key: auto reads auto");
    check(vrWorldKeyFromText("AUTO") == VrWorldKey::Auto && vrWorldKeyFromText("Auto") == VrWorldKey::Auto,
          "key: any case of auto reads auto");
    const char* notAuto[] = {"off", "", "on", "autox", "aut", "auto ", " auto", "true", "1", "dlss", "automatic", "yes"};
    bool allOff = true;
    for (const char* t : notAuto) allOff = allOff && vrWorldKeyFromText(t) == VrWorldKey::Off;
    check(allOff && vrWorldKeyFromText(nullptr) == VrWorldKey::Off,
          "key: anything that is not exactly auto, a typo included, reads off: the route is never switched on by accident");
    check(std::strcmp(vrWorldKeyName(VrWorldKey::Auto), "auto") == 0 && std::strcmp(vrWorldKeyName(VrWorldKey::Off), "off") == 0,
          "key: the names are the ini's words");
}

// ---- 2. the machine --------------------------------------------------------------------------------------------------
VrWorldFrameEnd frame(bool key, bool layer, bool gate, bool treated, bool late = false, bool reset = false) {
    VrWorldFrameEnd f;
    f.keyOn = key; f.layerLive = layer; f.gate = gate; f.treated = treated; f.lateWrites = late; f.sceneReset = reset;
    return f;
}
VrWorldStep run(VrWorldMachine& m, unsigned n, const VrWorldFrameEnd& f) {
    VrWorldStep last;
    for (unsigned i = 0; i < n; ++i) last = m.frameEnd(f);
    return last;
}
VrWorldMachine owned() {
    VrWorldMachine m;
    run(m, kVrWorldWarmFrames, frame(true, true, true, true));
    return m;
}

void keyOffContract() {
    // With the key off, whatever the frames show, the route is not there.
    VrWorldMachine m;
    bool neverOff = true, neverOwned = true, neverWants = true, neverEvents = true, neverSuppress = true, neverTake = true;
    uint32_t bits = 1;
    for (unsigned i = 0; i < 4096; ++i) {
        bits = bits * 1664525u + 1013904223u;
        VrWorldFrameEnd f = frame(false, bits & 1u, bits & 2u, bits & 4u, bits & 8u, bits & 16u);
        const VrWorldStep s = m.frameEnd(f);
        neverOff = neverOff && m.state == VrWorldState::Off;
        neverOwned = neverOwned && !m.owned();
        neverWants = neverWants && !m.wantsDraws();
        neverEvents = neverEvents && !s.entered && s.released == VrWorldRelease::None;
        neverSuppress = neverSuppress && !vrWorldSuppressesEyeShift(m.state);
        neverTake = neverTake && !vrWorldLayerMayTake(m.state, (bits & 4u) != 0) && !vrWorldDoorLayerOnly(0, bits);
    }
    check(neverOff && neverOwned && neverWants && neverEvents,
          "key off: 4096 frames of every combination of the other facts leave the machine Off, unowned, wanting no draws, with no events");
    check(neverSuppress && neverTake,
          "key off: the eye shift is never suppressed, the layer is never told the world is the route's, the door never runs layer-only");
    // A route that was owned when the key goes off lets go AT ONCE, with its reason.
    VrWorldMachine o = owned();
    const VrWorldStep s = o.frameEnd(frame(false, true, true, true));
    check(o.state == VrWorldState::Off && s.released == VrWorldRelease::KeyOff && !o.owned(),
          "key off: an owned route lets go at once and says why");
}

void machineCases() {
    VrWorldMachine m;
    check(m.state == VrWorldState::Off && !m.owned() && !m.wantsDraws(), "machine: starts Off");
    // The key comes on: the first frame is observed, a treated frame warms.
    m.frameEnd(frame(true, true, true, false));
    check(m.state == VrWorldState::Observing && m.wantsDraws() && !m.owned(), "machine: key on with no treated frame is Observing and wants draws");
    VrWorldStep s = m.frameEnd(frame(true, true, true, true));
    check(m.state == VrWorldState::Warming && !m.owned() && !s.entered, "machine: a treated frame makes it Warming");
    bool warm = true;
    for (unsigned i = 2; i < kVrWorldWarmFrames; ++i) {
        s = m.frameEnd(frame(true, true, true, true));
        warm = warm && m.state == VrWorldState::Warming && !s.entered;
    }
    check(warm, "machine: still Warming one treated frame short of the warm-up");
    s = m.frameEnd(frame(true, true, true, true));
    check(m.state == VrWorldState::Owned && m.owned() && s.entered && s.released == VrWorldRelease::None,
          "machine: the kVrWorldWarmFrames-th treated frame in a row makes it Owned, once");
    s = m.frameEnd(frame(true, true, true, true));
    check(m.owned() && !s.entered, "machine: an owned route that keeps treating stays owned and does not re-enter");

    // The warm-up is CONSECUTIVE frames: one refusal restarts it.
    VrWorldMachine w;
    run(w, kVrWorldWarmFrames - 1, frame(true, true, true, true));
    w.frameEnd(frame(true, true, true, false));
    check(w.state == VrWorldState::Observing, "machine: a refusal while warming drops to Observing");
    run(w, kVrWorldWarmFrames - 1, frame(true, true, true, true));
    check(!w.owned(), "machine: and the warm-up starts over (kVrWorldWarmFrames - 1 more treated frames is not enough)");
    w.frameEnd(frame(true, true, true, true));
    check(w.owned(), "machine: ... the next treated frame completes a fresh run");

    // The grace: a single refusal is not a release; kVrWorldGraceFrames in a row is.
    VrWorldMachine g = owned();
    bool graced = true;
    for (unsigned i = 0; i + 1 < kVrWorldGraceFrames; ++i) {
        s = g.frameEnd(frame(true, true, true, false));
        graced = graced && g.owned() && s.released == VrWorldRelease::None;
    }
    check(graced, "machine: fewer than kVrWorldGraceFrames untreated frames in a row keep the route owned");
    g.frameEnd(frame(true, true, true, true));
    for (unsigned i = 0; i + 1 < kVrWorldGraceFrames; ++i) g.frameEnd(frame(true, true, true, false));
    check(g.owned(), "machine: a treated frame in between resets the count of untreated ones");
    s = g.frameEnd(frame(true, true, true, false));
    check(!g.owned() && g.state == VrWorldState::Observing && s.released == VrWorldRelease::Declined,
          "machine: kVrWorldGraceFrames untreated frames in a row release it, with the reason frames-not-treated");

    // Releases by the gate, the layer and a scene reset, each immediate, each named; a warming route drops without an event.
    VrWorldMachine a = owned();
    s = a.frameEnd(frame(true, true, false, true));
    check(a.state == VrWorldState::Observing && s.released == VrWorldRelease::GateLost,
          "machine: the gate lost releases an owned route at once (on-foot-gate-lost)");
    VrWorldMachine b = owned();
    s = b.frameEnd(frame(true, false, true, true));
    check(b.state == VrWorldState::Observing && s.released == VrWorldRelease::LayerNotLive, "machine: the layer not live releases at once (layer-not-live)");
    VrWorldMachine c = owned();
    s = c.frameEnd(frame(true, true, true, true, false, true));
    check(c.state == VrWorldState::Observing && s.released == VrWorldRelease::SceneReset, "machine: a scene reset releases at once (scene-reset)");
    run(c, kVrWorldWarmFrames - 1, frame(true, true, true, true));
    check(!c.owned(), "machine: after a scene reset the warm-up starts over");
    VrWorldMachine d;
    run(d, 3, frame(true, true, true, true));
    s = d.frameEnd(frame(true, true, false, true));
    check(d.state == VrWorldState::Observing && s.released == VrWorldRelease::None, "machine: a warming route that loses the gate drops to Observing with no release event");

    // The latch: three treated frames with late writes turn the route off until the key is flipped.
    VrWorldMachine l = owned();
    s = l.frameEnd(frame(true, true, true, true, true));
    s = l.frameEnd(frame(true, true, true, true, true));
    check(l.owned(), "latch: two treated frames with late writes do not trip it");
    s = l.frameEnd(frame(true, true, true, true, true));
    check(l.state == VrWorldState::Latched && !l.owned() && !l.wantsDraws() && s.released == VrWorldRelease::Latched,
          "latch: the third treated frame with late writes latches the route, and an owned route says so");
    run(l, 100, frame(true, true, true, true));
    check(l.state == VrWorldState::Latched, "latch: it stays latched with the key on whatever the frames show");
    l.frameEnd(frame(false, true, true, true));
    check(l.state == VrWorldState::Off, "latch: the key off ends it");
    l.frameEnd(frame(true, true, true, false));
    check(l.state == VrWorldState::Observing, "latch: and the key back on starts the route afresh");
    VrWorldMachine scattered = owned();
    for (int i = 0; i < 3; ++i) { scattered.frameEnd(frame(true, true, true, true, true)); scattered.frameEnd(frame(true, true, true, true, false)); }
    check(scattered.state == VrWorldState::Latched, "latch: it counts frames with late writes, not a streak: three scattered ones trip it too");

    // The state names and release names are distinct words.
    bool distinct = true;
    const VrWorldState states[] = {VrWorldState::Off, VrWorldState::Observing, VrWorldState::Warming, VrWorldState::Owned, VrWorldState::Latched};
    for (size_t i = 0; i < 5; ++i) for (size_t j = i + 1; j < 5; ++j) distinct = distinct && std::strcmp(vrWorldStateName(states[i]), vrWorldStateName(states[j])) != 0;
    const VrWorldRelease rel[] = {VrWorldRelease::None, VrWorldRelease::KeyOff, VrWorldRelease::LayerNotLive, VrWorldRelease::GateLost,
                                  VrWorldRelease::Declined, VrWorldRelease::SceneReset, VrWorldRelease::Latched};
    for (size_t i = 0; i < 7; ++i) for (size_t j = i + 1; j < 7; ++j) distinct = distinct && std::strcmp(vrWorldReleaseName(rel[i]), vrWorldReleaseName(rel[j])) != 0;
    check(distinct, "names: every state and every release reason has its own name");
}

void predicateCases() {
    const VrWorldState all[] = {VrWorldState::Off, VrWorldState::Observing, VrWorldState::Warming, VrWorldState::Owned, VrWorldState::Latched};
    bool suppress = true, take = true;
    for (VrWorldState s : all) {
        suppress = suppress && vrWorldSuppressesEyeShift(s) == (s == VrWorldState::Owned);
        for (bool treated : {false, true}) take = take && vrWorldLayerMayTake(s, treated) == (s == VrWorldState::Owned && treated);
    }
    check(suppress, "eye shift: suppressed in exactly one state, Owned (the frame after ownership began, until it ends)");
    check(take, "layer: may take the screen draw only when the route is Owned AND treated this frame");
    check(!vrWorldDoorLayerOnly(0, 0) && !vrWorldDoorLayerOnly(0, 5) && !vrWorldDoorLayerOnly(4, 5) && vrWorldDoorLayerOnly(5, 5),
          "door: layer-only only for an eye whose screen draw the layer took in THIS sequence; sequence 0 is never");
}

// ---- 3. the selector -------------------------------------------------------------------------------------------------
VrWorldSelectFacts goodFacts() {
    VrWorldSelectFacts f;
    f.triggered = true; f.ambiguous = false; f.depthKnown = true; f.depthNamed = true; f.engineViews = true;
    f.cameraRows = true; f.extentOk = true; f.engineKnown = true;
    return f;
}
void selectorCases() {
    check(vrWorldSelect(goodFacts()) == VrWorldSelect::Selected, "selector: every fact good selects the frame");
    struct Case { const char* what; void (*mut)(VrWorldSelectFacts&); VrWorldSelect want; };
    const Case cases[] = {
        {"no trigger", [](VrWorldSelectFacts& f) { f.triggered = false; }, VrWorldSelect::NoTrigger},
        {"ambiguous H", [](VrWorldSelectFacts& f) { f.ambiguous = true; }, VrWorldSelect::Ambiguous},
        {"no engine", [](VrWorldSelectFacts& f) { f.engineKnown = false; }, VrWorldSelect::NoEngine},
        {"extent", [](VrWorldSelectFacts& f) { f.extentOk = false; }, VrWorldSelect::ExtentMismatch},
        {"depth mixed", [](VrWorldSelectFacts& f) { f.depthKnown = false; }, VrWorldSelect::DepthMixed},
        {"depth not named", [](VrWorldSelectFacts& f) { f.depthNamed = false; }, VrWorldSelect::DepthNotNamed},
        {"engine views", [](VrWorldSelectFacts& f) { f.engineViews = false; }, VrWorldSelect::NoEngineViews},
        {"camera rows", [](VrWorldSelectFacts& f) { f.cameraRows = false; }, VrWorldSelect::NoCameraRows},
    };
    for (const auto& c : cases) {
        VrWorldSelectFacts f = goodFacts();
        c.mut(f);
        char what[160];
        std::snprintf(what, sizeof(what), "selector: %s alone refuses the frame with its own reason", c.what);
        check(vrWorldSelect(f) == c.want, what);
    }
    // The order of the tests is the order of the reasons: for EVERY pair of facts false at once the earlier test's reason
    // answers (a swap of any two tests in the function changes one of these).
    bool orderOk = true;
    const size_t cn = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < cn; ++i)
        for (size_t j = i + 1; j < cn; ++j) {
            VrWorldSelectFacts f = goodFacts();
            cases[i].mut(f);
            cases[j].mut(f);
            orderOk = orderOk && vrWorldSelect(f) == cases[i].want;
        }
    check(orderOk, "selector: every pair of false facts reports the earlier test's reason (the order is the contract)");
    VrWorldSelectFacts none;
    check(vrWorldSelect(none) == VrWorldSelect::NoTrigger, "selector: with every fact false the first test answers (no trigger)");
    VrWorldSelectFacts two = goodFacts();
    two.depthNamed = false; two.cameraRows = false;
    check(vrWorldSelect(two) == VrWorldSelect::DepthNotNamed, "selector: two failures report the earlier test's reason");
    bool distinct = true;
    const VrWorldSelect names[] = {VrWorldSelect::Selected, VrWorldSelect::NoTrigger, VrWorldSelect::Ambiguous, VrWorldSelect::NoEngine,
                                   VrWorldSelect::ExtentMismatch, VrWorldSelect::DepthMixed, VrWorldSelect::DepthNotNamed,
                                   VrWorldSelect::NoEngineViews, VrWorldSelect::NoCameraRows};
    for (size_t i = 0; i < 9; ++i) for (size_t j = i + 1; j < 9; ++j) distinct = distinct && std::strcmp(vrWorldSelectName(names[i]), vrWorldSelectName(names[j])) != 0;
    check(distinct, "selector: every reason has its own name");
}

// ---- 4. the glue and the trigger on the retake chain ---------------------------------------------------------------------
// Resources (identity only). Sizes and formats as the census retake showed them (design doc section 82).
int g_res[32];
const void* R(int i) { return &g_res[i]; }
enum { RH = 1, RDEPTH, RG2, RG4, RCOPY, RFOG, RSMALL1, RSMALL2, RSMALL3, RTONE, RSCREEN, RHUDDEPTH, REYE0, REYE1, RH2, RDEPTH2, RBLOOM };
constexpr uint32_t W = 5040, Hh = 2835;
constexpr uint32_t F_H = 26, F_RGBA8 = 27, F_R10 = 23;

VrWorldView view(int res, uint32_t w, uint32_t h, uint32_t fmt, bool known = true) {
    VrWorldView v;
    v.resource = R(res); v.width = w; v.height = h; v.format = fmt; v.known = known; v.texture2d = true;
    return v;
}
struct Drawn { uint32_t q; int target; uint32_t tw, th, tf; int depth; uint32_t dw, dh; int srv[4]; uint64_t vs, ps; };
struct Replay {
    FlatHdrFrame f;
    FlatContractObservation k;
    uint32_t triggerQ = 0, triggers = 0;
    Replay() { flatHdrBeginFrame(f, W, Hh); }
    bool draw(const Drawn& d) {
        VrWorldView c = view(d.target, d.tw, d.th, d.tf);
        VrWorldView dv = d.depth ? view(d.depth, d.dw, d.dh, 39) : VrWorldView{};
        if (!vrWorldFillObservation(k, &c, d.depth ? &dv : nullptr, &c, d.depth ? &dv : nullptr, d.vs, d.ps)) return false;
        const void* srv[4] = {};
        bool known = false;
        if (!f.triggered && flatHdrCouldConsume(f, k)) {
            for (int i = 0; i < 4; ++i) srv[i] = d.srv[i] ? R(d.srv[i]) : nullptr;
            known = true;
        }
        const bool t = flatHdrObserveDraw(f, k, d.q, srv, known);
        if (t) { triggerQ = d.q; ++triggers; }
        return t;
    }
};
Drawn none() { Drawn d{}; return d; }
Drawn into(uint32_t q, int target, uint32_t w, uint32_t h, uint32_t fmt, int depth = 0) {
    Drawn d = none();
    d.q = q; d.target = target; d.tw = w; d.th = h; d.tf = fmt; d.depth = depth; d.dw = w; d.dh = h;
    return d;
}

// The retake chain, q numbers as the census numbered them. World draws into the G-buffer and H's draws carry the screen
// depth; the exposure reduction runs on a COPY of H (a copy, not a draw); the tone reads H at t1.
void chain(Replay& r, bool withTone = true, int toneDepth = 0, int toneTarget = RTONE, bool toneReadsH = true,
           uint32_t smallW = 630, uint32_t smallH = 354) {
    for (uint32_t q = 3794; q < 3800; ++q) r.draw(into(q, RG2, W, Hh, F_R10, RDEPTH));          // the world: G-buffer with the screen depth
    for (uint32_t q = 7846; q <= 7856; ++q) r.draw(into(q, RH, W, Hh, F_H, RDEPTH));            // 11 deferred-light draws into H
    for (uint32_t q = 7859; q < 7870; ++q) r.draw(into(q, RH, W, Hh, F_H, RDEPTH));             // sky, glass, particles, holograms
    // q 8157: CopySubresourceRegion(H -> RCOPY): not a draw, nothing reaches the detector.
    Drawn e = into(8158, RBLOOM, 1261, 709, F_RGBA8); e.srv[0] = RCOPY; r.draw(e);              // the exposure reduction reads the COPY
    e = into(8159, RFOG, 320, 180, F_RGBA8); e.srv[0] = RCOPY; r.draw(e);
    for (uint32_t q = 8162; q <= 8196; ++q) { Drawn l = into(q, RH, W, Hh, F_H, RDEPTH); l.srv[1] = RCOPY; r.draw(l); }   // 35 late draws
    for (uint32_t q = 8198; q <= 8200; ++q) { Drawn s = into(q, RSMALL1, smallW, smallH, F_RGBA8); s.srv[0] = RH; r.draw(s); }   // 630x354 reading H
    if (withTone) {
        Drawn t = into(8203, toneTarget, W, Hh, F_RGBA8, toneDepth);                              // THE TONE: no depth, not H, reads H at t1
        t.srv[0] = RFOG; t.srv[1] = toneReadsH ? RH : RCOPY;
        t.vs = 0xF9CFC798F21E9AEAull; t.ps = 0xFEE777E92850B390ull;
        r.draw(t);
    }
}
void afterTone(Replay& r) {
    Drawn c = into(8204, RSCREEN, W, Hh, F_RGBA8); c.srv[0] = RTONE; r.draw(c);                   // the game copy into the screen texture
    for (uint32_t q = 8205; q <= 8211; ++q) { Drawn h = into(q, RSCREEN, W, Hh, F_RGBA8, RHUDDEPTH); r.draw(h); }   // the HUD, own depth
    Drawn e0 = into(8213, REYE0, 2620, 2533, F_RGBA8); e0.srv[0] = RSCREEN; r.draw(e0);           // the eye composites
    Drawn e1 = into(8220, REYE1, 2620, 2533, F_RGBA8); e1.srv[0] = RSCREEN; r.draw(e1);
}

void glueCases() {
    FlatContractObservation k;
    VrWorldView c = view(RH, W, Hh, F_H), d = view(RDEPTH, W, Hh, 39);
    check(vrWorldFillObservation(k, &c, &d, R(100), R(101), 7, 8) && k.color == R(RH) && k.depth == R(RDEPTH) && k.dsv == R(101) &&
              k.rtv == R(100) && k.width == W && k.height == Hh && k.format == F_H && k.depthWidth == W && k.depthHeight == Hh && k.vs == 7 && k.ps == 8,
          "glue: a coloured draw with a readable depth fills every field the detector reads");
    VrWorldView unknown = view(RH, W, Hh, F_H, false);
    check(!vrWorldFillObservation(k, &unknown, nullptr, nullptr, nullptr, 0, 0) && !vrWorldFillObservation(k, nullptr, nullptr, nullptr, nullptr, 0, 0),
          "glue: a colour target the shadow could not resolve is ignored");
    VrWorldView buffer = c; buffer.texture2d = false;
    check(!vrWorldFillObservation(k, &buffer, nullptr, nullptr, nullptr, 0, 0), "glue: a colour target that is not a 2D texture is ignored");
    VrWorldView badDepth = view(RDEPTH, W, Hh, 39, false);
    FlatContractObservation k2;
    check(vrWorldFillObservation(k2, &c, &badDepth, R(100), R(101), 0, 0) && k2.depth == nullptr && k2.dsv == R(101),
          "glue: a depth target the shadow could not read still counts as a bound depth (dsv set, resource null)");
    FlatHdrFrame f;
    flatHdrBeginFrame(f, W, Hh);
    FlatContractObservation h;
    VrWorldView hv = view(RH, W, Hh, F_H), dv = view(RDEPTH, W, Hh, 39);
    vrWorldFillObservation(h, &hv, &dv, R(100), R(101), 0, 0);
    flatHdrObserveDraw(f, h, 1, nullptr, false);
    FlatContractObservation tone;
    VrWorldView tv = view(RTONE, W, Hh, F_RGBA8);
    vrWorldFillObservation(tone, &tv, &badDepth, R(102), R(103), 0, 0);
    check(!flatHdrCouldConsume(f, tone), "glue: rule (i) is 'no depth bound': an unreadable bound depth is not 'none', so it cannot be the consumer");
}

void triggerCases() {
    {
        Replay r;
        chain(r);
        check(r.triggers == 1 && r.triggerQ == 8203, "trigger: on the retake chain the trigger is q 8203, the tone, once");
        check(r.f.triggered && r.f.trigger.hdr == R(RH) && r.f.trigger.srvSlot == 1 && r.f.trigger.srvKnown && !r.f.trigger.ambiguous,
              "trigger: H is the scene target and the tone reads it at t1, unambiguously");
        check(r.f.trigger.targetWidth == W && r.f.trigger.targetHeight == Hh && r.f.trigger.targetFormat == F_RGBA8 &&
                  r.f.trigger.vs == 0xF9CFC798F21E9AEAull && r.f.trigger.ps == 0xFEE777E92850B390ull,
              "trigger: the consumer is the tone pair's 5040x2835 RGBA8 draw (VS F9CFC798, PS FEE777E9)");
        check(r.f.candidateCount == 1 && r.f.candidates[0].resource == R(RH) && !r.f.candidateOverflow,
              "trigger: H is the only candidate: the copy of H the exposure chain reads is never drawn into");
        afterTone(r);
        check(flatHdrLateWrites(r.f) == 0 && r.triggers == 1,
              "trigger: nothing writes H after the tone on this chain (the game copy and the HUD write the screen texture; the eye composites read it)");
    }
    {   // rule (iv): the 630x354 draws that read H (q 8198..8200) are an eighth per axis and are not the consumer.
        Replay r;
        chain(r, false);
        check(r.triggers == 0 && !r.f.triggered, "trigger: rule (iv): without the tone the three 630x354 draws that read H do not trigger");
    }
    {   // rule (iv) mutated: a half-size target reading H IS accepted (the rule is the extent, not the q).
        Replay r;
        chain(r, false, 0, RTONE, true, 2520, 1418);
        check(r.triggers == 1 && r.triggerQ == 8198, "trigger: a half-size target reading H (rule iv's edge) triggers at q 8198");
    }
    {   // rule (i): a depth bound on the tone
        Replay r;
        chain(r, true, RHUDDEPTH);
        check(r.triggers == 0, "trigger: rule (i): a tone with a depth target bound is not the consumer");
    }
    {   // rule (ii): the target is H itself
        Replay r;
        chain(r, true, 0, RH);
        check(r.triggers == 0, "trigger: rule (ii): a draw whose target is H is not its consumer (it is a write)");
    }
    {   // rule (iii): H not bound as a pixel-shader resource
        Replay r;
        chain(r, true, 0, RTONE, false);
        check(r.triggers == 0, "trigger: rule (iii): a tone that does not read H (it reads the copy) does not trigger");
    }
    {   // a frame with no H at all (a 3D map, a menu): nothing to trigger
        Replay r;
        for (uint32_t q = 1; q < 50; ++q) r.draw(into(q, RG2, W, Hh, F_R10, RDEPTH));
        Drawn t = into(60, RTONE, W, Hh, F_RGBA8); t.srv[1] = RCOPY; r.draw(t);
        check(r.triggers == 0 && flatHdrFrameVerdict(r.f) == FlatHdrFrameVerdict::NoHdr, "trigger: a frame with no HDR scene target has no trigger (NoHdr)");
    }
    {   // a second H candidate read by the consumer: ambiguous, never guessed
        Replay r;
        for (uint32_t q = 10; q < 13; ++q) { r.draw(into(q, RH, W, Hh, F_H, RDEPTH)); r.draw(into(q + 100, RH2, W, Hh, F_H, RDEPTH2)); }
        Drawn t = into(200, RTONE, W, Hh, F_RGBA8); t.srv[0] = RH; t.srv[1] = RH2; r.draw(t);
        check(r.triggers == 1 && r.f.trigger.ambiguous, "trigger: two H candidates both bound at the consumer: ambiguous (the selector refuses it)");
    }
    {   // the eye composites alone (an eye-sized target) never trigger, with or without an H
        Replay r;
        for (uint32_t q = 10; q < 13; ++q) r.draw(into(q, RH, W, Hh, F_H, RDEPTH));
        Drawn e0 = into(300, REYE0, 2620, 2533, F_RGBA8); e0.srv[0] = RSCREEN; r.draw(e0);
        check(r.triggers == 0, "trigger: an eye composite that reads the screen texture, not H, is not a consumer");
    }
    {   // late writes after the trigger: a draw, an explicit write and a dispatch into H count; writes elsewhere do not
        Replay r;
        chain(r);
        r.draw(into(8300, RH, W, Hh, F_H, RDEPTH));
        flatHdrObserveExplicitWrite(r.f, R(RH));
        flatHdrObserveDispatchWrite(r.f, R(RH));
        flatHdrObserveExplicitWrite(r.f, R(RCOPY));
        flatHdrObserveDispatchWrite(r.f, R(RSCREEN));
        check(r.f.lateDraws == 1 && r.f.lateExplicit == 1 && r.f.lateDispatches == 1 && flatHdrLateWrites(r.f) == 3,
              "late writes: a draw, a copy/clear and a UAV write into H after the tone count; writes into the copy and the screen texture do not");
    }
    {   // the copy of H mid-frame is normal: a detector that counted the copy's source as a consumer would fire before the late draws
        Replay r;
        for (uint32_t q = 7846; q <= 7856; ++q) r.draw(into(q, RH, W, Hh, F_H, RDEPTH));
        Drawn e = into(8158, RBLOOM, 1261, 709, F_RGBA8); e.srv[0] = RCOPY; r.draw(e);
        check(r.triggers == 0, "trigger: the exposure draws that read the COPY of H (and run before the 35 late draws) do not trigger");
    }
}

// ---- 5. the lines -------------------------------------------------------------------------------------------------------
void lineCases() {
    VrWorldWindow w;
    char line[1024];
    int n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Observing, true, true, w);
    check(n > 0 && n < 1024 && std::strstr(line, "vr world route 5s: key=auto state=observing layer=live gate=held frames=0") &&
              std::strstr(line, "selection=none"),
          "line: the 5 s line prints zeros and 'selection=none' when nothing happened: an absent line is what 'never ran' looks like");
    w.hdr.frames = 450; w.gateFrames = 450; w.hdr.hdrFrames = 450; w.hdr.triggerFrames = 448; w.hdr.noTriggerFrames = 2;
    w.hdr.treated = 440; w.hdr.declined = 8; w.ownedFrames = 300; w.takes = 600; w.layerOnly = 600; w.enters = 1; w.releases = 1; w.gateFlips = 2;
    w.lastRelease = "on-foot-gate-lost"; w.resets = 2; w.hdr.lastVerdict = "treated";
    w.hdr.noteSelection("selected"); w.hdr.noteSelection("engine-views-unavailable");
    n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Owned, true, true, w);
    check(n > 0 && n < 1024 && std::strstr(line, "state=owned") && std::strstr(line, "gate-flips=2") && std::strstr(line, "treated=440 declined=8") &&
              std::strstr(line, "eye-takes=600 door-layer-only=600") && std::strstr(line, "releases=1 (last=on-foot-gate-lost)") &&
              std::strstr(line, "selection=selected:1,engine-views-unavailable:1"),
          "line: state, treated, declined, takes, layer-only doors, releases with their reason and the selector's tally are all there");
    // Worst realistic case: a 5 s window is at most a few hundred frames at 90 Hz (a thousand with a generous margin), two
    // takes a frame; the widths are at their widest, every selection slot full, the longest reason names.
    VrWorldWindow big;
    big.hdr.frames = big.hdr.hdrFrames = big.hdr.triggerFrames = big.hdr.noTriggerFrames = big.hdr.ambiguousFrames = 99999;
    big.hdr.treated = big.hdr.declined = big.hdr.lateWrites = big.hdr.lateWriteFrames = 99999;
    big.gateFrames = big.gateFlips = big.ownedFrames = big.takes = big.layerOnly = big.enters = big.releases = big.resets = 99999;
    big.lastRelease = "late-writes-latched"; big.hdr.lastVerdict = "actual-hdr-binding-or-depth-view-refused";
    big.hdr.lastTriggerVs = big.hdr.lastTriggerPs = ~0ull;
    big.hdr.lastHdrWidth = big.hdr.lastHdrHeight = big.hdr.lastTargetWidth = big.hdr.lastTargetHeight = ~0u;
    const char* names[] = {"selected", "hdr-depth-not-single", "depth-not-screen-motion-source", "engine-views-unavailable",
                           "camera-rows-unavailable", "hdr-not-screen-sized"};
    for (const char* s : names) for (int i = 0; i < 3; ++i) big.hdr.noteSelection(s);
    big.hdr.noteSelection("ambiguous-hdr");
    n = vrWorldFormatWindow(line, sizeof(line), VrWorldKey::Auto, VrWorldState::Latched, false, false, big);
    std::printf("        (the worst realistic 5 s line is %d characters; the log keeps about 1166)\n", n);
    check(n > 0 && n < 1100, "line: at its worst the 5 s line fits the log's line (about 1166 characters) and is never truncated");
    check(std::strstr(line, "other:1") != nullptr, "line: a selection beyond the table's six falls into 'other'");
    char buf[512];
    n = vrWorldFormatEntered(buf, sizeof(buf), 1234, kVrWorldWarmFrames);
    check(n > 0 && std::strstr(buf, "OWNS the world from frame=1234") && std::strstr(buf, "8 treated frames"), "line: the ownership line names the frame and the warm-up");
    n = vrWorldFormatReleased(buf, sizeof(buf), 99, VrWorldRelease::SceneReset, 77);
    check(n > 0 && std::strstr(buf, "RELEASED the world at frame=99 (scene-reset) after 77 owned frame(s)"), "line: the release line names the frame, the reason and the owned frames");
    Replay r;
    chain(r);
    n = vrWorldFormatFirstTrigger(buf, sizeof(buf), 5, r.f);
    check(n > 0 && std::strstr(buf, "first trigger at frame=5 seq=8203") && std::strstr(buf, "target=5040x2835 fmt=27") && std::strstr(buf, "at t1"),
          "line: the first trigger line names the tone draw, its target and the slot it reads H at");
}

// ---- 6. the source pins --------------------------------------------------------------------------------------------------
std::string g_root;
std::string readFile(const char* rel) {
    std::ifstream in(g_root + "\\" + rel, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}
// The text of the function whose definition starts at `head` (first occurrence), up to its closing brace at column 0.
std::string functionBody(const std::string& src, const std::string& head) {
    const size_t at = src.find(head);
    if (at == std::string::npos) return {};
    const size_t end = src.find("\n}\n", at);
    return src.substr(at, end == std::string::npos ? std::string::npos : end - at + 2);
}
bool before(const std::string& s, const char* a, const char* b) {
    const size_t x = s.find(a), y = s.find(b);
    return x != std::string::npos && y != std::string::npos && x < y;
}
// `a` first occurs before the LAST occurrence of `b` (the game's own forward is the last real call in a thunk).
bool beforeLast(const std::string& s, const char* a, const char* b) {
    const size_t x = s.find(a), y = s.rfind(b);
    return x != std::string::npos && y != std::string::npos && x < y;
}
size_t count(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) ++n;
    return n;
}

void sourcePins() {
    if (g_root.empty()) { check(false, "source pins: the repo root was not given (vr_world_route_test --self-test <root>)"); return; }
    const std::string vs = readFile("src\\d3d11\\vscreen.cpp");
    const std::string ex = readFile("src\\d3d11\\exposure_fix.cpp");
    const std::string rt = readFile("src\\d3d11\\vr_world_route.cpp");
    check(!vs.empty() && !ex.empty() && !rt.empty(), "source pins: the three sources are readable");

    // The internal-call early return on every draw and dispatch thunk, BEFORE anything the VR path does with the call.
    struct Thunk { const char* head; const char* first; };
    const Thunk thunks[] = {
        {"void STDMETHODCALLTYPE hookedDraw(", "gpuFrameCommand(self);"},
        {"void STDMETHODCALLTYPE hookedDrawAuto(", "uiLayerIssueBlocked()"},
        {"void STDMETHODCALLTYPE hookedDrawIndexed(", "gpuFrameCommand(self);"},
        {"void STDMETHODCALLTYPE hookedDrawInstanced(", "gpuFrameCommand(self);"},
        {"void STDMETHODCALLTYPE hookedDrawIndexedInstanced(", "gpuFrameCommand(self);"},
        {"void STDMETHODCALLTYPE hookedDrawIndexedInstancedIndirect(", "uiLayerIssueBlocked()"},
        {"void STDMETHODCALLTYPE hookedDrawInstancedIndirect(", "uiLayerIssueBlocked()"},
    };
    bool early = true, detector = true, afterFlat = true;
    for (const Thunk& t : thunks) {
        const std::string body = functionBody(vs, t.head);
        early = early && !body.empty() && before(body, "if (g_vrWorldInternal)", t.first);
        detector = detector && beforeLast(body, "if (g_vrWorldWants && self == g_state->ownerCtx) vrWorldRouteDraw(self);", "g_state->realDraw");
        afterFlat = afterFlat && before(body, "if (runtimeFlatProfile())", "if (g_vrWorldInternal)");
    }
    check(early, "hooks: all seven draw thunks step aside for the route's own draws (g_vrWorldInternal) before their VR path begins");
    check(detector, "hooks: all seven draw thunks call vrWorldRouteDraw, guarded by g_vrWorldWants and the owner context, before the game's draw is issued");
    check(afterFlat, "hooks: the early return sits after the flat profile's branch, so the flat path is untouched");
    check(count(vs, "if (g_vrWorldInternal)") == 7, "hooks: g_vrWorldInternal is tested in exactly the seven draw thunks (no other hook)");
    const std::string d1 = functionBody(ex, "void STDMETHODCALLTYPE hookedDispatch(");
    const std::string d2 = functionBody(ex, "void STDMETHODCALLTYPE hookedDispatchIndirect(");
    check(before(d1, "if (g_vrWorldInternal)", "gpuFrameCommand(self);") && before(d2, "if (g_vrWorldInternal)", "gpuFrameCommand(self);"),
          "hooks: both dispatch thunks step aside for the route's own dispatches (the exposure fix would read them as the game's)");
    check(before(d1, "vrWorldRouteNoteDispatch();", "drawCensusDispatch") , "hooks: a dispatch after the trigger tells the route what its UAVs write");
    const std::string cs = functionBody(ex, "void STDMETHODCALLTYPE hookedClearState(");
    check(before(cs, "if (g_vrWorldInternal)", "bindingForgetAll();") && count(ex, "if (g_vrWorldInternal)") == 3,
          "hooks: the exposure fix's ClearState hook steps aside for the route's own isolation of the context (it would wipe the binding shadow); the exposure file tests the flag in exactly three hooks");
    // The write observers.
    const char* writers[] = {"void STDMETHODCALLTYPE hookedCopyResource(", "void STDMETHODCALLTYPE hookedCopySubresourceRegion(",
                             "void STDMETHODCALLTYPE hookedUpdateSubresource(", "void STDMETHODCALLTYPE hookedClearRtv("};
    bool writes = true;
    for (const char* w : writers) writes = writes && functionBody(vs, w).find("g_vrWorldWatchWrites") != std::string::npos;
    check(writes, "hooks: copy, copy-region, update and clear-RTV tell the route what they write once the trigger has fired (g_vrWorldWatchWrites)");
    check(before(vs, "tkUiLayer.run(", "tkVrWorldRoute.run(") && before(vs, "tkVrWorldRoute.run(", "tkScreenMotion.run("),
          "hooks: the route's boundary runs after the UI layer's (it reads the gate that boundary computed) and before screen motion's");

    // The route itself.
    const std::string draw = functionBody(rt, "void vrWorldRouteDraw(");
    check(before(draw, "bindingGet(BindSlot::Rtv0)", "lookup(") && before(draw, "if (!rtv) return;", "lookup("),
          "cost: a draw with no colour target returns at one shadow load, before any view is looked up");
    check(draw.find("vrWorldFillObservation(") != std::string::npos && draw.find("flatHdrObserveDraw(") != std::string::npos &&
              draw.find("flatHdrCandidateDraw(") != std::string::npos,
          "trigger: the per-draw path runs the flat HDR route's own detector through the route's glue");
    check(rt.find("bindingResolveProbe(") != std::string::npos && rt.find("bindingResolve(") == std::string::npos,
          "cost: views are resolved through the shadow's guarded instrument resolver (its own fault budget), once a frame per view");
    check(draw.find("new ") == std::string::npos && draw.find("std::vector") == std::string::npos && draw.find("std::string") == std::string::npos &&
              draw.find("std::mutex") == std::string::npos && draw.find("lock_guard") == std::string::npos,
          "cost: the per-draw path allocates nothing and takes no lock");
    const std::string boundary = functionBody(rt, "void vrWorldRouteFrameBoundary(");
    check(boundary.find("getString(\"experimental.temporal_aa_on_foot_world\", \"off\")") != std::string::npos &&
              before(boundary, "if (key == VrWorldKey::Off && g_machine.state == VrWorldState::Off && !g_census)", "++g_frameNo"),
          "key off: the boundary reads the key with the default off and returns before doing anything when nothing is to be accounted");
    check(rt.find("flat_camera_inject") == std::string::npos && rt.find("flatCameraInject") == std::string::npos,
          "jitter seam: the route does not install or drive the camera detour in this build");
    check(rt.find("WorldPhase worldPhase() { return {}; }") != std::string::npos, "jitter seam: the world's phase is zero in this build (the flight judges cost and plumbing, not image quality)");
    check(rt.find("flatMonoResolve(") != std::string::npos && before(rt, "VrWorldInternalScope internal;", "flatMonoResolve(") &&
              rt.find("kVrWorldFeatureSlot") != std::string::npos,
          "resolve: the resolver runs inside the internal scope, on the world's own upscaler slot");
    check(rt.find("GpuCensusSection::FrameWorldResolve") != std::string::npos, "census: the resolve is timed on its own GPU census section");
}

int runSelfTest() {
    keyCases();
    keyOffContract();
    machineCases();
    predicateCases();
    selectorCases();
    glueCases();
    triggerCases();
    lineCases();
    sourcePins();
    if (g_failures) {
        std::printf("vr world route: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("vr world route: PASS (%d checks)\n", g_checks);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--self-test") == 0) {
        if (argc >= 3) g_root = argv[2];
        return runSelfTest();
    }
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("vr world route test: dry run (no checks run)\n");
        return 0;
    }
    std::printf("usage: vr_world_route_test --self-test <repo root> | --dry-run\n");
    return 2;
}

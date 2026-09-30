// The VR on-foot world route, the pure half (docs/design-flat-temporal-aa-2026-09-23.md, section 82).
//
// WHY. On foot Elite draws the world ONCE, flat, into the 2D screen's target and shows that screen to each eye
// with one composite draw; today each eye then gets its own upscaler pass over its view of that finished
// texture. The route resolves the world once, where the game drew it (the HDR scene target H, at the flat HDR
// route's trigger, the tone: flat_hdr_route.h), and hands the eyes the resolved screen through the UI layer, so
// the two eye passes stand aside for the frames the route owns.
//
// WHAT IS HERE, all pure (tools\vr_world_route_test drives every function, and the runtime calls the very same
// code):
//   - the key: experimental.temporal_aa_on_foot_world, off (the default) or auto;
//   - the ownership machine: the route treats frames, is WARM after kVrWorldWarmFrames treated frames in a row,
//     OWNS the world from then on, and is released by the gate, the layer, a scene reset, a run of frames it did not
//     treat, or the late-write latch. Only an owned route moves the eye shift and the screen draws;
//   - the selector's facts and reasons (what "the trigger is a world the route may resolve" means in VR, where
//     there is no prefix model: screen motion's named source stands in for it);
//   - the layer's and the door's per-frame predicates, and the 5 s window and the take/release lines.
// It never touches D3D: the runtime hands it facts.
//
// THE KEY-OFF CONTRACT. With the key off, every function here answers "the route is not there": the machine never
// leaves Off, owned() is false, the eye shift is never suppressed, the layer is never told the world is the route's,
// and the door never runs layer-only. The rig pins each answer.
#pragma once
#include "flat_hdr_route.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

// ---- the key ---------------------------------------------------------------------------------------------
// experimental.temporal_aa_on_foot_world: auto (the route where it applies) or off. Off by default: the route has
// not flown. A key that is absent reads as the default; a value that is present and is not "auto" reads as off, so a
// typo leaves on-foot VR exactly as it was and never switches the route on by accident.
enum class VrWorldKey : uint8_t { Off, Auto };
inline VrWorldKey vrWorldKeyFromText(const char* text) {
    if (!text) return VrWorldKey::Off;
    const char* a = "auto";
    for (; *a; ++a, ++text) {
        char c = *text;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != *a) return VrWorldKey::Off;
    }
    return *text == 0 ? VrWorldKey::Auto : VrWorldKey::Off;
}
inline const char* vrWorldKeyName(VrWorldKey key) { return key == VrWorldKey::Auto ? "auto" : "off"; }

// ---- the ownership machine --------------------------------------------------------------------------------
// Treated frames in a row before the layer takes the screen: the world's own history is warm (an upscaler needs
// about this many frames to stop reading its first ones as new). The eye route serves the frames until then.
constexpr uint32_t kVrWorldWarmFrames = 8;
// Frames in a row the route may fail to treat while it owns the world before it lets go (the eye route serves each
// of those frames, with the eye shift still off: a single refusal is not worth a flap back to the eye jitter).
constexpr uint32_t kVrWorldGraceFrames = 3;

enum class VrWorldState : uint8_t {
    Off,        // the key is off: the route does nothing and is not there
    Observing,  // the key is on, the route is not treating (no gate, no layer, no trigger, or a refusal)
    Warming,    // treating, fewer than kVrWorldWarmFrames in a row: the eyes are still the eye route's
    Owned,      // warm: the layer takes the screen on every frame the route treats, and the eye shift is off
    Latched,    // the late-write latch tripped: off until the key is flipped
};
inline const char* vrWorldStateName(VrWorldState s) {
    switch (s) {
        case VrWorldState::Off: return "off";
        case VrWorldState::Observing: return "observing";
        case VrWorldState::Warming: return "warming";
        case VrWorldState::Owned: return "owned";
        case VrWorldState::Latched: return "latched";
    }
    return "?";
}

// Why an owned route stopped owning, for the release line.
enum class VrWorldRelease : uint8_t { None, KeyOff, LayerNotLive, GateLost, Declined, SceneReset, Latched };
inline const char* vrWorldReleaseName(VrWorldRelease r) {
    switch (r) {
        case VrWorldRelease::None: return "none";
        case VrWorldRelease::KeyOff: return "key-off";
        case VrWorldRelease::LayerNotLive: return "layer-not-live";
        case VrWorldRelease::GateLost: return "on-foot-gate-lost";
        case VrWorldRelease::Declined: return "frames-not-treated";
        case VrWorldRelease::SceneReset: return "scene-reset";
        case VrWorldRelease::Latched: return "late-writes-latched";
    }
    return "?";
}

// What the frame that just ended showed.
struct VrWorldFrameEnd {
    bool keyOn = false;
    bool layerLive = false;    // fix.ui_quality is on and a temporal mode runs: the layer can take the screen
    bool gate = false;         // the on-foot world-screen gate held for the frame
    bool treated = false;      // the route resolved the frame (and its H write-back is in the frame)
    bool lateWrites = false;   // a treated frame whose H was written after the resolve (the latch's input)
    bool sceneReset = false;   // boarding, disembarking or a jump: the transition detector reset the eyes' history
};
struct VrWorldStep {
    bool entered = false;                         // the route became owned at this boundary
    VrWorldRelease released = VrWorldRelease::None;   // it stopped owning, and why
};

struct VrWorldMachine {
    VrWorldState state = VrWorldState::Off;
    uint32_t run = 0;    // treated frames in a row
    uint32_t miss = 0;   // frames in a row, with the gate held, the route did not treat
    FlatHdrLatch latch;

    bool owned() const { return state == VrWorldState::Owned; }
    // The route has work for the per-draw hooks: the key is on, the layer is live and the gate holds (and the latch has
    // not turned the route off). Decided from the boundary's facts, so a draw never asks the gate itself.
    bool wantsDraws() const { return state == VrWorldState::Observing || state == VrWorldState::Warming || state == VrWorldState::Owned; }

    // One frame boundary. The state it leaves is what the NEXT frame starts in: the eye shift (native_temporal begin)
    // and the layer read it from there.
    VrWorldStep frameEnd(const VrWorldFrameEnd& f) {
        VrWorldStep step;
        const VrWorldState before = state;
        const auto release = [&](VrWorldRelease why) { if (before == VrWorldState::Owned) step.released = why; };
        if (!f.keyOn) {
            release(VrWorldRelease::KeyOff);
            state = VrWorldState::Off; run = miss = 0; latch.reset();   // flipping the key is what un-latches
            return step;
        }
        if (before == VrWorldState::Latched) return step;   // stays off until the key is flipped
        if (before == VrWorldState::Off) { state = VrWorldState::Observing; run = miss = 0; }   // the key just came on
        if (!f.layerLive) { release(VrWorldRelease::LayerNotLive); state = VrWorldState::Observing; run = miss = 0; return step; }
        if (!f.gate) { release(VrWorldRelease::GateLost); state = VrWorldState::Observing; run = miss = 0; return step; }
        if (f.sceneReset) { release(VrWorldRelease::SceneReset); state = VrWorldState::Observing; run = miss = 0; return step; }
        if (f.treated) {
            miss = 0; ++run;
            if (latch.treatedFrame(f.lateWrites)) { release(VrWorldRelease::Latched); state = VrWorldState::Latched; run = 0; return step; }
            if (before != VrWorldState::Owned && run >= kVrWorldWarmFrames) { state = VrWorldState::Owned; step.entered = true; }
            else if (before != VrWorldState::Owned) state = VrWorldState::Warming;
            return step;
        }
        run = 0; ++miss;
        if (before == VrWorldState::Owned && miss < kVrWorldGraceFrames) return step;   // one refusal is not a release
        release(VrWorldRelease::Declined);
        state = VrWorldState::Observing;
        return step;
    }
};

// ---- what the state means for the rest of the frame -----------------------------------------------------------
// The eye shift (native_temporal begin): advertised to the game for each eye, every frame, today. An owned route
// has no eye pass to feed, and an eye shift with no pass to resolve it would be a shimmer; so it is off from the frame
// after the route became owned until it is released. The state is the one the last boundary left (begin runs before
// the frame's draws), so an entry is one frame late and a release is one frame late, and each costs nothing worse
// than one frame of the eye route with or without its jitter.
inline bool vrWorldSuppressesEyeShift(VrWorldState stateAtBegin) { return stateAtBegin == VrWorldState::Owned; }

// The layer takes the screen draw (the world-screen composite) only for a frame the route treated and owns.
inline bool vrWorldLayerMayTake(VrWorldState state, bool treatedThisFrame) {
    return state == VrWorldState::Owned && treatedThisFrame;
}

// The door runs layer-only for an eye whose screen draw the layer took in THIS sequence: no eye upscaler, no motion
// prep, no UI resolve. Sequence 0 is "never": a zero-initialised tag cannot match the first frame.
inline bool vrWorldDoorLayerOnly(uint64_t takenSequence, uint64_t sequence) {
    return sequence != 0 && takenSequence == sequence;
}

// ---- the selector ---------------------------------------------------------------------------------------------
// The HDR route's selector (flat_hdr_route.h) reads the flat runtime's prefix model of every draw of the frame. The
// VR path has no such model and cannot afford one on 21.8k draws a frame; what it has is the source screen motion
// names from the first non-weapon pool draw into the screen-sized depth, and the engine-record views keyed by it. So
// the VR selection is a handful of facts the runtime gathers at the trigger, and the reasons a frame is refused.
struct VrWorldSelectFacts {
    bool triggered = false;       // the detector found the tone: H, its consumer and the extent rules
    bool ambiguous = false;       // more than one H candidate matched the consumer
    bool depthKnown = false;      // every draw into H used one depth target, and it is known
    bool depthNamed = false;      // that depth is the source screen motion named THIS frame
    bool engineViews = false;     // the engine-record views for it answered (slots, pool, scene constants now and before)
    bool cameraRows = false;      // the source camera's CPU rows for this frame are known
    bool extentOk = false;        // H is the screen's size on both axes (R = D: the route resolves at E = R)
    bool engineKnown = false;     // fix.temporal_aa names an engine the resolver runs
};
enum class VrWorldSelect : uint8_t {
    Selected, NoTrigger, Ambiguous, NoEngine, ExtentMismatch, DepthMixed, DepthNotNamed, NoEngineViews, NoCameraRows,
};
inline VrWorldSelect vrWorldSelect(const VrWorldSelectFacts& f) {
    if (!f.triggered) return VrWorldSelect::NoTrigger;
    if (f.ambiguous) return VrWorldSelect::Ambiguous;
    if (!f.engineKnown) return VrWorldSelect::NoEngine;
    if (!f.extentOk) return VrWorldSelect::ExtentMismatch;
    if (!f.depthKnown) return VrWorldSelect::DepthMixed;
    if (!f.depthNamed) return VrWorldSelect::DepthNotNamed;
    if (!f.engineViews) return VrWorldSelect::NoEngineViews;
    if (!f.cameraRows) return VrWorldSelect::NoCameraRows;
    return VrWorldSelect::Selected;
}
inline const char* vrWorldSelectName(VrWorldSelect s) {
    switch (s) {
        case VrWorldSelect::Selected: return "selected";
        case VrWorldSelect::NoTrigger: return "no-trigger";
        case VrWorldSelect::Ambiguous: return "ambiguous-hdr";
        case VrWorldSelect::NoEngine: return "no-temporal-engine";
        case VrWorldSelect::ExtentMismatch: return "hdr-not-screen-sized";
        case VrWorldSelect::DepthMixed: return "hdr-depth-not-single";
        case VrWorldSelect::DepthNotNamed: return "depth-not-screen-motion-source";
        case VrWorldSelect::NoEngineViews: return "engine-views-unavailable";
        case VrWorldSelect::NoCameraRows: return "camera-rows-unavailable";
    }
    return "?";
}

// ---- the census -----------------------------------------------------------------------------------------------
// One 5 s window, reset when it prints. The HDR route's window is the detector's half; the rest is the route's.
struct VrWorldWindow {
    FlatHdrWindow hdr;            // frames, hdr-frames, trigger, none, ambiguous, treated, declined, late writes, selection tally
    uint64_t gateFrames = 0;      // frames the route watched with the gate held
    uint64_t ownedFrames = 0;     // frames that started owned (eye shift off)
    uint64_t takes = 0;           // eye screen draws the layer took (two a frame)
    uint64_t layerOnly = 0;       // eyes whose door ran layer-only
    uint64_t enters = 0, releases = 0, resets = 0;
    const char* lastRelease = "none";
    void reset() { *this = VrWorldWindow{}; }
};
// The 5 s line, printed every window while the key is auto, zeros included: an absent line is what "the route never
// ran" looks like, and the stop signals in the flight plan read it.
inline int vrWorldFormatWindow(char* out, size_t size, VrWorldKey key, VrWorldState state, bool layerLive, bool gate,
                               const VrWorldWindow& w) {
    int n = std::snprintf(out, size,
        "vr world route 5s: key=%s state=%s layer=%s gate=%s frames=%llu gate-frames=%llu hdr-frames=%llu trigger=%llu "
        "none=%llu ambiguous=%llu treated=%llu declined=%llu owned-frames=%llu eye-takes=%llu door-layer-only=%llu "
        "enters=%llu releases=%llu (last=%s) scene-resets=%llu late-hdr-writes=%llu (in %llu frames) last=%s "
        "last-trigger=VS=%016llX PS=%016llX target=%ux%u hdr=%ux%u selection=",
        vrWorldKeyName(key), vrWorldStateName(state), layerLive ? "live" : "not-live", gate ? "held" : "no",
        static_cast<unsigned long long>(w.hdr.frames), static_cast<unsigned long long>(w.gateFrames),
        static_cast<unsigned long long>(w.hdr.hdrFrames), static_cast<unsigned long long>(w.hdr.triggerFrames),
        static_cast<unsigned long long>(w.hdr.noTriggerFrames), static_cast<unsigned long long>(w.hdr.ambiguousFrames),
        static_cast<unsigned long long>(w.hdr.treated), static_cast<unsigned long long>(w.hdr.declined),
        static_cast<unsigned long long>(w.ownedFrames), static_cast<unsigned long long>(w.takes),
        static_cast<unsigned long long>(w.layerOnly), static_cast<unsigned long long>(w.enters),
        static_cast<unsigned long long>(w.releases), w.lastRelease, static_cast<unsigned long long>(w.resets),
        static_cast<unsigned long long>(w.hdr.lateWrites), static_cast<unsigned long long>(w.hdr.lateWriteFrames),
        w.hdr.lastVerdict, static_cast<unsigned long long>(w.hdr.lastTriggerVs),
        static_cast<unsigned long long>(w.hdr.lastTriggerPs), w.hdr.lastTargetWidth, w.hdr.lastTargetHeight,
        w.hdr.lastHdrWidth, w.hdr.lastHdrHeight);
    const auto append = [&](const char* first, const char* name, unsigned long long count) {
        if (n < 0 || static_cast<size_t>(n) >= size) return;
        const int more = std::snprintf(out + n, size - static_cast<size_t>(n), "%s%s:%llu", first, name, count);
        if (more > 0) n += more;
    };
    bool any = false;
    for (const auto& s : w.hdr.selections) {
        if (!s.name) break;
        append(any ? "," : "", s.name, static_cast<unsigned long long>(s.count));
        any = true;
    }
    if (w.hdr.selectionOther) { append(any ? "," : "", "other", static_cast<unsigned long long>(w.hdr.selectionOther)); any = true; }
    if (!any && n >= 0 && static_cast<size_t>(n) < size) { std::snprintf(out + n, size - static_cast<size_t>(n), "none"); n += 4; }
    return n;
}
// The route took the world: the first frame its layer took a screen draw after being warm.
inline int vrWorldFormatEntered(char* out, size_t size, uint64_t frame, uint32_t warmFrames) {
    return std::snprintf(out, size,
        "vr world route: OWNS the world from frame=%llu after %u treated frames in a row; the eye shift is off and the "
        "layer takes the screen draw on every frame the route treats (the eye route serves the rest)",
        static_cast<unsigned long long>(frame), warmFrames);
}
// The route let go: why, and what it had done.
inline int vrWorldFormatReleased(char* out, size_t size, uint64_t frame, VrWorldRelease why, uint64_t ownedFrames) {
    return std::snprintf(out, size,
        "vr world route: RELEASED the world at frame=%llu (%s) after %llu owned frame(s); the eye shift is back on and "
        "the eye route serves the eyes",
        static_cast<unsigned long long>(frame), vrWorldReleaseName(why), static_cast<unsigned long long>(ownedFrames));
}
// The first trigger of a session, once.
inline int vrWorldFormatFirstTrigger(char* out, size_t size, uint64_t frame, const FlatHdrFrame& f) {
    const auto& t = f.trigger;
    const FlatHdrCandidate* h = flatHdrFindCandidate(f, t.hdr);
    return std::snprintf(out, size,
        "vr world route: first trigger at frame=%llu seq=%u: VS=%016llX PS=%016llX target=%ux%u fmt=%u reads the scene "
        "HDR %ux%u at t%d; its %u draw(s) ran seq %u..%u%s",
        static_cast<unsigned long long>(frame), t.sequence, static_cast<unsigned long long>(t.vs),
        static_cast<unsigned long long>(t.ps), t.targetWidth, t.targetHeight, t.targetFormat, t.hdrWidth, t.hdrHeight,
        t.srvKnown ? static_cast<int>(t.srvSlot) : -1, h ? h->draws : 0u, h ? h->firstSeq : 0u, h ? h->lastSeq : 0u,
        t.ambiguous ? "; MORE THAN ONE HDR candidate matched, so the route declines" : "");
}

}  // namespace edvr

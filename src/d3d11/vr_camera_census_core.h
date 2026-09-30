// The VR camera census, the pure half (design doc section 82, "Pre-build findings and the stop").
//
// THE QUESTION. In VR on foot the game draws the world once (a flat "screen" camera) and then one composite draw an
// eye whose VS reads b1 rows 270..273, a per-eye camera matrix in the composer's layout. The VR world route will
// jitter the WORLD camera through the camera injector's detour; that phase must not reach the eye cameras. The
// injector admits every kind-3 camera, and no signal is known that tells an eye camera from the world's:
//   (A) a field signature at the refresh (aspect, near, field of view, the bound pair, the viewport),
//   (B) a join by content between the rows the composer produced for a camera and the eye draws' b1 rows,
//   (C) the call's place in the frame against the tone draw,
//   (D) the caller address,
//   (E) the camera's tangents against the eye frusta EDVR itself advertises.
// One flight with advanced.vr_camera_census = on records enough to decide: this header is what it records and how each
// record is written. `python tools\edvr_log.py --camera-census` reads it back and does the join offline.
//
// WHAT IS HERE. Everything the census decides and every line it prints, with no D3D, no log, no game and no allocation
// (tools\vr_camera_census_test runs each function, and the DLL compiles the very same text):
//   - the key and the decision to run (VR profile and the key on; anything else is nothing at all);
//   - which frames are sampled (the tone seen, and the journal, when it is read, saying on foot);
//   - the camera struct's field signature and the rows the composer writes for it, ported from
//     tools\c2_derive_test\c2_derive_model.h (composeSceneCb), so a camera's rows are known without reading the game's
//     own constant buffer and can be compared with an eye draw's;
//   - the bounded tables: one row per distinct camera (64), the call sequence of one frame (160), the lock-free table
//     of calls on other threads (16), the eye-draw allowance (8), and the 5 s window;
//   - the log-line budget per class, so the whole census stays near 400 lines a session whatever the flight does;
//   - the text of every line.
//
// THE ZERO-MUTATION CONTRACT is the detour's (flat_camera_inject.cpp, observe-only mode): nothing here writes the game.
#pragma once
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

// ---- the key ----------------------------------------------------------------------------------------------------
// advanced.vr_camera_census: on or off, off by default. A value that is present and is not "on" reads as off, so a typo
// can never install a hook. VR profile only: a flat profile reads the key off whatever the file says (Config refuses
// it, runtimeProfileAllowsKey does not list it), and the wanted decision asks the profile a second time.
enum class VrCameraCensusKey : uint8_t { Off, On };
inline VrCameraCensusKey vrCameraCensusKeyFromText(const char* text) {
    if (!text) return VrCameraCensusKey::Off;
    const char* on = "on";
    for (; *on; ++on, ++text) {
        char c = *text;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != *on) return VrCameraCensusKey::Off;
    }
    return *text == 0 ? VrCameraCensusKey::On : VrCameraCensusKey::Off;
}
inline const char* vrCameraCensusKeyName(VrCameraCensusKey key) { return key == VrCameraCensusKey::On ? "on" : "off"; }
constexpr bool vrCameraCensusWantedFor(bool vrProfile, VrCameraCensusKey key) {
    return vrProfile && key == VrCameraCensusKey::On;
}

// ---- the camera struct -------------------------------------------------------------------------------------------
// Camera-relative offsets (the typed table in c2_derive_model.h, which the rig compares these against).
constexpr uint32_t kVrCensusAxes = 0x20;      // source 3x4 view axes, 12 floats (+0x20..+0x4C)
constexpr uint32_t kVrCensusProj = 0x1D0;     // projection 4x4, 16 floats (+0x1D0..+0x20C)
constexpr uint32_t kVrCensusFlags = 0x250;    // dirty flag word: bit 4 = projection dirty
constexpr uint32_t kVrCensusNear = 0x254;
constexpr uint32_t kVrCensusFar = 0x258;
constexpr uint32_t kVrCensusAspect = 0x260;
constexpr uint32_t kVrCensusKind = 0x264;
constexpr uint32_t kVrCensusFov = 0x280;      // the vertical field of view, radians (tan(fov/2) is the half-height)
constexpr uint32_t kVrCensusBoundX = 0x28C;   // the off-centre bound pair (projection[8] = 2 x boundX)
constexpr uint32_t kVrCensusBoundY = 0x290;
constexpr uint32_t kVrCensusViewportW = 0x2A0;
constexpr uint32_t kVrCensusViewportH = 0x2A4;
constexpr uint32_t kVrCensusFlagProj = 4;
// One SEH-guarded copy of camera+0x20 .. +0x2AF is enough for every field above.
constexpr uint32_t kVrCensusSnapFrom = 0x20;
constexpr uint32_t kVrCensusSnapBytes = 0x290;

struct VrCensusSnap {
    uint8_t bytes[kVrCensusSnapBytes];
    float f(uint32_t cameraOffset) const {
        float v;
        std::memcpy(&v, bytes + (cameraOffset - kVrCensusSnapFrom), sizeof(v));
        return v;
    }
    uint32_t u(uint32_t cameraOffset) const {
        uint32_t v;
        std::memcpy(&v, bytes + (cameraOffset - kVrCensusSnapFrom), sizeof(v));
        return v;
    }
};

// What a camera is, by its fields (A). p0, p5, p8, p9 are the derived projection terms: read AFTER the body they are
// the derivation's result (p8 = 2 boundX and p9 = 2 boundY for a perspective camera, p0 = 1/(tan(fov/2) aspect),
// p5 = 1/tan(fov/2)); before it they may be stale.
struct VrCensusSig {
    uint32_t kind = 0;
    float aspect = 0, nearZ = 0, farZ = 0, fov = 0, boundX = 0, boundY = 0, viewportW = 0, viewportH = 0;
    float p0 = 0, p5 = 0, p8 = 0, p9 = 0;
    uint32_t flags = 0;
};
inline void vrCensusSigFromSnap(const VrCensusSnap& s, VrCensusSig* out) {
    out->kind = s.u(kVrCensusKind);
    out->aspect = s.f(kVrCensusAspect);
    out->nearZ = s.f(kVrCensusNear);
    out->farZ = s.f(kVrCensusFar);
    out->fov = s.f(kVrCensusFov);
    out->boundX = s.f(kVrCensusBoundX);
    out->boundY = s.f(kVrCensusBoundY);
    out->viewportW = s.f(kVrCensusViewportW);
    out->viewportH = s.f(kVrCensusViewportH);
    out->p0 = s.f(kVrCensusProj + 0);
    out->p5 = s.f(kVrCensusProj + 4 * 5);
    out->p8 = s.f(kVrCensusProj + 4 * 8);
    out->p9 = s.f(kVrCensusProj + 4 * 9);
    out->flags = s.u(kVrCensusFlags);
}

// The four tangents the camera's frustum spans at unit distance, {left, right, bottom, top}, from the derived terms
// (E). The game builds the projection from a window [L, R] x [B, T] (c2_derive_model.h buildProjection):
// p0 = 2/(R-L) and p8 = -(R+L)/(R-L), so L = -(1+p8)/p0 and R = (1-p8)/p0; likewise p5 = 2/(T-B), p9 = -(T+B)/(T-B),
// B = -(1+p9)/p5 and T = (1-p9)/p5. A symmetric camera has p8 = p9 = 0 and L = -R, B = -T. Perspective kinds only
// (0 and 3); false otherwise or when a term is degenerate.
inline bool vrCensusTangents(const VrCensusSig& sig, float out[4]) {
    if (sig.kind != 0 && sig.kind != 3) return false;
    if (!std::isfinite(sig.p0) || !std::isfinite(sig.p5) || !std::isfinite(sig.p8) || !std::isfinite(sig.p9)) return false;
    if (sig.p0 == 0.0f || sig.p5 == 0.0f) return false;
    out[0] = -(1.0f + sig.p8) / sig.p0;
    out[1] = (1.0f - sig.p8) / sig.p0;
    out[2] = -(1.0f + sig.p9) / sig.p5;
    out[3] = (1.0f - sig.p9) / sig.p5;
    return true;
}

// Rows 270..273 of the scene constant buffer as the composer (FUN_140596830) writes them for this camera (B): the
// source axes rows (lane 3 masked to zero) times the projection, with the projection's own translation row last -- the
// model's composeSceneCb, term for term. It is what the composer WOULD write given the derived blocks, so it is only
// offered when the projection is clean (flag bit 4 clear): after the body ran, a dirty projection means the composer
// never derived it for this call. Sixteen floats, rows 270, 271, 272, 273 in order.
inline bool vrCensusComposeRows(const VrCensusSnap& snap, float out[16]) {
    if (snap.u(kVrCensusFlags) & kVrCensusFlagProj) return false;
    float s[11], p[16];
    for (uint32_t i = 0; i < 11; ++i) s[i] = snap.f(kVrCensusAxes + 4 * i);
    for (uint32_t i = 0; i < 16; ++i) p[i] = snap.f(kVrCensusProj + 4 * i);
    out[0]  = p[8] * s[8]  + p[0] * s[0] + p[4] * s[4];
    out[1]  = p[9] * s[8]  + p[1] * s[0] + p[5] * s[4];
    out[2]  = p[10] * s[8] + p[2] * s[0] + p[6] * s[4];
    out[3]  = p[11] * s[8] + p[3] * s[0] + p[7] * s[4];
    out[4]  = p[8] * s[9]  + p[0] * s[1] + p[4] * s[5];
    out[5]  = p[9] * s[9]  + p[1] * s[1] + p[5] * s[5];
    out[6]  = p[10] * s[9] + p[2] * s[1] + p[6] * s[5];
    out[7]  = p[11] * s[9] + p[3] * s[1] + p[7] * s[5];
    out[8]  = p[8] * s[10] + p[0] * s[2] + p[4] * s[6];
    out[9]  = p[9] * s[10] + p[1] * s[2] + p[5] * s[6];
    out[10] = p[10] * s[10] + p[2] * s[2] + p[6] * s[6];
    out[11] = p[11] * s[10] + p[3] * s[2] + p[7] * s[6];
    out[12] = p[12];
    out[13] = p[13];
    out[14] = p[14];
    out[15] = p[15];
    for (int i = 0; i < 16; ++i) if (!std::isfinite(out[i])) return false;
    return true;
}

// Two sets of rows are the same camera's when every float is within the tolerance (the offline join's rule, 1e-5).
inline bool vrCensusRowsMatch(const float a[16], const float b[16], float tolerance) {
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return false;
        if (std::fabs(a[i] - b[i]) > tolerance) return false;
    }
    return true;
}

// The off-centre terms a projection built from these four tangents carries: measured from the eye's own rows
// (flatCameraMeasureRowShift) they read (p8, p9) = (-(R+L)/(R-L), -(U+D)/(U-D)). With the shift EDVR advertised for the
// eye added to both edges of an axis (the host moves the frustum by it) this is what the rows should measure; without
// it, the true frustum's own. The leak is the measured value minus the shifted expectation.
inline bool vrCensusExpectedMeasure(const float frustum[4], float shiftX, float shiftY, double* x, double* y) {
    const double l = static_cast<double>(frustum[0]) + shiftX, r = static_cast<double>(frustum[1]) + shiftX;
    const double d = static_cast<double>(frustum[2]) + shiftY, u = static_cast<double>(frustum[3]) + shiftY;
    if (!(r - l != 0.0) || !(u - d != 0.0)) return false;
    *x = -(r + l) / (r - l);
    *y = -(u + d) / (u - d);
    return std::isfinite(*x) && std::isfinite(*y);
}

// The bounds of what one camera may print: its first-sight line and at most this many "changed:" lines.
constexpr uint32_t kVrCensusMaxChangesPerCamera = 4;

// A camera has changed when a field that does not move between frames did, or a bound moved by more than a rounding. An
// eye camera's aspect and field of view are re-derived every frame from the frustum EDVR hands the game and differ in
// their last bits; those are noise, and they must not use up the four lines a camera may print (kVrCensusFieldEpsilon is
// relative, about sixteen units in the last place, and absolute below one). The bound pair IS the signal: it moves with
// the eye shift every frame, so its threshold is absolute and tight.
constexpr float kVrCensusBoundEpsilon = 1.0e-7f;
constexpr float kVrCensusFieldEpsilon = 2.0e-6f;
inline bool vrCensusFieldMoved(float a, float b) {
    if (std::isnan(a) && std::isnan(b)) return false;
    const float m = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
    return !(std::fabs(a - b) <= kVrCensusFieldEpsilon * (m > 1.0f ? m : 1.0f));
}
inline bool vrCensusBoundMoved(float a, float b) { return !(std::fabs(a - b) <= kVrCensusBoundEpsilon); }
inline bool vrCensusSigDiffers(const VrCensusSig& a, const VrCensusSig& b) {
    if (a.kind != b.kind) return true;
    if (vrCensusFieldMoved(a.aspect, b.aspect) || vrCensusFieldMoved(a.nearZ, b.nearZ) || vrCensusFieldMoved(a.farZ, b.farZ) ||
        vrCensusFieldMoved(a.fov, b.fov) || vrCensusFieldMoved(a.viewportW, b.viewportW) ||
        vrCensusFieldMoved(a.viewportH, b.viewportH)) return true;
    return vrCensusBoundMoved(a.boundX, b.boundX) || vrCensusBoundMoved(a.boundY, b.boundY);
}

// ---- where in the frame a call falls ---------------------------------------------------------------------------
// From vrWorldRouteDrawProgress: the detector's tone draw (the HDR scene's tone, the on-foot chain's marker). None when
// the route does not report draw progress (the skeleton, or the detector is off).
enum class VrCensusTone : uint8_t { Before, After, None };
inline const char* vrCensusToneName(VrCensusTone t) {
    return t == VrCensusTone::Before ? "before" : t == VrCensusTone::After ? "after" : "none";
}

// ---- which frames are sampled ------------------------------------------------------------------------------------
// The brief's on-foot frame is "the tone was seen". The detector that reports the tone watches draws for the census in
// every frame, and a cockpit, a hangar and a menu draw the same tone: without a second witness the first three call
// sequences and the first four eye frames would be spent before the commander is on foot. The second witness is Elite's
// own journal (Status.json Flags2 bit 0, journal_watch.h): while it is being read, a frame is sampled only when it says on
// foot; with no journal the tone alone decides, as the brief has it. The journal lags the game by about a second.
enum class VrCensusFoot : uint8_t {
    Off,      // the journal is not being read (disabled, no folder, faults): no second witness
    Unknown,  // read, but no Flags2 in the file: a menu, or shutdown
    No,       // Flags2 says not on foot: a ship, a vehicle
    Yes,      // Flags2 says on foot
};
inline const char* vrCensusFootName(VrCensusFoot f) {
    return f == VrCensusFoot::Yes ? "yes" : f == VrCensusFoot::No ? "no" : f == VrCensusFoot::Unknown ? "unknown" : "off";
}
inline VrCensusFoot vrCensusFootFrom(bool journalActive, bool known, bool onFoot) {
    return !journalActive ? VrCensusFoot::Off : !known ? VrCensusFoot::Unknown : onFoot ? VrCensusFoot::Yes : VrCensusFoot::No;
}
// A frame the census samples (a call sequence, an eye readback). With the world route's draw progress available it is the
// brief's rule plus the journal's: the tone was seen, and the journal, if it is read, says on foot. With no progress (the
// route does not report, or does not watch draws) there is no tone to see: the journal alone decides, and only a journal
// that positively says on foot does (neither witness would sample the first frames of a session, menu frames, for nothing).
constexpr bool vrCensusSamplesFrame(bool toneSeen, bool progressAvailable, VrCensusFoot foot) {
    return progressAvailable ? (toneSeen && (foot == VrCensusFoot::Yes || foot == VrCensusFoot::Off)) : foot == VrCensusFoot::Yes;
}
// Whether a call is worth recording at all: a frame the journal says is not on foot is never sampled, so its calls are only counted.
constexpr bool vrCensusMayRecord(VrCensusFoot foot) { return foot == VrCensusFoot::Yes || foot == VrCensusFoot::Off; }

// ---- the bounded tables -----------------------------------------------------------------------------------------
struct VrCensusCall {
    uintptr_t camera = 0;
    uintptr_t view = 0;     // the refresh's second argument: the view (pass) object the camera belongs to
    uint32_t kind = 0;
    uint32_t callerRva = 0;
    uint32_t draw = 0;
    uint32_t preFlags = 0, postFlags = 0;
    bool kindReadable = false, drawKnown = false, postSeen = false, rowsValid = false;
    VrCensusTone tone = VrCensusTone::None;
    float rows[16] = {};
};

// One frame's calls, in order. A frame keeps its first kCapacity calls and counts the rest; it is rolled at the Present
// boundary and is printed only when it is one of the first on-foot frames.
struct VrCensusFrame {
    static constexpr size_t kCapacity = 160;
    VrCensusCall call[kCapacity];
    uint32_t calls = 0;     // owner-thread calls this frame, recorded or not
    uint32_t recorded = 0;
    bool toneSeen = false;  // the tone was seen at some call or eye draw: an on-foot frame (see vrCensusSamplesFrame)
    bool progress = false;  // the world route reported its draw progress at some call or eye draw this frame
    void reset() { calls = 0; recorded = 0; toneSeen = false; progress = false; }
    // The next call's record (initialised), or null when the frame is full; the call is counted either way.
    VrCensusCall* add() {
        ++calls;
        if (recorded >= kCapacity) return nullptr;
        VrCensusCall* c = &call[recorded++];
        *c = VrCensusCall{};
        return c;
    }
    uint32_t truncated() const { return calls - recorded; }
};

struct VrCensusCamera {
    uintptr_t camera = 0;
    uintptr_t firstView = 0, firstCtx = 0;   // the first call's second argument (the view) and first (the view-constant context)
    VrCensusSig firstSig, sig, changeFrom;
    float tan[4] = {};
    bool tanValid = false;
    uint32_t callerRva = 0, firstOrdinal = 0, firstDraw = 0;
    bool firstDrawKnown = false, linePending = false, changePending = false;
    VrCensusTone firstTone = VrCensusTone::None;
    uint64_t firstFrame = 0, changeFrame = 0, calls = 0;
    uint32_t changes = 0, changeLines = 0;
};

// One row per distinct camera pointer, the first kCapacity of a session. A full table counts what it cannot hold.
class VrCensusCameraTable {
public:
    static constexpr size_t kCapacity = 64;
    size_t used() const { return used_; }
    uint64_t overflow() const { return overflow_; }
    VrCensusCamera& at(size_t i) { return entry_[i]; }
    const VrCensusCamera& at(size_t i) const { return entry_[i]; }
    size_t find(uintptr_t camera) const {
        for (size_t i = 0; i < used_; ++i) if (entry_[i].camera == camera) return i;
        return kCapacity;
    }
    // The boundary's two questions per row: does its first-sight line still wait, and may its pending change print?
    // A change past kVrCensusMaxChangesPerCamera is counted (changes) and never printed.
    bool takeCameraLine(size_t i) {
        VrCensusCamera& e = entry_[i];
        if (!e.linePending) return false;
        e.linePending = false;
        return true;
    }
    bool takeChangeLine(size_t i) {
        VrCensusCamera& e = entry_[i];
        if (!e.changePending) return false;
        e.changePending = false;
        if (e.changeLines >= kVrCensusMaxChangesPerCamera) return false;
        ++e.changeLines;
        return true;
    }
    enum class Event : uint8_t { Known, New, Changed, Full };
    // What the call that reported a camera knew: the frame in progress and where in it the call fell. A new row keeps
    // these as its "first call"; a known row takes only the frame (when its signature moved).
    struct Call {
        uint64_t frame = 0;
        uint32_t callerRva = 0, ordinal = 0, draw = 0;
        bool drawKnown = false;
        VrCensusTone tone = VrCensusTone::None;
        uintptr_t view = 0, ctx = 0;
    };
    // One post-half report for `camera`: a new row (its line pending), a known one whose signature moved (a pending
    // change), or the same again.
    Event note(uintptr_t camera, const VrCensusSig& sig, const float tan[4], bool tanValid, const Call& call) {
        size_t i = find(camera);
        if (i == kCapacity) {
            if (used_ >= kCapacity) { ++overflow_; return Event::Full; }
            i = used_++;
            VrCensusCamera& e = entry_[i];
            e = VrCensusCamera{};
            e.camera = camera;
            e.firstView = call.view;
            e.firstCtx = call.ctx;
            e.firstSig = e.sig = sig;
            if (tanValid) std::memcpy(e.tan, tan, sizeof(e.tan));
            e.tanValid = tanValid;
            e.callerRva = call.callerRva;
            e.firstOrdinal = call.ordinal;
            e.firstDraw = call.draw;
            e.firstDrawKnown = call.drawKnown;
            e.firstTone = call.tone;
            e.firstFrame = call.frame;
            e.calls = 1;
            e.linePending = true;
            return Event::New;
        }
        VrCensusCamera& e = entry_[i];
        ++e.calls;
        if (!vrCensusSigDiffers(e.sig, sig)) { e.sig.flags = sig.flags; return Event::Known; }
        ++e.changes;
        if (!e.changePending) { e.changePending = true; e.changeFrom = e.sig; e.changeFrame = call.frame; }
        e.sig = sig;
        return Event::Changed;
    }
private:
    VrCensusCamera entry_[kCapacity];
    size_t used_ = 0;
    uint64_t overflow_ = 0;
};

// Calls on a thread other than the owner's: counted by the detour and offered here, from any thread, so the table is
// lock-free. A slot is claimed (0 -> 1), filled, then published (2) with a release store; readers see a slot only at 2.
// Distinct (thread, camera, kind, caller) tuples, the first kCapacity; the rest are counted in overflow().
class VrCensusOffThread {
public:
    static constexpr size_t kCapacity = 16;
    struct Entry {
        uint32_t thread = 0, kind = 0, callerRva = 0;
        bool kindReadable = false;
        uintptr_t camera = 0;
        uint64_t calls = 0;
    };
    void note(uint32_t thread, uintptr_t camera, uint32_t kind, bool kindReadable, uint64_t callerRva) noexcept {
        total_.fetch_add(1, std::memory_order_relaxed);
        const uint32_t caller32 = static_cast<uint32_t>(callerRva);
        const uint32_t kind32 = kindReadable ? kind : 0xffffffffu;
        for (size_t i = 0; i < kCapacity; ++i) {
            Slot& s = slot_[i];
            uint32_t state = s.state.load(std::memory_order_acquire);
            if (state == 2 && s.thread == thread && s.camera == camera && s.kind == kind32 && s.callerRva == caller32) {
                s.calls.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (state == 0 && s.state.compare_exchange_strong(state, 1, std::memory_order_acq_rel)) {
                s.thread = thread;
                s.camera = camera;
                s.kind = kind32;
                s.callerRva = caller32;
                s.calls.store(1, std::memory_order_relaxed);
                s.state.store(2, std::memory_order_release);
                return;
            }
        }
        overflow_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t total() const noexcept { return total_.load(std::memory_order_relaxed); }
    uint64_t overflow() const noexcept { return overflow_.load(std::memory_order_relaxed); }
    // The owner thread, once a boundary: the next published slot not yet reported (and marks it reported).
    bool takeNew(Entry* out) {
        for (size_t i = 0; i < kCapacity; ++i) {
            Slot& s = slot_[i];
            if (s.state.load(std::memory_order_acquire) != 2 || s.reported) continue;
            s.reported = true;
            out->thread = s.thread;
            out->kindReadable = s.kind != 0xffffffffu;
            out->kind = s.kind;
            out->callerRva = s.callerRva;
            out->camera = s.camera;
            out->calls = s.calls.load(std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    size_t used() const {
        size_t n = 0;
        for (size_t i = 0; i < kCapacity; ++i) if (slot_[i].state.load(std::memory_order_acquire) == 2) ++n;
        return n;
    }
private:
    struct Slot {
        std::atomic<uint32_t> state{0};
        bool reported = false;   // owner thread only
        uint32_t thread = 0, kind = 0, callerRva = 0;
        uintptr_t camera = 0;
        std::atomic<uint64_t> calls{0};
    };
    Slot slot_[kCapacity];
    std::atomic<uint64_t> total_{0}, overflow_{0};
};

// The 5 s window (owner thread). Zeros are printed: an absent line is what "the census never ran" looks like.
struct VrCensusWindow {
    static constexpr size_t kCallers = 16, kCameras = 64;
    struct Caller { uint32_t rva = 0; uint64_t n = 0; };
    uint64_t frames = 0, calls = 0, posts = 0, stale = 0;
    uint64_t kinds[8] = {};                 // 0..5, 6 = other, 7 = unreadable
    Caller callers[kCallers];
    uint32_t callerCount = 0;
    uint64_t callerOverflow = 0;
    uintptr_t cameras[kCameras] = {};
    uint32_t cameraCount = 0;
    uint64_t cameraOverflow = 0;
    uint64_t toneBefore = 0, toneAfter = 0, toneNone = 0;
    uint64_t toneFrames = 0;                // frames in which the tone was seen, sampled or not
    uint64_t onFootFrames = 0;              // ... of which the census samples (the journal, if it is read, says on foot)
    uint64_t eyeDraws = 0, eyeOnFoot = 0;   // eye composite draws reported, and those of a sampled frame
    uint32_t windows = 0;                   // 5 s windows this line covers
    bool progressSeen = false;              // vrWorldRouteDrawProgress answered at least once
    void noteCall(bool kindReadable, uint32_t kind, uint32_t callerRva, uintptr_t camera, VrCensusTone tone, bool lapsed) {
        ++calls;
        if (lapsed) ++stale;
        ++kinds[!kindReadable ? 7 : kind <= 5 ? kind : 6];
        size_t i = 0;
        for (; i < callerCount; ++i) if (callers[i].rva == callerRva) break;
        if (i < callerCount) ++callers[i].n;
        else if (callerCount < kCallers) { callers[callerCount].rva = callerRva; callers[callerCount].n = 1; ++callerCount; }
        else ++callerOverflow;
        size_t c = 0;
        for (; c < cameraCount; ++c) if (cameras[c] == camera) break;
        if (c == cameraCount) { if (cameraCount < kCameras) cameras[cameraCount++] = camera; else ++cameraOverflow; }
        if (tone == VrCensusTone::Before) ++toneBefore; else if (tone == VrCensusTone::After) ++toneAfter; else ++toneNone;
    }
    void reset() { *this = VrCensusWindow{}; }
};
// The first kVrCensusEveryWindow windows (three minutes) print each; after that one line in kVrCensusThinTo covers that
// many windows (the counters keep adding up, so nothing is lost), which keeps a long session's 5 s lines near a hundred.
constexpr uint32_t kVrCensusEveryWindow = 36;
constexpr uint32_t kVrCensusThinTo = 12;
constexpr bool vrCensusWindowPrints(uint32_t tick) {   // tick: 1-based count of 5 s windows since the census started
    return tick <= kVrCensusEveryWindow || tick % kVrCensusThinTo == 0;
}

// ---- the budget ---------------------------------------------------------------------------------------------------
// A line class has a cap; past it a line is counted, not printed. The sum of the caps is the census's own worst case
// (about 700 lines, three call sequences of the full 160 calls). A real session is near the sum of what it saw: one 5 s
// line a window for three minutes and one a minute after (about fifty in ten minutes), a line per camera (a dozen or
// two), three call sequences of a frame's length (about 110 calls each) and eight eye draws: about four hundred.
enum class VrCensusLines : uint8_t { Window, Camera, Changed, Call, Eye, Thread, Info, kCount };
inline const char* vrCensusLinesName(VrCensusLines c) {
    switch (c) {
        case VrCensusLines::Window: return "5s";
        case VrCensusLines::Camera: return "camera";
        case VrCensusLines::Changed: return "changed";
        case VrCensusLines::Call: return "call";
        case VrCensusLines::Eye: return "eye";
        case VrCensusLines::Thread: return "other-thread";
        case VrCensusLines::Info: return "info";
        case VrCensusLines::kCount: break;
    }
    return "?";
}
struct VrCensusBudget {
    static constexpr uint32_t kCap[static_cast<size_t>(VrCensusLines::kCount)] = {100, 64, 24, 480, 16, 16, 24};
    uint32_t used[static_cast<size_t>(VrCensusLines::kCount)] = {};
    uint32_t suppressed[static_cast<size_t>(VrCensusLines::kCount)] = {};
    bool take(VrCensusLines c) {
        const size_t i = static_cast<size_t>(c);
        if (used[i] < kCap[i]) { ++used[i]; return true; }
        ++suppressed[i];
        return false;
    }
    uint32_t total() const {
        uint32_t n = 0;
        for (size_t i = 0; i < static_cast<size_t>(VrCensusLines::kCount); ++i) n += used[i];
        return n;
    }
    static uint32_t capTotal() {
        uint32_t n = 0;
        for (size_t i = 0; i < static_cast<size_t>(VrCensusLines::kCount); ++i) n += kCap[i];
        return n;
    }
};
constexpr uint32_t kVrCensusMaxSequences = 3;   // on-foot frames whose whole call sequence is printed
constexpr uint32_t kVrCensusMaxEyeFrames = 4;   // on-foot frames whose eye draws are read back
constexpr uint32_t kVrCensusMaxEyeDraws = 8;    // and the eye draws in all: each is a staging readback that stalls once
constexpr size_t kVrCensusLineBytes = 400;      // a line is at most this many characters
// The frame that just ended prints its call sequence when the census samples it (vrCensusSamplesFrame: the tone was seen
// and the journal, if it is read, says on foot) and fewer than kVrCensusMaxSequences have printed. The recording of calls
// stops with the last sequence: a call is then only counted.
constexpr bool vrCensusPrintsSequence(bool sampledFrame, uint32_t sequencesLogged) {
    return sampledFrame && sequencesLogged < kVrCensusMaxSequences;
}

// Which eye draws are read back: the first kVrCensusMaxEyeDraws draws of the first kVrCensusMaxEyeFrames on-foot frames.
class VrCensusEyeBudget {
public:
    bool take(uint64_t frame) {
        if (draws_ >= kVrCensusMaxEyeDraws) return false;
        bool known = false;
        for (uint32_t i = 0; i < frames_; ++i) if (frame_[i] == frame) known = true;
        if (!known) {
            if (frames_ >= kVrCensusMaxEyeFrames) return false;
            frame_[frames_++] = frame;
        }
        ++draws_;
        return true;
    }
    uint32_t draws() const { return draws_; }
    uint32_t frames() const { return frames_; }
private:
    uint64_t frame_[kVrCensusMaxEyeFrames] = {};
    uint32_t frames_ = 0, draws_ = 0;
};

// ---- the text of every line -----------------------------------------------------------------------------------
// Each returns snprintf's length and is handed at most kVrCensusLineBytes + 1 bytes. No field ever holds a space inside
// a (..) or [..] value, so edvr_log.py can split a line into key=value tokens.
namespace vrcensus_detail {
struct Out {
    char* p; size_t size; size_t n = 0;
    Out(char* out, size_t cap) : p(out), size(cap) { if (cap) out[0] = 0; }
    template <class... A> void put(const char* fmt, A... args) {
        if (n + 1 >= size) return;
        const int w = std::snprintf(p + n, size - n, fmt, args...);
        if (w > 0) n += static_cast<size_t>(w) < size - n ? static_cast<size_t>(w) : size - n - 1;
    }
    // A float as the log prints it: seven significant digits, "nan" for anything not finite.
    void f(float v) { if (std::isfinite(v)) put("%.7g", static_cast<double>(v)); else put("nan"); }
    void d(double v) { if (std::isfinite(v)) put("%.7g", v); else put("nan"); }
    void e(double v) { if (std::isfinite(v)) put("%.3e", v); else put("nan"); }
    void list(const float* v, int count) {
        put("[");
        for (int i = 0; i < count; ++i) { if (i) put(","); f(v[i]); }
        put("]");
    }
    void pair(float a, float b) { put("("); f(a); put(","); f(b); put(")"); }
    void draw(bool known, uint32_t draw) { if (known) put("%u", draw); else put("-"); }
};
}  // namespace vrcensus_detail

struct VrCensusWindowText {
    const char* hook = "pending";
    VrCensusFoot foot = VrCensusFoot::Off;   // what the journal said when the window closed
    uint64_t offThread = 0, offThreadOverflow = 0;
    uint32_t camerasTotal = 0;
    uint64_t cameraTableOverflow = 0;
};
inline int vrCensusFormatWindow(char* out, size_t size, const VrCensusWindow& w, const VrCensusWindowText& t) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census 5s: frames=%llu calls=%llu posts=%llu off-thread=%llu stale=%llu kinds=",
          (unsigned long long)w.frames, (unsigned long long)w.calls, (unsigned long long)w.posts,
          (unsigned long long)t.offThread, (unsigned long long)w.stale);
    bool any = false;
    for (int k = 0; k < 8; ++k) {
        if (!w.kinds[k]) continue;
        if (k < 6) o.put("%s%d:%llu", any ? "," : "", k, (unsigned long long)w.kinds[k]);
        else o.put("%s%s:%llu", any ? "," : "", k == 6 ? "other" : "unreadable", (unsigned long long)w.kinds[k]);
        any = true;
    }
    if (!any) o.put("-");
    o.put(" callers=");
    // The busiest first, at most six: the rest are named in a count.
    bool taken[VrCensusWindow::kCallers] = {};
    uint32_t printed = 0;
    for (; printed < 6 && printed < w.callerCount; ++printed) {
        size_t best = VrCensusWindow::kCallers;
        for (size_t i = 0; i < w.callerCount; ++i)
            if (!taken[i] && (best == VrCensusWindow::kCallers || w.callers[i].n > w.callers[best].n)) best = i;
        if (best == VrCensusWindow::kCallers) break;
        taken[best] = true;
        o.put("%s+0x%X:%llu", printed ? "," : "", w.callers[best].rva, (unsigned long long)w.callers[best].n);
    }
    if (!printed) o.put("-");
    const uint64_t unnamed = static_cast<uint64_t>(w.callerCount - printed) + w.callerOverflow;
    if (unnamed) o.put(",+more:%llu", (unsigned long long)unnamed);
    o.put(" cameras-seen=%u cameras-total=%u tone=%llu/%llu/%llu tone-frames=%llu on-foot-frames=%llu foot=%s "
          "eye-draws=%llu/%llu progress=%s hook=%s windows=%u cam-overflow=%llu thread-overflow=%llu",
          w.cameraCount, t.camerasTotal, (unsigned long long)w.toneBefore, (unsigned long long)w.toneAfter,
          (unsigned long long)w.toneNone, (unsigned long long)w.toneFrames, (unsigned long long)w.onFootFrames,
          vrCensusFootName(t.foot), (unsigned long long)w.eyeDraws, (unsigned long long)w.eyeOnFoot,
          w.progressSeen ? "yes" : "no", t.hook, w.windows,
          (unsigned long long)(w.cameraOverflow + t.cameraTableOverflow), (unsigned long long)t.offThreadOverflow);
    return static_cast<int>(o.n);
}

inline int vrCensusFormatCamera(char* out, size_t size, const VrCensusCamera& c) {
    vrcensus_detail::Out o(out, size);
    const VrCensusSig& s = c.firstSig;
    o.put("vr camera census: camera=0x%llx kind=%u caller=+0x%X thread=owner aspect=", (unsigned long long)c.camera, s.kind,
          c.callerRva);
    o.f(s.aspect); o.put(" near="); o.f(s.nearZ); o.put(" far="); o.f(s.farZ); o.put(" fov="); o.f(s.fov);
    o.put(" bound="); o.pair(s.boundX, s.boundY);
    o.put(" offcentre="); o.pair(s.p8, s.p9);
    o.put(" viewport="); o.pair(s.viewportW, s.viewportH);
    o.put(" tan=");
    if (c.tanValid) { o.put("("); o.f(c.tan[0]); o.put(","); o.f(c.tan[1]); o.put(","); o.f(c.tan[2]); o.put(","); o.f(c.tan[3]); o.put(")"); }
    else o.put("-");
    // The first call's second argument (the view, the pass the camera belongs to) and first (the view-constant context).
    o.put(" view=0x%llx vctx=0x%llx", (unsigned long long)c.firstView, (unsigned long long)c.firstCtx);
    o.put(" first-call=%u draw=", c.firstOrdinal);
    o.draw(c.firstDrawKnown, c.firstDraw);
    o.put(" tone=%s frame=%llu", vrCensusToneName(c.firstTone), (unsigned long long)c.firstFrame);
    return static_cast<int>(o.n);
}

// "changed:" names only the fields that moved, old->new. n is the camera's running count of changed calls.
inline int vrCensusFormatChanged(char* out, size_t size, const VrCensusCamera& c) {
    vrcensus_detail::Out o(out, size);
    const VrCensusSig& a = c.changeFrom;
    const VrCensusSig& b = c.sig;
    o.put("vr camera census: changed: camera=0x%llx frame=%llu n=%u", (unsigned long long)c.camera,
          (unsigned long long)c.changeFrame, c.changes);
    if (a.kind != b.kind) o.put(" kind=%u->%u", a.kind, b.kind);
    auto field = [&](const char* name, float from, float to) {
        if (!vrCensusFieldMoved(from, to)) return;
        o.put(" %s=", name); o.f(from); o.put("->"); o.f(to);
    };
    field("aspect", a.aspect, b.aspect);
    field("near", a.nearZ, b.nearZ);
    field("far", a.farZ, b.farZ);
    field("fov", a.fov, b.fov);
    if (vrCensusBoundMoved(a.boundX, b.boundX) || vrCensusBoundMoved(a.boundY, b.boundY)) {
        o.put(" bound="); o.pair(a.boundX, a.boundY); o.put("->"); o.pair(b.boundX, b.boundY);
    }
    if (vrCensusFieldMoved(a.viewportW, b.viewportW) || vrCensusFieldMoved(a.viewportH, b.viewportH)) {
        o.put(" viewport="); o.pair(a.viewportW, a.viewportH); o.put("->"); o.pair(b.viewportW, b.viewportH);
    }
    return static_cast<int>(o.n);
}

inline int vrCensusFormatSequence(char* out, size_t size, uint64_t frame, uint32_t index, VrCensusFoot foot, uint32_t calls,
                                  uint32_t recorded) {
    return std::snprintf(out, size, "vr camera census: sequence frame=%llu index=%u/%u foot=%s calls=%u recorded=%u truncated=%u",
                         (unsigned long long)frame, index, kVrCensusMaxSequences, vrCensusFootName(foot), calls, recorded,
                         calls - recorded);
}

inline int vrCensusFormatCall(char* out, size_t size, uint64_t frame, uint32_t ordinal, const VrCensusCall& c) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: call frame=%llu n=%u camera=0x%llx kind=", (unsigned long long)frame, ordinal,
          (unsigned long long)c.camera);
    if (c.kindReadable) o.put("%u", c.kind); else o.put("-");
    o.put(" caller=+0x%X draw=", c.callerRva);
    o.draw(c.drawKnown, c.draw);
    o.put(" tone=%s fl=0x%X>", vrCensusToneName(c.tone), c.preFlags);
    if (c.postSeen) o.put("0x%X", c.postFlags); else o.put("-");
    o.put(" view=0x%llx rows=", (unsigned long long)c.view);
    if (c.rowsValid) o.list(c.rows, 16); else o.put("-");
    return static_cast<int>(o.n);
}

// The eye draw's own rows and what they measure. why: null when the rows were read, else the reason they were not.
inline int vrCensusFormatEye(char* out, size_t size, uint32_t eye, uint64_t frame, VrCensusFoot foot, bool drawKnown,
                             uint32_t draw, uint64_t b1, uint32_t firstConstant, uint32_t bytes, const float rows[16],
                             bool measured, double measX, double measY, const char* why) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: eye=%u frame=%llu foot=%s draw=", eye, (unsigned long long)frame, vrCensusFootName(foot));
    o.draw(drawKnown, draw);
    o.put(" b1=0x%llx first=%u bytes=%u rows=", (unsigned long long)b1, firstConstant, bytes);
    if (rows) o.list(rows, 16); else o.put("-");
    o.put(" meas=");
    if (measured) { o.put("("); o.d(measX); o.put(","); o.d(measY); o.put(")"); } else o.put("-");
    if (why) o.put(" why=%s", why);
    return static_cast<int>(o.n);
}

// What EDVR advertised for the eye this sequence, and the leak: the measured shift of the eye's rows minus the shift the
// advertised frustum and shift should give. Nothing but the eye shift moves an eye camera today, so the leak reads zero
// (within float rounding) in this census; the number is the baseline the world route's leak detector will be judged against.
inline int vrCensusFormatEyeGeometry(char* out, size_t size, uint32_t eye, uint64_t frame, bool known, uint64_t sequence,
                                     const float frustum[4], const float shift[2], bool measured, double measX,
                                     double measY) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: eye-geometry eye=%u frame=%llu", eye, (unsigned long long)frame);
    if (!known) { o.put(" geometry=unavailable"); return static_cast<int>(o.n); }
    o.put(" seq=%llu frustum=", (unsigned long long)sequence);
    o.list(frustum, 4);
    o.put(" shift="); o.pair(shift[0], shift[1]);
    double tx = 0, ty = 0, sx = 0, sy = 0;
    const bool haveTrue = vrCensusExpectedMeasure(frustum, 0.0f, 0.0f, &tx, &ty);
    const bool haveShifted = vrCensusExpectedMeasure(frustum, shift[0], shift[1], &sx, &sy);
    o.put(" expect=");
    if (haveTrue) { o.put("("); o.d(tx); o.put(","); o.d(ty); o.put(")"); } else o.put("-");
    o.put(" expect-shifted=");
    if (haveShifted) { o.put("("); o.d(sx); o.put(","); o.d(sy); o.put(")"); } else o.put("-");
    o.put(" leak=");
    if (measured && haveShifted) { o.put("("); o.e(measX - sx); o.put(","); o.e(measY - sy); o.put(")"); } else o.put("-");
    return static_cast<int>(o.n);
}

inline int vrCensusFormatOtherThread(char* out, size_t size, const VrCensusOffThread::Entry& e) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: other-thread tid=%u camera=0x%llx kind=", e.thread, (unsigned long long)e.camera);
    if (e.kindReadable) o.put("%u", e.kind); else o.put("-");
    o.put(" caller=+0x%X calls=%llu", e.callerRva, (unsigned long long)e.calls);
    return static_cast<int>(o.n);
}

}  // namespace edvr

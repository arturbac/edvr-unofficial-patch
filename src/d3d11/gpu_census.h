#pragma once
#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

// How much of a frame's GPU time is EDVR's own work, feature by feature
// (issue #38): one line every 30 s. Each section wraps a CALL SITE, so any
// timer inside it closes first, and counts every occurrence. Only one
// section is timed per frame, round-robin, and at most K occurrences of it
// (4 at the door, 8 per draw), because the shared disjoint clock fits
// about 31 spans a frame across the whole DLL (gpu_disjoint_clock.h).
// ms/frame = mean timed ms per occurrence x occurrences per frame.
// temporal_pass.cpp's price report times some of the same door work on its
// own event-driven windows; this census reads neither it nor the per-pass
// timers, so every figure comes from this window's live calls.
//
// A call site must be where GPU work is actually issued, not merely where a
// feature's entry point is invoked: most calls into engine velocity or
// screen motion return without submitting anything, and a timestamp pair
// around a no-op mostly measures its own pipeline-drain cost. To correct for
// that, a section's turn also times one empty begin/end pair (nothing
// between) at its first timed call, in a second sampler; ms/frame above
// subtracts that pair's own mean cost from the real one's, floored at zero.
enum class GpuCensusSection : uint8_t {
    // Door: once or twice a frame, at Submit. K = 2 (both eyes) while active.
    DoorTemporalWhole = 0,    // the whole temporalInner call, both eyes (edvrTemporalAa)
    DoorUpscaler,             // fsr/dlaa, and the fovea crops (periphery + centre) -- nested inside DoorTemporalWhole
    DoorMotionPrep,           // the motion-vector dispatch (mvCs) -- nested inside DoorTemporalWhole
    DoorHologramResolve,      // uiDepthHologramResolve + celestialMotionViews -- nested inside DoorTemporalWhole
    DoorUiResolve,            // applyUiResolve -- nested inside DoorTemporalWhole
    DoorSharpen,              // sharpen_pass.cpp's dispatch
    DoorMenu,                 // menu_panel.cpp's dispatch
    DoorUiLayerComposite,     // ui_layer.cpp's kComposite dispatch
    DoorFssHeal,              // fss_heal.cpp's dispatch
    // In-frame: per game draw. K = 8 while active.
    FrameHologramPasses,      // the two hologram/icon depth reissues
    FrameUiDepthCoverage,     // the UI-depth family reissue (UiContent::prepare, stellar coverage nest inside)
    FramePlanet,              // the planet/solar terrain reissue
    FrameTerrain,             // the terrain/celestial motion reissue
    FrameScreenMotion,        // screen_motion.cpp's own GPU work: the UI mask clear+reissue,
                              // the eye's buffer/size copies, clear and projection draw, the
                              // panel-count readback -- not screenMotionUiDraw/screenMotionDraw's
                              // call sites, most of which return without issuing anything
    FrameWeaponMotion,        // the weapon motion-vector reissue
    FrameEngineVelocity,      // engine_velocity.cpp's own GPU work: the eye-frame clear, the
                              // pool+scene snapshot copy and the append refresh copy -- not
                              // engineVelocityBeforeDraw's call sites, which mostly return
                              // without reaching the slow path at all
    FrameUiLayerReissues,     // the UI layer's multiply/write-back reissues
    // Elite's OWN draws that EDVR alters (see AlteredDrawClass below): the game's
    // draw timed whole, so each figure holds the game's own work in it plus what
    // EDVR adds by binding its target or swapping its shader. NOT EDVR's cost,
    // and never part of "EDVR ~X". Per draw like the sections above (K = 8, the
    // same stride and rotation); they own the line after the main one.
    AlteredPoolFamily,        // a pool-family draw with EDVR's MRT6 slot target bound and its shaders substituted
    AlteredTerrain,           // a null-pixel-shader terrain prepass with EDVR's motion target bound and a pixel shader added
    AlteredUiLayer,           // a UI draw redirected into EDVR's UI layer target (fix.ui_quality)
    AlteredVerdict,           // a draw wrapped in another fix's state change (RemLok, holo, scrim, particles, the panel ...)
    Count
};

// Which of the classes above a draw of Elite's is, decided where forwardWithVerdict
// issues the game's own draw. One class per draw, in this priority: a pool-family
// draw (only when no verdict claimed it, as engineVelocityBeforeDraw is), a terrain
// original, a UI-layer redirect, then any other verdict's wrapper. None for a draw
// EDVR leaves as the game issued it, for a foreign context, and for every reissue.
enum class AlteredDrawClass : uint8_t { None = 0, PoolFamily, TerrainOriginal, UiLayer, Verdict };
inline AlteredDrawClass classifyAlteredDraw(bool owner, bool verdictNone, bool poolSubstituted,
                                            bool terrainOriginal, bool uiLayered) noexcept {
    if (!owner) return AlteredDrawClass::None;
    if (verdictNone && poolSubstituted) return AlteredDrawClass::PoolFamily;
    if (terrainOriginal) return AlteredDrawClass::TerrainOriginal;
    if (uiLayered) return AlteredDrawClass::UiLayer;
    if (!verdictNone) return AlteredDrawClass::Verdict;
    return AlteredDrawClass::None;
}
inline GpuCensusSection alteredSectionOf(AlteredDrawClass c) noexcept {
    return static_cast<GpuCensusSection>(static_cast<int>(GpuCensusSection::AlteredPoolFamily) +
                                         static_cast<int>(c) - 1);
}

// Begin around a call site's GPU work, End right after it. Begin ALWAYS
// counts the occurrence (cheap: one branch and an increment when this is
// not this frame's rotated section). It returns true only when this IS
// that section and a timer lease was actually opened; End is safe to call
// unconditionally either way (a no-op when nothing is open -- gpu_interval.h),
// so a plain Begin/.../End pair never needs to branch on Begin's result.
//
// Wrap CALL SITES, not the functions they call: any pre-existing timer a
// wrapped call opens and closes on its own nests correctly for free, since
// it completes before the wrapping End() runs.
bool gpuCensusBegin(ID3D11DeviceContext* ctx, GpuCensusSection section) noexcept;
void gpuCensusEnd(ID3D11DeviceContext* ctx, GpuCensusSection section) noexcept;

// RAII alternative for a call site that is a clean lexical block: construct
// at the top, let it close at scope exit (including an early return) so a
// span can never outlive the work it times.
class GpuCensusScope {
public:
    GpuCensusScope(ID3D11DeviceContext* ctx, GpuCensusSection section) noexcept
        : ctx_(ctx), section_(section) { gpuCensusBegin(ctx_, section_); }
    ~GpuCensusScope() { gpuCensusEnd(ctx_, section_); }
    GpuCensusScope(const GpuCensusScope&) = delete;
    GpuCensusScope& operator=(const GpuCensusScope&) = delete;

private:
    ID3D11DeviceContext* ctx_;
    GpuCensusSection section_;
};

// The scope for one of Elite's altered draws: wraps ONLY the game's own real draw
// call (never the reissues or the extra work the thunk does after it, which have
// sections of their own), counts the occurrence, and times it on the section's
// turn. A None class is one compare and nothing else. Defined after
// gpuCensusBegin/End, which it calls.
class GpuCensusAlteredScope {
public:
    GpuCensusAlteredScope(ID3D11DeviceContext* ctx, AlteredDrawClass c) noexcept
        : ctx_(ctx), section_(c == AlteredDrawClass::None ? GpuCensusSection::Count : alteredSectionOf(c)) {
        if (c != AlteredDrawClass::None) open_ = gpuCensusBegin(ctx_, section_);
    }
    ~GpuCensusAlteredScope() {
        if (open_) gpuCensusEnd(ctx_, section_);
    }
    GpuCensusAlteredScope(const GpuCensusAlteredScope&) = delete;
    GpuCensusAlteredScope& operator=(const GpuCensusAlteredScope&) = delete;

private:
    ID3D11DeviceContext* ctx_;
    GpuCensusSection section_;
    bool open_ = false;
};

// Once a frame, from vScreenFrameBoundary (after the frame's own Begin/End
// calls, alongside the other features' *FrameBoundary calls): polls every
// section's sampler, advances the rotation for the frame about to start,
// and -- every 30 s of wall clock -- logs one summary line and resets the
// window. No ini key: this is always on.
void gpuCensusFrame(ID3D11DeviceContext* ctx) noexcept;

// Quiescent cleanup, alongside the other feature modules' Shutdown().
void gpuCensusShutdown() noexcept;

} // namespace edvr

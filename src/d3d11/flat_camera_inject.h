#pragma once

#include <cstdint>

#include "flat_camera_phase.h"

// The upstream camera injector (docs/design-flat-camera-integration.md, the
// C3 wiring plan addendum): a CodeHook detour on the game's view-constant
// refresh (FUN_1405921f0) that applies the temporal phase to the camera's
// frustum parameters, transiently, so every projection-dependent consumer in
// the call derives from the jittered values through the game's own
// finalizers. On whenever the flat profile has a temporal mode selected (no
// setting: the draw-time adapter is only the automatic fallback -- no
// injectable camera, a prologue mismatch after a game update, or too many
// failed writes); with the mode off nothing is installed or logged, and a hook
// that stood down stays inert behind its gate for process lifetime (the
// producer probe's discipline).
//
// The frame protocol flat_runtime drives, in order, once per Present:
//   flatCameraInjectDisarm()      the Present edge: the frame window closes
//   flatCameraInjectClose(...)    the frame that ended is closed (history)
//   flatCameraInjectFrame(n, on)  owner selection for the new frame, the tick
//   flatCameraInjectTakeHistoryReset()  one-shot, honored before the phase
//   flatCameraInjectRoute()       decides whether the phase machine runs
//   flatCameraInjectArm()         the phase is chosen: the window opens
namespace edvr {

// Frame start, called BEFORE the phase machine's beginFrame: ownership begin
// and selection for the new frame, the hook install (once), the 5s tick.
// temporalModeEnabled: a temporal mode is selected (flatCameraPathWanted).
void flatCameraInjectFrame(uint64_t frame, bool temporalModeEnabled);

// This frame's route (Off when the injector is not wanted).
FlatCameraRoute flatCameraInjectRoute();

// True once for a decision that switched the history identity between the
// Legacy and Upstream routes: the runtime resets its temporal history.
bool flatCameraInjectTakeHistoryReset();

// The window the refresh detour may inject in: open after the frame's phase is
// chosen, closed at every Present edge (on the thread that runs Present, which
// becomes the only thread the detour may act on).
void flatCameraInjectArm();
void flatCameraInjectDisarm();

// The frame that just ended, after the phase machine finished it: phaseNonzero
// is its phase, applied whether an injection landed, clean the phase machine's
// own verdict on it, sceneNamed whether a scene depth was named.
void flatCameraInjectClose(bool phaseNonzero, bool applied, bool clean, bool sceneNamed);

// Resize or device change: history and the decision do not survive; cameras
// this session injected stay known so their first un-injected call flushes.
void flatCameraInjectReset();

// The runtime's stand-down (flat_standdown.h) pauses the refresh hook while every
// frame is refused: the relay's gate closes and the game's camera refresh runs
// straight through to the original, and reopens on resume. Called every frame from
// the owner thread with the frame's desired state. A hook that stood down for good
// (write failures) is never reopened by it, and the gate closes only once no camera
// still holds an injected phase, because that camera's first un-injected call is the
// flush and a closed relay would never see it.
void flatCameraInjectPause(bool paused);

// What the rest of flat_runtime needs:
bool flatCameraInjectUpstreamOwns();  // this frame's ownership decision is Upstream
bool flatCameraInjectBypassRefusal(const char* reason); // the legacy-only refusal classes

// ---------------------------------------------------------------------------
// THE OBSERVE-ONLY MODE (2026-09-30): the VR camera census
// (src/d3d11/vr_camera_census.cpp, design doc section 82, "Pre-build
// findings") runs this same detour in the VR profile to learn which cameras
// reach the refresh and how an eye camera differs from the world's. In this
// mode the detour NEVER writes a camera: no bound pair, no dirty flag, no row,
// no flush, and flatCameraAdmit cannot answer Inject (FlatCameraAdmit::Observed).
// The one store it makes is the body's return slot, redirected to stubB so the
// post half can read what the body derived; stubB jumps to the real return
// address, exactly as it does for an injected call. The flat runtime is never
// asked anything: no phase, no legacy plan, no "camera applied" note.
//
// The census's per-frame protocol replaces the flat one above:
//   flatCameraInjectDisarm()         the Present edge: this thread is the owner
//   flatCameraInjectObserveFrame()   installs the hook (once), opens the window
// and it registers its observer with flatCameraInjectSetObserver before the
// first call can arrive.
// ---------------------------------------------------------------------------
struct FlatCameraObserveCall {
    uintptr_t camera = 0;      // r8, the camera struct (non-null: a null camera is not reported)
    uintptr_t ctx = 0;         // rcx, the view-constant context
    uintptr_t p2 = 0;          // rdx
    uint64_t callerRva = 0;    // the call site's offset from the game module, 0 when unknown
    uint64_t callNo = 0;       // the detour's own call counter, 1-based
    uint32_t kind = 0;         // camera+0x264 (meaningless unless kindReadable)
    bool kindReadable = false;
    uint8_t window = 0;        // the frame window at the call: 0 open, 1 closed, 2 lapsed
};
struct FlatCameraObserver {
    // The owner thread, before the game's body runs. True asks for post() after it (the return is redirected).
    bool (*pre)(const FlatCameraObserveCall& call) noexcept = nullptr;
    // The owner thread, after the body returned.
    void (*post)(uintptr_t camera, uintptr_t ctx) noexcept = nullptr;
    // ANY thread but the owner's (so it must be lock-free): the call is counted and passed through untouched.
    void (*offThread)(uintptr_t camera, uint64_t callerRva, uint32_t kind, bool kindReadable,
                      uint32_t thread) noexcept = nullptr;
};
// The observer the detour reports to; null detaches it. The pointed-to struct must outlive the process's last call.
void flatCameraInjectSetObserver(const FlatCameraObserver* observer);
// The per-frame step of observe mode: switches the detour to observe-only (before the first install, so its first
// call already is), installs the hook once, opens the frame window. False while there is no live hook (a failed or
// stood-down install): the census then reports what it did not see.
bool flatCameraInjectObserveFrame();
// "pending" (no install tried), "installed", "failed" (a refused install is final for the session) or "down".
const char* flatCameraInjectObserveStatus();

} // namespace edvr

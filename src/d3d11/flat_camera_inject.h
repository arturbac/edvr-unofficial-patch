#pragma once

#include <cstdint>

// The upstream camera injector (docs/design-flat-camera-integration.md, the
// C3 wiring plan addendum): a CodeHook detour on the game's view-constant
// refresh (FUN_1405921f0) that applies the temporal phase to the camera's
// frustum parameters, transiently, so every projection-dependent consumer in
// the call derives from the jittered values through the game's own
// finalizers. Live mutation only under fix.temporal_aa_camera = on; the hook
// otherwise sits inert behind its gate for process lifetime (the producer
// probe's discipline).
namespace edvr {

// Frame-boundary drive, called from flat_runtime next to the phase machine's
// beginFrame: ownership begin for the new frame and the injector's own
// counters cadence.
void flatCameraInjectFrame(uint64_t frame);

// What the rest of flat_runtime needs:
bool flatCameraInjectWanted();        // the config key is on and the profile is flat
bool flatCameraInjectUpstreamOwns();  // this frame's ownership decision is Upstream
bool flatCameraInjectBypassRefusal(const char* reason); // the observation-veto class

} // namespace edvr

// The VR on-foot world route, the runtime (vr_world_route.h; design doc section 82).
// SKELETON of the published state and its accessors: the detector, the resolve and the frame boundary are filled in by the
// route's own build step. The accessors below are final; other modules compile and link against them.
#include "vr_world_route.h"
#include "../common/config.h"

namespace edvr {

bool g_vrWorldWants = false;
thread_local bool g_vrWorldInternal = false;

namespace {
VrWorldMachine g_machine;
bool g_enabled = false;
bool g_treatedThisFrame = false;
uint64_t g_takenSequence[2] = {0, 0};
std::atomic<bool> g_ownsNext{false};
}  // namespace

bool vrWorldRouteEnabled() { return g_enabled; }
VrWorldState vrWorldRouteState() { return g_machine.state; }
bool vrWorldRouteOwnsNextFrame() { return g_ownsNext.load(std::memory_order_acquire); }
bool vrWorldRouteTreatedThisFrame() { return g_treatedThisFrame; }
bool vrWorldRouteLayerMayTake() { return vrWorldLayerMayTake(g_machine.state, g_treatedThisFrame); }
void vrWorldRouteNoteEyeTaken(uint32_t eye, uint64_t sequence) { if (eye < 2) g_takenSequence[eye] = sequence; }
bool vrWorldRouteDoorLayerOnly(uint32_t eye, uint64_t sequence) {
    return eye < 2 && vrWorldDoorLayerOnly(g_takenSequence[eye], sequence);
}
void vrWorldRouteNoteSceneReset() {}
bool vrWorldRouteDrawProgress(uint32_t*, bool*, uint64_t*) { return false; }
void vrWorldRouteDraw(ID3D11DeviceContext*) {}
void vrWorldRouteFrameBoundary() {
    // The key is read live, at the boundary, so the frame that starts now is the first to see a change. Off unless the value
    // is "auto" (vrWorldKeyFromText): a typo leaves on-foot VR as it was.
    g_enabled = vrWorldKeyFromText(Config::get().getString("experimental.temporal_aa_on_foot_world", "off").c_str()) ==
                VrWorldKey::Auto;
}

}  // namespace edvr

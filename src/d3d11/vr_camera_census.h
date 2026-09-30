// The VR camera census (design doc section 82, "Pre-build findings"): a ZERO-MUTATION observation of the game's view-constant
// refresh (the camera injector's detour) in the VR profile, to answer the one question the world route's jitter waits on:
// how to tell the eye views' cameras from the world's. Key: advanced.vr_camera_census = off|on, default off. With it on in the
// VR profile the detour is installed and only ever OBSERVES (it never writes a bound pair, a flag or a row); with it off,
// nothing is installed and nothing changes.
//
// What it records, bounded (see vr_camera_census.cpp): per distinct camera, its kind, caller, thread and field signature
// (aspect, near, field of view, the bound pairs, the viewport); the place of each refresh call against the game's draws and
// the world route's tone trigger; the rows the composer produced; and, at each eye composite draw, the eye's b1 rows and the
// frusta EDVR itself advertised, so the eye cameras can be joined to their rows offline.
#pragma once
#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

// The key is on in the VR profile (read at the last boundary). Render thread. The world route's detector watches draws while
// this is true, so the census can say where in the frame each refresh call falls.
bool vrCameraCensusWanted();
// Once a frame at the Present boundary, AFTER vrWorldRouteFrameBoundary(): runs the injector's per-frame protocol in observe
// mode (Close the frame that ended, Frame, Arm), rolls the per-frame call sequence, prints the 5 s line and the bounded
// per-camera lines.
void vrCameraCensusFrameBoundary();
// At each eye composite draw (2 a frame), after the game's own draw: logs, for the first few on-foot frames, the eye's VS b1
// rows 270..273 and the frustum and shift EDVR advertised for that eye this sequence. Render thread.
void vrCameraCensusEyeDraw(ID3D11DeviceContext* ctx, uint32_t eye);

}  // namespace edvr

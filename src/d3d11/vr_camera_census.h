// The VR camera census (design doc section 82, "Pre-build findings and the stop"): a ZERO-MUTATION observation of the
// game's view-constant refresh (the camera injector's detour) in the VR profile, to answer the one question the world
// route's jitter waits on: how to tell the eye views' cameras from the world's. Key: advanced.vr_camera_census = off|on,
// default off. With it on in the VR profile the detour is installed and only ever OBSERVES (it never writes a bound
// pair, a flag or a row); with it off, nothing is installed and nothing changes: every entry point below returns at
// once, nothing is allocated and no line is logged (tools\vr_camera_census_test pins each).
//
// What it records, bounded (vr_camera_census_core.h has the tables, the budget and the text of every line):
//   - a 5 s line every window (zeros included): calls, calls on other threads, kinds, callers, distinct cameras, where in
//     the frame the calls fell against the tone draw;
//   - per distinct camera (the first 64): its kind, caller, view and field signature, and any later change of it;
//   - the FULL call sequence of the first three on-foot frames, one line a call, with the rows the composer produced;
//   - at the eye composite draw of the first four on-foot frames (eight draws): the eye's b1 rows 270..273 read back
//     from the GPU, what EDVR advertised for that eye, and the leak measure.
// An on-foot frame is one in which the world route's detector saw the tone AND Elite's journal, when it is read, says on
// foot (vrCensusSamplesFrame in the core): the detector draws the same tone in a cockpit, a hangar and a menu, and those
// frames must not spend the samples. When the route reports no draw progress at all (vrWorldRouteDrawProgress false) there
// is no tone and the journal alone decides. `python tools\edvr_log.py --camera-census` reads the log back and does the join.
#pragma once
#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

// The key is on in the VR profile (read at the last boundary). Render thread. The world route's detector watches draws while
// this is true, so the census can say where in the frame each refresh call falls.
bool vrCameraCensusWanted();
// Once a frame at the Present boundary, AFTER vrWorldRouteFrameBoundary(): runs the injector's per-frame protocol in observe
// mode (the Present edge, the hook, the window), rolls the per-frame call sequence, prints the 5 s line and the bounded
// per-camera lines.
void vrCameraCensusFrameBoundary();
// At each eye composite draw (2 a frame), after the game's own draw: logs, for the first few on-foot frames, the eye's VS b1
// rows 270..273 and the frustum and shift EDVR advertised for that eye this sequence. Render thread.
void vrCameraCensusEyeDraw(ID3D11DeviceContext* ctx, uint32_t eye);

// What EDVR advertised for `eye` this sequence (native_temporal.cpp, beside nativeTemporalDrawJitter): the frustum
// {left, right, down, up} tangents the host was given and the tangent shift the eye jitter moved it by. The render thread,
// outside treat(); false when there is no frame to describe.
bool nativeTemporalEyeGeometry(uint32_t eye, uint64_t* sequence, float frustum[4], float shift[2]);

}  // namespace edvr

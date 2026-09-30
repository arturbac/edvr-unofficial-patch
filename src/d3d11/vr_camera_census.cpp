// The VR camera census (vr_camera_census.h). SKELETON: the interface only; the implementation is the census agent's task
// (design doc section 82).
#include "vr_camera_census.h"

namespace edvr {

bool vrCameraCensusWanted() { return false; }
void vrCameraCensusFrameBoundary() {}
void vrCameraCensusEyeDraw(ID3D11DeviceContext*, uint32_t) {}

}  // namespace edvr

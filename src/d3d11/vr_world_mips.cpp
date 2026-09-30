// The VR world route's mipped screen (vr_world_mips.h). SKELETON: the interface only; the implementation is the layer/door
// agent's first task (design doc section 82).
#include "vr_world_mips.h"

namespace edvr {

ID3D11ShaderResourceView* vrWorldMipsScreen(ID3D11DeviceContext*, ID3D11Texture2D*, uint64_t) { return nullptr; }
ID3D11SamplerState* vrWorldMipsSampler(ID3D11Device*, const D3D11_SAMPLER_DESC&) { return nullptr; }
void vrWorldMipsReset() {}

}  // namespace edvr

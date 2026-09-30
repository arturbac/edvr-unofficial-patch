// The VR world route's mipped screen (design doc section 82, "Layer"): the game's screen texture, copied once a frame into
// a texture with a full mip chain, so the layer's re-issue of the eye composite minifies 5040 to about 3500 without aliasing.
//
// THE COLOUR-SPACE DECISION (section 82, 2026-09-30). The screen texture is R8G8B8A8_TYPELESS and the game views it as
// UNORM: display-encoded 8-bit, gamma-space numbers. The mips are generated through an _SRGB view of the mipped copy, so the
// box filter averages in linear light and re-encodes (energy preserving: a bright thin stroke, which is what HUD text is, is
// not dimmed as a gamma-space average dims it), and the layer samples it through the UNORM view the game's own shader
// expects (a sampled _SRGB view would hand the shader linear values and the layer would come out dark). Mip 0 is a
// byte copy either way.
#pragma once
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11SamplerState;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct D3D11_SAMPLER_DESC;

namespace edvr {

// Copy `screen` (a 2D, single-sample, single-mip R8G8B8A8-family texture) into the route's mipped texture and generate its
// mips, once per `frame`: a second call for the same frame and the same texture returns the same view without any GPU
// work. Returns the UNORM SRV over all mip levels to sample it through, valid until the next frame's call; null on any
// refusal (the reason is logged once), and the caller leaves the eye route to serve the eye. The GPU work is timed on
// GpuCensusSection::FrameWorldMips. Render thread.
ID3D11ShaderResourceView* vrWorldMipsScreen(ID3D11DeviceContext* ctx, ID3D11Texture2D* screen, uint64_t frame);

// A sampler like `game` (the address modes, the comparison-free rest) but trilinear with the full LOD range, created once
// per distinct game sampler and cached: the game's own s0 may have MaxLOD 0 or a point mip filter, which would waste the
// mips. Null on failure.
ID3D11SamplerState* vrWorldMipsSampler(ID3D11Device* device, const D3D11_SAMPLER_DESC& game);

// Let go of everything (device change, the key turned off, a size change). Render thread.
void vrWorldMipsReset();

}  // namespace edvr

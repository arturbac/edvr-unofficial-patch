// The VR on-foot world route's layer half, THE REAL CODE on WARP (docs/design-flat-temporal-aa-2026-09-23.md, section 82).
//
// src/d3d11/ui_layer.cpp is linked WHOLE -- the gatherer (uiLayerDecide), the re-issue (uiLayerWorldReissueBegin /
// End), the layer's own machinery they reuse (beginInner, restore, the composite), the door's preflight
// (uiLayerWorldDoorGap) -- and its neighbours are stubbed: the binding shadow is a few slots this rig writes, the
// route (vr_world_route.h) and the mips module (vr_world_mips.h) answer what the case asks, the raw OM/RS entry points
// go straight to the context. vscreen.cpp's own helper (worldScreenReissue) is four lines and is reproduced at the
// call site below; tools/ui_quality_test scans the real one.
//
// What this proves, each against the production functions and real D3D11 (WARP):
//   1. KEY OFF. With the route not owning the frame the screen composite is left in the picture (kWorldScreen),
//      nothing is pending, the mips module and the route are never asked.
//   2. THE RE-ISSUE. On an owned frame the decision is never a take (false), the game's own draw lands in its eye
//      image unchanged, and the re-issue draws the same quad into the eye's LAYER through the map (eye pixels x the
//      layer's scale), opaque, from the route's mipped screen through the route's trilinear sampler -- proven with a
//      mip chain of one flat colour a level, where the layer shows the level the map's minification selects (mip 1) and
//      the game's eye shows mip 0 -- and the composite of that layer over a frame IS the screen where the quad is and
//      the frame where it is not. The route is told which eye was taken, once, for that sequence.
//   3. EVERY CHANGED STATE COMES BACK: targets, depth target, viewport, scissor, blend (object, factor, mask), depth
//      state, PS slots 0 and 1 (textures and samplers), shaders -- exactly the game's objects, after a landed
//      re-issue and after every refusal.
//   4. EVERY REFUSAL: a named counter, the game's state untouched, nothing taken, the route never told (no mips, no
//      sampler, no sampler bound, no source texture, a curved screen, a blending draw, depth state, not armed, the
//      draw's bindings changed before the re-issue, abandoned).
//   5. THE HOOKS STEP ASIDE: every raw entry the re-issue calls runs with VrWorldInternalScope up, without an outer one.
//   6. THE DOOR'S PREFLIGHT and the accessors, the lost-draw counter, the stats.
// Exit codes: 0 pass, 1 a check failed, 2 usage. --dry-run touches nothing.
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/guard.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/depth_probe.h"
#include "../../src/d3d11/device_hook.h"
#include "../../src/d3d11/gpu_census.h"
#include "../../src/d3d11/gpu_timing.h"
#include "../../src/d3d11/journal_watch.h"
#include "../../src/d3d11/shader_swap.h"
#include "../../src/d3d11/ui_depth.h"
#include "../../src/d3d11/ui_layer.h"
#include "../../src/d3d11/ui_layer_math.h"
#include "../../src/d3d11/ui_panel_scale.h"
#include "../../src/d3d11/ui_surfaces.h"
#include "../../src/d3d11/vr_world_mips.h"
#include "../../src/d3d11/vr_world_route.h"
#include "../../src/d3d11/vscreen.h"

using Microsoft::WRL::ComPtr;

namespace {
unsigned g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("FAIL: %s\n", what);
    }
}

constexpr uint32_t kEyeW = 64, kEyeH = 48;   // the eye images the game draws the screen into
constexpr uint32_t kDoorW = 96, kDoorH = 72;  // the frame the door hands on: the layer's size at 1.0
constexpr uint32_t kScreenW = 144, kScreenH = 96;

// ------------------------------------------------------------------------------------ what the stubs answer
struct Stubs {
    // the route (vr_world_route.h)
    bool mayTake = false, enabled = false;
    unsigned tookCalls = 0;
    uint32_t tookEye = 9;
    uint64_t tookSeq = 0;
    // the mips module (vr_world_mips.h)
    ID3D11ShaderResourceView* mipsAnswer = nullptr;
    ID3D11SamplerState* samplerAnswer = nullptr;
    unsigned mipsCalls = 0, samplerCalls = 0;
    uint64_t lastMipsFrame = 0;
    const void* lastMipsScreen = nullptr;
    // the draw-time jitter the native temporal channel reports
    uint64_t seq = 0;
    bool jitterOk = true;
    float jx = 0.0f, jy = 0.0f;
    // the journal, the eye table
    bool journalKnown = true, journalOnFoot = true;
    const void* eyeRes[2] = {nullptr, nullptr};
    // raw entries: did any run with the route's internal scope up / down
    unsigned rawCalls = 0, rawInternal = 0;
};
Stubs g_stubs;
}  // namespace

// ============================================================================== the neighbours, stubbed
namespace edvr {
thread_local bool g_flatComputeInternal = false;
thread_local bool g_vrWorldInternal = false;
namespace detail {
BindingSlot g_bindingSlots[static_cast<size_t>(BindSlot::Count)];
}  // namespace detail
void breadcrumb(const char*) {}  // production guard.cpp's crash-channel dependency

bool vrWorldRouteEnabled() { return g_stubs.enabled; }
bool vrWorldRouteLayerMayTake() { return g_stubs.mayTake; }
void vrWorldRouteNoteEyeTaken(uint32_t eye, uint64_t sequence) {
    ++g_stubs.tookCalls;
    g_stubs.tookEye = eye;
    g_stubs.tookSeq = sequence;
}
ID3D11ShaderResourceView* vrWorldMipsScreen(ID3D11DeviceContext*, ID3D11Texture2D* screen, uint64_t frame) {
    ++g_stubs.mipsCalls;
    g_stubs.lastMipsFrame = frame;
    g_stubs.lastMipsScreen = screen;
    return g_stubs.mipsAnswer;
}
ID3D11SamplerState* vrWorldMipsSampler(ID3D11Device*, const D3D11_SAMPLER_DESC&) {
    ++g_stubs.samplerCalls;
    return g_stubs.samplerAnswer;
}
bool nativeTemporalDrawJitter(uint32_t eye, uint64_t* sequence, float* jx, float* jy, uint32_t* w, uint32_t* h) {
    if (!g_stubs.jitterOk || eye > 1) return false;
    if (sequence) *sequence = g_stubs.seq;
    if (jx) *jx = g_stubs.jx;
    if (jy) *jy = g_stubs.jy;
    if (w) *w = kEyeW;
    if (h) *h = kEyeH;
    return true;
}
// The binding shadow's resolver, from the views themselves.
bool bindingResolve(void* view, ResourceInfo* out) {
    if (!view || !out) return false;
    ComPtr<ID3D11Resource> res;
    static_cast<ID3D11View*>(view)->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    if (!res || FAILED(res.As(&tex))) return false;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    *out = ResourceInfo{};
    out->isTexture2D = true;
    out->a = d.Width;
    out->b = d.Height;
    out->fmt = d.Format;
    out->resource = res.Get();
    return true;
}
bool bindingResolveResource(void*, ResourceInfo*) { return false; }
bool depthProbeDrawsAtSize(uint32_t, uint32_t, uint32_t*) { return false; }
bool deviceHookHmdQuality(float*) { return false; }
bool gpuCensusBegin(ID3D11DeviceContext*, GpuCensusSection) noexcept { return false; }
void gpuCensusEnd(ID3D11DeviceContext*, GpuCensusSection) noexcept {}
void gpuCensusNoteSeedTarget(const GpuCensusSeedTarget&) noexcept {}
bool gpuTimingBind(ID3D11Device*, ID3D11DeviceContext*, const GpuSpanD3D11Ops&) noexcept { return false; }
bool gpuTimingAccepts(ID3D11DeviceContext*) noexcept { return false; }
bool gpuTimingOwns(ID3D11DeviceContext*) noexcept { return false; }
bool GpuTimer::begin(ID3D11Device*, ID3D11DeviceContext*) noexcept { return false; }
bool GpuTimer::beginBorrowedFrame(ID3D11Device*, ID3D11DeviceContext*) noexcept { return false; }
bool GpuTimer::end(ID3D11DeviceContext*) noexcept { return false; }
GpuTimerPoll GpuTimer::poll(ID3D11DeviceContext*, double&) noexcept { return GpuTimerPoll::Pending; }
void GpuTimer::reset(ID3D11DeviceContext*) noexcept {}
bool journalWatchActive() { return true; }
bool journalGuiFocus(uint32_t*) { return false; }
bool journalOnFootKnown() { return g_stubs.journalKnown; }
bool journalOnFoot() { return g_stubs.journalOnFoot; }
ID3D11VertexShader* shaderSwapCreateVs(ID3D11DeviceContext* ctx, const void* bytes, size_t n, const char*, const char*) {
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ID3D11VertexShader* s = nullptr;
    return dev && SUCCEEDED(dev->CreateVertexShader(bytes, n, nullptr, &s)) ? s : nullptr;
}
ID3D11ComputeShader* shaderSwapCreateCs(ID3D11DeviceContext* ctx, const void* bytes, size_t n, const char*, const char*) {
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ID3D11ComputeShader* s = nullptr;
    return dev && SUCCEEDED(dev->CreateComputeShader(bytes, n, nullptr, &s)) ? s : nullptr;
}
ID3D11PixelShader* shaderSwapCreatePs(ID3D11DeviceContext* ctx, const void* bytes, size_t n, const char*, const char*) {
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ID3D11PixelShader* s = nullptr;
    return dev && SUCCEEDED(dev->CreatePixelShader(bytes, n, nullptr, &s)) ? s : nullptr;
}
int uiDepthEyeOfTarget(const void* res, uint32_t, uint32_t, uint32_t) {
    return res && res == g_stubs.eyeRes[0] ? 0 : res && res == g_stubs.eyeRes[1] ? 1 : -1;
}
int uiDepthEyeOfTargetReadOnly(const void* res) { return uiDepthEyeOfTarget(res, 0, 0, 0); }
void uiPanelScaleSetTarget(float) {}
void uiPanelScaleFrameBoundary() {}
void uiPanelScaleLog() {}
void uiSurfacesSetTarget(float) {}
void uiSurfacesFrameBoundary() {}
void uiSurfacesLogAtlas() {}
bool vScreenIsEyeSized(uint32_t w, uint32_t h) { return w == kEyeW && h == kEyeH; }
bool vScreenPanelSize(uint32_t* w, uint32_t* h) {
    if (w) *w = kScreenW;
    if (h) *h = kScreenH;
    return true;
}
// The raw entries go straight to the context; each notes whether the route's internal scope was up.
static void rawNote() {
    ++g_stubs.rawCalls;
    if (g_vrWorldInternal && g_flatComputeInternal) ++g_stubs.rawInternal;
}
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* c, uint32_t n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
    rawNote();
    c->OMSetRenderTargets(n, r, d);
}
void vScreenRSSetViewportsRaw(ID3D11DeviceContext* c, uint32_t n, const D3D11_VIEWPORT* v) {
    rawNote();
    c->RSSetViewports(n, v);
}
void vScreenClearRenderTargetViewRaw(ID3D11DeviceContext* c, ID3D11RenderTargetView* r, const float col[4]) {
    rawNote();
    c->ClearRenderTargetView(r, col);
}
void vScreenPSSetShaderResourcesRaw(ID3D11DeviceContext* c, uint32_t start, uint32_t n, ID3D11ShaderResourceView* const* s) {
    rawNote();
    c->PSSetShaderResources(start, n, s);
}
void vScreenOMSetBlendStateRaw(ID3D11DeviceContext* c, ID3D11BlendState* s, const float f[4], uint32_t m) {
    rawNote();
    c->OMSetBlendState(s, f, m);
}
void vScreenPSSetShaderRaw(ID3D11DeviceContext* c, ID3D11PixelShader* s, ID3D11ClassInstance* const* ci, uint32_t n) {
    rawNote();
    c->PSSetShader(s, ci, n);
}
void vScreenCopyResourceRaw(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* s) {
    rawNote();
    c->CopyResource(d, s);
}
void vScreenExecuteCommandListRaw(ID3D11DeviceContext* c, ID3D11CommandList* l, int restore) {
    rawNote();
    c->ExecuteCommandList(l, restore ? TRUE : FALSE);
}
}  // namespace edvr

using namespace edvr;

namespace {
// ============================================================================================ the harness
struct Tex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t w = 0, h = 0;
};

constexpr uint32_t kRed = 0xFF0000FFu, kGreen = 0xFF00FF00u, kBlue = 0xFFFF0000u, kWhite = 0xFFFFFFFFu;
constexpr uint32_t kBlack = 0xFF000000u, kGrey = 0xFF808080u, kVoid = 0xFF282828u;

struct Rig {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    Tex eye[2], screen, frame[2], tiny;
    ComPtr<ID3D11Texture2D> mipsTex;
    ComPtr<ID3D11ShaderResourceView> mipsSrv, otherSrv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> rectCb;
    ComPtr<ID3D11SamplerState> gameSampler, otherSampler, mipSampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend, premul;
    ComPtr<ID3D11DepthStencilState> dsOff, dsOn;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    uint64_t seq = 1;
};

Tex makeTex(Rig& r, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, DXGI_FORMAT viewFmt, uint32_t color) {
    Tex t;
    t.w = w;
    t.h = h;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.BindFlags = bind;
    std::vector<uint32_t> px(static_cast<size_t>(w) * h, color);
    D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4, 0};
    if (FAILED(r.dev->CreateTexture2D(&d, &sd, &t.tex))) return t;
    if (bind & D3D11_BIND_RENDER_TARGET) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = viewFmt;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        r.dev->CreateRenderTargetView(t.tex.Get(), &rd, &t.rtv);
    }
    if (bind & D3D11_BIND_SHADER_RESOURCE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = viewFmt;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        r.dev->CreateShaderResourceView(t.tex.Get(), &vd, &t.srv);
    }
    return t;
}

std::vector<uint32_t> readPixels(Rig& r, ID3D11Texture2D* tex) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    std::vector<uint32_t> out(static_cast<size_t>(d.Width) * d.Height, 0);
    if (FAILED(r.dev->CreateTexture2D(&d, nullptr, &st))) return out;
    r.ctx->CopyResource(st.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(r.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return out;
    for (uint32_t y = 0; y < d.Height; ++y)
        std::memcpy(&out[static_cast<size_t>(y) * d.Width], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, d.Width * 4);
    r.ctx->Unmap(st.Get(), 0);
    return out;
}
uint32_t at(const std::vector<uint32_t>& p, uint32_t w, uint32_t x, uint32_t y) { return p[static_cast<size_t>(y) * w + x]; }

// Every pixel of [x0,x1) x [y0,y1) is `want`.
bool region(const std::vector<uint32_t>& p, uint32_t w, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t want) {
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
            if (at(p, w, x, y) != want) return false;
    return true;
}

const char kShaders[] = R"HLSL(
cbuffer Q : register(b0) { float4 rect; };           // x0 y0 x1 y1, NDC
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vsMain(uint id : SV_VertexID) {
    V v;
    float2 c = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);   // strip: (0,0) (1,0) (0,1) (1,1)
    v.pos = float4(lerp(rect.x, rect.z, c.x), lerp(rect.w, rect.y, c.y), 0, 1);
    v.uv = c;
    return v;
}
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
float4 psMain(V v) : SV_Target { return t0.Sample(s0, v.uv); }
)HLSL";

bool compile(const char* entry, const char* profile, ComPtr<ID3DBlob>* out) {
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "ui_layer_world_test", nullptr, nullptr, entry, profile,
                                  D3DCOMPILE_ENABLE_STRICTNESS, 0, out->GetAddressOf(), &err);
    if (FAILED(hr) && err) std::printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
    return SUCCEEDED(hr);
}

// Four mips of flat colour: 144x96 red, 72x48 green, 36x24 blue, 18x12 white.
bool makeMips(Rig& r) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = kScreenW;
    d.Height = kScreenH;
    d.MipLevels = 4;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const uint32_t colours[4] = {kRed, kGreen, kBlue, kWhite};
    std::vector<uint32_t> level[4];
    D3D11_SUBRESOURCE_DATA sd[4];
    for (uint32_t m = 0; m < 4; ++m) {
        const uint32_t w = kScreenW >> m, h = kScreenH >> m;
        level[m].assign(static_cast<size_t>(w) * h, colours[m]);
        sd[m] = {level[m].data(), w * 4, 0};
    }
    if (FAILED(r.dev->CreateTexture2D(&d, sd, &r.mipsTex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 4;
    return SUCCEEDED(r.dev->CreateShaderResourceView(r.mipsTex.Get(), &vd, &r.mipsSrv));
}

bool setup(Rig& r) {
    D3D_FEATURE_LEVEL fl{};
    const auto create = edvr::systemD3D11CreateDevice();
    if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &r.dev, &fl, &r.ctx))) return false;
    check(edvr::reportSystemD3D11Only("ui_layer_world_test"), "the rig runs on System32's d3d11.dll and on no other d3d11.dll");
    for (int e = 0; e < 2; ++e) {
        r.eye[e] = makeTex(r, kEyeW, kEyeH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                           DXGI_FORMAT_R8G8B8A8_UNORM, kVoid);
        r.frame[e] = makeTex(r, kDoorW, kDoorH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                             DXGI_FORMAT_R8G8B8A8_UNORM, kBlack);
        g_stubs.eyeRes[e] = r.eye[e].tex.Get();
    }
    r.screen = makeTex(r, kScreenW, kScreenH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R8G8B8A8_UNORM, kRed);
    r.tiny = makeTex(r, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R8G8B8A8_UNORM, kBlue);
    r.otherSrv = r.tiny.srv;
    if (!r.eye[0].rtv || !r.eye[1].rtv || !r.screen.srv || !r.frame[0].srv || !makeMips(r)) return false;
    ComPtr<ID3DBlob> v, p;
    if (!compile("vsMain", "vs_5_0", &v) || !compile("psMain", "ps_5_0", &p)) return false;
    if (FAILED(r.dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &r.vs)) ||
        FAILED(r.dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &r.ps)))
        return false;
    // The quad: eye pixels [8,56) x [8,40) in NDC.
    const float rect[4] = {-1.0f + 2.0f * 8 / kEyeW, 1.0f - 2.0f * 8 / kEyeH, -1.0f + 2.0f * 56 / kEyeW, 1.0f - 2.0f * 40 / kEyeH};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{rect, 0, 0};
    if (FAILED(r.dev->CreateBuffer(&bd, &sd, &r.rectCb))) return false;
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = 0.0f;   // the game's own: no mips
    smp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(r.dev->CreateSamplerState(&smp, &r.gameSampler))) return false;
    smp.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    if (FAILED(r.dev->CreateSamplerState(&smp, &r.otherSampler))) return false;
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(r.dev->CreateSamplerState(&smp, &r.mipSampler))) return false;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    if (FAILED(r.dev->CreateRasterizerState(&rd, &r.raster))) return false;
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(r.dev->CreateBlendState(&bl, &r.blend))) return false;
    bl.RenderTarget[0].BlendEnable = TRUE;   // premultiplied over: a draw that blends
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(r.dev->CreateBlendState(&bl, &r.premul))) return false;
    D3D11_DEPTH_STENCIL_DESC dso{};
    dso.DepthEnable = FALSE;
    dso.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(r.dev->CreateDepthStencilState(&dso, &r.dsOff))) return false;
    dso.DepthEnable = TRUE;
    dso.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dso.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(r.dev->CreateDepthStencilState(&dso, &r.dsOn))) return false;
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = kEyeW;
    dd.Height = kEyeH;
    dd.MipLevels = dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(r.dev->CreateTexture2D(&dd, nullptr, &r.depth)) || FAILED(r.dev->CreateDepthStencilView(r.depth.Get(), nullptr, &r.dsv))) return false;
    g_stubs.mipsAnswer = r.mipsSrv.Get();
    g_stubs.samplerAnswer = r.mipSampler.Get();
    return true;
}

// The game's state for the screen composite draw into `eye`: every slot the re-issue could touch set to a sentinel.
void bindGame(Rig& r, int eye, ID3D11BlendState* blend = nullptr, ID3D11DepthStencilState* ds = nullptr) {
    ID3D11RenderTargetView* rtv = r.eye[eye].rtv.Get();
    const float voidColour[4] = {40.0f / 255.0f, 40.0f / 255.0f, 40.0f / 255.0f, 1.0f};
    r.ctx->ClearRenderTargetView(rtv, voidColour);   // the game clears its eye image each frame
    r.ctx->OMSetRenderTargets(1, &rtv, r.dsv.Get());
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(kEyeW), static_cast<float>(kEyeH), 0, 1};
    r.ctx->RSSetViewports(1, &vp);
    r.ctx->RSSetState(r.raster.Get());
    const D3D11_RECT sc{2, 2, 60, 44};
    r.ctx->RSSetScissorRects(1, &sc);
    const float bf[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    r.ctx->OMSetBlendState(blend ? blend : r.blend.Get(), bf, 0xFFFFFFFDu);
    r.ctx->OMSetDepthStencilState(ds ? ds : r.dsOff.Get(), 7);
    ID3D11ShaderResourceView* srvs[2] = {r.screen.srv.Get(), r.otherSrv.Get()};
    r.ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* smp[2] = {r.gameSampler.Get(), r.otherSampler.Get()};
    r.ctx->PSSetSamplers(0, 2, smp);
    r.ctx->VSSetShader(r.vs.Get(), nullptr, 0);
    r.ctx->PSSetShader(r.ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = r.rectCb.Get();
    r.ctx->VSSetConstantBuffers(0, 1, &cb);
    r.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    auto& slot = detail::g_bindingSlots[static_cast<size_t>(BindSlot::Rtv0)];
    slot.ptr = rtv;
    ++slot.gen;
}

// Everything the re-issue could have changed, as raw pointers and values: equal before and after means restored.
struct Snap {
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    UINT nvp = 16;
    D3D11_VIEWPORT vp[16] = {};
    UINT nsc = 16;
    D3D11_RECT sc[16] = {};
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr;
    float bf[4] = {};
    UINT mask = 0;
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    ID3D11ShaderResourceView* srv[4] = {};
    ID3D11SamplerState* smp[4] = {};
    ID3D11PixelShader* ps = nullptr;
    ID3D11VertexShader* vs = nullptr;
};
Snap take(ID3D11DeviceContext* c) {
    Snap s;
    ID3D11RenderTargetView* rtv[8] = {};
    c->OMGetRenderTargets(8, rtv, &s.dsv);
    for (int i = 0; i < 8; ++i) {
        s.rtv[i] = rtv[i];
        if (rtv[i]) rtv[i]->Release();
    }
    if (s.dsv) s.dsv->Release();
    c->RSGetViewports(&s.nvp, s.vp);
    c->RSGetScissorRects(&s.nsc, s.sc);
    c->RSGetState(&s.rs);
    if (s.rs) s.rs->Release();
    c->OMGetBlendState(&s.bs, s.bf, &s.mask);
    if (s.bs) s.bs->Release();
    c->OMGetDepthStencilState(&s.dss, &s.ref);
    if (s.dss) s.dss->Release();
    ID3D11ShaderResourceView* srv[4] = {};
    c->PSGetShaderResources(0, 4, srv);
    ID3D11SamplerState* smp[4] = {};
    c->PSGetSamplers(0, 4, smp);
    for (int i = 0; i < 4; ++i) {
        s.srv[i] = srv[i];
        s.smp[i] = smp[i];
        if (srv[i]) srv[i]->Release();
        if (smp[i]) smp[i]->Release();
    }
    c->PSGetShader(&s.ps, nullptr, nullptr);
    if (s.ps) s.ps->Release();
    c->VSGetShader(&s.vs, nullptr, nullptr);
    if (s.vs) s.vs->Release();
    return s;
}
bool same(const Snap& a, const Snap& b) {
    return std::memcmp(a.rtv, b.rtv, sizeof(a.rtv)) == 0 && a.dsv == b.dsv && a.nvp == b.nvp &&
           std::memcmp(a.vp, b.vp, sizeof(D3D11_VIEWPORT) * a.nvp) == 0 && a.nsc == b.nsc &&
           std::memcmp(a.sc, b.sc, sizeof(D3D11_RECT) * a.nsc) == 0 && a.rs == b.rs && a.bs == b.bs &&
           std::memcmp(a.bf, b.bf, sizeof(a.bf)) == 0 && a.mask == b.mask && a.dss == b.dss && a.ref == b.ref &&
           std::memcmp(a.srv, b.srv, sizeof(a.srv)) == 0 && std::memcmp(a.smp, b.smp, sizeof(a.smp)) == 0 && a.ps == b.ps && a.vs == b.vs;
}

// The door's side of a frame, as native_temporal / native_sharpen call it: the eye's submitted texture, the pass's
// output (the black frame), and the door that saw it -- which is what arms the layer for the NEXT frame.
void doorFrame(Rig& r, uint64_t seq) {
    for (uint32_t e = 0; e < 2; ++e) {
        uiLayerNoteSubmitted(seq, e, r.eye[e].tex.Get());
        uiLayerNoteTemporal(seq, e, r.frame[e].tex.Get());
        uiLayerDoorSeen(seq, e, r.frame[e].tex.Get());
    }
}

// vscreen.cpp's worldScreenReissue, line for line (the draw is the game's own: the quad once more).
bool reissue(Rig& r) {
    VrWorldInternalScope internal;
    if (!uiLayerWorldReissueBegin(r.ctx.Get())) return false;
    r.ctx->Draw(4, 0);
    uiLayerWorldReissueEnd(r.ctx.Get());
    return true;
}

// One screen-composite draw as forwardWithVerdict handles it: the decision, the game's own draw, the re-issue.
struct Drawn {
    bool taken = false, pendingAfterDecide = false, reissued = false;
};
Drawn drawScreen(Rig& r, bool substituted = false) {
    Drawn d;
    d.taken = uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, substituted);
    d.pendingAfterDecide = uiLayerWorldReissuePending();
    r.ctx->Draw(4, 0);  // the game's own issue, always
    if (d.pendingAfterDecide) d.reissued = reissue(r);
    else uiLayerWorldReissueAbandon();
    return d;
}

// A fresh sequence with the door's previous frame noted for both eyes, so the layer is armed for it.
uint64_t nextArmed(Rig& r) {
    doorFrame(r, r.seq);
    ++r.seq;
    g_stubs.seq = r.seq;
    return r.seq;
}

UiLayerWorldStats statsNow() { return uiLayerWorldStats(); }

// ======================================================================================== the scenarios
void testKeyOff(Rig& r) {
    g_stubs.mayTake = false;
    const uint64_t seq = nextArmed(r);
    bindGame(r, 0);
    const Snap before = take(r.ctx.Get());
    const auto s0 = statsNow();
    const unsigned mips0 = g_stubs.mipsCalls, took0 = g_stubs.tookCalls;
    const Drawn d = drawScreen(r);
    check(!d.taken && !d.pendingAfterDecide && !d.reissued, "key off: the screen composite is not taken and nothing is pending");
    const auto s1 = statsNow();
    check(s1.screenDecided[static_cast<size_t>(UiLayerDecision::kWorldScreen)] == s0.screenDecided[static_cast<size_t>(UiLayerDecision::kWorldScreen)] + 1 &&
              s1.screenAsked == s0.screenAsked + 1 && s1.reissued == s0.reissued,
          "key off: the draw is counted as kWorldScreen, left in the picture, exactly as before the route");
    check(g_stubs.mipsCalls == mips0 && g_stubs.samplerCalls == 0 && g_stubs.tookCalls == took0,
          "key off: the mips module and the route are never asked");
    check(same(before, take(r.ctx.Get())), "key off: the game's state is exactly as it was");
    const auto px = readPixels(r, r.eye[0].tex.Get());
    check(region(px, kEyeW, 8, 8, 56, 40, kRed) && at(px, kEyeW, 2, 2) == kVoid, "key off: the game's draw landed in its eye image (the quad, red, on the void)");
    check(!uiLayerWorldReissueBegin(r.ctx.Get()), "key off: Begin with nothing pending does nothing");
    (void)seq;
}

void testReissue(Rig& r) {
    g_stubs.mayTake = true;
    const uint64_t seq = nextArmed(r);
    bindGame(r, 0);
    const Snap before = take(r.ctx.Get());
    const auto s0 = statsNow();
    const unsigned raw0 = g_stubs.rawCalls, internal0 = g_stubs.rawInternal, took0 = g_stubs.tookCalls, mips0 = g_stubs.mipsCalls;
    const Drawn d = drawScreen(r);
    check(!d.taken, "the route's mode is never a take: uiLayerDecide answers false for the game's draw");
    check(d.pendingAfterDecide, "...and holds the draw for the re-issue that follows the game's own");
    check(d.reissued, "the re-issue ran");
    check(g_stubs.mipsCalls == mips0 + 1 && g_stubs.lastMipsFrame == seq && g_stubs.lastMipsScreen == r.screen.tex.Get() && g_stubs.samplerCalls >= 1,
          "the mips module is asked once, for this frame's sequence and the draw's own screen texture; the sampler for the draw's own");
    check(same(before, take(r.ctx.Get())), "EVERY changed state is back: targets, depth target, viewport, scissor, blend object/factor/mask, depth state, PS slots 0 and 1, samplers, shaders");
    check(g_stubs.tookCalls == took0 + 1 && g_stubs.tookEye == 0 && g_stubs.tookSeq == seq, "the route is told eye 0 was taken, for this sequence, once");
    check(!uiLayerWorldReissuePending() && !uiLayerRedirecting(), "nothing is pending and the layer is not redirecting afterwards");
    const auto s1 = statsNow();
    check(s1.reissued == s0.reissued + 1 && s1.screenDecided[static_cast<size_t>(UiLayerDecision::kRedirect)] == s0.screenDecided[static_cast<size_t>(UiLayerDecision::kRedirect)],
          "the draw is counted as a re-issue -- and NOT as a screen draw redirected into the layer");
    check(g_stubs.rawCalls > raw0 && g_stubs.rawInternal == internal0 + (g_stubs.rawCalls - raw0),
          "every raw entry the re-issue called ran with the route's internal scope up (the hooks step aside)");

    // The game's eye image: the game's own draw, unchanged by the re-issue (its own texture, its own sampler).
    const auto eyePx = readPixels(r, r.eye[0].tex.Get());
    check(region(eyePx, kEyeW, 8, 8, 56, 40, kRed) && at(eyePx, kEyeW, 7, 20) == kVoid && at(eyePx, kEyeW, 56, 20) == kVoid && at(eyePx, kEyeW, 30, 7) == kVoid &&
              at(eyePx, kEyeW, 30, 40) == kVoid,
          "the game's eye image is what the game drew: the quad from ITS screen texture (mip 0, red) on the void, untouched by the re-issue");

    // The layer: composited over a grey frame, then over the black one the door hands on.
    const int gap = uiLayerWorldDoorGap(seq, 0, r.frame[0].tex.Get());
    check(gap == 0, "the door's preflight: the layer holds this frame's eye, able to composite over the black frame");
    const uint32_t whole[4] = {0, 0, kDoorW, kDoorH};
    const float uv[4] = {0, 0, 1, 1};
    ID3D11Texture2D* out = uiLayerComposite(seq, 0, r.frame[0].tex.Get(), whole, uv);
    check(out != nullptr, "the composite runs over the black frame");
    if (out) {
        const auto px = readPixels(r, out);
        // eye pixels [8,56) x [8,40) at the map's scale 1.5 -> layer pixels [12,84) x [12,60)
        check(region(px, kDoorW, 12, 12, 84, 60, kGreen),
              "the layer holds the screen where the quad is -- the MIP LEVEL the map's 2x minification selects (mip 1: green), through the route's trilinear sampler, not the game's mip-0 red");
        check(region(px, kDoorW, 0, 0, kDoorW, 12, kBlack) && region(px, kDoorW, 0, 60, kDoorW, kDoorH, kBlack) && region(px, kDoorW, 0, 12, 12, 60, kBlack) &&
                  region(px, kDoorW, 84, 12, kDoorW, 60, kBlack),
              "...and over the black frame everything the quad does not cover is black (the layer over black IS the eye)");
        out->Release();
    }
    check(uiLayerWorldDoorGap(seq, 0, r.frame[0].tex.Get()) == static_cast<int>(UiWorldDoorGap::kAlreadyDone), "once composited, the preflight says so");

    // Opaque means opaque: over a mid-grey frame the quad still replaces it.
    {
        const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        r.ctx->ClearRenderTargetView(r.frame[1].rtv.Get(), grey);
        bindGame(r, 1);
        const Drawn d1 = drawScreen(r);
        check(d1.reissued && g_stubs.tookEye == 1 && g_stubs.tookSeq == seq, "the other eye, same frame: re-issued, and the route is told eye 1");
        check(g_stubs.mipsCalls == mips0 + 2, "...the mips module is asked again per eye (it answers a second eye from its cache)");
        ID3D11Texture2D* o1 = uiLayerComposite(seq, 1, r.frame[1].tex.Get(), whole, uv);
        check(o1 != nullptr, "eye 1's composite runs");
        if (o1) {
            const auto px = readPixels(r, o1);
            check(region(px, kDoorW, 12, 12, 84, 60, kGreen) && at(px, kDoorW, 2, 2) == kGrey && at(px, kDoorW, 90, 66) == kGrey,
                  "over a grey frame: the screen replaces it where the quad is (opaque), the frame shows through where it is not");
            o1->Release();
        }
        const float black[4] = {0, 0, 0, 1};
        r.ctx->ClearRenderTargetView(r.frame[1].rtv.Get(), black);
    }
    check(uiLayerWorldStats().lostDraws == 0, "nothing was left in the game's frame: no lost draws");
}

// The eye shift is normally off while the route owns the frame (native_temporal begin), so the jitter the decision reads is
// zero -- but the frame the route entered, or was released on, is one late either way, and the layer must cancel whatever shift
// the game drew with, exactly as for any UI draw. An exaggerated, exactly representable jitter (1 eye pixel right, 1 down): the
// game's quad moves by it in the eye; the layer's must NOT move.
void testJitterCancel(Rig& r) {
    g_stubs.mayTake = true;
    const uint64_t seq = nextArmed(r);
    g_stubs.jx = 1.0f;
    g_stubs.jy = 1.0f;
    const float base[4] = {-1.0f + 2.0f * 8 / kEyeW, 1.0f - 2.0f * 8 / kEyeH, -1.0f + 2.0f * 56 / kEyeW, 1.0f - 2.0f * 40 / kEyeH};
    const float dx = 2.0f * g_stubs.jx / kEyeW, dy = -2.0f * g_stubs.jy / kEyeH;   // the game's projection: content jx right, jy down
    const float shifted[4] = {base[0] + dx, base[1] + dy, base[2] + dx, base[3] + dy};
    r.ctx->UpdateSubresource(r.rectCb.Get(), 0, nullptr, shifted, 0, 0);
    bindGame(r, 0);
    const Drawn d = drawScreen(r);
    check(d.reissued, "a jittered frame: re-issued");
    const auto eyePx = readPixels(r, r.eye[0].tex.Get());
    check(region(eyePx, kEyeW, 9, 9, 57, 41, kRed) && at(eyePx, kEyeW, 8, 20) == kVoid && at(eyePx, kEyeW, 57, 20) == kVoid,
          "the game's own quad moved by the jitter (one eye pixel right and down), as the game drew it");
    const uint32_t whole[4] = {0, 0, kDoorW, kDoorH};
    const float uv[4] = {0, 0, 1, 1};
    ID3D11Texture2D* out = uiLayerComposite(seq, 0, r.frame[0].tex.Get(), whole, uv);
    check(out != nullptr, "the composite runs");
    if (out) {
        const auto px = readPixels(r, out);
        check(region(px, kDoorW, 12, 12, 84, 60, kGreen) && at(px, kDoorW, 11, 30) == kBlack && at(px, kDoorW, 84, 30) == kBlack &&
                  at(px, kDoorW, 40, 11) == kBlack && at(px, kDoorW, 40, 60) == kBlack,
              "the layer's quad did NOT move: the jitter the decision read (1, 1 eye pixels) was cancelled through the map, to the layer pixel");
        out->Release();
    }
    g_stubs.jx = g_stubs.jy = 0.0f;
    r.ctx->UpdateSubresource(r.rectCb.Get(), 0, nullptr, base, 0, 0);
}

// A refusal: the game's draw still landed, nothing was taken, the game's state is untouched, the route was not told,
// and the counter says why.
template <class Arrange>
void refusal(Rig& r, const char* name, UiWorldRefuse reason, UiLayerDecision decision, Arrange arrange, bool substituted = false) {
    g_stubs.mayTake = true;
    g_stubs.mipsAnswer = r.mipsSrv.Get();
    g_stubs.samplerAnswer = r.mipSampler.Get();
    nextArmed(r);
    bindGame(r, 0);
    arrange();
    const Snap before = take(r.ctx.Get());
    const auto s0 = statsNow();
    const unsigned took0 = g_stubs.tookCalls;
    const Drawn d = drawScreen(r, substituted);
    char msg[320];
    std::snprintf(msg, sizeof(msg), "%s: not taken, not re-issued, the game's state untouched", name);
    check(!d.taken && !d.reissued && same(before, take(r.ctx.Get())) && !uiLayerWorldReissuePending() && !uiLayerRedirecting(), msg);
    const auto s1 = statsNow();
    std::snprintf(msg, sizeof(msg), "%s: the route is not told, and the refusal is counted", name);
    const bool byRoute = reason != UiWorldRefuse::kNone && s1.refused[static_cast<size_t>(reason)] == s0.refused[static_cast<size_t>(reason)] + 1;
    const bool byDecision = decision != UiLayerDecision::kRedirect &&
                            s1.screenDecided[static_cast<size_t>(decision)] == s0.screenDecided[static_cast<size_t>(decision)] + 1;
    const bool counted = byRoute || byDecision;
    check(g_stubs.tookCalls == took0 && counted && s1.reissued == s0.reissued, msg);
    g_stubs.mipsAnswer = r.mipsSrv.Get();
    g_stubs.samplerAnswer = r.mipSampler.Get();
}

void testRefusals(Rig& r) {
    refusal(r, "no mipped screen", UiWorldRefuse::kMipsNull, UiLayerDecision::kRedirect, [&] { g_stubs.mipsAnswer = nullptr; });
    refusal(r, "no trilinear sampler", UiWorldRefuse::kSamplerNull, UiLayerDecision::kRedirect, [&] { g_stubs.samplerAnswer = nullptr; });
    refusal(r, "no sampler bound at slot 0", UiWorldRefuse::kNoSampler, UiLayerDecision::kRedirect, [&] {
        ID3D11SamplerState* none = nullptr;
        r.ctx->PSSetSamplers(0, 1, &none);
    });
    refusal(r, "no texture at slot 0", UiWorldRefuse::kNoSource, UiLayerDecision::kRedirect, [&] {
        ID3D11ShaderResourceView* none = nullptr;
        r.ctx->PSSetShaderResources(0, 1, &none);
    });
    refusal(r, "a curved screen", UiWorldRefuse::kCurved, UiLayerDecision::kRedirect, [] {}, /*substituted=*/true);
    refusal(r, "a blending draw", UiWorldRefuse::kNotOpaque, UiLayerDecision::kRedirect, [&] {
        bindGame(r, 0, r.premul.Get());
    });
    refusal(r, "a depth-tested draw", UiWorldRefuse::kDepthState, UiLayerDecision::kDepthStencilTest, [&] {
        bindGame(r, 0, nullptr, r.dsOn.Get());
    });
    // not armed: the door did not run for the previous sequence
    {
        g_stubs.mayTake = true;
        ++r.seq;
        g_stubs.seq = r.seq + 5;  // a sequence the door never saw
        bindGame(r, 0);
        const Snap before = take(r.ctx.Get());
        const auto s0 = statsNow();
        const Drawn d = drawScreen(r);
        const auto s1 = statsNow();
        check(!d.taken && !d.reissued && same(before, take(r.ctx.Get())), "not armed: not taken, not re-issued, the game's state untouched");
        check(s1.screenDecided[static_cast<size_t>(UiLayerDecision::kNotArmed)] == s0.screenDecided[static_cast<size_t>(UiLayerDecision::kNotArmed)] + 1,
              "not armed: counted as the decision's own kNotArmed");
        g_stubs.seq = r.seq;
    }
    // the eye could not be told
    {
        g_stubs.mayTake = true;
        nextArmed(r);
        bindGame(r, 0);
        const void* keep = g_stubs.eyeRes[0];
        g_stubs.eyeRes[0] = nullptr;
        const auto s0 = statsNow();
        const Drawn d = drawScreen(r);
        g_stubs.eyeRes[0] = keep;
        check(!d.taken && !d.reissued && statsNow().screenDecided[static_cast<size_t>(UiLayerDecision::kNoEye)] == s0.screenDecided[static_cast<size_t>(UiLayerDecision::kNoEye)] + 1,
              "an eye that cannot be told: counted as kNoEye, nothing re-issued");
    }
    // the draw's bindings changed between its decision and the re-issue
    {
        g_stubs.mayTake = true;
        nextArmed(r);
        bindGame(r, 0);
        const auto s0 = statsNow();
        const unsigned took0 = g_stubs.tookCalls;
        const bool taken = uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, false);
        check(!taken && uiLayerWorldReissuePending(), "changed bindings: the decision held the draw");
        r.ctx->Draw(4, 0);
        ID3D11ShaderResourceView* other = r.otherSrv.Get();
        r.ctx->PSSetShaderResources(0, 1, &other);   // something else at slot 0 by the time the re-issue asks
        const Snap mid = take(r.ctx.Get());
        VrWorldInternalScope internal;
        const bool began = uiLayerWorldReissueBegin(r.ctx.Get());
        check(!began && same(mid, take(r.ctx.Get())) && !uiLayerRedirecting(), "...the re-issue finds slot 0 is not the decided draw's texture: refused, the state untouched");
        const auto s1 = statsNow();
        check(s1.refused[static_cast<size_t>(UiWorldRefuse::kStateChanged)] == s0.refused[static_cast<size_t>(UiWorldRefuse::kStateChanged)] + 1 && g_stubs.tookCalls == took0,
              "...counted as a changed binding, the route not told");
    }
    // the target changed
    {
        g_stubs.mayTake = true;
        nextArmed(r);
        bindGame(r, 0);
        const unsigned took0 = g_stubs.tookCalls;
        const bool taken = uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, false);
        r.ctx->Draw(4, 0);
        bindGame(r, 1);   // another eye's target is bound when the re-issue asks
        VrWorldInternalScope internal;
        check(!taken && !uiLayerWorldReissueBegin(r.ctx.Get()) && g_stubs.tookCalls == took0, "a changed render target: refused at the re-issue");
    }
    // abandoned: the game's draw was swallowed
    {
        g_stubs.mayTake = true;
        nextArmed(r);
        bindGame(r, 0);
        const bool taken = uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, false);
        check(!taken && uiLayerWorldReissuePending(), "abandon: held");
        uiLayerWorldReissueAbandon();
        VrWorldInternalScope internal;
        check(!uiLayerWorldReissuePending() && !uiLayerWorldReissueBegin(r.ctx.Get()), "abandon: the flag is down and Begin finds nothing to do");
    }
    // a new family decision forgets a pending re-issue; the after-UI retry's kAfterUi decision does not
    {
        g_stubs.mayTake = true;
        nextArmed(r);
        bindGame(r, 0);
        uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, false);
        check(uiLayerWorldReissuePending(), "retry: the screen draw's re-issue is pending");
        uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kAfterUi), true, false, 0);
        check(uiLayerWorldReissuePending(), "...and the after-UI retry's decision (kAfterUi) leaves it alone");
        uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kPanel), true, false);
        check(!uiLayerWorldReissuePending(), "...while a real family's decision forgets it");
        uiLayerWorldReissueAbandon();
    }
}

void testBeginWithoutOuterScope(Rig& r) {
    // Begin and End carry the internal scope themselves: no outer VrWorldInternalScope here.
    g_stubs.mayTake = true;
    nextArmed(r);
    bindGame(r, 0);
    uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, false);
    r.ctx->Draw(4, 0);
    const unsigned raw0 = g_stubs.rawCalls, internal0 = g_stubs.rawInternal;
    const bool began = uiLayerWorldReissueBegin(r.ctx.Get());
    if (began) {
        r.ctx->Draw(4, 0);
        uiLayerWorldReissueEnd(r.ctx.Get());
    }
    check(began && g_stubs.rawCalls > raw0 && g_stubs.rawInternal == internal0 + (g_stubs.rawCalls - raw0),
          "Begin and End set the internal scope themselves: every raw entry they call ran with it up");
    check(!g_vrWorldInternal && !g_flatComputeInternal, "...and leave it down (no scope leaks out of either)");
    uiLayerWorldReissueEnd(r.ctx.Get());   // a second End without a Begin is harmless
    check(!uiLayerRedirecting(), "...a stray End does nothing");
}

void testLostDraws(Rig& r) {
    // After the screen was re-issued into an eye, a draw the game leaves in that eye's image is lost while the route owns
    // the eye: counted (a post pass over the eye that writes it, the case uiLayerNoteOther names).
    g_stubs.mayTake = true;
    const uint64_t seq = nextArmed(r);
    bindGame(r, 0);
    const Drawn d = drawScreen(r);
    check(d.reissued, "lost draws: the screen was re-issued into eye 0");
    const auto s0 = statsNow();
    // a pass into the same eye image that samples an eye-sized input: left in the game's frame as a post pass
    ID3D11ShaderResourceView* post = r.eye[1].srv.Get();   // eye-sized, not the screen
    r.ctx->PSSetShaderResources(0, 1, &post);
    detail::g_bindingSlots[static_cast<size_t>(BindSlot::PsSrv0)].ptr = post;
    uiLayerNoteOther(r.ctx.Get(), 3, true, false, false, false, 1, 0, 'D');
    const auto s1 = statsNow();
    check(s1.lostDraws == s0.lostDraws + 1, "a post pass into a re-issued eye is left in the game's frame and COUNTED as lost");
    (void)seq;
    detail::g_bindingSlots[static_cast<size_t>(BindSlot::PsSrv0)].ptr = nullptr;
}

void testAccessors(Rig& r) {
    check(uiLayerLiveForWorldRoute() && uiLayerNotLiveReason() == nullptr, "the layer is live with fix.ui_quality on, a temporal mode on and the jitter as shipped");
    check(uiLayerWorldScreenHeld(), "the gate holds the screen (the journal says on foot)");
    auto& cfg = Config::get();
    cfg.set("fix.ui_quality", "off");
    uiLayerConfigure(cfg);
    check(!uiLayerLiveForWorldRoute() && uiLayerNotLiveReason() && std::strcmp(uiLayerNotLiveReason(), "fix.ui_quality is off") == 0 && !uiLayerLive(),
          "fix.ui_quality off: not live, with its reason, and the draw path's own gate agrees");
    cfg.set("fix.ui_quality", "100");
    cfg.set("fix.temporal_aa", "off");
    uiLayerConfigure(cfg);
    check(!uiLayerLiveForWorldRoute() && uiLayerNotLiveReason() && std::strstr(uiLayerNotLiveReason(), "fix.temporal_aa"), "no temporal mode: not live, with its reason");
    cfg.set("fix.temporal_aa", "dlss");
    cfg.set("advanced.temporal_aa_jitter_sign", "flip_x");
    uiLayerConfigure(cfg);
    check(!uiLayerLiveForWorldRoute() && uiLayerNotLiveReason() && std::strstr(uiLayerNotLiveReason(), "jitter"), "jitter not as shipped: not live, with its reason");
    cfg.set("advanced.temporal_aa_jitter_sign", "as_is");
    uiLayerConfigure(cfg);
    check(uiLayerLiveForWorldRoute() && uiLayerNotLiveReason() == nullptr && uiLayerLive(), "back as shipped: live again");
    (void)r;
}

void testDoorGaps(Rig& r) {
    g_stubs.mayTake = true;
    const uint64_t seq = nextArmed(r);
    bindGame(r, 0);
    // before any re-issue this frame: the layer holds nothing of it
    check(uiLayerWorldDoorGap(seq, 0, r.frame[0].tex.Get()) == static_cast<int>(UiWorldDoorGap::kNoContent),
          "the preflight before the re-issue: the layer holds nothing of this frame");
    drawScreen(r);
    check(uiLayerWorldDoorGap(seq, 0, r.frame[0].tex.Get()) == 0, "...and after it: ready");
    check(uiLayerWorldDoorGap(seq, 2, r.frame[0].tex.Get()) == static_cast<int>(UiWorldDoorGap::kNoLayer) &&
              uiLayerWorldDoorGap(seq, 0, nullptr) == static_cast<int>(UiWorldDoorGap::kNoLayer),
          "an eye that does not exist, or no frame: no layer");
    Tex wide = makeTex(r, 200, 72, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R8G8B8A8_UNORM, kBlack);
    check(uiLayerWorldDoorGap(seq, 0, wide.tex.Get()) == static_cast<int>(UiWorldDoorGap::kAspect), "a frame of another shape than the layer: refused (the door's size is changing)");
    Tex half = makeTex(r, kDoorW, kDoorH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
    check(uiLayerWorldDoorGap(seq, 0, half.tex.Get()) == static_cast<int>(UiWorldDoorGap::kCannotComposite),
          "a frame the composite does not run over (half-float): refused, so the black frame is never handed to a door that cannot put a picture over it");
    check(uiLayerWorldDoorGap(seq, 1, r.frame[1].tex.Get()) == static_cast<int>(UiWorldDoorGap::kNoContent), "the other eye, nothing re-issued into it yet: no content");
    const uint32_t whole[4] = {0, 0, kDoorW, kDoorH};
    const float unit[4] = {0, 0, 1, 1};
    ID3D11Texture2D* out = uiLayerComposite(seq, 0, r.frame[0].tex.Get(), whole, unit);
    if (out) out->Release();
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("ui_layer_world_test: dry-run (no device, no files)");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("usage: ui_layer_world_test --self-test | --dry-run");
        return 2;
    }
    Rig r;
    if (!setup(r)) {
        std::puts("FAIL: a device, the textures and the test shaders");
        return 1;
    }
    auto& cfg = Config::get();
    cfg.set("fix.ui_quality", "100");
    cfg.set("fix.temporal_aa", "dlss");
    cfg.set("advanced.temporal_aa_jitter_sign", "as_is");
    cfg.set("advanced.temporal_aa_jitter_lag", "0");
    uiLayerConfigure(cfg);
    check(uiLayerLive(), "the layer is live for the rig (fix.ui_quality 100, dlss, jitter as shipped)");
    // The first boundary computes the world-screen gate (the journal: on foot) and warms the layer's shaders.
    uiLayerFrameBoundary(r.ctx.Get());
    check(uiLayerWorldScreenHeld(), "the gate holds the screen after the first boundary");
    // The door's first frame, so sequence 2 is armed.
    r.seq = 1;
    testKeyOff(r);
    testReissue(r);
    testJitterCancel(r);
    testRefusals(r);
    testBeginWithoutOuterScope(r);
    testLostDraws(r);
    testDoorGaps(r);
    testAccessors(r);
    uiLayerShutdown();
    std::printf("ui_layer_world_test: %u checks, %u failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

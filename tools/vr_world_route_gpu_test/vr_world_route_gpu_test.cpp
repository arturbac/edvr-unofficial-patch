// The VR on-foot world route's runtime on WARP (docs/design-flat-temporal-aa-2026-09-23.md, section 82).
//
// vr_world_route.cpp is linked AS SHIPPED, with the real binding shadow, the real Config and the real flat resolver (its
// backends stubbed the way tools\flat_mono_resolve_test stubs them: they see the textures the SDK would, and their image
// quality is separate). The world is drawn at 128x72 instead of 5040x2835: a synthetic copy of the census retake's chain
// (design doc section 82), draw by draw, through the same per-draw entry the hooks call. What is pinned:
//   - KEY OFF: nothing runs, nothing is logged, nothing is held;
//   - the happy path: the resolver is called once at the tone, on slot 2, on H, with the route's flags; warm-up, ownership,
//     and the published state the eye shift and the layer read;
//   - every refusal the selector names: no resolver call, the eye route serves the frame, the reason in the log;
//   - the route's own calls pass the hooks' internal flags, and leave the game's pipeline state as they found it;
//   - late writes into H latch the route off; a scene reset releases and resets; a frame gap resets the history;
//   - the key going off while owned lets go of everything.
// --self-test runs it; --dry-run says what it would do and writes nothing.

#include "../../src/d3d11/vr_world_route.h"
#include "../../src/d3d11/vr_world_route_math.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/flat_mono_resolve.h"
#include "../../src/d3d11/dlaa.h"
#include "../../src/d3d11/fsr3_engine.h"
#include "../../src/d3d11/engine_velocity.h"
#include "../../src/d3d11/gpu_census.h"
#include "../../src/d3d11/ui_layer.h"
#include "../../src/d3d11/vr_camera_census.h"
#include "../../src/d3d11/vr_world_mips.h"
#include "../../src/d3d11/vscreen.h"
#include "../../src/d3d11/weapon_motion.h"
#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/system_d3d11.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;

namespace {
int g_failures = 0, g_checks = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) { std::printf("  ok    %s\n", what); return; }
    ++g_failures;
    std::printf("  FAIL  %s\n", what);
}

constexpr uint32_t kW = 128, kH = 72;
// The stub world the route's neighbours answer from.
bool g_gate = true, g_layerLive = true, g_named = true, g_rowsKnown = true, g_viewsReady = true;
bool g_realMismatch = false;   // the real context's t1 is not what the shadow says (the route verifies the actual binding)
uint32_t g_panelW = kW, g_panelH = kH;
const ID3D11Texture2D* g_namedDepth = nullptr;
float g_rows[6][4];
int g_mipsResets = 0;
std::vector<std::string> g_log;
// What the stub backend saw.
int g_backendCalls = 0;
struct BackendCall { int slot; bool reset, hdr, internalFlags; uint32_t w, h, outW, outH; DXGI_FORMAT colour; };
std::vector<BackendCall> g_calls;
ComPtr<ID3D11ShaderResourceView> g_slotsSrv, g_poolSrv, g_weaponMapSrv;   // the weapon map the stub weapon_motion answers with (null: no weapon drew)
ComPtr<ID3D11Buffer> g_sceneNow, g_scenePrev;

// Real rows (Epic frame 71751, b1[270..275]): the shape the game composes for a kind-3 camera (flat_mono_resolve_test).
constexpr float kEpicRows[6][4] = {
    {.674860716f, -.714774430f, 0, .684166729f},
    {-.804393589f, -.069881566f, 0, .664024174f},
    {-.240084499f, -1.77504551f, 0, -.301641792f},
    {0, 0, .0250000004f, 0},
    {.684166729f, .664024174f, -.301641792f, 0},
    {-21.0930309f, -24.5114784f, -1.11009693f, 0}};
}  // namespace

namespace edvr {
thread_local bool g_flatComputeInternal = false;
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char* fmt, ...) {
    char line[1536]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    g_log.emplace_back(line);
}
bool vScreenPanelSize(uint32_t* w, uint32_t* h) { if (w) *w = g_panelW; if (h) *h = g_panelH; return true; }
bool uiLayerWorldScreenHeld() { return g_gate; }
bool uiLayerLiveForWorldRoute() { return g_layerLive; }
const char* uiLayerNotLiveReason() { return g_layerLive ? nullptr : "fix.ui_quality is 0, so the UI layer is off"; }
bool vrCameraCensusWanted() { return false; }
void vrCameraCensusFrameBoundary() {}
void vrCameraCensusEyeDraw(ID3D11DeviceContext*, uint32_t) {}
ID3D11ShaderResourceView* vrWorldMipsScreen(ID3D11DeviceContext*, ID3D11Texture2D*, uint64_t) { return nullptr; }
ID3D11SamplerState* vrWorldMipsSampler(ID3D11Device*, const D3D11_SAMPLER_DESC&) { return nullptr; }
void vrWorldMipsReset() { ++g_mipsResets; }
bool engineVelocitySourceIsNamed(const ID3D11Texture2D* depth) { return g_named && depth && depth == g_namedDepth; }
bool engineVelocitySourceCameraRows(float (&rows)[6][4]) {
    if (!g_rowsKnown) return false;
    std::memcpy(rows, g_rows, sizeof(rows));
    return true;
}
bool engineVelocitySourceViews(ID3D11Texture2D* depth, EngineVelocityViews* out) {
    if (out) *out = EngineVelocityViews{};
    if (!g_viewsReady || !out || depth != g_namedDepth) return false;
    out->slots = g_slotsSrv.Get(); out->pool = g_poolSrv.Get(); out->sceneNow = g_sceneNow.Get(); out->scenePrev = g_scenePrev.Get();
    out->slots->AddRef(); out->pool->AddRef(); out->sceneNow->AddRef(); out->scenePrev->AddRef();
    return true;
}
ID3D11ShaderResourceView* weaponMotionView() { return g_weaponMapSrv.Get(); }
bool gpuCensusBegin(ID3D11DeviceContext*, GpuCensusSection) noexcept { return false; }
void gpuCensusEnd(ID3D11DeviceContext*, GpuCensusSection) noexcept {}
bool dlaaAvailable(ID3D11Device*, const char**) { return true; }
bool fsr3Available(ID3D11Device*, const char**) { return true; }
bool dlaaEvaluate(ID3D11DeviceContext* c, int slot, ID3D11Texture2D* colour, ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D* out,
                  ID3D11Texture2D*, uint32_t w, uint32_t h, uint32_t outW, uint32_t outH, float, float, bool reset, float,
                  const char**, bool hdr) {
    ++g_backendCalls;
    BackendCall b{};
    b.slot = slot; b.reset = reset; b.hdr = hdr; b.w = w; b.h = h; b.outW = outW; b.outH = outH;
    b.internalFlags = g_vrWorldInternal && g_flatComputeInternal;
    D3D11_TEXTURE2D_DESC d{};
    colour->GetDesc(&d);
    b.colour = d.Format;
    g_calls.push_back(b);
    c->ClearState();   // an SDK may alter every stage: the resolver's isolation must contain it
    ComPtr<ID3D11Device> dev;
    c->GetDevice(dev.GetAddressOf());
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (FAILED(dev->CreateUnorderedAccessView(out, nullptr, uav.GetAddressOf()))) return false;
    const float green[4] = {0, 1, 0, 1};
    c->ClearUnorderedAccessViewFloat(uav.Get(), green);
    return true;
}
bool fsr3Evaluate(ID3D11DeviceContext*, unsigned, ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D*,
                  ID3D11Texture2D*, uint32_t, uint32_t, uint32_t, uint32_t, float, float, bool, float, float, float, float,
                  const char**, bool, bool) { return false; }
}  // namespace edvr

namespace {
using namespace edvr;

ComPtr<ID3D11Texture2D> makeTexture(ID3D11Device* d, UINT w, UINT h, DXGI_FORMAT fmt, UINT binds) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w; desc.Height = h; desc.ArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Format = fmt; desc.BindFlags = binds; desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> t;
    check(SUCCEEDED(d->CreateTexture2D(&desc, nullptr, t.GetAddressOf())), "fixture texture");
    return t;
}
ComPtr<ID3D11RenderTargetView> rtvOf(ID3D11Device* d, ID3D11Texture2D* t, DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN) {
    D3D11_RENDER_TARGET_VIEW_DESC v{};
    v.Format = fmt; v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> r;
    check(SUCCEEDED(d->CreateRenderTargetView(t, fmt == DXGI_FORMAT_UNKNOWN ? nullptr : &v, r.GetAddressOf())), "fixture RTV");
    return r;
}
ComPtr<ID3D11ShaderResourceView> srvOf(ID3D11Device* d, ID3D11Resource* t) {
    ComPtr<ID3D11ShaderResourceView> s;
    check(SUCCEEDED(d->CreateShaderResourceView(t, nullptr, s.GetAddressOf())), "fixture SRV");
    return s;
}

struct World {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> h, depth, depth2, g2, tone, copy, tiny, other, weapon, weaponSmall;
    ComPtr<ID3D11RenderTargetView> rh, rg2, rtone, rsmall;
    ComPtr<ID3D11DepthStencilView> dsv, dsv2;
    ComPtr<ID3D11ShaderResourceView> sh, scopy, sother, sweapon, sweaponSmall;
    uint64_t seq = 0;
};

bool build(World& w) {
    const auto createDevice = systemD3D11CreateDevice();
    check(createDevice != nullptr, "system D3D11 factory");
    if (!createDevice) return false;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, w.dev.GetAddressOf(), &level, w.ctx.GetAddressOf());
    check(SUCCEEDED(hr), "WARP device");
    if (FAILED(hr)) return false;
    ID3D11Device* d = w.dev.Get();
    w.h = makeTexture(d, kW, kH, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    w.copy = makeTexture(d, kW, kH, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    w.g2 = makeTexture(d, kW, kH, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_RENDER_TARGET);
    w.tone = makeTexture(d, kW, kH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    w.other = makeTexture(d, kW, kH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    w.tiny = makeTexture(d, kW / 8, kH / 8, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    w.depth = makeTexture(d, kW, kH, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    w.depth2 = makeTexture(d, kW, kH, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    check(SUCCEEDED(d->CreateDepthStencilView(w.depth.Get(), &dd, w.dsv.GetAddressOf())) &&
              SUCCEEDED(d->CreateDepthStencilView(w.depth2.Get(), &dd, w.dsv2.GetAddressOf())), "fixture DSVs");
    w.rh = rtvOf(d, w.h.Get()); w.rg2 = rtvOf(d, w.g2.Get()); w.rtone = rtvOf(d, w.tone.Get()); w.rsmall = rtvOf(d, w.tiny.Get());
    w.sh = srvOf(d, w.h.Get()); w.scopy = srvOf(d, w.copy.Get()); w.sother = srvOf(d, w.other.Get());
    w.weapon = makeTexture(d, kW, kH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    w.weaponSmall = makeTexture(d, kW / 2, kH / 2, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    w.sweapon = srvOf(d, w.weapon.Get()); w.sweaponSmall = srvOf(d, w.weaponSmall.Get());
    const float gray[4] = {0.25f, 0.25f, 0.25f, 1.0f};
    w.ctx->ClearRenderTargetView(w.rh.Get(), gray);
    // The engine's source data the resolver's prep reads: slots (R32G32), the pool (structured, stride 336), two scene buffers.
    ComPtr<ID3D11Texture2D> slots = makeTexture(d, kW, kH, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    g_slotsSrv = srvOf(d, slots.Get());
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 336 * 4; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 336;
    ComPtr<ID3D11Buffer> pool;
    check(SUCCEEDED(d->CreateBuffer(&bd, nullptr, pool.GetAddressOf())), "fixture pool");
    g_poolSrv = srvOf(d, pool.Get());
    bd = {};
    bd.ByteWidth = 277 * 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(SUCCEEDED(d->CreateBuffer(&bd, nullptr, g_sceneNow.GetAddressOf())) && SUCCEEDED(d->CreateBuffer(&bd, nullptr, g_scenePrev.GetAddressOf())), "fixture scene buffers");
    g_namedDepth = w.depth.Get();
    std::memcpy(g_rows, kEpicRows, sizeof(g_rows));
    return true;
}

// One game draw, as the VR thunks present it to the route: the binding shadow as the hooks left it, then the route's entry.
void draw(World& w, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, std::initializer_list<ID3D11ShaderResourceView*> srvs = {},
          uint64_t vs = 0x1111, uint64_t ps = 0x2222) {
    bindingSet(BindSlot::Rtv0, rtv);
    bindingSet(BindSlot::Dsv0, dsv);
    ID3D11ShaderResourceView* bound[4] = {};
    size_t n = 0;
    for (auto* s : srvs) if (n < 4) bound[n++] = s;
    for (uint32_t i = 0; i < 4; ++i) bindingSet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + i), bound[i]);
    bindingSetShader(BindSlot::Vs, reinterpret_cast<void*>(uintptr_t(0x10)), vs);
    bindingSetShader(BindSlot::Ps, reinterpret_cast<void*>(uintptr_t(0x20)), ps);
    ID3D11ShaderResourceView* real[4] = {bound[0], bound[1], bound[2], bound[3]};
    if (g_realMismatch && real[1]) real[1] = w.sother.Get();
    w.ctx->PSSetShaderResources(0, 4, real);   // the real context, for the route's own PSGetShaderResources verification
    ID3D11RenderTargetView* r = rtv;
    w.ctx->OMSetRenderTargets(1, &r, dsv);
    ++w.seq;
    if (g_vrWorldWants) vrWorldRouteDraw(w.ctx.Get());
}

struct FrameOpts {
    bool lateWrite = false;       // something writes H after the tone (a copy into it)
    bool secondDepth = false;     // H is drawn with two different depth targets
    bool noTone = false;          // the frame has H but no consumer
    int gbufferDraws = 4;         // draws into one target pair before H: the run the route skips at two compares
};
// The retake chain at tiny scale (design doc section 82): the world into the G-buffer, H's draws (with the screen depth), the
// exposure reduction on a COPY of H, late draws, the 630x354-style tiny draws reading H, the tone, then what follows it.
void frame(World& w, const FrameOpts& o = {}) {
    for (int i = 0; i < o.gbufferDraws; ++i) draw(w, w.rg2.Get(), w.dsv.Get());
    for (int i = 0; i < 6; ++i) draw(w, w.rh.Get(), (o.secondDepth && i == 3) ? w.dsv2.Get() : w.dsv.Get());
    // q 8157, the copy of H, is not a draw: nothing reaches the route.
    draw(w, w.rsmall.Get(), nullptr, {w.scopy.Get()});
    for (int i = 0; i < 12; ++i) draw(w, w.rh.Get(), w.dsv.Get(), {w.sother.Get(), w.scopy.Get()});
    for (int i = 0; i < 3; ++i) draw(w, w.rsmall.Get(), nullptr, {w.sh.Get()});   // reads H, but an eighth per axis: rule (iv)
    if (!o.noTone) draw(w, w.rtone.Get(), nullptr, {w.sother.Get(), w.sh.Get()}, 0xF9CFC798F21E9AEAull, 0xFEE777E92850B390ull);   // the tone
    if (o.lateWrite && g_vrWorldWatchWrites) vrWorldRouteNoteWrite(w.h.Get());   // the hooks' own guard
    draw(w, w.rtone.Get(), nullptr, {w.sother.Get()});   // the game copy, the HUD, the eye composites: writes elsewhere
    vrWorldRouteFrameBoundary();
    bindingFrameBoundary();
}

size_t countLines(const char* needle) {
    size_t n = 0;
    for (const auto& l : g_log) if (l.find(needle) != std::string::npos) ++n;
    return n;
}
void configure(bool routeOn) {
    Config::get().set("experimental.temporal_aa_on_foot_world", routeOn ? "auto" : "off");
    Config::get().set("fix.temporal_aa", "dlss");
}
void reset(World& w) {
    // Every scenario starts from a cleared route: the key off for a boundary lets go of everything.
    configure(false);
    vrWorldRouteFrameBoundary();
    vrWorldRouteFrameBoundary();
    g_log.clear(); g_calls.clear(); g_backendCalls = 0; g_mipsResets = 0;
    g_gate = g_layerLive = g_named = g_rowsKnown = g_viewsReady = true;
    g_panelW = kW; g_panelH = kH; g_realMismatch = false;
    g_namedDepth = w.depth.Get();
}

void scenarios(World& w) {
    g_runtimeProfile = RuntimeProfile::Vr;

    // 1. KEY OFF: nothing runs, nothing is logged, nothing is held.
    reset(w);
    for (int i = 0; i < 30; ++i) frame(w);
    check(g_backendCalls == 0 && !g_vrWorldWants && !vrWorldRouteOwnsNextFrame() && !vrWorldRouteTreatedThisFrame() &&
              vrWorldRouteState() == VrWorldState::Off && !vrWorldRouteLayerMayTake() && !vrWorldRouteEnabled(),
          "key off: 30 frames of the chain: no resolve, no draw watched, nothing owned, the state Off");
    check(g_log.empty() && g_mipsResets == 0, "key off: not one log line and nothing released");

    // 2. THE HAPPY PATH.
    reset(w);
    configure(true);
    vrWorldRouteFrameBoundary();   // the first boundary with the key on: Observing, wanting draws
    check(vrWorldRouteState() == VrWorldState::Observing && g_vrWorldWants && vrWorldRouteEnabled(), "auto: the first boundary leaves the route Observing and wanting draws");
    frame(w);
    check(g_backendCalls == 1 && g_vrWorldWants && vrWorldRouteState() == VrWorldState::Warming,
          "auto: the tone triggers one resolve in the first frame, and the route is Warming");
    check(!g_calls.empty() && g_calls[0].slot == int(kVrWorldFeatureSlot), "auto: the backend was asked for the world's own upscaler slot (2)");
    check(!g_calls.empty() && g_calls[0].hdr && g_calls[0].reset && g_calls[0].internalFlags &&
              g_calls[0].w == kW && g_calls[0].h == kH && g_calls[0].outW == kW && g_calls[0].outH == kH && g_calls[0].colour == DXGI_FORMAT_R11G11B10_FLOAT,
          "auto: the backend saw the HDR flag, a RESET, both internal flags, H's size and format, at E = R = D");
    check(!g_vrWorldInternal && !g_flatComputeInternal, "auto: the internal flags are down again after the call");
    check(countLines("first trigger at frame=") == 1, "auto: the first trigger is logged once");
    for (int i = 1; i < int(kVrWorldWarmFrames) - 1; ++i) frame(w);
    check(g_backendCalls == int(kVrWorldWarmFrames) - 1 && vrWorldRouteState() == VrWorldState::Warming && !vrWorldRouteOwnsNextFrame(),
          "auto: one treated frame short of the warm-up the route is still Warming and the eye shift is still on");
    check(g_calls.size() >= 2 && !g_calls[1].reset, "auto: the second frame continues the history (no reset)");
    frame(w);
    check(vrWorldRouteState() == VrWorldState::Owned && vrWorldRouteOwnsNextFrame() && countLines("OWNS the world") == 1,
          "auto: the kVrWorldWarmFrames-th treated frame makes it Owned, publishes it for the eye shift, and says so once");
    check(!vrWorldRouteTreatedThisFrame(), "auto: the flags of a new frame are clear until its tone");
    // A frame while owned: the route resolves at the tone and the layer may then take the screen.
    bool tookMay = false;
    for (int i = 0; i < 3; ++i) draw(w, w.rg2.Get(), w.dsv.Get());
    for (int i = 0; i < 6; ++i) draw(w, w.rh.Get(), w.dsv.Get());
    for (int i = 0; i < 3; ++i) draw(w, w.rsmall.Get(), nullptr, {w.sh.Get()});
    check(!vrWorldRouteLayerMayTake(), "owned: before the tone of the frame the layer may not take the screen");
    draw(w, w.rtone.Get(), nullptr, {w.sother.Get(), w.sh.Get()});
    tookMay = vrWorldRouteLayerMayTake();
    check(tookMay && vrWorldRouteTreatedThisFrame(), "owned: once the route treated the frame (at the tone) the layer may take the screen draw");
    // The game's pipeline state is as the route found it (the SDK stub cleared every stage inside the resolver's isolation).
    ComPtr<ID3D11RenderTargetView> rtvNow;
    ComPtr<ID3D11ShaderResourceView> srv1;
    w.ctx->OMGetRenderTargets(1, rtvNow.GetAddressOf(), nullptr);
    w.ctx->PSGetShaderResources(1, 1, srv1.GetAddressOf());
    check(rtvNow.Get() == w.rtone.Get() && srv1.Get() == w.sh.Get(),
          "owned: after the resolve the game's render target and t1 are exactly what the tone draw had bound (state isolation)");
    vrWorldRouteNoteEyeTaken(0, 77);
    check(vrWorldRouteDoorLayerOnly(0, 77) && !vrWorldRouteDoorLayerOnly(1, 77) && !vrWorldRouteDoorLayerOnly(0, 78),
          "owned: the layer's take of eye 0 in sequence 77 is what makes the door layer-only, for that eye and sequence only");
    // The temporal door and then the sharpen pass both ask, so the same eye and sequence is asked twice: the answer is the
    // same and the 5 s line counts it once. The window prints at the first boundary after five seconds (real time).
    check(vrWorldRouteDoorLayerOnly(0, 77), "owned: the door's second question about the same eye and sequence gets the same answer");
    Sleep(5100);
    vrWorldRouteFrameBoundary();
    bindingFrameBoundary();
    {
        bool lineOk = false, stateOk = false;
        for (const auto& l : g_log) {
            if (l.find("vr world route 5s:") == std::string::npos) continue;
            lineOk = l.find("eye-takes=1 door-layer-only=1 ") != std::string::npos;
            stateOk = l.find("state=owned") != std::string::npos && l.find("selection=selected") != std::string::npos;
        }
        check(lineOk, "owned: the 5 s line counts the eye's take once and the layer-only door once (asked twice)");
        check(stateOk, "owned: the 5 s line names the state (owned) and the selection (selected)");
    }
    check(!vrWorldRouteDoorLayerOnly(0, 77), "owned: the per-frame tags are cleared at the boundary");
    const auto stats = flatMonoResolveStats();
    check(stats.hdrResolves >= uint64_t(kVrWorldWarmFrames), "auto: the resolver counted its HDR resolves");

    // A long run of draws into one uninteresting target pair (the shadow atlas, the G-buffer) must not hide what follows it.
    {
        const int before = g_backendCalls;
        FrameOpts o;
        o.gbufferDraws = 6000;
        frame(w, o);
        check(g_backendCalls == before + 1, "run skip: 6000 draws into one G-buffer target pair, then H and the tone: the tone still triggers the resolve");
    }

    // The weapon fold-in seam: a map and the depth's stencil view go to the resolver together or not at all.
    {
        const auto before = flatMonoResolveStats();
        frame(w);
        check(flatMonoResolveStats().firstPersonFrames == before.firstPersonFrames && flatMonoResolveStats().firstPersonRefused == before.firstPersonRefused,
              "weapon: no weapon map this frame: the resolver gets no first-person inputs (the flat path's weapon handling)");
        g_weaponMapSrv = w.sweapon;
        frame(w);
        check(flatMonoResolveStats().firstPersonFrames == before.firstPersonFrames + 1 && flatMonoResolveStats().firstPersonRefused == before.firstPersonRefused,
              "weapon: a weapon map and the stencil view reach the resolver together and are taken");
        g_weaponMapSrv = w.sweaponSmall;
        const int calls = g_backendCalls;
        frame(w);
        check(g_backendCalls == calls + 1 && flatMonoResolveStats().firstPersonRefused == before.firstPersonRefused + 1,
              "weapon: a map of the wrong size is refused and counted by the resolver, and the frame is still treated without it");
        g_weaponMapSrv.Reset();
    }
    // 3. LATE WRITES latch the route off.
    frame(w, {true});
    frame(w, {true});
    check(vrWorldRouteState() == VrWorldState::Owned, "latch: two treated frames with a write into H after the tone do not trip it");
    frame(w, {true});
    check(vrWorldRouteState() == VrWorldState::Latched && !vrWorldRouteOwnsNextFrame() && countLines("turned off at frame=") == 1 &&
              countLines("wrote the scene HDR after the trigger") == 1,
          "latch: the third one latches the route off, releases the eye shift, and names what wrote H");
    const int callsBefore = g_backendCalls;
    for (int i = 0; i < 5; ++i) frame(w);
    check(g_backendCalls == callsBefore && !g_vrWorldWants, "latch: a latched route does not watch draws or resolve");

    // 4. REFUSALS: each selector reason, the eye route serves the frame.
    struct Refusal { const char* what; const char* reason; void (*set)(World&, bool); };
    const Refusal refusals[] = {
        {"the source was not named this frame", "depth-not-screen-motion-source", [](World&, bool on) { g_named = !on; }},
        {"the engine views are not ready", "engine-views-unavailable", [](World&, bool on) { g_viewsReady = !on; }},
        {"the source camera's rows are unknown", "camera-rows-unavailable", [](World&, bool on) { g_rowsKnown = !on; }},
        {"H is not the screen's size", "hdr-not-screen-sized", [](World&, bool on) { g_panelW = on ? kW * 2 : kW; g_panelH = on ? kH * 2 : kH; }},
    };
    for (const auto& r : refusals) {
        reset(w);
        configure(true);
        vrWorldRouteFrameBoundary();
        r.set(w, true);
        const size_t lines = g_log.size();
        (void)lines;
        for (int i = 0; i < 4; ++i) frame(w);
        char what[200];
        std::snprintf(what, sizeof(what), "refusal: %s: no resolve in 4 frames, the state never warms, and the reason %s is logged", r.what, r.reason);
        check(g_backendCalls == 0 && vrWorldRouteState() == VrWorldState::Observing && countLines(r.reason) >= 1, what);
        r.set(w, false);
        frame(w);
        check(g_backendCalls == 1, "refusal: and the next frame with the fact restored is treated");
    }
    {   // the shadow says H is bound at t1 but the real context disagrees: the route verifies the actual binding once
        reset(w);
        configure(true);
        vrWorldRouteFrameBoundary();
        g_realMismatch = true;
        for (int i = 0; i < 3; ++i) frame(w);
        check(g_backendCalls == 0 && countLines("actual-hdr-binding-or-depth-view-refused") >= 1,
              "refusal: the shadow nominated a draw whose real t1 is not H: the route verifies the actual binding and does not resolve");
        g_realMismatch = false;
    }
    {   // H drawn with two depth targets: the route cannot name its depth
        reset(w);
        configure(true);
        vrWorldRouteFrameBoundary();
        for (int i = 0; i < 3; ++i) frame(w, {false, true});
        check(g_backendCalls == 0 && countLines("hdr-depth-not-single") >= 1, "refusal: H drawn with two different depth targets is not resolved (hdr-depth-not-single)");
    }
    {   // a frame with H and no consumer
        reset(w);
        configure(true);
        vrWorldRouteFrameBoundary();
        for (int i = 0; i < 3; ++i) frame(w, {false, false, true});
        check(g_backendCalls == 0, "refusal: H with no consumer draws no resolve");
    }
    {   // the layer not live: the route stays off, with one line saying why
        reset(w);
        configure(true);
        g_layerLive = false;
        for (int i = 0; i < 5; ++i) frame(w);
        check(g_backendCalls == 0 && !g_vrWorldWants && countLines("the route stays off") == 1 && countLines("fix.ui_quality") >= 1,
              "layer not live: five frames, no resolve, draws not watched, ONE line saying the route stays off and why");
        g_layerLive = true;
        frame(w); frame(w);
        check(g_backendCalls >= 1, "layer not live: live again, the route treats");
    }
    {   // the gate lost while owned, then regained
        reset(w);
        configure(true);
        vrWorldRouteFrameBoundary();
        for (int i = 0; i < int(kVrWorldWarmFrames); ++i) frame(w);
        check(vrWorldRouteState() == VrWorldState::Owned, "gate: owned after the warm-up");
        g_gate = false;
        frame(w);
        check(vrWorldRouteState() == VrWorldState::Observing && !vrWorldRouteOwnsNextFrame() && !g_vrWorldWants && countLines("on-foot-gate-lost") == 1,
              "gate: the gate lost releases the world at once (on-foot-gate-lost), puts the eye shift back and stops watching draws");
    }

    // 5. A SCENE RESET releases and resets; a FRAME GAP resets the history.
    reset(w);
    configure(true);
    vrWorldRouteFrameBoundary();
    for (int i = 0; i < int(kVrWorldWarmFrames); ++i) frame(w);
    g_calls.clear();
    vrWorldRouteNoteSceneReset();
    frame(w);   // the frame still draws and treats; the boundary releases
    check(g_calls.size() == 1 && g_calls[0].reset, "scene reset: the next resolve after the event resets the history");
    check(vrWorldRouteState() == VrWorldState::Observing && !vrWorldRouteOwnsNextFrame() && countLines("(scene-reset)") == 1,
          "scene reset: an owned route is released at the boundary (scene-reset) and the eye shift is back");
    frame(w);
    check(g_calls.size() == 2 && !g_calls[1].reset, "scene reset: and the history continues from there");
    g_named = false;
    frame(w);   // a refused frame: a gap
    g_named = true;
    frame(w);
    check(g_calls.back().reset, "frame gap: a treat after an untreated frame resets the history");
    frame(w);
    check(!g_calls.back().reset, "frame gap: and the frame after that continues it");

    // 6. THE KEY GOES OFF while owned: everything is let go.
    reset(w);
    configure(true);
    vrWorldRouteFrameBoundary();
    for (int i = 0; i < int(kVrWorldWarmFrames); ++i) frame(w);
    const uint64_t resetsBefore = flatMonoResolveStats().fullResets;
    const int mipsBefore = g_mipsResets;
    configure(false);
    vrWorldRouteFrameBoundary();
    check(vrWorldRouteState() == VrWorldState::Off && !vrWorldRouteOwnsNextFrame() && !g_vrWorldWants && countLines("(key-off)") == 1,
          "key off while owned: released at once (key-off), the eye shift back on, draws no longer watched");
    check(flatMonoResolveStats().fullResets == resetsBefore + 1 && g_mipsResets == mipsBefore + 1,
          "key off while owned: the resolver's textures and the mipped screen are let go, once");
    vrWorldRouteFrameBoundary();
    check(flatMonoResolveStats().fullResets == resetsBefore + 1 && g_mipsResets == mipsBefore + 1, "key off: and not again on the boundaries after");
}

int runSelfTest() {
    World w;
    if (!build(w)) { std::printf("vr world route gpu: could not build the fixture\n"); return 1; }
    scenarios(w);
    if (g_failures) {
        std::printf("vr world route gpu: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("vr world route gpu: PASS (%d checks)\n", g_checks);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return runSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("vr world route gpu test: would run the route's runtime on WARP against a synthetic chain; writes no files.\n");
        return 0;
    }
    std::printf("usage: vr_world_route_gpu_test --self-test|--dry-run\n");
    return 2;
}

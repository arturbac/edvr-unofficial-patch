// What the shared pair's sprite draws cost, at the game's size, in the UI layer's size, with a scissor, and
// drawn at 1.0 scale inside a 1.25 layer (src/d3d11/shared_pair.h; docs/openxr-performance-review-2026-09-14.md,
// "the landing pad in the layer").
//
// The pair is VS 94D5C556DFD6D705 / PS 912477AEF6958379: an instanced screen-space sprite stamp, six vertices an
// instance, per-instance records in a vertex buffer, two 2048x1024 BC7 sheets. The landing-pad display's rings
// and crosshair are two draws of it an eye (about 3400 instances, then about 95). Moving them from the game's
// frame into the UI layer redraws them at the layer's size, and the layer's size is a setting: 4032x3896 at UI
// quality 100, 5040x4870 at 125 on the flights that measured this, against a game eye of 2016x1948 or 2620x2532.
//
// What is real here: the pixel shader is transcribed instruction for instruction from the game's (its two
// texture reads and blend inputs), the states are the census's (source alpha, one, add; depth test, no write),
// the formats are the game's HDR target (R11G11B10) and the layer's (RGBA16F with a D32S8 depth target at the
// layer's size), the instance counts are the captures'. What is NOT real is the instance data: it lives in a
// vertex buffer no CPU-side hook reads, and the captures hold only the draws' shape. So the sprites are
// synthetic, laid on concentric rings and a crosshair in the display's region of the eye, and their size is the
// one free parameter: the bench finds the size at which the game-size draw costs what the census measured on
// this card (0.077 ms a draw, `sun glare steady` in flight 132352), then reads what the same content costs at
// the other sizes, and compares with the layer price the stock windows of that flight measured (0.36 ms a
// draw at 5040x4870). The vertex shader is a synthetic one with the real one's texture reads (17 depth taps a
// vertex); its cost is the size-independent part, measured with the sprites collapsed.
//
//   pair_price_bench --self-test | --dry-run | --bench [--adapter nvidia] [--game-ms MS]
//
// --self-test runs on WARP (the build gate): the bench draws what it says it draws -- the covered pixels, the
// blend, the scissor, the size scaling -- exactly. --bench is a desk run on the RTX (never part of the build).
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

unsigned g_checks = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::exit(1);
    }
}
void hr(HRESULT r, const char* what) {
    if (FAILED(r)) {
        std::printf("FAIL: %s (0x%08lX)\n", what, static_cast<unsigned long>(r));
        std::exit(1);
    }
}

// ------------------------------------------------------------------------------------------- shaders --

// The vertex shader: quads at NDC centres with NDC half-sizes, and the real one's cost shape (17 depth taps).
constexpr const char* kVs = R"(
cbuffer Frame : register(b0) { float2 invDepthSize; float depthZ; float spriteScale; };
Texture2D<float> gDepth : register(t0);
SamplerState sPoint : register(s0);
struct VSIn { float2 corner : TEXCOORD0; float4 posSize : POSITION0; float4 tile : TEXCOORD1; float4 color : COLOR0; };
struct VSOut { float4 pos : SV_POSITION; float4 c : TEXCOORD1; float4 f : TEXCOORD2; float4 uv : TEXCOORD3; };
static const float2 kTaps[17] = {
    float2(0,0), float2(-2,-1), float2(-1,-1), float2(0,-1), float2(1,-1), float2(2,-1), float2(-2,0), float2(-1,0), float2(1,0),
    float2(2,0), float2(-2,1), float2(-1,1), float2(0,1), float2(1,1), float2(2,1), float2(0,-2), float2(0,2) };
VSOut main(VSIn v) {
    VSOut o;
    float2 ndc = v.posSize.xy + v.corner * v.posSize.zw * spriteScale;
    float2 uv = v.posSize.xy * float2(0.5, -0.5) + 0.5;
    float vis = 0;
    [unroll] for (int i = 0; i < 17; ++i) vis += gDepth.SampleLevel(sPoint, uv + kTaps[i] * invDepthSize, 0) > depthZ ? 1.0 : 0.0;
    o.pos = float4(ndc, 0.5, 1);
    o.c = float4(v.color.rgb, v.color.a * (0.5 + vis / 34.0));
    o.f = float4(0.5, 0, 0, 0);
    float2 k = v.corner * 0.5 + 0.5;
    o.uv = float4(lerp(v.tile.xy, v.tile.zw, k), lerp(v.tile.zw, v.tile.xy, k));
    return o;
}
)";

// The game's pixel shader 912477AEF6958379, instruction for instruction (the disassembly in the log's
// dumped shaders): two texture reads, a lerp between them by v1.x, three scale constants.
constexpr const char* kPs = R"(
cbuffer PsK : register(b1) { float4 k90; float4 k92; };
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
SamplerState s0 : register(s0);
SamplerState s1 : register(s1);
float4 main(float4 pos : SV_POSITION, float4 v0 : TEXCOORD1, float4 v1w : TEXCOORD2, float4 v2 : TEXCOORD3) : SV_TARGET {
    float v1 = v1w.x;
    float3 r0 = t1.Sample(s0, v2.zw).xyz * k92.x;
    float4 r1 = t0.Sample(s1, v2.xy);
    r0 = r0 * r1.xyz - r1.xyz;
    r0 = v1 * r0 + r1.xyz;
    float a = r1.w * v0.w;
    r0 = r0 * v0.xyz * k92.y;
    return float4(r0 * k90.y, a);
}
)";

ComPtr<ID3DBlob> compile(const char* src, const char* profile, const char* what) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT r = D3DCompile(src, std::strlen(src), what, nullptr, nullptr, "main", profile, 0, 0, &code, &errors);
    if (FAILED(r)) {
        std::printf("FAIL: compiling %s: %s\n", what, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        std::exit(1);
    }
    return code;
}

// ------------------------------------------------------------------------------------------- the device --

struct Device {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    bool warp = true;
    std::wstring name;
};

Device makeDevice(bool nvidia) {
    Device d;
    ComPtr<IDXGIAdapter> adapter;
    D3D_DRIVER_TYPE type = D3D_DRIVER_TYPE_WARP;
    if (nvidia) {
        ComPtr<IDXGIFactory1> factory;
        hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter> a;
            if (factory->EnumAdapters(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC desc{};
            a->GetDesc(&desc);
            if (desc.VendorId == 0x10DE) {
                adapter = a;
                d.name = desc.Description;
                break;
            }
        }
        if (!adapter) {
            std::puts("FAIL: no NVIDIA adapter for --adapter nvidia");
            std::exit(1);
        }
        type = D3D_DRIVER_TYPE_UNKNOWN;
        d.warp = false;
    } else {
        d.name = L"WARP";
    }
    D3D_FEATURE_LEVEL level{};
    hr(D3D11CreateDevice(adapter.Get(), type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d.dev, &level, &d.ctx), "D3D11CreateDevice");
    return d;
}

// ------------------------------------------------------------------------------------------- the scene --

struct Instance {
    float cx, cy, hx, hy;     // NDC centre and half-size (the sprite scale multiplies the half-size)
    float u0, v0, u1, v1;     // atlas rectangle
    float r, g, b, a;
};

// The display's region of the eye, in NDC: from the landing-pad dump the rings and crosshair sit around
// (0.52, 0.55) of the eye, about 0.14 wide and 0.15 tall.
constexpr float kDisplayCx = 0.04f, kDisplayCy = -0.10f, kDisplayRx = 0.075f, kDisplayRy = 0.078f;
float g_regionScale = 1.0f;   // the layout sensitivity run widens or narrows the region the rings and crosshair are laid in

uint32_t g_rng = 0x2545F491u;
float rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return static_cast<float>(g_rng & 0xFFFFFFu) / 16777216.0f;
}

// n sprites on concentric rings and a crosshair inside the display's region; `outsideFraction` of them are
// placed anywhere in the eye instead (a content leak past the display, for the scissor's upper bound).
std::vector<Instance> makeInstances(unsigned n, float outsideFraction, float halfSize) {
    g_rng = 0x2545F491u + n;
    std::vector<Instance> v;
    v.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        Instance s{};
        if (rnd() < outsideFraction) {
            s.cx = rnd() * 2.0f - 1.0f;
            s.cy = rnd() * 2.0f - 1.0f;
        } else if (i % 5 == 0) {   // the crosshair: a horizontal and a vertical line through the centre
            const float t = rnd() * 2.0f - 1.0f;
            const bool horizontal = (i / 5) & 1;
            s.cx = kDisplayCx + (horizontal ? t * kDisplayRx * g_regionScale : 0.0f);
            s.cy = kDisplayCy + (horizontal ? 0.0f : t * kDisplayRy * g_regionScale);
        } else {                   // the rings: seven of them, dashes along each
            const float ring = 0.18f + 0.82f * static_cast<float>(i % 7) / 6.0f;
            const float th = rnd() * 6.2831853f;
            s.cx = kDisplayCx + std::cos(th) * ring * kDisplayRx * g_regionScale;
            s.cy = kDisplayCy + std::sin(th) * ring * kDisplayRy * g_regionScale;
        }
        s.hx = s.hy = halfSize;
        s.u0 = 0.25f; s.v0 = 0.25f; s.u1 = 0.35f; s.v1 = 0.35f;   // inside one atlas tile
        s.r = 0.25f; s.g = 0.9f; s.b = 1.0f; s.a = 0.5f;
        v.push_back(s);
    }
    return v;
}

// One render size and format with everything a draw of the pair needs bound to it.
struct Rig {
    Device* d = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT rtFormat = DXGI_FORMAT_UNKNOWN;
    bool bc7 = true;
    ComPtr<ID3D11Texture2D> rt, ds, depthSrv, atlas0, atlas1;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> depthView, a0, a1;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> corners, vsCb, psCb;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> dss;
    ComPtr<ID3D11RasterizerState> rsNoScissor, rsScissor;
    ComPtr<ID3D11SamplerState> point, linear;
    ComPtr<ID3D11Query> statsQuery;
};

ComPtr<ID3D11Texture2D> makeTexture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, UINT mips, const void* seedData) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = mips;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    std::vector<uint8_t> bytes;
    if (seedData) {
        const bool block = fmt == DXGI_FORMAT_BC7_UNORM;
        UINT mw = w, mh = h;
        for (UINT m = 0; m < mips; ++m) {
            const UINT rowBytes = block ? std::max(1u, (mw + 3) / 4) * 16 : mw * 4;
            const UINT rows = block ? std::max(1u, (mh + 3) / 4) : mh;
            bytes.resize(bytes.size() + static_cast<size_t>(rowBytes) * rows);
            mw = std::max(1u, mw / 2);
            mh = std::max(1u, mh / 2);
        }
        size_t at = 0;
        mw = w;
        mh = h;
        std::vector<size_t> starts;
        for (UINT m = 0; m < mips; ++m) {
            const UINT rowBytes = block ? std::max(1u, (mw + 3) / 4) * 16 : mw * 4;
            const UINT rows = block ? std::max(1u, (mh + 3) / 4) : mh;
            starts.push_back(at);
            at += static_cast<size_t>(rowBytes) * rows;
            mw = std::max(1u, mw / 2);
            mh = std::max(1u, mh / 2);
        }
        // random blocks for BC7 (any 16 bytes decode to something), the seed colour for the plain formats
        if (block) {
            for (auto& b : bytes) b = static_cast<uint8_t>(rnd() * 255.0f);
        } else {
            const uint32_t c = *static_cast<const uint32_t*>(seedData);
            for (size_t i = 0; i + 3 < bytes.size(); i += 4) std::memcpy(&bytes[i], &c, 4);
        }
        mw = w;
        mh = h;
        for (UINT m = 0; m < mips; ++m) {
            D3D11_SUBRESOURCE_DATA sd{};
            sd.pSysMem = bytes.data() + starts[m];
            sd.SysMemPitch = block ? std::max(1u, (mw + 3) / 4) * 16 : mw * 4;
            init.push_back(sd);
            mw = std::max(1u, mw / 2);
            mh = std::max(1u, mh / 2);
        }
    }
    ComPtr<ID3D11Texture2D> t;
    hr(dev->CreateTexture2D(&td, seedData ? init.data() : nullptr, &t), "CreateTexture2D");
    return t;
}

// bc7: the game's two 2048x1024 sheets (random blocks); otherwise RGBA8 sheets of one colour, for the self-test.
void makeRig(Device& d, Rig& r, UINT w, UINT h, DXGI_FORMAT rtFormat, bool bc7, uint32_t sheetColour = 0x80808080u, uint32_t sheet1Colour = 0xFFFFFFFFu) {
    ID3D11Device* dev = d.dev.Get();
    r.d = &d;
    r.w = w;
    r.h = h;
    r.rtFormat = rtFormat;
    r.bc7 = bc7;
    r.rt = makeTexture(dev, w, h, rtFormat, D3D11_BIND_RENDER_TARGET, 1, nullptr);
    hr(dev->CreateRenderTargetView(r.rt.Get(), nullptr, &r.rtv), "RTV");
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = w;
    dd.Height = h;
    dd.MipLevels = dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr(dev->CreateTexture2D(&dd, nullptr, &r.ds), "depth-stencil");
    hr(dev->CreateDepthStencilView(r.ds.Get(), nullptr, &r.dsv), "DSV");
    // the depth the vertex shader taps: a plain R32 texture of the game's eye size, all far
    const UINT dw = 512, dh = 512;
    std::vector<float> farDepth(static_cast<size_t>(dw) * dh, 1.0f);
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = dw;
    sd.Height = dh;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.SampleDesc.Count = 1;
    sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sdata{farDepth.data(), dw * 4, 0};
    hr(dev->CreateTexture2D(&sd, &sdata, &r.depthSrv), "depth sheet");
    hr(dev->CreateShaderResourceView(r.depthSrv.Get(), nullptr, &r.depthView), "depth SRV");
    if (bc7) {
        r.atlas0 = makeTexture(dev, 2048, 1024, DXGI_FORMAT_BC7_UNORM, D3D11_BIND_SHADER_RESOURCE, 12, &sheetColour);
        r.atlas1 = makeTexture(dev, 2048, 1024, DXGI_FORMAT_BC7_UNORM, D3D11_BIND_SHADER_RESOURCE, 12, &sheet1Colour);
    } else {
        r.atlas0 = makeTexture(dev, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, 1, &sheetColour);
        r.atlas1 = makeTexture(dev, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, 1, &sheet1Colour);
    }
    hr(dev->CreateShaderResourceView(r.atlas0.Get(), nullptr, &r.a0), "atlas 0 SRV");
    hr(dev->CreateShaderResourceView(r.atlas1.Get(), nullptr, &r.a1), "atlas 1 SRV");
    ComPtr<ID3DBlob> vsCode = compile(kVs, "vs_5_0", "pair vertex shader");
    ComPtr<ID3DBlob> psCode = compile(kPs, "ps_5_0", "pair pixel shader");
    hr(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &r.vs), "CreateVertexShader");
    hr(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &r.ps), "CreatePixelShader");
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    hr(dev->CreateInputLayout(il, 4, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &r.layout), "CreateInputLayout");
    const float corners[12] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(corners);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA cd{corners, 0, 0};
    hr(dev->CreateBuffer(&bd, &cd, &r.corners), "corner buffer");
    bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr(dev->CreateBuffer(&bd, nullptr, &r.vsCb), "VS constants");
    bd.ByteWidth = 32;
    hr(dev->CreateBuffer(&bd, nullptr, &r.psCb), "PS constants");
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr(dev->CreateBlendState(&bl, &r.blend), "blend");
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    hr(dev->CreateDepthStencilState(&ds, &r.dss), "depth state");
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    hr(dev->CreateRasterizerState(&rs, &r.rsNoScissor), "rasterizer");
    rs.ScissorEnable = TRUE;
    hr(dev->CreateRasterizerState(&rs, &r.rsScissor), "scissor rasterizer");
    D3D11_SAMPLER_DESC sm{};
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    hr(dev->CreateSamplerState(&sm, &r.point), "point sampler");
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    hr(dev->CreateSamplerState(&sm, &r.linear), "linear sampler");
    D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
    dev->CreateQuery(&qd, &r.statsQuery);   // absent on some paths: only the pixel count uses it
}

ComPtr<ID3D11Buffer> instanceBuffer(Rig& r, const std::vector<Instance>& v) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(v.size() * sizeof(Instance));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{v.data(), 0, 0};
    ComPtr<ID3D11Buffer> b;
    hr(r.d->dev->CreateBuffer(&bd, &sd, &b), "instance buffer");
    return b;
}

struct Draw {
    ComPtr<ID3D11Buffer> buf;
    UINT instances = 0;
};

struct Pass {
    std::vector<Draw> draws;      // one draw of the pair each, in order
    float spriteScale = 1.0f;     // the sprites' half-size multiplier (0 collapses them: the vertex work alone)
    bool scissor = false;
    D3D11_RECT rect{};
};

void setup(Rig& r, const Pass& p) {
    ID3D11DeviceContext* ctx = r.d->ctx.Get();
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(ctx->Map(r.vsCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "map VS constants");
    float* c = static_cast<float*>(m.pData);
    c[0] = 1.0f / 512.0f;
    c[1] = 1.0f / 512.0f;
    c[2] = 0.5f;
    c[3] = p.spriteScale;
    ctx->Unmap(r.vsCb.Get(), 0);
    hr(ctx->Map(r.psCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "map PS constants");
    float* k = static_cast<float*>(m.pData);
    for (int i = 0; i < 8; ++i) k[i] = 1.0f;
    ctx->Unmap(r.psCb.Get(), 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(r.layout.Get());
    ctx->VSSetShader(r.vs.Get(), nullptr, 0);
    ctx->PSSetShader(r.ps.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, r.vsCb.GetAddressOf());
    ctx->PSSetConstantBuffers(1, 1, r.psCb.GetAddressOf());
    ctx->VSSetShaderResources(0, 1, r.depthView.GetAddressOf());
    ctx->VSSetSamplers(0, 1, r.point.GetAddressOf());
    ID3D11ShaderResourceView* sr[2] = {r.a0.Get(), r.a1.Get()};
    ctx->PSSetShaderResources(0, 2, sr);
    ID3D11SamplerState* ss[2] = {r.linear.Get(), r.linear.Get()};
    ctx->PSSetSamplers(0, 2, ss);
    const float bf[4] = {};
    ctx->OMSetBlendState(r.blend.Get(), bf, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(r.dss.Get(), 0);
    ctx->OMSetRenderTargets(1, r.rtv.GetAddressOf(), r.dsv.Get());
    ctx->RSSetState(p.scissor ? r.rsScissor.Get() : r.rsNoScissor.Get());
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(r.w), static_cast<float>(r.h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    if (p.scissor) ctx->RSSetScissorRects(1, &p.rect);
    const UINT strides[1] = {8}, offs[1] = {0};
    ctx->IASetVertexBuffers(0, 1, r.corners.GetAddressOf(), strides, offs);
}

void clearAll(Rig& r) {
    const float z[4] = {};
    r.d->ctx->ClearRenderTargetView(r.rtv.Get(), z);
    r.d->ctx->ClearDepthStencilView(r.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
}

void issue(Rig& r, const Pass& p) {
    ID3D11DeviceContext* ctx = r.d->ctx.Get();
    for (const Draw& d : p.draws) {
        const UINT stride = sizeof(Instance), off = 0;
        ctx->IASetVertexBuffers(1, 1, d.buf.GetAddressOf(), &stride, &off);
        ctx->DrawInstanced(6, d.instances, 0, 0);
    }
}

// The median of `rounds` timestamped batches, in ms per pass. Each round clears first, outside the timestamps.
double timePass(Rig& r, const Pass& p, int batch, int rounds) {
    ID3D11DeviceContext* ctx = r.d->ctx.Get();
    std::vector<double> t;
    for (int round = 0; round < rounds + 2; ++round) {
        clearAll(r);
        setup(r, p);
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ComPtr<ID3D11Query> disjoint, t0, t1;
        hr(r.d->dev->CreateQuery(&qd, &disjoint), "disjoint query");
        qd.Query = D3D11_QUERY_TIMESTAMP;
        hr(r.d->dev->CreateQuery(&qd, &t0), "timestamp query");
        hr(r.d->dev->CreateQuery(&qd, &t1), "timestamp query");
        ctx->Begin(disjoint.Get());
        ctx->End(t0.Get());
        for (int i = 0; i < batch; ++i) issue(r, p);
        ctx->End(t1.Get());
        ctx->End(disjoint.Get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        while (ctx->GetData(disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) {}
        UINT64 a = 0, b = 0;
        while (ctx->GetData(t0.Get(), &a, sizeof(a), 0) == S_FALSE) {}
        while (ctx->GetData(t1.Get(), &b, sizeof(b), 0) == S_FALSE) {}
        if (round >= 2 && !dj.Disjoint) t.push_back(static_cast<double>(b - a) / static_cast<double>(dj.Frequency) * 1000.0 / batch);
    }
    if (t.empty()) return -1.0;
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

// The pixel shader's invocations for one pass (Mpx), where the device reports them.
double shadedMpx(Rig& r, const Pass& p) {
    if (!r.statsQuery) return -1.0;
    ID3D11DeviceContext* ctx = r.d->ctx.Get();
    clearAll(r);
    setup(r, p);
    ctx->Begin(r.statsQuery.Get());
    issue(r, p);
    ctx->End(r.statsQuery.Get());
    D3D11_QUERY_DATA_PIPELINE_STATISTICS st{};
    while (ctx->GetData(r.statsQuery.Get(), &st, sizeof(st), 0) == S_FALSE) {}
    return static_cast<double>(st.PSInvocations) / 1.0e6;
}

// ------------------------------------------------------------------------------------------- readback --

std::vector<uint8_t> readbackRgba8(Rig& r) {
    D3D11_TEXTURE2D_DESC td{};
    r.rt->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    hr(r.d->dev->CreateTexture2D(&td, nullptr, &st), "staging");
    r.d->ctx->CopyResource(st.Get(), r.rt.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(r.d->ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m), "map staging");
    std::vector<uint8_t> out(static_cast<size_t>(r.w) * r.h * 4);
    for (UINT y = 0; y < r.h; ++y) std::memcpy(&out[static_cast<size_t>(y) * r.w * 4], static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, r.w * 4);
    r.d->ctx->Unmap(st.Get(), 0);
    return out;
}
size_t covered(const std::vector<uint8_t>& px) {
    size_t n = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) n += (px[i] | px[i + 1] | px[i + 2]) != 0;
    return n;
}

Instance one(float cx, float cy, float half) {
    Instance s{};
    s.cx = cx; s.cy = cy; s.hx = s.hy = half;
    s.u0 = 0.25f; s.v0 = 0.25f; s.u1 = 0.35f; s.v1 = 0.35f;
    s.r = s.g = s.b = 1.0f; s.a = 1.0f;
    return s;
}

// ------------------------------------------------------------------------------------------- self-test --

void selfTest() {
    Device d = makeDevice(false);
    // 64x64, sheets of (0.5, 0.5, 0.5, 1) and white: the pixel shader gives (0.5, 0.5, 0.5, a) for a sprite of
    // colour 1, alpha 1 -- the lerp term is zero because t1 * k * t0 - t0 = 0 for t1 = 1 -- and the blend adds it.
    Rig r;
    makeRig(d, r, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, false, 0xFF808080u, 0xFFFFFFFFu);
    Pass p;
    p.draws.push_back({instanceBuffer(r, {one(0.0f, 0.0f, 0.5f)}), 1});
    clearAll(r);
    setup(r, p);
    issue(r, p);
    std::vector<uint8_t> px = readbackRgba8(r);
    check(covered(px) == 32u * 32u, "self-test: a sprite of half-size 0.5 NDC covers 32x32 pixels of a 64x64 target");
    const size_t centre = (static_cast<size_t>(32) * 64 + 32) * 4;
    check(px[centre] >= 126 && px[centre] <= 130 && px[centre + 1] == px[centre] && px[centre + 2] == px[centre],
          "self-test: the transcribed pixel shader and the additive blend give (0.5, 0.5, 0.5) at the centre");
    // additive: the same sprite twice is (1, 1, 1) and still 1024 pixels
    Pass two;
    two.draws.push_back({instanceBuffer(r, {one(0.0f, 0.0f, 0.5f), one(0.0f, 0.0f, 0.5f)}), 2});
    clearAll(r);
    setup(r, two);
    issue(r, two);
    px = readbackRgba8(r);
    check(covered(px) == 1024u && px[centre] >= 253, "self-test: two overlapping instances add (the blend accumulates), covering the same 1024 pixels");
    // instancing: two apart is twice the pixels
    Pass apart;
    apart.draws.push_back({instanceBuffer(r, {one(-0.5f, 0.0f, 0.25f), one(0.5f, 0.0f, 0.25f)}), 2});
    clearAll(r);
    setup(r, apart);
    issue(r, apart);
    check(covered(readbackRgba8(r)) == 2u * 16u * 16u, "self-test: two instances apart cover two sprites' pixels");
    // two draws of the pair in one pass, as a display's two
    Pass draws;
    draws.draws.push_back({instanceBuffer(r, {one(-0.5f, 0.0f, 0.25f)}), 1});
    draws.draws.push_back({instanceBuffer(r, {one(0.5f, 0.0f, 0.25f)}), 1});
    clearAll(r);
    setup(r, draws);
    issue(r, draws);
    check(covered(readbackRgba8(r)) == 2u * 16u * 16u, "self-test: two draws in a pass both land");
    // the scissor: the top half of the target only
    Pass sc;
    sc.draws.push_back({instanceBuffer(r, {one(0.0f, 0.0f, 0.5f)}), 1});
    sc.scissor = true;
    sc.rect = {0, 0, 64, 32};
    clearAll(r);
    setup(r, sc);
    issue(r, sc);
    check(covered(readbackRgba8(r)) == 32u * 16u, "self-test: a scissor to the top half keeps exactly the top half of the sprite");
    // the sprite scale collapses the sprites (the vertex work alone)
    Pass collapsed;
    collapsed.draws.push_back({instanceBuffer(r, {one(0.0f, 0.0f, 0.5f)}), 1});
    collapsed.spriteScale = 0.0f;
    clearAll(r);
    setup(r, collapsed);
    issue(r, collapsed);
    check(covered(readbackRgba8(r)) == 0, "self-test: a sprite scale of zero covers nothing, so the vertex work can be timed alone");
    // the size scaling the bench rests on: the same NDC sprite at twice the size covers four times the pixels
    Rig big;
    makeRig(d, big, 128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, false, 0xFF808080u, 0xFFFFFFFFu);
    Pass bigPass;
    bigPass.draws.push_back({instanceBuffer(big, {one(0.0f, 0.0f, 0.5f)}), 1});
    clearAll(big);
    setup(big, bigPass);
    issue(big, bigPass);
    check(covered(readbackRgba8(big)) == 4u * 32u * 32u, "self-test: the same sprite in a target twice as wide and tall covers four times the pixels");
    // the generator: n instances, in the display's region unless a fraction is placed outside it
    const std::vector<Instance> in = makeInstances(3392, 0.0f, 0.01f);
    bool inside = in.size() == 3392;
    for (const Instance& s : in)
        inside = inside && std::fabs(s.cx - kDisplayCx) <= kDisplayRx + 1e-4f && std::fabs(s.cy - kDisplayCy) <= kDisplayRy + 1e-4f;
    check(inside, "self-test: the generator puts every instance inside the display's region");
    const std::vector<Instance> leak = makeInstances(3392, 0.5f, 0.01f);
    unsigned outside = 0;
    for (const Instance& s : leak) outside += std::fabs(s.cx - kDisplayCx) > kDisplayRx + 1e-4f || std::fabs(s.cy - kDisplayCy) > kDisplayRy + 1e-4f;
    check(outside > 1500 && outside < 1900, "self-test: half the instances placed outside are outside");
    // the timer works on this device and returns a number
    check(timePass(r, p, 2, 3) > 0.0, "self-test: the timestamp pair returns a time");
    std::printf("PASS: %u pair price bench checks (WARP: correctness only, no timing is claimed)\n", g_checks);
}

// ------------------------------------------------------------------------------------------------ bench --

struct Size {
    const char* name;
    UINT w, h;
    DXGI_FORMAT fmt;
};

// One pass of the pad's two draws at a size: 3392 and 94 instances, sprites of half-size `half` (NDC).
struct Measured {
    double ms = 0, mpx = 0;
};
Measured measure(Device& d, const Size& s, float half, float outside, bool scissor, float scissorMargin, int batch, int rounds, Rig* reuse = nullptr) {
    Rig local;
    Rig& r = reuse ? *reuse : local;
    if (!reuse) makeRig(d, r, s.w, s.h, s.fmt, true);
    Pass p;
    p.draws.push_back({instanceBuffer(r, makeInstances(3392, outside, half)), 3392});
    p.draws.push_back({instanceBuffer(r, makeInstances(94, outside, half)), 94});
    if (scissor) {
        const float mx = kDisplayRx + scissorMargin, my = kDisplayRy + scissorMargin;
        auto px = [&](float ndc, UINT extent, bool flip) {
            const float u = flip ? (1.0f - (ndc * 0.5f + 0.5f)) : (ndc * 0.5f + 0.5f);
            return static_cast<LONG>(std::max(0.0f, std::min(1.0f, u)) * static_cast<float>(extent));
        };
        p.scissor = true;
        p.rect.left = px(kDisplayCx - mx, s.w, false);
        p.rect.right = px(kDisplayCx + mx, s.w, false);
        p.rect.top = px(kDisplayCy + my, s.h, true);
        p.rect.bottom = px(kDisplayCy - my, s.h, true);
    }
    Measured m;
    m.ms = timePass(r, p, batch, rounds);
    m.mpx = shadedMpx(r, p);
    return m;
}

void bench(Device& d, double gameMsPerDraw) {
    std::wprintf(L"adapter: %s\n", d.name.c_str());
    const Size game{"game eye 2016x1948, R11G11B10 (flight 132352)", 2016, 1948, DXGI_FORMAT_R11G11B10_FLOAT};
    const Size game2{"game eye 2620x2532, R11G11B10 (capture 143837)", 2620, 2532, DXGI_FORMAT_R11G11B10_FLOAT};
    const Size ui100{"UI layer 100, 4032x3896 RGBA16F + D32S8", 4032, 3896, DXGI_FORMAT_R16G16B16A16_FLOAT};
    const Size ui125{"UI layer 125, 5040x4870 RGBA16F + D32S8", 5040, 4870, DXGI_FORMAT_R16G16B16A16_FLOAT};
    const int batch = 6, rounds = 21;
    // 1. the size-independent part: the sprites collapsed to nothing (all the vertex work, no pixel work)
    {
        Rig r;
        makeRig(d, r, game.w, game.h, game.fmt, true);
        Pass p;
        p.draws.push_back({instanceBuffer(r, makeInstances(3392, 0, 0.01f)), 3392});
        p.draws.push_back({instanceBuffer(r, makeInstances(94, 0, 0.01f)), 94});
        p.spriteScale = 0.0f;
        std::printf("vertex work alone (3392 + 94 instances, sprites collapsed): %.4f ms for the pair of draws\n", timePass(r, p, batch, rounds));
    }
    // 2. calibrate the sprite half-size (NDC) so the game-size pair of draws costs what the census measured
    const double target = gameMsPerDraw * 2.0;
    double lo = 0.0005, hi = 0.08, half = 0.01;
    Rig gameRig;
    makeRig(d, gameRig, game.w, game.h, game.fmt, true);
    for (int i = 0; i < 14; ++i) {
        half = std::sqrt(lo * hi);
        const double t = measure(d, game, static_cast<float>(half), 0, false, 0, batch, 11, &gameRig).ms;
        if (t < target) lo = half; else hi = half;
    }
    half = std::sqrt(lo * hi);
    std::printf("calibrated: sprites of half-size %.4f of the eye (%.0f px at 2016 wide) make the game-size pair of draws cost %.3f ms (census: %.3f ms a draw, two draws)\n",
                half, half * 1008.0, target, gameMsPerDraw);
    std::printf("\n%-52s %10s %10s %12s\n", "ms for the pair of draws, one eye", "ms", "Mpx shaded", "vs game");
    Measured g = measure(d, game, static_cast<float>(half), 0, false, 0, batch, rounds, &gameRig);
    auto row = [&](const char* name, const Measured& m) {
        std::printf("%-52s %10.4f %10.2f %11.2fx\n", name, m.ms, m.mpx, g.ms > 0 ? m.ms / g.ms : 0.0);
    };
    row("game frame, 2016x1948 (the price now)", g);
    row("game frame, 2620x2532 (capture size)", measure(d, game2, static_cast<float>(half), 0, false, 0, batch, rounds));
    const Measured l100 = measure(d, ui100, static_cast<float>(half), 0, false, 0, batch, rounds);
    const Measured l125 = measure(d, ui125, static_cast<float>(half), 0, false, 0, batch, rounds);
    row("layer at UI 100, full route (also route (b))", l100);
    row("layer at UI 125, full route", l125);
    std::printf("  flight 132352 measured 0.36 ms a draw redirected at UI 125: %.2f ms for the pair of draws; the bench says %.2f\n", 0.36 * 2, l125.ms);
    // 3. route (a): a scissor to the display's box, with the sprites all inside it and with a leak past it
    std::printf("\nroute (a), a scissor to the display's box (+0.02 NDC margin) at UI 125, the sprites in the box or not:\n");
    for (float leak : {0.0f, 0.10f, 0.25f, 0.50f}) {
        const Measured full = measure(d, ui125, static_cast<float>(half), leak, false, 0, batch, rounds);
        const Measured cut = measure(d, ui125, static_cast<float>(half), leak, true, 0.02f, batch, rounds);
        std::printf("  %3.0f%% of the instances outside the box: %.4f ms unscissored, %.4f ms scissored, saves %5.1f%%\n", leak * 100.0f, full.ms, cut.ms,
                    full.ms > 0 ? 100.0 * (full.ms - cut.ms) / full.ms : 0.0);
    }
    // 4. how the layer price follows the game price, over what the flights measured (0.04 to 0.115 ms a draw)
    std::printf("\nthe layer's price for the game-size price the census read in different scenes (0.04-0.115 ms a draw):\n");
    for (double gd : {0.040, 0.077, 0.115}) {
        double a = 0.0005, b = 0.08, h2 = 0.01;
        for (int i = 0; i < 12; ++i) {
            h2 = std::sqrt(a * b);
            const double t = measure(d, game, static_cast<float>(h2), 0, false, 0, batch, 11, &gameRig).ms;
            if (t < gd * 2.0) a = h2; else b = h2;
        }
        h2 = std::sqrt(a * b);
        const Measured g2 = measure(d, game, static_cast<float>(h2), 0, false, 0, batch, rounds, &gameRig);
        const Measured a100 = measure(d, ui100, static_cast<float>(h2), 0, false, 0, batch, rounds);
        const Measured a125 = measure(d, ui125, static_cast<float>(h2), 0, false, 0, batch, rounds);
        std::printf("  game %.3f ms a draw (pair %.3f): layer UI 100 %.3f, UI 125 %.3f ms for the pair of draws; x2 eyes: game %.2f, UI 100 %.2f, UI 125 %.2f ms a frame\n",
                    gd, g2.ms, a100.ms, a125.ms, g2.ms * 2, a100.ms * 2, a125.ms * 2);
    }
    // 5. how much the answer depends on the layout the sprites are given: the display's region 0.5, 1, 2 and 4 times as wide
    //    and tall, each calibrated to the same game-size price. If the ratios held whatever the layout, the layer price
    //    follows from the game price whatever the real instance data is.
    std::printf("\nlayout sensitivity: the sprites laid in a region 0.5x to 4x the display's, each calibrated to the game-size price (%.3f ms the pair):\n", target);
    std::printf("  %-8s %10s %10s %10s %10s %10s %10s\n", "region", "half-size", "game Mpx", "UI 100 ms", "UI 125 ms", "125/game", "100/125");
    for (float scale : {0.5f, 1.0f, 2.0f, 4.0f}) {
        g_regionScale = scale;
        double a = 0.0005, b = 0.12, h3 = 0.01;
        for (int i = 0; i < 13; ++i) {
            h3 = std::sqrt(a * b);
            const double t = measure(d, game, static_cast<float>(h3), 0, false, 0, batch, 11, &gameRig).ms;
            if (t < target) a = h3; else b = h3;
        }
        h3 = std::sqrt(a * b);
        const Measured g3 = measure(d, game, static_cast<float>(h3), 0, false, 0, batch, rounds, &gameRig);
        const Measured a100 = measure(d, ui100, static_cast<float>(h3), 0, false, 0, batch, rounds);
        const Measured a125 = measure(d, ui125, static_cast<float>(h3), 0, false, 0, batch, rounds);
        std::printf("  %-8.1f %10.4f %10.2f %10.4f %10.4f %9.2fx %9.2fx   (game %.3f)\n", scale, h3, g3.mpx, a100.ms, a125.ms,
                    g3.ms > 0 ? a125.ms / g3.ms : 0.0, a125.ms > 0 ? a100.ms / a125.ms : 0.0, g3.ms);
    }
    g_regionScale = 1.0f;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool nvidia = false, benchMode = false, self = false, dry = false;
    double gameMs = 0.077;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) self = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--bench")) benchMode = true;
        else if (!std::strcmp(argv[i], "--adapter") && i + 1 < argc && !std::strcmp(argv[i + 1], "nvidia")) { nvidia = true; ++i; }
        else if (!std::strcmp(argv[i], "--game-ms") && i + 1 < argc) { gameMs = std::atof(argv[++i]); }
        else { std::fputs("usage: pair_price_bench --self-test | --dry-run | --bench [--adapter nvidia] [--game-ms MS]\n", stderr); return 2; }
    }
    if (dry) {
        std::puts("dry-run: no device, no draws, no files");
        return 0;
    }
    if (self) {
        selfTest();
        return 0;
    }
    if (benchMode) {
        Device d = makeDevice(nvidia);
        if (d.warp) std::puts("note: --bench on WARP times a CPU; use --adapter nvidia.");
        bench(d, gameMs);
        return 0;
    }
    std::fputs("usage: pair_price_bench --self-test | --dry-run | --bench [--adapter nvidia] [--game-ms MS]\n", stderr);
    return 2;
}

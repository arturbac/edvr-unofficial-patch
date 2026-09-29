#include "temporal_shader_bytecode.h"
#include "fss_theater.h"
#include "graphics_runtime.h"

#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include "../common/guard.h"
#include "../common/log.h"
#include "shader_swap.h"

namespace edvr {
namespace {

// Per output pixel: build this eye's view ray from the frustum tangents,
// rotate it into the frozen-head frame (the head's look-around since the
// zoom began), intersect the panel plane at -Z*dist, sample the captured
// image inside the panel or return black. The surround was black in the
// game's own render, so the panel's edges land invisibly at first and
// only head-look reveals the screen.


DXGI_FORMAT theaterTypedOf(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:                                return f;
    }
}

ID3D11ComputeShader*       g_cs = nullptr;
bool                       g_csTried = false;
ID3D11Texture2D*           g_out[2] = {};
ID3D11UnorderedAccessView* g_outUav[2] = {};
// The copy-through pairs (one per content eye): when a submitted texture
// refuses a shader view (the series capture met the same refusal), the
// content is copied into an own samplable texture instead.
ID3D11Texture2D*           g_copy[2] = {};
ID3D11ShaderResourceView*  g_copySrv[2] = {};
uint32_t g_outW = 0, g_outH = 0;
ID3D11Buffer*       g_cb = nullptr;
ID3D11SamplerState* g_samp = nullptr;
uint64_t g_frames = 0;
bool     g_engagedNoted = false;
bool     g_failNoted = false;

FaultBudget g_budget("fssTheater", 8);

void failOnce(const char* what) {
    if (!g_failNoted) {
        g_failNoted = true;
        Log::get().note("fss theater: %s; the eyes submit stock.", what);
    }
}

// One content texture -> a shader view, direct or through the slot's
// copy. Returns null when neither path serves.
ID3D11ShaderResourceView* contentView(ID3D11DeviceContext* ctx,
                                      ID3D11Device* dev,
                                      ID3D11Texture2D* ct, int slot) {
    D3D11_TEXTURE2D_DESC cd{};
    ct->GetDesc(&cd);
    const DXGI_FORMAT typed = theaterTypedOf(cd.Format);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = typed;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    if ((cd.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
        SUCCEEDED(dev->CreateShaderResourceView(ct, &vd, &srv))) {
        return srv;
    }
    if (!g_copy[slot]) {
        D3D11_TEXTURE2D_DESC pd = cd;
        pd.Format = typed;
        pd.Usage = D3D11_USAGE_DEFAULT;
        pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        pd.CPUAccessFlags = 0;
        pd.MiscFlags = 0;
        pd.MipLevels = 1;
        pd.SampleDesc.Count = 1;
        pd.SampleDesc.Quality = 0;
        if (FAILED(dev->CreateTexture2D(&pd, nullptr, &g_copy[slot])) ||
            FAILED(dev->CreateShaderResourceView(g_copy[slot], &vd,
                                                 &g_copySrv[slot]))) {
            if (g_copy[slot]) {
                g_copy[slot]->Release();
                g_copy[slot] = nullptr;
            }
            g_copySrv[slot] = nullptr;
            return nullptr;
        }
    }
    if (cd.SampleDesc.Count > 1) {
        ctx->ResolveSubresource(g_copy[slot], 0, ct, 0, typed);
    } else {
        ctx->CopySubresourceRegion(g_copy[slot], 0, 0, 0, 0, ct, 0,
                                   nullptr);
    }
    g_copySrv[slot]->AddRef();
    return g_copySrv[slot];
}

void* theaterInner(void* contentTex, void* contentTexR, int eye,
                   float outerMag, float innerMag,
                   const float* xf, float dist, float scale, float curve,
                   float aspect, const float* rect) {
    ID3D11Texture2D* ct = nullptr;
    static_cast<IUnknown*>(contentTex)
        ->QueryInterface(__uuidof(ID3D11Texture2D),
                         reinterpret_cast<void**>(&ct));
    if (!ct) return nullptr;
    D3D11_TEXTURE2D_DESC cd{};
    ct->GetDesc(&cd);

    ID3D11Device* dev = nullptr;
    ct->GetDevice(&dev);
    if (!dev) {
        ct->Release();
        return nullptr;
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) {
        dev->Release();
        ct->Release();
        return nullptr;
    }

    bool ok = true;
    const DXGI_FORMAT typed = theaterTypedOf(cd.Format);

    if (!g_out[0] || g_outW != cd.Width || g_outH != cd.Height) {
        for (int e = 0; e < 2; ++e) {
            if (g_outUav[e]) { g_outUav[e]->Release(); g_outUav[e] = nullptr; }
            if (g_out[e]) { g_out[e]->Release(); g_out[e] = nullptr; }
        }
        for (int e2 = 0; e2 < 2; ++e2) {
            if (g_copySrv[e2]) {
                g_copySrv[e2]->Release();
                g_copySrv[e2] = nullptr;
            }
            if (g_copy[e2]) {
                g_copy[e2]->Release();
                g_copy[e2] = nullptr;
            }
        }
        for (int e = 0; e < 2 && ok; ++e) {
            D3D11_TEXTURE2D_DESC od = cd;
            od.Format = typed;
            od.Usage = D3D11_USAGE_DEFAULT;
            od.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                           D3D11_BIND_UNORDERED_ACCESS;
            od.CPUAccessFlags = 0;
            od.MiscFlags = 0;
            od.MipLevels = 1;
            ok = SUCCEEDED(dev->CreateTexture2D(&od, nullptr, &g_out[e])) &&
                 SUCCEEDED(dev->CreateUnorderedAccessView(g_out[e], nullptr,
                                                          &g_outUav[e]));
        }
        if (!ok) failOnce("the output textures could not be created");
        g_outW = cd.Width;
        g_outH = cd.Height;
    }
    if (ok && !g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 144;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
        if (!ok) failOnce("the constant buffer could not be created");
    }
    if (ok && !g_samp) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(dev->CreateSamplerState(&sd, &g_samp));
        if (!ok) failOnce("the sampler could not be created");
    }
    if (ok && !g_cs && !g_csTried) {
        g_csTried = true;
        g_cs = shaderSwapCreateCs(ctx, kFssTheaterBytecode, sizeof(kFssTheaterBytecode), "fss_theater_cs", "fss theater");
    }
    ok = ok && g_cs != nullptr;

    ID3D11ShaderResourceView* cs = nullptr;
    ID3D11ShaderResourceView* cs2 = nullptr;
    if (ok) {
        cs = contentView(ctx, dev, ct, 0);
        ok = cs != nullptr;
        if (!ok) failOnce("the content texture refuses a shader view");
    }
    if (ok && contentTexR && rect) {
        // The right eye, when supplied and a stitch rect exists. Its
        // failure is not the theater's: the left eye alone still draws.
        ID3D11Texture2D* ctR = nullptr;
        static_cast<IUnknown*>(contentTexR)
            ->QueryInterface(__uuidof(ID3D11Texture2D),
                             reinterpret_cast<void**>(&ctR));
        if (ctR) {
            cs2 = contentView(ctx, dev, ctR, 1);
            ctR->Release();
        }
    }

    if (ok) {
        // The eye's frustum: the OUTER tangent sits temporal (left edge of
        // the left eye, right edge of the right), the INNER nasal. The
        // panel matches the content's own frustum span so the image
        // appears at its native angular size and the swap-in is seamless.
        const float lt = eye == 0 ? outerMag : innerMag;
        const float rt = eye == 0 ? innerMag : outerMag;
        const float vt =
            (outerMag + innerMag) * 0.5f *
            (static_cast<float>(cd.Height) / static_cast<float>(cd.Width));
        // The screen's half-extents: the content's native angular span
        // at the chosen distance, times the user's scale. The eye capture
        // is nearly SQUARE (the VR frustum is), which the field read as
        // "more vertical than horizontal" -- so the aspect knob makes the
        // screen widescreen and centre-crops the content's height to
        // match, undistorted: full width kept, the top and bottom of the
        // square frustum (mostly void) trimmed. band is the fraction of
        // the texture's height shown; aspect 0 = the native square.
        const float halfW = dist * (outerMag + innerMag) * 0.5f * scale;
        const float nativeHalfH =
            halfW * (static_cast<float>(cd.Height) /
                     static_cast<float>(cd.Width));
        float halfH = nativeHalfH;
        float band = 1.0f;
        if (aspect > 0.01f) {
            halfH = halfW / aspect;
            if (halfH > nativeHalfH) halfH = nativeHalfH;
            band = halfH / nativeHalfH;
        }
        // The square->quad homography (Heckbert's closed form) from unit
        // screen coordinates (TL origin, y down) to the derived corner
        // UVs. All-zero when no quad was supplied.
        float hA[4] = {}, hB[4] = {}, hA2[4] = {}, hB2[4] = {};
        auto homography = [](const float* c8, float* a, float* b) {
            const float x0 = c8[0], y0 = c8[1];   // TL
            const float x1 = c8[2], y1 = c8[3];   // TR
            const float x2 = c8[4], y2 = c8[5];   // BR
            const float x3 = c8[6], y3 = c8[7];   // BL
            const float sx = x0 - x1 + x2 - x3;
            const float sy = y0 - y1 + y2 - y3;
            float g = 0.0f, h = 0.0f;
            const float dx1 = x1 - x2, dx2 = x3 - x2;
            const float dy1 = y1 - y2, dy2 = y3 - y2;
            const float den = dx1 * dy2 - dx2 * dy1;
            if (den != 0.0f && (sx != 0.0f || sy != 0.0f)) {
                g = (sx * dy2 - sy * dx2) / den;
                h = (dx1 * sy - dy1 * sx) / den;
            }
            a[0] = x1 - x0 + g * x1;
            a[1] = x3 - x0 + h * x3;
            a[2] = x0;
            a[3] = y1 - y0 + g * y1;
            b[0] = y3 - y0 + h * y3;
            b[1] = y0;
            b[2] = g;
            b[3] = h;
        };
        if (rect) {
            homography(rect, hA, hB);
            // The right eye's set: all-zero corners mean unavailable, and
            // the stitch needs a view to sample.
            bool haveR = cs2 != nullptr;
            for (int i = 8; i < 16 && haveR; ++i) {
                if (rect[i] != 0.0f) break;
                if (i == 15) haveR = false;
            }
            if (haveR) homography(rect + 8, hA2, hB2);
        }
        float cbData[36] = {
            lt, rt, vt, dist,
            xf[0], xf[1], xf[2], xf[9],
            xf[3], xf[4], xf[5], xf[10],
            xf[6], xf[7], xf[8], xf[11],
            band, halfW, halfH, curve,
            hA[0], hA[1], hA[2], hA[3],
            hB[0], hB[1], hB[2], hB[3],
            hA2[0], hA2[1], hA2[2], hA2[3],
            hB2[0], hB2[1], hB2[2], hB2[3],
        };
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) &&
            m.pData) {
            memcpy(m.pData, cbData, sizeof(cbData));
            ctx->Unmap(g_cb, 0);
        } else {
            ok = false;
        }
    }

    void* result = nullptr;
    if (ok) {
        ID3D11ComputeShader* savedCs = nullptr;
        ID3D11ShaderResourceView* savedSrv[2] = {};
        ID3D11UnorderedAccessView* savedUav = nullptr;
        ID3D11Buffer* savedCb = nullptr;
        ID3D11SamplerState* savedSamp = nullptr;
        ctx->CSGetShader(&savedCs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 2, savedSrv);
        ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
        ctx->CSGetConstantBuffers(0, 1, &savedCb);
        ctx->CSGetSamplers(0, 1, &savedSamp);

        UINT keep = 0;
        ID3D11ShaderResourceView* setSrv[2] = {cs, cs2 ? cs2 : cs};
        ctx->CSSetShader(g_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 2, setSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_outUav[eye], &keep);
        ctx->CSSetConstantBuffers(0, 1, &g_cb);
        ctx->CSSetSamplers(0, 1, &g_samp);
        ctx->Dispatch((g_outW + 15) / 16, (g_outH + 15) / 16, 1);

        ID3D11ShaderResourceView* nullSrv[2] = {};
        ID3D11UnorderedAccessView* nullUav = nullptr;
        ctx->CSSetShaderResources(0, 2, nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
        ctx->CSSetShader(savedCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 2, savedSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, &keep);
        ctx->CSSetConstantBuffers(0, 1, &savedCb);
        ctx->CSSetSamplers(0, 1, &savedSamp);
        if (savedCs) savedCs->Release();
        for (int s2i = 0; s2i < 2; ++s2i) {
            if (savedSrv[s2i]) savedSrv[s2i]->Release();
        }
        if (savedUav) savedUav->Release();
        if (savedCb) savedCb->Release();
        if (savedSamp) savedSamp->Release();

        ++g_frames;
        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                "fss theater: engaged -- the zoomed scanner is a single "
                "rendering shown to both eyes as a screen at %.1f m "
                "(scale %.2f, curve %.2f, aspect %.2f). One render, two "
                "displays: no per-eye artifact can exist.",
                static_cast<double>(dist), static_cast<double>(scale),
                static_cast<double>(curve),
                static_cast<double>(aspect));
        }
        result = g_out[eye];
    }

    if (cs) cs->Release();
    if (cs2) cs2->Release();
    ctx->Release();
    dev->Release();
    ct->Release();
    return result;
}

}  // namespace

void fssTheaterWarm(ID3D11DeviceContext* ctx) {
    if (g_cs || g_csTried) return;
    g_csTried = true;
    g_cs = shaderSwapCreateCs(ctx, kFssTheaterBytecode, sizeof(kFssTheaterBytecode), "fss_theater_cs", "fss theater");
    if (g_cs) {
        Log::get().note(
            "fss theater: shader warmed at session start -- the first "
            "engage pays no compile.");
    }
}

}  // namespace edvr

extern "C" __declspec(dllexport) void* edvrFssTheater(void* contentTex,
                                                      void* contentTexR,
                                                      int eye,
                                                      float outerMag,
                                                      float innerMag,
                                                      const float* xf,
                                                      float dist,
                                                      float scale,
                                                      float curve,
                                                      float aspect,
                                                      const float* rect) {
    if (edvr::graphicsRuntimeDisabled() || !contentTex || !xf || eye < 0 || eye > 1) return nullptr;
    void* out = nullptr;
    edvr::guardedBudget(edvr::g_budget, [&] {
        out = edvr::theaterInner(contentTex, contentTexR, eye, outerMag,
                                 innerMag, xf, dist, scale, curve, aspect,
                                 rect);
    });
    return out;
}

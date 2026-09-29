// Output-identity rig for the UI resolve (src/d3d11/ui_resolve.h), on WARP.
//
// The UI resolve is the post-DLSS pass that bounds NVIDIA's output against the
// frame's own raster wherever the interface is (and, at fix.temporal_aa's
// corona level, holds faint flat glow anywhere). A change to it that is only
// about WHERE the data comes from -- a groupshared window instead of point
// taps, an R8 history instead of an RGBA8 one whose only .a is read -- must
// leave every output byte and every history byte exactly where they were. This
// rig is that proof, three ways:
//
//   goldens   FNV-1a 64 hashes of the output texture and the history the
//             production shader wrote for fixed fixtures, recorded from the
//             UNMODIFIED shader (--print-goldens) before the change existed.
//             They are the "before". To re-record after a legitimate change,
//             check out the commit before it and run --print-goldens; never
//             paste this build's output over a failure.
//   fuzz      (added with the change) the same random inputs through a frozen
//             copy of the old shader and the production bytecode, compared
//             byte for byte.
//   control   (added with the change) a deliberately short window must FAIL
//             the same comparison, so a green run means the fixtures reach the
//             window's edges.
//
// Runs on WARP by default (the build gate). --adapter nvidia runs the same
// checks on the RTX (a desk check, never part of the build).
//
//   ui_holo_pass_test --self-test | --dry-run | --print-goldens
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
#include <string>
#include <vector>

#include "../../src/d3d11/ui_resolve.h"
#include "temporal_shader_bytecode.h"  // edvr::kUiResolveBytecode, from build\gen

using Microsoft::WRL::ComPtr;

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}
void hr(HRESULT r, const char* what) {
    if (FAILED(r)) {
        std::printf("FAIL: %s (0x%08lX)\n", what, static_cast<unsigned long>(r));
        std::exit(1);
    }
}

// ------------------------------------------------------------------ inputs

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int range(int n) { return static_cast<int>(next() % static_cast<uint32_t>(n)); }
};

uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t mant = x & 0x7FFFFFu;
    const int32_t e8 = static_cast<int32_t>((x >> 23) & 0xFFu);
    if (e8 == 0xFF) return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
    int32_t exp = e8 - 127 + 15;
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        const uint32_t m = mant | 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t half = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1u), mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (half & 1u))) ++half;
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) ++half;
    return static_cast<uint16_t>(sign | half);
}

// One fixture: the sizes, the jitter, b1, and which inputs exist and how they
// are filled. Every fill is a pure function of these fields (and the seed).
struct Fix {
    const char* name;
    uint32_t seed;
    int w, h, ow, oh;   // the frame's raster (input) size and the output size
    float jx, jy;       // the jitter, in input pixels
    float tol, hold;    // b1: the clamp's tolerance and the hold's limit; tol < 0 leaves b1 unbound
    int content;        // 0 faint sky with stars, 1 bands, 2 bright interior, 3 mixed quadrants
    int marks;          // 0 none, 1 corner, 2 tile edges, 3 corona edge, 4 whole frame, 5 screen, 6 mixed
    int hist;           // 0 no history bound, 1 sparse influence, 2 dense influence
    int motion;         // 0 zero, 1 small, 2 large, 3 hostile (NaN, Inf, huge)
    int rx, ry;         // the screen map's offset of the frame's region
    int sx, sy;         // the screen map's extra size beyond the frame; sx < 0: no screen map
};

struct Inputs {
    std::vector<uint8_t> raw, trained, cover, edits, hist;
    std::vector<uint16_t> screen, motion;
    int screenW = 0, screenH = 0;
};

const Fix kFix[] = {
    // name, seed, w,h, ow,oh, jx,jy, tol,hold, content,marks,hist,motion, rx,ry, sx,sy
    {"sky_no_ui_2x", 11, 61, 47, 122, 94, 0.25f, -0.40f, 12 / 255.f, 64 / 255.f, 0, 0, 0, 0, 0, 0, -1, 0},
    {"sky_hold_off_b1_unbound", 12, 61, 47, 122, 94, 0.25f, -0.40f, -1.f, 0.f, 0, 0, 0, 0, 0, 0, -1, 0},
    {"sky_tol_zero_hold_on", 13, 61, 47, 122, 94, -0.10f, 0.30f, 0.f, 64 / 255.f, 0, 0, 0, 0, 0, 0, -1, 0},
    {"corner_ui", 21, 64, 64, 128, 128, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 3, 1, 0, 0, 0, 0, -1, 0},
    {"corner_ui_jit_edge", 22, 64, 64, 128, 128, 0.5f, -0.5f, 12 / 255.f, 64 / 255.f, 3, 1, 0, 0, 0, 0, -1, 0},
    {"tile_edges_ui", 31, 96, 80, 192, 160, -0.3f, 0.2f, 12 / 255.f, 64 / 255.f, 3, 2, 0, 0, 0, 0, -1, 0},
    {"tile_edges_ui_odd", 32, 61, 47, 122, 94, 0.5f, 0.5f, 12 / 255.f, 64 / 255.f, 1, 2, 0, 0, 0, 0, -1, 0},
    {"corona_edge", 41, 64, 64, 128, 128, 0.1f, 0.1f, 12 / 255.f, 64 / 255.f, 0, 3, 0, 0, 0, 0, -1, 0},
    {"corona_edge_15x", 42, 64, 64, 96, 96, -0.2f, 0.4f, 12 / 255.f, 64 / 255.f, 0, 3, 0, 0, 0, 0, -1, 0},
    {"menu_full_frame", 51, 61, 47, 122, 94, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 2, 4, 0, 0, 0, 0, -1, 0},
    {"screen_ui", 52, 61, 47, 122, 94, 0.2f, -0.2f, 12 / 255.f, 64 / 255.f, 3, 5, 0, 0, 3, 2, 6, 4},
    {"screen_mixed_region_max", 53, 64, 48, 128, 96, 0.f, 0.3f, 12 / 255.f, 64 / 255.f, 3, 6, 0, 0, 8, 8, 8, 8},
    {"history_sparse_small_motion", 61, 61, 47, 122, 94, 0.25f, -0.4f, 12 / 255.f, 64 / 255.f, 3, 0, 1, 1, 0, 0, -1, 0},
    {"history_dense_large_motion", 62, 61, 47, 122, 94, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 0, 0, 2, 2, 0, 0, -1, 0},
    {"history_hostile_motion", 63, 61, 47, 122, 94, 0.1f, 0.1f, 12 / 255.f, 64 / 255.f, 3, 0, 2, 3, 0, 0, -1, 0},
    {"history_with_ui", 64, 64, 64, 128, 128, 0.25f, 0.25f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 2, 0, 0, -1, 0},
    {"ratio_1x", 71, 33, 17, 33, 17, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 3, 1, 1, 1, 0, 0, -1, 0},
    {"ratio_133", 72, 33, 17, 44, 23, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 3, 1, 1, 1, 0, 0, -1, 0},
    {"ratio_25", 73, 33, 17, 83, 43, 0.25f, 0.25f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 1, 0, 0, -1, 0},
    {"ratio_075_down", 74, 33, 17, 25, 13, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 3, 1, 1, 1, 0, 0, -1, 0},
    {"jitter_1_3", 81, 64, 64, 128, 128, 1.3f, -1.3f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 1, 0, 0, -1, 0},
    {"jitter_0_9", 82, 64, 64, 128, 128, 0.9f, 0.9f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 1, 0, 0, -1, 0},
    {"jitter_neg_1_7", 83, 64, 64, 128, 128, -1.7f, 0.2f, 12 / 255.f, 64 / 255.f, 0, 3, 1, 1, 0, 0, -1, 0},
    {"tiny_7x5", 91, 7, 5, 14, 10, 0.f, 0.f, 12 / 255.f, 64 / 255.f, 3, 1, 1, 1, 0, 0, -1, 0},
    {"wide_130x9", 92, 130, 9, 260, 18, 0.25f, 0.f, 12 / 255.f, 64 / 255.f, 3, 2, 1, 1, 0, 0, -1, 0},
    {"tall_9x130", 93, 9, 130, 18, 260, 0.f, -0.25f, 12 / 255.f, 64 / 255.f, 3, 2, 1, 1, 0, 0, -1, 0},
    {"eye_left", 101, 80, 72, 160, 144, 0.25f, -0.4f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 1, 0, 0, -1, 0},
    {"eye_right", 102, 80, 72, 160, 144, -0.25f, 0.4f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 1, 0, 0, -1, 0},
    {"production_shape_eighth", 111, 252, 244, 504, 488, 0.1f, -0.1f, 12 / 255.f, 64 / 255.f, 3, 6, 1, 1, 0, 0, -1, 0},
};
constexpr size_t kFixtures = sizeof(kFix) / sizeof(kFix[0]);

// The faint-sky, star and band content, all integer arithmetic.
void fillRaw(const Fix& f, Rng& rng, std::vector<uint8_t>& raw) {
    raw.assign(static_cast<size_t>(f.w) * f.h * 4, 255);
    for (int y = 0; y < f.h; ++y) {
        for (int x = 0; x < f.w; ++x) {
            int style = f.content;
            if (style == 3) style = ((x >= f.w / 2) ? 1 : 0) + ((y >= f.h / 2) ? 2 : 0);   // quadrants 0,1,2,3
            uint8_t* p = &raw[(static_cast<size_t>(y) * f.w + x) * 4];
            for (int c = 0; c < 3; ++c) {
                int v;
                switch (style) {
                    case 0: v = 1 + rng.range(4); break;                                   // faint sky
                    case 1: v = ((x * 3 + y * 2 + c * 5) / 4) % 80; break;               // slow bands, faint and not
                    case 2: v = 60 + rng.range(196); break;                                // bright interior
                    default: v = (x < f.w / 4) ? 1 + rng.range(3) : 20 + rng.range(60); break;
                }
                p[c] = static_cast<uint8_t>(v);
            }
        }
    }
    // Stars: a hot core with a halo that falls off through the faint range,
    // so the hold's brightness limit and flatness edge both cross real pixels.
    const int stars = std::max(1, f.w * f.h / 900);
    for (int s = 0; s <= stars; ++s) {
        const int cx = s == 0 ? f.w / 2 : rng.range(f.w), cy = s == 0 ? f.h / 2 : rng.range(f.h);
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= f.w || y >= f.h) continue;
                const int v = 255 - 40 * (dx * dx + dy * dy);
                if (v <= 0) continue;
                uint8_t* p = &raw[(static_cast<size_t>(y) * f.w + x) * 4];
                for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(std::max<int>(p[c], v - c));
            }
        }
    }
}

void fillTrained(const Fix& f, Rng& rng, const std::vector<uint8_t>& raw, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(f.ow) * f.oh * 4, 0);
    for (int y = 0; y < f.oh; ++y) {
        for (int x = 0; x < f.ow; ++x) {
            const int qx = std::min(f.w - 1, x * f.w / f.ow), qy = std::min(f.h - 1, y * f.h / f.oh);
            const uint8_t* s = &raw[(static_cast<size_t>(qy) * f.w + qx) * 4];
            uint8_t* d = &out[(static_cast<size_t>(y) * f.ow + x) * 4];
            const bool big = rng.range(16) == 0;
            for (int c = 0; c < 3; ++c) {
                int v = s[c] + (rng.range(9) - 3) + (big ? (rng.range(2) ? 20 + rng.range(40) : -(20 + rng.range(40))) : 0);
                d[c] = static_cast<uint8_t>(std::min(255, std::max(0, v)));
            }
            d[3] = (rng.next() & 1) ? 255 : 127;
        }
    }
}

void fillMarks(const Fix& f, Rng& rng, Inputs& in) {
    const bool wantCover = f.marks != 0, wantEdits = f.marks != 0;
    if (wantCover) in.cover.assign(static_cast<size_t>(f.w) * f.h, 0);
    if (wantEdits) in.edits.assign(static_cast<size_t>(f.w) * f.h, 0);
    auto cov = [&](int x, int y, uint8_t v) {
        if (x >= 0 && y >= 0 && x < f.w && y < f.h && !in.cover.empty()) in.cover[static_cast<size_t>(y) * f.w + x] = v;
    };
    auto edi = [&](int x, int y, uint8_t v) {
        if (x >= 0 && y >= 0 && x < f.w && y < f.h && !in.edits.empty()) in.edits[static_cast<size_t>(y) * f.w + x] = v;
    };
    auto scr = [&](int x, int y, int w) {
        if (in.screen.empty()) return;
        const int sx = f.rx + x, sy = f.ry + y;
        if (sx < 0 || sy < 0 || sx >= in.screenW || sy >= in.screenH) return;
        in.screen[(static_cast<size_t>(sy) * in.screenW + sx) * 4 + 3] = toHalf(static_cast<float>(w));
    };
    const int m = f.marks;
    if (m == 1 || m == 6) {
        for (int y = 0; y <= 4; ++y) for (int x = 0; x <= 5; ++x) cov(x, y, (x + y) & 1 ? 1 : 2);
        for (int y = 1; y <= 3; ++y) for (int x = 1; x <= 3; ++x) edi(x, y, 255);
        cov(10, 10, 3);                 // smoke: not marked
        cov(11, 10, 5);                 // floating (k=1) with high bits set
        cov(12, 10, 10);                // attached (k=2) with high bits set
        cov(13, 10, 4);                 // k=0
        edi(9, 9, 1);                   // the smallest edit
        cov(f.w - 1, f.h - 1, 1);       // the far corner
        edi(f.w - 2, f.h - 1, 128);
    }
    if (m == 2 || m == 6) {
        for (int y = 3; y <= 20; ++y) { cov(7, y, 1); cov(8, y, 2); }
        for (int x = 20; x <= 40; ++x) { cov(x, 15, 1); cov(x, 16, 1); }
        for (int y = 0; y <= 30; ++y) edi(23, y, 255);
        for (int x = 0; x < f.w; ++x) edi(x, 31, 64);
        for (int x = 15; x <= 17; ++x) cov(x, f.h / 2, 2);
    }
    if (m == 3) {
        cov(f.w / 2 + 5, f.h / 2, 1);
        cov(f.w / 2 + 6, f.h / 2 + 1, 2);
        edi(f.w / 2 - 5, f.h / 2 + 1, 255);
        cov(f.w / 2, f.h / 2 - 6, 1);
    }
    if (m == 4) {
        for (int y = 0; y < f.h; ++y) for (int x = 0; x < f.w; ++x) {
            cov(x, y, static_cast<uint8_t>((rng.range(5) == 0) ? 3 : (1 + (rng.range(2)) + 4 * rng.range(4))));
            if (rng.range(3) == 0) edi(x, y, static_cast<uint8_t>(1 + rng.range(255)));
        }
    }
    if (m == 5 || m == 6) {
        for (int y = 2; y <= 6; ++y) for (int x = 4; x <= 9; ++x) scr(x, y, 3);
        for (int x = 0; x < f.w; x += 9) scr(x, f.h - 1, 3);
        scr(0, 0, 3);
    }
}

Inputs makeInputs(const Fix& f) {
    Inputs in;
    Rng rng(f.seed * 2654435761u + 12345u);
    fillRaw(f, rng, in.raw);
    fillTrained(f, rng, in.raw, in.trained);
    if (f.sx >= 0) {
        in.screenW = f.w + f.sx;
        in.screenH = f.h + f.sy;
        in.screen.assign(static_cast<size_t>(in.screenW) * in.screenH * 4, 0);
        for (size_t i = 0; i < in.screen.size(); i += 4) {   // colour and motion noise; validity 0..2, never 3 here
            in.screen[i] = toHalf(static_cast<float>(rng.range(100)) / 10.f);
            in.screen[i + 1] = toHalf(static_cast<float>(rng.range(100)) / 10.f);
            in.screen[i + 2] = toHalf(static_cast<float>(rng.range(100)) / 10.f);
            in.screen[i + 3] = toHalf(static_cast<float>(rng.range(3)));
        }
    }
    fillMarks(f, rng, in);
    if (f.hist) {
        in.hist.assign(static_cast<size_t>(f.w) * f.h, 0);
        const int every = f.hist == 1 ? 50 : 3;
        for (auto& v : in.hist) {
            if (rng.range(every) == 0) {
                const int pick = rng.range(6);
                v = pick == 0 ? 255 : pick == 1 ? 1 : static_cast<uint8_t>(1 + rng.range(255));
            }
        }
        // A few exact spikes the transported taps land on.
        if (f.w > 8 && f.h > 8) {
            in.hist[static_cast<size_t>(2) * f.w + 2] = 255;
            in.hist[static_cast<size_t>(f.h - 2) * f.w + (f.w - 2)] = 200;
        }
    }
    in.motion.assign(static_cast<size_t>(f.w) * f.h * 2, 0);
    if (f.motion) {
        for (size_t i = 0; i < in.motion.size(); ++i) {
            float v = 0.f;
            switch (f.motion) {
                case 1: v = (static_cast<float>(rng.range(301)) - 150.f) / 100.f; break;
                case 2: v = (static_cast<float>(rng.range(2801)) - 1400.f) / 100.f; break;
                default: {
                    const int k = rng.range(12);
                    if (k == 0) { in.motion[i] = 0x7E00; continue; }            // NaN
                    if (k == 1) { in.motion[i] = 0x7C00; continue; }            // +Inf
                    if (k == 2) { in.motion[i] = 0xFC00; continue; }            // -Inf
                    if (k == 3) { in.motion[i] = 0x7BFF; continue; }            // 65504
                    v = (static_cast<float>(rng.range(2001)) - 1000.f) / 100.f;
                }
            }
            in.motion[i] = toHalf(v);
        }
    }
    return in;
}

// ------------------------------------------------------------------ the device

struct Device {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11InfoQueue> info;
    bool warp = true;
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
            if (desc.VendorId == 0x10DE) { adapter = a; break; }
        }
        if (!adapter) { std::puts("FAIL: no NVIDIA adapter for --adapter nvidia"); std::exit(1); }
        type = D3D_DRIVER_TYPE_UNKNOWN;
        d.warp = false;
    }
    D3D_FEATURE_LEVEL level{};
    HRESULT made = D3D11CreateDevice(adapter.Get(), type, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                     D3D11_SDK_VERSION, &d.dev, &level, &d.ctx);
    if (made == DXGI_ERROR_SDK_COMPONENT_MISSING)
        made = D3D11CreateDevice(adapter.Get(), type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d.dev, &level, &d.ctx);
    hr(made, "D3D11CreateDevice");
    d.dev.As(&d.info);
    return d;
}

ComPtr<ID3D11ComputeShader> shaderFromBytecode(ID3D11Device* dev, const unsigned char* bytes, size_t n) {
    ComPtr<ID3D11ComputeShader> cs;
    hr(dev->CreateComputeShader(bytes, n, nullptr, &cs), "CreateComputeShader");
    return cs;
}

ComPtr<ID3D11Texture2D> tex(ID3D11Device* dev, int w, int h, DXGI_FORMAT fmt, UINT bind, const void* data, UINT rowBytes) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = fmt;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{data, rowBytes, 0};
    ComPtr<ID3D11Texture2D> t;
    hr(dev->CreateTexture2D(&d, data ? &init : nullptr, &t), "CreateTexture2D");
    return t;
}
ComPtr<ID3D11ShaderResourceView> srvOf(ID3D11Device* dev, ID3D11Texture2D* t) {
    ComPtr<ID3D11ShaderResourceView> v;
    hr(dev->CreateShaderResourceView(t, nullptr, &v), "CreateShaderResourceView");
    return v;
}
ComPtr<ID3D11UnorderedAccessView> uavOf(ID3D11Device* dev, ID3D11Texture2D* t) {
    ComPtr<ID3D11UnorderedAccessView> v;
    hr(dev->CreateUnorderedAccessView(t, nullptr, &v), "CreateUnorderedAccessView");
    return v;
}
std::vector<uint8_t> readBytes(Device& d, ID3D11Texture2D* t, int bytesPerPixel) {
    D3D11_TEXTURE2D_DESC td{};
    t->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;
    hr(d.dev->CreateTexture2D(&td, nullptr, &stage), "staging texture");
    d.ctx->CopyResource(stage.Get(), t);
    D3D11_MAPPED_SUBRESOURCE map{};
    hr(d.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map), "Map");
    std::vector<uint8_t> out(static_cast<size_t>(td.Width) * td.Height * bytesPerPixel);
    for (UINT y = 0; y < td.Height; ++y)
        std::memcpy(&out[static_cast<size_t>(y) * td.Width * bytesPerPixel],
                    static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                    static_cast<size_t>(td.Width) * bytesPerPixel);
    d.ctx->Unmap(stage.Get(), 0);
    return out;
}

// ------------------------------------------------------------------ one dispatch

struct Params {   // the shader's cbuffer P, as temporal_pass.cpp lays it out
    int region[4];
    int size[2];
    int texSize[2];
    float tanNow[4], tanPrev[4], jit[4];
};

struct Result {
    std::vector<uint8_t> out;        // ow*oh*4, the output texture
    std::vector<uint8_t> influence;  // w*h, the history the pass wrote (its only meaningful channel)
};

// historyR8: the history the shader reads and writes is R8_UNORM (one channel);
// otherwise RGBA8 with the influence in alpha, as the pass has always had it.
Result run(Device& d, ID3D11ComputeShader* cs, const Fix& f, const Inputs& in, bool historyR8) {
    ID3D11Device* dev = d.dev.Get();
    auto raw = tex(dev, f.w, f.h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, in.raw.data(), f.w * 4);
    auto trained = tex(dev, f.ow, f.oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, in.trained.data(), f.ow * 4);
    auto output = tex(dev, f.ow, f.oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    auto motion = tex(dev, f.w, f.h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, in.motion.data(), f.w * 4);
    const DXGI_FORMAT histFmt = historyR8 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    const int histBytes = historyR8 ? 1 : 4;
    auto next = tex(dev, f.w, f.h, histFmt, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);

    ComPtr<ID3D11Texture2D> cover, edits, screen, prev;
    if (!in.cover.empty()) cover = tex(dev, f.w, f.h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, in.cover.data(), f.w);
    if (!in.edits.empty()) edits = tex(dev, f.w, f.h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, in.edits.data(), f.w);
    if (!in.screen.empty()) screen = tex(dev, in.screenW, in.screenH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, in.screen.data(), in.screenW * 8);
    if (!in.hist.empty()) {
        if (historyR8) {
            prev = tex(dev, f.w, f.h, histFmt, D3D11_BIND_SHADER_RESOURCE, in.hist.data(), f.w);
        } else {
            // The colour channels of an old history are never read; fill them
            // with something that would show if they were.
            std::vector<uint8_t> rgba(static_cast<size_t>(f.w) * f.h * 4);
            for (size_t i = 0; i < in.hist.size(); ++i) {
                rgba[i * 4] = static_cast<uint8_t>(37 + i);
                rgba[i * 4 + 1] = static_cast<uint8_t>(91 + 3 * i);
                rgba[i * 4 + 2] = static_cast<uint8_t>(203 + 7 * i);
                rgba[i * 4 + 3] = in.hist[i];
            }
            prev = tex(dev, f.w, f.h, histFmt, D3D11_BIND_SHADER_RESOURCE, rgba.data(), f.w * 4);
        }
    }

    Params p{};
    p.region[0] = f.rx; p.region[1] = f.ry; p.region[2] = f.rx + f.w; p.region[3] = f.ry + f.h;
    p.size[0] = p.texSize[0] = f.w;
    p.size[1] = p.texSize[1] = f.h;
    p.jit[0] = f.jx; p.jit[1] = f.jy;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA pd{&p, 0, 0};
    ComPtr<ID3D11Buffer> cbP, cbR;
    hr(dev->CreateBuffer(&bd, &pd, &cbP), "cbuffer P");
    if (f.tol >= 0.f) {
        const float r[4] = {f.tol, f.hold, 0.f, 0.f};
        D3D11_BUFFER_DESC rd{};
        rd.ByteWidth = 16;
        rd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA rdd{r, 0, 0};
        hr(dev->CreateBuffer(&rd, &rdd, &cbR), "cbuffer R");
    }

    auto rawV = srvOf(dev, raw.Get()), trainedV = srvOf(dev, trained.Get()), motionV = srvOf(dev, motion.Get());
    ComPtr<ID3D11ShaderResourceView> coverV, editsV, screenV, prevV;
    if (cover) coverV = srvOf(dev, cover.Get());
    if (edits) editsV = srvOf(dev, edits.Get());
    if (screen) screenV = srvOf(dev, screen.Get());
    if (prev) prevV = srvOf(dev, prev.Get());
    auto outU = uavOf(dev, output.Get()), nextU = uavOf(dev, next.Get());

    ID3D11DeviceContext* ctx = d.ctx.Get();
    const FLOAT poison[4] = {1.f, 0.f, 1.f, 0.03f};   // a hole in the dispatch's coverage shows as this
    ctx->ClearUnorderedAccessViewFloat(outU.Get(), poison);
    const FLOAT zero[4] = {0.f, 0.f, 0.f, 0.f};
    ctx->ClearUnorderedAccessViewFloat(nextU.Get(), zero);
    ID3D11ShaderResourceView* srvs[7] = {rawV.Get(), trainedV.Get(), coverV.Get(), prevV.Get(), motionV.Get(), editsV.Get(), screenV.Get()};
    ID3D11UnorderedAccessView* uavs[2] = {outU.Get(), nextU.Get()};
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 7, srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ID3D11Buffer* cbs[2] = {cbP.Get(), cbR.Get()};
    ctx->CSSetConstantBuffers(0, 2, cbs);
    ctx->Dispatch(static_cast<UINT>((f.w + 7) / 8), static_cast<UINT>((f.h + 7) / 8), 1);
    ctx->ClearState();

    Result r;
    r.out = readBytes(d, output.Get(), 4);
    const std::vector<uint8_t> h = readBytes(d, next.Get(), histBytes);
    r.influence.resize(static_cast<size_t>(f.w) * f.h);
    for (size_t i = 0; i < r.influence.size(); ++i) r.influence[i] = h[i * histBytes + (historyR8 ? 0 : 3)];
    return r;
}

uint64_t fnv(const std::vector<uint8_t>& a, uint64_t h) {
    for (uint8_t b : a) { h ^= b; h *= 1099511628211ull; }
    return h;
}
uint64_t hashOf(const Result& r) {
    return fnv(r.influence, fnv(r.out, 14695981039346656037ull));
}

// What a fixture actually exercised, so a green run cannot be vacuous: output
// pixels the pass changed from NVIDIA's own (in RGB), history bytes it wrote
// nonzero, and history bytes that are not simply the marked footprint.
struct Stats {
    size_t changed = 0, influence = 0, unmarkedInfluence = 0, holes = 0;
};
Stats statsOf(const Fix& f, const Inputs& in, const Result& r) {
    Stats s;
    for (int y = 0; y < f.oh; ++y) {
        for (int x = 0; x < f.ow; ++x) {
            const size_t o = (static_cast<size_t>(y) * f.ow + x) * 4;
            if (r.out[o] == 0xFF && r.out[o + 1] == 0x00 && r.out[o + 2] == 0xFF && r.out[o + 3] == 0x08) ++s.holes;
            else if (std::memcmp(&r.out[o], &in.trained[o], 3) != 0) ++s.changed;
        }
    }
    for (size_t i = 0; i < r.influence.size(); ++i) {
        if (!r.influence[i]) continue;
        ++s.influence;
        if (r.influence[i] != 255) ++s.unmarkedInfluence;   // a decayed or transported value, not a fresh mark
    }
    return s;
}

// Recorded from the UNMODIFIED shader (RGBA8 history, point taps; commit
// 446d7e7a) on WARP under Windows 11 build 26200; see the header comment before
// re-recording. One per fixture, in kFix's order.
const uint64_t kGolden[] = {
    0xc9b3da52cacbc8ceull,  // sky_no_ui_2x
    0x62f9a5286ee11c77ull,  // sky_hold_off_b1_unbound
    0x7e25b389d2427364ull,  // sky_tol_zero_hold_on
    0x6c314b3d74bc30acull,  // corner_ui
    0xf6dbfc12c58fad28ull,  // corner_ui_jit_edge
    0x40e50ad3058a150aull,  // tile_edges_ui
    0xafff68269fe4b9ceull,  // tile_edges_ui_odd
    0xe6663939bb0743fcull,  // corona_edge
    0x1b9baf607c1501efull,  // corona_edge_15x
    0xe3763485b7f5acbcull,  // menu_full_frame
    0xdb0b6865c3c6a9afull,  // screen_ui
    0x8eb29c5a350291fcull,  // screen_mixed_region_max
    0x54655b3a250a4ea8ull,  // history_sparse_small_motion
    0xb436368871587d18ull,  // history_dense_large_motion
    0x01884880e78903a8ull,  // history_hostile_motion
    0x6ee86010d1db5c07ull,  // history_with_ui
    0xe8ced885b7ff336cull,  // ratio_1x
    0x8bbb2b4ea6c87dddull,  // ratio_133
    0x3a26effaf4455afeull,  // ratio_25
    0xba9cc4585510b3e7ull,  // ratio_075_down
    0xffacb9cc988504e1ull,  // jitter_1_3
    0xd5f69822bab7b63eull,  // jitter_0_9
    0xc172c0b90c3a74b1ull,  // jitter_neg_1_7
    0x7d3a5917875f3b7full,  // tiny_7x5
    0x79af1c447b2d5e05ull,  // wide_130x9
    0xa06e3d190bc881a7ull,  // tall_9x130
    0x05b55b2f3044e2c0ull,  // eye_left
    0xfc7b75ba4cf4a372ull,  // eye_right
    0xd15f5d8cc5a6b07aull,  // production_shape_eighth
};

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false, dry = false, print = false, nvidia = false, stats = false;
    const char* usage = "usage: ui_holo_pass_test --self-test|--dry-run|--print-goldens|--stats [--adapter nvidia]";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) selfTest = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--print-goldens")) print = true;
        else if (!std::strcmp(argv[i], "--stats")) stats = true;
        else if (!std::strcmp(argv[i], "--adapter") && i + 1 < argc && !std::strcmp(argv[i + 1], "nvidia")) { nvidia = true; ++i; }
        else { std::puts(usage); return 2; }
    }
    if (dry) {
        std::puts("Would run the UI resolve fixtures on WARP and compare every output and history byte; writes no files.");
        return 0;
    }
    if (!selfTest && !print && !stats) { std::puts(usage); return 2; }

    Device d = makeDevice(nvidia);
    auto cs = shaderFromBytecode(d.dev.Get(), edvr::kUiResolveBytecode, sizeof(edvr::kUiResolveBytecode));

    std::vector<uint64_t> got;
    std::vector<Stats> exercised;
    for (size_t i = 0; i < kFixtures; ++i) {
        const Inputs in = makeInputs(kFix[i]);
        const Result r = run(d, cs.Get(), kFix[i], in, /*historyR8=*/false);
        got.push_back(hashOf(r));
        exercised.push_back(statsOf(kFix[i], in, r));
        if (stats) {
            const Stats& s = exercised.back();
            std::printf("%-30s out %4dx%-4d changed %6zu  influence %5zu (decayed/transported %5zu)  holes %zu\n", kFix[i].name,
                        kFix[i].ow, kFix[i].oh, s.changed, s.influence, s.unmarkedInfluence, s.holes);
        }
    }
    if (print) {
        std::printf("goldens: %zu fixtures\n", got.size());
        for (size_t i = 0; i < got.size(); ++i)
            std::printf("    0x%016llxull,  // %s\n", static_cast<unsigned long long>(got[i]), kFix[i].name);
    }
    if (selfTest) {
        check(sizeof(kGolden) / sizeof(kGolden[0]) == kFixtures, "the goldens cover every fixture");
        if (d.warp) {
            for (size_t i = 0; i < got.size() && i < sizeof(kGolden) / sizeof(kGolden[0]); ++i) {
                const bool same = got[i] == kGolden[i];
                if (!same)
                    std::printf("FAIL: golden %s: got 0x%016llx want 0x%016llx\n", kFix[i].name,
                                static_cast<unsigned long long>(got[i]), static_cast<unsigned long long>(kGolden[i]));
                ++g_checks;
                if (!same) ++g_failures;
            }
        } else {
            // A GPU's float arithmetic is not WARP's (rounding, fused multiply-add,
            // the unorm conversion), so a hash recorded on WARP does not carry to
            // hardware. A hardware run compares old against new on the same adapter.
            std::puts("note: the goldens are WARP's; skipped on this adapter.");
        }
        // A green run must not be a vacuous one: what each fixture was built to
        // reach, it reached.
        for (size_t i = 0; i < kFixtures; ++i) {
            const Fix& f = kFix[i];
            const Stats& s = exercised[i];
            char label[160];
            std::snprintf(label, sizeof(label), "%s: the dispatch covers every output pixel", f.name);
            check(s.holes == 0, label);
            const bool idle = f.tol < 0.f && f.hold == 0.f && f.marks == 0 && f.hist == 0;
            std::snprintf(label, sizeof(label), "%s: %s", f.name, idle ? "an idle pass is the identity" : "the pass changed pixels");
            check(idle ? (s.changed == 0 && s.influence == 0) : s.changed > 0, label);
            if (f.marks != 0) {
                std::snprintf(label, sizeof(label), "%s: marks leave influence in the history", f.name);
                check(s.influence > 0, label);
            }
            if (f.hist != 0 && f.motion != 0 && f.w > 8 && f.h > 8) {   // the exact spikes need room
                std::snprintf(label, sizeof(label), "%s: transport carries influence past the marks", f.name);
                check(s.unmarkedInfluence > 0, label);
            }
        }
        if (d.info) {
            const UINT64 n = d.info->GetNumStoredMessagesAllowedByRetrievalFilter();
            UINT64 errors = 0;
            for (UINT64 m = 0; m < n; ++m) {
                SIZE_T len = 0;
                d.info->GetMessage(m, nullptr, &len);
                std::vector<char> buf(len);
                auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
                if (SUCCEEDED(d.info->GetMessage(m, msg, &len)) && msg->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                    ++errors;
                    std::printf("D3D11 %s\n", msg->pDescription);
                }
            }
            check(errors == 0, "the D3D11 debug layer reported no errors");
        }
        if (g_failures) { std::printf("FAILED: %d of %d checks\n", g_failures, g_checks); return 1; }
        std::printf("PASS: %d checks (%zu UI resolve fixtures, output and history byte for byte).\n", g_checks, kFixtures);
    }
    return 0;
}

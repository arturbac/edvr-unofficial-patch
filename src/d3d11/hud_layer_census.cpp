// advanced.hud_census -- Phase 0 "census gates" of the crisp-HUD design
// (docs/cockpit-hud-layer-design-2026-09-27.md). The header comment says
// what each gate measures, what it costs armed and unarmed, and why it
// changes no rendering; this file is the machinery. Everything runs on the
// game's render thread, called from vscreen's eye-draw branch (per draw,
// owner context only) and from vScreenFrameBoundary (per frame). The one
// other thread that matters -- the XR owner inside the temporal pass -- is
// read only through nativeTemporalProjectionReference, under
// native_temporal's own lock, which answers "no" from inside treat().
#include "hud_layer_census.h"

#include "binding_shadow.h"       // the owner-context binding shadow
#include "draw_state_describe.h"  // viewName, describeBlend/describeDs, shapeOf, dsStateOf
#include "tonemap_admit.h"        // the tonemap draw's structural admission, shared with the crisp-HUD half of fix.ui_quality
#include "ui_depth.h"             // uiDepthEyeOfTarget: the eye, by the pass's own table
#include "ui_layer.h"             // nativeTemporalProjectionReference
#include "ui_layer_math.h"        // the family VS hashes
#include "vscreen.h"              // vScreenSetRenderTargetsRaw

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"

#include <windows.h>

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace edvr {

namespace detail {
bool g_hudLayerCensusOn = false;
}  // namespace detail

namespace {

template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;

constexpr uint64_t kTotalsMs = 30000;
constexpr int kFamilies = 3;
enum : int { kFamHolo = 0, kFamFlightHud = 1, kFamSprite = 2 };
const char* const kFamNames[kFamilies] = {"holo", "flighthud", "sprite"};

int familyOfVs(uint64_t vs) {
    if (vs == kUiVsHolo) return kFamHolo;
    if (vs == kUiVsFlightHud) return kFamFlightHud;
    if (vs == kUiVsSprite) return kFamSprite;
    return -1;
}

// The measured tonemap pair, the EDHM swap (both in tonemap_admit.h now,
// shared with the crisp-HUD half of fix.ui_quality's re-issue), and the composite pass the
// design doc's bloom question turns on. Recognition is STRUCTURE-FIRST;
// the hashes only name what was found.
constexpr uint64_t kCompositeVs = 0x953C8123AD8DC13Bull;

// ------------------------------------------------------------------ window

struct FamWindow {
    uint64_t draws = 0;
    uint32_t framesWithDraws = 0;
    uint32_t minPerFrame = ~0u, maxPerFrame = 0;
    uint32_t firstOrdMin = ~0u, lastOrdMax = 0;
    uint64_t eyeDraws[2] = {0, 0};
    // G-C verdicts (both eyes).
    uint64_t gcJittered = 0, gcUnjittered = 0, gcOther = 0, gcNotScene = 0, gcAbstain = 0;
    // G-D occlusion-pair sample counts, per eye.
    uint64_t gdOn[2] = {0, 0}, gdOff[2] = {0, 0};
    uint32_t gdPairs[2] = {0, 0};
};

// Why an occlusion pair declined, counted separately since flight 1: the
// combined counter could not tell the guard from the budget, and the one
// number hid the frame-wide TIMESTAMP_DISJOINT story for a whole flight.
enum GdDecline : uint8_t {
    kGdDeclPredication = 0,
    kGdDeclQuery,   // a sample-counting game query is open on the context
    kGdDeclRing,    // every pair still awaits its GPU result
    kGdDeclUav,     // a pixel-shader UAV is bound
    kGdDeclSo,      // a stream-out target is bound
    kGdDeclCount
};

struct Window {
    uint32_t frames = 0;
    uint32_t eyeDrawsMax = 0;
    FamWindow fam[kFamilies];
    uint32_t tonemaps = 0;
    uint32_t tonemapOrdMin = ~0u, tonemapOrdMax = 0;
    uint32_t tonemapRejected = 0;   // structure pass, SRV shape fail
    uint32_t tonemapHdrIsFam = 0;   // tonemaps whose HDR SRV was a family target
    uint32_t bloomSightings = 0;    // vs 953C8123AD8DC13B
    uint32_t bloomInFamilyFrames = 0;
    uint32_t gdDecl[kGdDeclCount] = {};
    uint32_t gdDropped = 0;         // pairs whose result never arrived
    uint32_t gcSkipped = 0;         // ring full, no or small cb0
};

Window g_win;
uint64_t g_winStartMs = 0;

// ------------------------------------------------------------ frame scratch

struct FrameScratch {
    uint32_t eyeDrawsMax = 0;
    struct Fam {
        uint32_t draws = 0, firstOrd = ~0u, lastOrd = 0;
        uint64_t eyeDraws[2] = {0, 0};
    } fam[kFamilies];
    uint32_t gdBudget[kFamilies][2] = {{2, 2}, {2, 2}, {2, 2}};  // pairs per family per eye per frame
    const void* famTargets[4] = {};
    uint32_t famTargetCount = 0;
    bool anyFamily = false;
    uint32_t tonemapOrd[4] = {};
    int tonemapEye[4] = {-1, -1, -1, -1};
    bool tonemapHdrIsFam[4] = {false, false, false, false};
    uint32_t tonemapCount = 0;
};

FrameScratch g_frame;

// ------------------------------------------------------- target (per frame)

// The draw's render target, cached per binding generation (the frame
// boundary bumps every generation, so this is fresh each frame): resource,
// size, resource format, and the RTV's view format. uiLayerTargetKind is
// not reused: it answers "which kind of eye target", and the sprite's
// target is exactly what G-A must not presume.
struct TargetCache {
    uint32_t gen = 0;
    bool ok = false;
    void* resource = nullptr;
    uint32_t w = 0, h = 0;
    uint32_t fmt = 0;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
};
TargetCache g_tc;

const TargetCache& targetOf() {
    const uint32_t gen = bindingGeneration(BindSlot::Rtv0);
    if (g_tc.gen == gen) return g_tc;
    g_tc = TargetCache{};
    g_tc.gen = gen;
    void* rtv = bindingGet(BindSlot::Rtv0);
    ResourceInfo info;
    if (rtv && bindingResolve(rtv, &info) && info.isTexture2D) {
        g_tc.ok = true;
        g_tc.resource = info.resource;
        g_tc.w = info.a;
        g_tc.h = info.b;
        g_tc.fmt = info.fmt;
        D3D11_RENDER_TARGET_VIEW_DESC d{};
        if (guarded("hudCensus.rtvDesc",
                    [&] { static_cast<ID3D11RenderTargetView*>(rtv)->GetDesc(&d); })) {
            g_tc.view = d.Format;
        }
    }
    return g_tc;
}

void noteFamTarget(const void* res) {
    if (!res) return;
    for (uint32_t i = 0; i < g_frame.famTargetCount; ++i)
        if (g_frame.famTargets[i] == res) return;
    if (g_frame.famTargetCount < 4) g_frame.famTargets[g_frame.famTargetCount++] = res;
}

bool famTargeted(const void* res) {
    if (!res) return false;
    for (uint32_t i = 0; i < g_frame.famTargetCount; ++i)
        if (g_frame.famTargets[i] == res) return true;
    return false;
}

// ------------------------------------------- G-A: per-family state, on change

// Everything the gate wants, hashed so the full line prints only on a
// change: target resource and view format, the PS (EDHM swaps it), blend,
// depth-stencil state and reference, and the four PS SRV slots' sizes.
struct FamFingerprint {
    void* res;
    uint32_t view;
    uint64_t ps;
    UiBlendRt blend;
    UiDsState ds;
    uint32_t ref;
    uint32_t dsViewFmt;
    uint32_t srv[4][3];  // w, h, fmt per slot; zero when unbound or not 2D
};

// First-seen state sets, per family: a state's full fingerprint is logged
// the first time it appears in the session and never again. Flight 2's
// single last-fingerprint compare re-logged every panel of every frame
// (a dozen holo panels with distinct interface surfaces cycle through one
// slot), ate the 4 MB log cap two minutes into the cockpit, and cost that
// flight its G-D window. 64 is far past the ~dozen states a family shows.
constexpr uint32_t kFamFpMax = 64;
uint64_t g_famFpSeen[kFamilies][kFamFpMax] = {};
uint32_t g_famFpCount[kFamilies] = {};
bool g_famFpFullNoted[kFamilies] = {};

void noteFamilyState(ID3D11DeviceContext* ctx, int fam, int eye, uint32_t ord, const TargetCache& tc,
                     uint64_t vs, uint64_t ps) {
    Ptr<ID3D11BlendState> bs;
    FLOAT factor[4]{};
    UINT mask = 0;
    ctx->OMGetBlendState(&bs, factor, &mask);
    FamFingerprint fp;
    std::memset(&fp, 0, sizeof(fp));
    shapeOf(bs.Get(), &fp.blend);
    Ptr<ID3D11DepthStencilView> dsv;
    ctx->OMGetRenderTargets(0, nullptr, &dsv);
    DXGI_FORMAT dsFmt = DXGI_FORMAT_UNKNOWN;
    UINT ref = 0;
    fp.ds = dsStateOf(ctx, dsv.Get(), &ref, &dsFmt);
    fp.res = tc.resource;
    fp.view = static_cast<uint32_t>(tc.view);
    fp.ps = ps;
    fp.ref = ref;
    fp.dsViewFmt = static_cast<uint32_t>(dsFmt);
    for (int i = 0; i < 4; ++i) {
        void* v = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + i));
        ResourceInfo info;
        if (v && bindingResolve(v, &info) && info.isTexture2D) {
            fp.srv[i][0] = info.a;
            fp.srv[i][1] = info.b;
            fp.srv[i][2] = info.fmt;
        }
    }
    const uint64_t h = fnv1a64(&fp, sizeof(fp));
    for (uint32_t i = 0; i < g_famFpCount[fam]; ++i)
        if (g_famFpSeen[fam][i] == h) return;
    if (g_famFpCount[fam] >= kFamFpMax) {
        if (!g_famFpFullNoted[fam]) {
            g_famFpFullNoted[fam] = true;
            Log::get().note(
                "hud layer census: ga %s: %u distinct states logged; further state changes are "
                "not printed (the window's draws/frame lines continue).",
                kFamNames[fam], kFamFpMax);
        }
        return;
    }
    g_famFpSeen[fam][g_famFpCount[fam]++] = h;
    char bl[64], dsb[320];
    describeBlend(fp.blend, bl, sizeof(bl));
    describeDs(fp.ds, ref, dsFmt, dsb, sizeof(dsb));
    std::string srvs;
    char one[64];
    for (int i = 0; i < 4; ++i) {
        if (fp.srv[i][0]) {
            _snprintf_s(one, sizeof(one), _TRUNCATE, "%ux%u %s", fp.srv[i][0], fp.srv[i][1],
                        viewName(static_cast<DXGI_FORMAT>(fp.srv[i][2])));
        } else {
            _snprintf_s(one, sizeof(one), _TRUNCATE, "-");
        }
        srvs += " t";
        srvs += char('0' + i);
        srvs += '=';
        srvs += one;
    }
    Log::get().note(
        "hud layer census: ga %s state: ord=%u eye=%d vs=%016llX ps=%016llX target=%ux%u %s "
        "res=%p | %s (%s) | %s |%s",
        kFamNames[fam], ord, eye, static_cast<unsigned long long>(vs),
        static_cast<unsigned long long>(ps), tc.w, tc.h, viewName(tc.view), tc.resource, bl,
        uiBlendShapeName(uiLayerBlendShape(fp.blend)), dsb, srvs.c_str());
}

// ------------------------------ G-C: VS cb0 rows 4..7 against the projection

// The three family shaders were expected to form clip position from four
// dp4 rows at VS cb0 rows 4..7 (flat_projection_recipes.h, the flat path's
// recipe). Flight 1 (2026-09-27) measured rows 4..7 as a COMPOSED
// model-view transform in all three families -- dense rows, no bare
// projection structure -- so the centre-term test had nothing to vote on.
// The read is the whole cb0 now (up to 16 rows, 256 bytes), each 4-row
// quad voted the same way; the first quad with bare-projection structure
// gives the verdict, and a draw with none dumps every row once per family
// per eye for offline factorisation. Copied into a ring of staging slots
// at the draw, mapped at the frame boundary -- one frame late, which the
// comparison does not care about: the frusta and shift the reference is
// rebuilt from are the same frame's.
constexpr uint32_t kGcRing = 16;
constexpr uint32_t kGcBytes = 256;
constexpr uint32_t kGcRowOffset = 0;
Ptr<ID3D11Buffer> g_gcStage;
struct GcSlot {
    bool pending = false;
    int fam = -1, eye = -1;
    uint32_t ord = 0;
    uint32_t bytes = 0;  // the cb0 bytes this slot actually holds
};
GcSlot g_gcSlots[kGcRing];
uint32_t g_gcSkipped = 0;
uint32_t g_gcWaitFrames = 0;  // boundaries the ring's map has deferred
bool g_gcSmallCbNoted[kFamilies] = {};

// What a sample voted, per family per eye, logged on change.
enum GcVerdict : uint8_t { kGcNoData = 0, kGcJittered, kGcUnjittered, kGcOther, kGcNotScene, kGcAbstain };
GcVerdict g_gcLast[kFamilies][2] = {};

const char* gcVerdictName(GcVerdict v) {
    switch (v) {
        case kGcJittered: return "jittered";
        case kGcUnjittered: return "unjittered";
        case kGcOther: return "other-shift";
        case kGcNotScene: return "not-the-scene-projection";
        case kGcAbstain: return "no-reference";
        default: return "none";
    }
}

void gcOnFamilyDraw(ID3D11DeviceContext* ctx, int fam, int eye, uint32_t ord) {
    int slot = -1;
    for (uint32_t i = 0; i < kGcRing; ++i)
        if (!g_gcSlots[i].pending) {
            slot = static_cast<int>(i);
            break;
        }
    if (slot < 0) {
        ++g_gcSkipped;
        return;
    }
    Ptr<ID3D11Buffer> cb;
    UINT first = 0, num = 0;
    Ptr<ID3D11DeviceContext1> ctx1;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))) && ctx1) {
        ctx1->VSGetConstantBuffers1(0, 1, &cb, &first, &num);
    } else {
        ctx->VSGetConstantBuffers(0, 1, &cb);
    }
    if (!cb) {
        ++g_gcSkipped;
        return;
    }
    D3D11_BUFFER_DESC bd{};
    cb->GetDesc(&bd);
    const uint64_t begin = static_cast<uint64_t>(first) * 16 + kGcRowOffset;
    uint64_t avail = begin < bd.ByteWidth ? bd.ByteWidth - begin : 0;
    if (avail > kGcBytes) avail = kGcBytes;
    avail &= ~15ull;  // whole rows only; the vote counts full 4-row quads
    if (avail < 64) {
        if (!g_gcSmallCbNoted[fam]) {
            g_gcSmallCbNoted[fam] = true;
            Log::get().note(
                "hud layer census: gc %s: VS cb0 is %u bytes (first constant %u); one 4-row "
                "quad needs 64 -- G-C cannot read this family's projection.",
                kFamNames[fam], bd.ByteWidth, first, static_cast<unsigned long long>(avail));
        }
        ++g_gcSkipped;
        return;
    }
    if (!g_gcStage) {
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = kGcRing * kGcBytes;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Ptr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!dev || FAILED(dev->CreateBuffer(&sd, nullptr, &g_gcStage))) {
            ++g_gcSkipped;
            return;
        }
    }
    const D3D11_BOX box{static_cast<UINT>(begin), 0, 0, static_cast<UINT>(begin + avail), 1, 1};
    ctx->CopySubresourceRegion(g_gcStage.Get(), 0, slot * kGcBytes, 0, 0, cb.Get(), 0, &box);
    g_gcSlots[slot].pending = true;
    g_gcSlots[slot].fam = fam;
    g_gcSlots[slot].eye = eye;
    g_gcSlots[slot].ord = ord;
    g_gcSlots[slot].bytes = static_cast<uint32_t>(avail);
}

// One axis of the comparison: the measured centre term against its
// unjittered expectation and the expected jitter shift. The tolerance is
// 30% of the expected shift: the float noise between the game's own
// projection arithmetic and this reconstruction is ~1e-6 relative, orders
// under the shift itself (a Halton step at the flight's render size moves
// the term by ~1e-4..1e-3), so 30% separates "carried" from "absent"
// without ever calling noise a verdict. An axis whose expected shift is
// zero this phase (jx == 0 happens; jy never does) abstains.
int gcAxisVote(float meas, float unjit, float expShift) {
    if (std::fabs(expShift) < 1e-7f) return 0;
    const float d = meas - unjit;
    const float tol = 0.3f * std::fabs(expShift);
    if (std::fabs(d - expShift) <= tol) return 1;
    if (std::fabs(d) <= tol) return -1;
    return 2;
}

void gcVerdict(int fam, int eye, uint32_t ord, const float r[64], uint32_t bytes) {
    FamWindow& w = g_win.fam[fam];
    float frusta[4]{}, shift[2]{}, nearZ = 0.0f, farZ = 0.0f;
    GcVerdict v = kGcAbstain;
    int quad = -1;
    float m00 = 0, m02 = 0, m11 = 0, m12 = 0, c02u = 0, c12u = 0, sx = 0, sy = 0;
    bool haveRef = eye >= 0 && eye <= 1 &&
                   nativeTemporalProjectionReference(static_cast<uint32_t>(eye), frusta, shift,
                                                     &nearZ, &farZ);
    const float rl = frusta[1] - frusta[0], bt = frusta[3] - frusta[2];
    if (haveRef && rl != 0.0f && bt != 0.0f) {
        // The eye's projection, rebuilt from the same inputs the game built
        // it from: scales 2/(r-l) and 2/(u-d), centres (r+l)/(r-l) and
        // (u+d)/(u-d). The pass's jitter shifts every tangent by (dx, dy),
        // which leaves the scales untouched and moves the centres by
        // 2dx/(r-l), 2dy/(u-d) -- so the centre terms alone carry the
        // verdict, and the z/w rows' convention never has to be presumed.
        const float e00 = 2.0f / rl, e11 = 2.0f / bt;
        c02u = (frusta[1] + frusta[0]) / rl;
        c12u = (frusta[3] + frusta[2]) / bt;
        sx = 2.0f * shift[0] / rl;
        sy = 2.0f * shift[1] / bt;
        // Vote each full 4-row quad the slot holds: the FIRST whose rows
        // have bare-projection structure carries the verdict. Structure: a
        // bare projection's clip rows are (m00, 0, m02, 0) and (0, m11,
        // m12, 0); the zero slots are tested against the rows' own scale,
        // so a composed model transform (which fills them) is skipped
        // rather than mis-voted. Flight 1's rows 4..7 were all composed.
        const uint32_t quads = bytes / 64;
        for (uint32_t q = 0; q < quads && quad < 0; ++q) {
            const float* m = r + q * 16;
            const float q00 = m[0], q02 = m[2], q11 = m[5], q12 = m[6];
            const float scale =
                std::fabs(q00) + std::fabs(q02) + std::fabs(q11) + std::fabs(q12);
            const bool structure =
                std::fabs(m[1]) + std::fabs(m[3]) + std::fabs(m[4]) + std::fabs(m[7]) <=
                    1e-3f * (scale + 1e-6f) &&
                std::fabs(q00 - e00) <= 1e-3f * std::fabs(e00) &&
                std::fabs(q11 - e11) <= 1e-3f * std::fabs(e11);
            if (!structure) continue;
            quad = static_cast<int>(q);
            m00 = q00;
            m02 = q02;
            m11 = q11;
            m12 = q12;
            const int vx = gcAxisVote(m02, c02u, sx);
            const int vy = gcAxisVote(m12, c12u, sy);
            if ((vx == 1 || vy == 1) && vx != -1 && vy != -1 && vx != 2 && vy != 2)
                v = kGcJittered;
            else if ((vx == -1 || vy == -1) && vx != 1 && vy != 1 && vx != 2 && vy != 2)
                v = kGcUnjittered;
            else if (vx == 0 && vy == 0)
                v = kGcAbstain;
            else
                v = kGcOther;
        }
        if (quad < 0) v = kGcNotScene;
    }
    switch (v) {
        case kGcJittered: ++w.gcJittered; break;
        case kGcUnjittered: ++w.gcUnjittered; break;
        case kGcOther: ++w.gcOther; break;
        case kGcNotScene: ++w.gcNotScene; break;
        default: ++w.gcAbstain; break;
    }
    if (eye < 0 || eye > 1) return;
    GcVerdict& last = g_gcLast[fam][eye];
    if (last == v) return;
    const GcVerdict was = last;
    last = v;
    if (v == kGcNotScene) {
        // The offline factorisation input: every row the slot holds, once
        // per transition into not-scene (flight 1 proved rows 4..7 alone
        // cannot answer the gate).
        std::string rows;
        char num[320];
        const uint32_t n = bytes / 16;
        for (uint32_t i = 0; i < n; ++i) {
            _snprintf_s(num, _TRUNCATE, "%s[%.6g %.6g %.6g %.6g]", i ? " " : "", r[i * 4],
                        r[i * 4 + 1], r[i * 4 + 2], r[i * 4 + 3]);
            rows += num;
        }
        Log::get().note(
            "hud layer census: gc %s eye=%d verdict=%s (was %s): no 4-row quad of cb0 is the "
            "eye's projection; rows 0..%u = %s (ord=%u).",
            kFamNames[fam], eye, gcVerdictName(v), gcVerdictName(was), n ? n * 4 - 1 : 0,
            rows.c_str(), ord);
    } else {
        char qt[40] = "";
        if (quad >= 0)
            _snprintf_s(qt, _TRUNCATE, " on cb0 rows %d..%d", quad * 4, quad * 4 + 3);
        Log::get().note(
            "hud layer census: gc %s eye=%d verdict=%s (was %s)%s: m02 %.6g (jittered %.6g, "
            "unjittered %.6g), m12 %.6g (jittered %.6g, unjittered %.6g); m00 %.6g m11 %.6g, "
            "near %.4g far %.6g (ord=%u).",
            kFamNames[fam], eye, gcVerdictName(v), gcVerdictName(was), qt, m02, c02u + sx, c02u,
            m12, c12u + sy, c12u, m00, m11, nearZ, farZ, ord);
    }
}

void gcPoll(ID3D11DeviceContext* ctx) {
    bool any = false;
    for (auto& s : g_gcSlots) any = any || s.pending;
    if (!any || !g_gcStage) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = ctx->Map(g_gcStage.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        // The copies are a frame old, so a not-ready map should not exist
        // twice; a bounded force-block past eight boundaries, not a stall
        // per boundary (luma_probe.cpp's own fallback).
        if (++g_gcWaitFrames <= 8) return;
        hr = ctx->Map(g_gcStage.Get(), 0, D3D11_MAP_READ, 0, &m);
    }
    g_gcWaitFrames = 0;
    if (FAILED(hr) || !m.pData) {
        for (auto& s : g_gcSlots) s.pending = false;
        ++g_gcSkipped;
        return;
    }
    for (uint32_t i = 0; i < kGcRing; ++i) {
        if (!g_gcSlots[i].pending) continue;
        const float* rows =
            reinterpret_cast<const float*>(static_cast<const char*>(m.pData) + i * kGcBytes);
        gcVerdict(g_gcSlots[i].fam, g_gcSlots[i].eye, g_gcSlots[i].ord, rows, g_gcSlots[i].bytes);
        g_gcSlots[i].pending = false;
    }
    ctx->Unmap(g_gcStage.Get(), 0);
}

}  // namespace
}  // namespace edvr

namespace edvr {
namespace {

// --------------------- G-D: the occlusion pair, depth on against depth off
//
// Per sampled family draw: the game's own draw is re-issued twice (the
// caller's pureDrawReissue, zero hook recursion) with NO colour target --
// query A under a clone of the game's depth-stencil state with every write
// masked (the GEQUAL test exactly as the game runs it; the buffer cannot
// change), query B under the same clone with depth and stencil off (every
// sample passes). A samples / B samples is the share the depth test lets
// through; 1 - that is the rejected share the gate asks for. The game's
// own draw runs after, untouched, against the restored state.
constexpr uint32_t kGdPairRing = 8;
struct GdPair {
    Ptr<ID3D11Query> a, b;
    bool pending = false;
    int fam = -1, eye = -1;
    uint32_t waitFrames = 0;
};
GdPair g_gdPairs[kGdPairRing];

// The derived states, per game state object seen (the families share one):
// the game's own values are read and LOGGED before anything is mutated
// (the resolve_probe.cpp lesson: a line that prints our state under the
// game's name is worse than no line), then cloned twice. Four entries is
// more than the measured session needs; a fifth distinct state disables
// G-D rather than churn the cache.
struct GdDerived {
    const void* src = nullptr;
    Ptr<ID3D11DepthStencilState> writeMasked;
    Ptr<ID3D11DepthStencilState> testsOff;
    bool logged = false;
};
constexpr uint32_t kGdDerivedMax = 4;
GdDerived g_gdDerived[kGdDerivedMax];
bool g_gdDisabled = false;
uint32_t g_gdDecl[kGdDeclCount] = {};
uint32_t g_gdDropped = 0;
int g_gdPendingFam = -1, g_gdPendingEye = -1;  // set by EyeDraw, consumed by GdBegin

// The game queries currently open on the owner context (hudLayerCensusNoteGameQuery):
// a re-issue inside the game's own SAMPLE-COUNTING bracket -- occlusion,
// stream-out or pipeline statistics -- would feed its counter, and
// changing what the game measures changes what it draws a frame later.
// Timestamps, their disjoint and events count no samples and are ignored:
// flight 1's blanket guard declined every pair because EDVR's own
// gpu_span TIMESTAMP_DISJOINT is open across the frame. The census's own
// ring queries are filtered out. 16 is far past anything measured;
// overflow stands the gate down to "open" rather than guess.
void* g_openGameQueries[16] = {};
uint32_t g_openGameQueryCount = 0;
bool g_openGameQueryOverflow = false;

// True when an open query of this type would count the re-issue's
// samples; everything else (timestamp, disjoint, event, stream-out
// overflow) can stay open across the pair.
bool gdQueryTypeBlocks(D3D11_QUERY t) {
    switch (t) {
        case D3D11_QUERY_OCCLUSION:
        case D3D11_QUERY_PIPELINE_STATISTICS:
        case D3D11_QUERY_OCCLUSION_PREDICATE:
        case D3D11_QUERY_SO_STATISTICS:
        case D3D11_QUERY_SO_STATISTICS_STREAM0:
        case D3D11_QUERY_SO_STATISTICS_STREAM1:
        case D3D11_QUERY_SO_STATISTICS_STREAM2:
        case D3D11_QUERY_SO_STATISTICS_STREAM3:
            return true;
        default:
            return false;
    }
}

bool isOwnQuery(const void* q) {
    for (const auto& p : g_gdPairs)
        if (p.a.Get() == q || p.b.Get() == q) return true;
    return false;
}

void gdDisable(const char* why) {
    if (g_gdDisabled) return;
    g_gdDisabled = true;
    Log::get().note(
        "hud layer census: gd disabled for the rest of the session -- %s. The other gates are "
        "unaffected; no game state was left changed.",
        why);
}

GdDerived* gdDerivedFor(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* src, UINT ref) {
    for (auto& e : g_gdDerived)
        if (e.src == src) return &e;
    for (auto& e : g_gdDerived) {
        if (e.src || e.writeMasked || e.testsOff) continue;
        D3D11_DEPTH_STENCIL_DESC d{};
        if (src) {
            src->GetDesc(&d);
        } else {
            // No state bound is the D3D11 default: depth on, LESS, write
            // all, stencil off. Spelled out; zeroed would mean depth OFF.
            d.DepthEnable = TRUE;
            d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            d.DepthFunc = D3D11_COMPARISON_LESS;
            d.StencilEnable = FALSE;
        }
        // The game's values, logged BEFORE the clones mutate them.
        if (!e.logged) {
            e.logged = true;
            UiDsState s = uiLayerDsStateFrom(&d, 0);
            char dsb[320];
            describeDs(s, ref, DXGI_FORMAT_UNKNOWN, dsb, sizeof(dsb));
            Log::get().note(
                "hud layer census: gd: THE GAME'S depth state is %s. The pair's A keeps its tests "
                "with ALL writes masked (the test reads the same, the buffer cannot change); B "
                "has depth and stencil off.",
                dsb);
        }
        D3D11_DEPTH_STENCIL_DESC a = d, b = d;
        a.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        a.StencilWriteMask = 0;
        b.DepthEnable = FALSE;
        b.StencilEnable = FALSE;
        Ptr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!dev || FAILED(dev->CreateDepthStencilState(&a, &e.writeMasked)) ||
            FAILED(dev->CreateDepthStencilState(&b, &e.testsOff))) {
            e = GdDerived{};
            return nullptr;
        }
        e.src = src;
        return &e;
    }
    return nullptr;
}

void gdReleaseSave(HudCensusGdSave& save) {
    for (auto& r : save.rtvs) r.Reset();
    save.dsv.Reset();
    save.dss.Reset();
    save.stencilRef = 0;
}

void gdPoll(ID3D11DeviceContext* ctx) {
    for (auto& pair : g_gdPairs) {
        if (!pair.pending) continue;
        UINT64 on = 0, off = 0;
        const HRESULT ha = ctx->GetData(pair.a.Get(), &on, sizeof(on), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT hb = ctx->GetData(pair.b.Get(), &off, sizeof(off), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (ha == S_FALSE || hb == S_FALSE) {
            // Never waited on, by design; a pair the GPU never answers is
            // dropped after a minute of frames, with the query slots freed.
            if (++pair.waitFrames > 3600) {
                pair.pending = false;
                ++g_gdDropped;
            }
            continue;
        }
        pair.pending = false;
        if (ha != S_OK || hb != S_OK) {
            ++g_gdDropped;
            continue;
        }
        if (pair.fam >= 0 && pair.fam < kFamilies && pair.eye >= 0 && pair.eye < 2) {
            FamWindow& w = g_win.fam[pair.fam];
            w.gdOn[pair.eye] += on;
            w.gdOff[pair.eye] += off;
            ++w.gdPairs[pair.eye];
        }
    }
}

// --------------------------------- G-E: exposure beside HUD-region HDR luma
//
// Read at the tonemap draw itself: the exposure scalar at the draw's OWN
// VS t0 (not assumed to be exposure_fix's strip -- whatever this draw
// samples, whose shape is logged), and a centre crop of its PS t1 HDR
// source, R11G11B10 decoded. Copied at the draw, mapped at a later frame
// boundary (DO_NOT_WAIT, a bounded force-block past eight frames), one
// sample a second at most.
constexpr uint32_t kGeCrop = 512;
constexpr int kGeGrid = 16;
struct GeSample {
    Ptr<ID3D11Texture2D> exposure;
    uint32_t exposureW = 0, exposureH = 0;
    bool exposureCopied = false;
    Ptr<ID3D11Texture2D> luma;
    uint32_t crop = 0, srcW = 0, srcH = 0;
    bool lumaCopied = false;
    int eye = -1;
    int state = 0;  // 0 idle, 1 copied, 2 waiting
    uint32_t waitFrames = 0;
};
GeSample g_ge;
LARGE_INTEGER g_geLastQpc{};
bool g_geExposureNoted = false;
bool g_geLumaFmtNoted = false;
// The last resolved sample, for the window line.
bool g_geHaveLast = false;
int g_geLastEye = -1;
float g_geLastExposure = 0.0f, g_geLastLuma = 0.0f, g_geLastLumaMax = 0.0f;

// A 5-bit-exponent, N-bit-mantissa unsigned mini-float, bias 15 -- copied
// from luma_probe.cpp, whose R11G11B10 decode this gate reuses.
float geUnpackMiniFloat(uint32_t raw, int mantissaBits) {
    const uint32_t mantMask = (1u << mantissaBits) - 1u;
    const uint32_t exp = (raw >> mantissaBits) & 0x1Fu;
    const uint32_t mant = raw & mantMask;
    if (exp == 0u) {
        if (mant == 0u) return 0.0f;
        return ldexpf(static_cast<float>(mant), -mantissaBits - 14);
    }
    if (exp == 0x1Fu) {
        return mant == 0u ? std::numeric_limits<float>::infinity()
                          : std::numeric_limits<float>::quiet_NaN();
    }
    const float m = 1.0f + static_cast<float>(mant) / static_cast<float>(1u << mantissaBits);
    return ldexpf(m, static_cast<int>(exp) - 15);
}

void geOnTonemap(ID3D11DeviceContext* ctx, int eye) {
    if (g_ge.state != 0) return;
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (g_geLastQpc.QuadPart && freq.QuadPart > 0 &&
        static_cast<double>(now.QuadPart - g_geLastQpc.QuadPart) /
                static_cast<double>(freq.QuadPart) < 1.0) {
        return;
    }
    bool copiedAny = false;
    // VS t0: the scalar exposure.
    Ptr<ID3D11ShaderResourceView> vsT0;
    ctx->VSGetShaderResources(0, 1, &vsT0);
    if (vsT0) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vsT0->GetDesc(&vd);
        Ptr<ID3D11Resource> res;
        vsT0->GetResource(&res);
        Ptr<ID3D11Texture2D> tex;
        if (res && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            if (!g_geExposureNoted) {
                g_geExposureNoted = true;
                Log::get().note(
                    "hud layer census: ge: the tonemap's VS t0 (exposure) is %ux%u %s viewed as "
                    "%s (view dimension %d, resource %p).",
                    td.Width, td.Height, viewName(td.Format), viewName(vd.Format),
                    static_cast<int>(vd.ViewDimension), res.Get());
            }
            if ((td.Format == DXGI_FORMAT_R32_TYPELESS || td.Format == DXGI_FORMAT_R32_FLOAT) &&
                td.MipLevels == 1 && td.ArraySize == 1 && td.SampleDesc.Count == 1 &&
                uint64_t(td.Width) * td.Height <= 1024) {
                if (!g_ge.exposure || g_ge.exposureW != td.Width || g_ge.exposureH != td.Height) {
                    D3D11_TEXTURE2D_DESC sd = td;
                    sd.Usage = D3D11_USAGE_STAGING;
                    sd.BindFlags = 0;
                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    sd.MiscFlags = 0;
                    Ptr<ID3D11Device> dev;
                    ctx->GetDevice(&dev);
                    g_ge.exposure.Reset();
                    if (dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &g_ge.exposure))) {
                        g_ge.exposureW = td.Width;
                        g_ge.exposureH = td.Height;
                    }
                }
                if (g_ge.exposure) {
                    ctx->CopySubresourceRegion(g_ge.exposure.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);
                    g_ge.exposureCopied = true;
                    copiedAny = true;
                }
            }
        }
    }
    // PS t1: the HDR source, centre-cropped. The decode knows R11G11B10
    // only; anything else is named once and skipped, not assumed.
    Ptr<ID3D11ShaderResourceView> psT1;
    ctx->PSGetShaderResources(1, 1, &psT1);
    if (psT1) {
        Ptr<ID3D11Resource> res;
        psT1->GetResource(&res);
        Ptr<ID3D11Texture2D> tex;
        if (res && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            if (td.Format != DXGI_FORMAT_R11G11B10_FLOAT) {
                if (!g_geLumaFmtNoted) {
                    g_geLumaFmtNoted = true;
                    Log::get().note(
                        "hud layer census: ge: the tonemap's PS t1 (HDR source) is DXGI format %u, "
                        "not R11G11B10_FLOAT -- the luma decode skips it (not assumed).",
                        static_cast<unsigned>(td.Format));
                }
            } else if (td.SampleDesc.Count == 1 && td.MipLevels == 1 && td.ArraySize == 1 &&
                       td.Width >= 2 && td.Height >= 2) {
                const uint32_t crop = td.Width < td.Height ? (td.Width < kGeCrop ? td.Width : kGeCrop)
                                                           : (td.Height < kGeCrop ? td.Height : kGeCrop);
                if (!g_ge.luma || g_ge.crop != crop) {
                    D3D11_TEXTURE2D_DESC sd{};
                    sd.Width = crop;
                    sd.Height = crop;
                    sd.MipLevels = sd.ArraySize = 1;
                    sd.Format = td.Format;
                    sd.SampleDesc.Count = 1;
                    sd.Usage = D3D11_USAGE_STAGING;
                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    Ptr<ID3D11Device> dev;
                    ctx->GetDevice(&dev);
                    g_ge.luma.Reset();
                    if (dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &g_ge.luma))) {
                        g_ge.crop = crop;
                    }
                }
                if (g_ge.luma) {
                    const D3D11_BOX box{(td.Width - crop) / 2, (td.Height - crop) / 2, 0,
                                        (td.Width - crop) / 2 + crop, (td.Height - crop) / 2 + crop, 1};
                    ctx->CopySubresourceRegion(g_ge.luma.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
                    g_ge.lumaCopied = true;
                    g_ge.srcW = td.Width;
                    g_ge.srcH = td.Height;
                    copiedAny = true;
                }
            }
        }
    }
    if (copiedAny) {
        g_ge.eye = eye;
        g_ge.state = 1;
        g_ge.waitFrames = 0;
        g_geLastQpc = now;
    }
}

void geResolve(ID3D11DeviceContext* ctx) {
    if (g_ge.state == 0) return;
    const bool force = g_ge.waitFrames > 8;
    const UINT flags = force ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT;
    float exposure = 0.0f;
    bool haveExposure = false, deferred = false;
    double lumaSum = 0.0;
    float lumaMax = 0.0f;
    bool haveLuma = false;
    if (g_ge.exposureCopied && g_ge.exposure) {
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx->Map(g_ge.exposure.Get(), 0, D3D11_MAP_READ, flags, &m);
        if (SUCCEEDED(hr) && m.pData) {
            std::memcpy(&exposure, m.pData, sizeof(float));
            haveExposure = true;
            ctx->Unmap(g_ge.exposure.Get(), 0);
        } else if (hr == DXGI_ERROR_WAS_STILL_DRAWING && !force) {
            deferred = true;
        }
    }
    if (!deferred && g_ge.lumaCopied && g_ge.luma) {
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx->Map(g_ge.luma.Get(), 0, D3D11_MAP_READ, flags, &m);
        if (SUCCEEDED(hr) && m.pData && g_ge.crop) {
            // 16x16 grid over the centre crop, R11G11B10 decoded
            // (luma_probe.cpp's decodePixel), Rec. 709 luma.
            uint32_t n = 0;
            for (int gy = 0; gy < kGeGrid; ++gy) {
                const uint32_t y = static_cast<uint32_t>(gy) * g_ge.crop / kGeGrid;
                const auto* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                for (int gx = 0; gx < kGeGrid; ++gx) {
                    const uint32_t x = static_cast<uint32_t>(gx) * g_ge.crop / kGeGrid;
                    uint32_t v;
                    std::memcpy(&v, row + size_t(x) * 4, 4);
                    const float r = geUnpackMiniFloat(v & 0x7FFu, 6);
                    const float g = geUnpackMiniFloat((v >> 11) & 0x7FFu, 6);
                    const float b = geUnpackMiniFloat((v >> 22) & 0x3FFu, 5);
                    const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    if (std::isfinite(l)) {
                        lumaSum += l;
                        if (l > lumaMax) lumaMax = l;
                        ++n;
                    }
                }
            }
            if (n) {
                haveLuma = true;
                lumaSum /= n;
            }
            ctx->Unmap(g_ge.luma.Get(), 0);
        } else if (hr == DXGI_ERROR_WAS_STILL_DRAWING && !force) {
            deferred = true;
        }
    }
    if (deferred) {
        g_ge.state = 2;
        ++g_ge.waitFrames;
        return;
    }
    if (haveExposure || haveLuma) {
        Log::get().note(
            "hud layer census: ge eye=%d exposure=%.6g hud_luma=%.6g (max %.6g, crop %u of %ux%u)",
            g_ge.eye, haveExposure ? static_cast<double>(exposure) : -1.0,
            haveLuma ? lumaSum : -1.0, static_cast<double>(lumaMax), g_ge.crop, g_ge.srcW,
            g_ge.srcH);
        g_geHaveLast = true;
        g_geLastEye = g_ge.eye;
        g_geLastExposure = exposure;
        g_geLastLuma = haveLuma ? static_cast<float>(lumaSum) : -1.0f;
        g_geLastLumaMax = lumaMax;
    }
    g_ge.state = 0;
    g_ge.exposureCopied = false;
    g_ge.lumaCopied = false;
}

// ------------------------------------------------- G-B: the tonemap itself
//
// Structure-first (eye_tonemap_snapshot.h:170-191's checks): a 3-vertex,
// 1-instance draw, one 2D render target with no depth view, blend disabled,
// PS b2 at least 256 bytes. The exact vs/ps is logged for every structure
// match (deduplicated) so a variant names itself; the FULL tonemap adds
// the measured SRV shape -- VS t0 (exposure) present, PS t0 a 3D view
// (the colour LUT), the HDR source 2D at the ps table's slot. The structure
// and the shape read are tonemap_admit.h's, shared with the crisp-HUD half of fix.ui_quality's
// re-issue so the two can never drift apart about which draw the tonemap is;
// ToneInfo is the shared ToneAdmit under this file's own name.
using ToneInfo = ToneAdmit;

bool toneInspect(ID3D11DeviceContext* ctx, uint64_t ps, ToneInfo& ti) {
    return tonemapAdmitStructure(ctx, ps, ti);
}

// One line per distinct (vs, ps) that passes the structure test, with
// everything the gate wants to know about it -- so an EDHM swap or a
// settings-tier variant names itself, and the measured pair says it is
// the measured pair.
constexpr uint32_t kToneVariantMax = 16;
struct ToneVariant {
    uint64_t vs = 0, ps = 0;
};
ToneVariant g_toneVariants[kToneVariantMax];
uint32_t g_toneVariantCount = 0;
uint32_t g_toneVariantOverflow = 0;

void noteToneVariant(uint64_t vs, uint64_t ps, const ToneInfo& ti) {
    for (uint32_t i = 0; i < g_toneVariantCount; ++i)
        if (g_toneVariants[i].vs == vs && g_toneVariants[i].ps == ps) return;
    if (g_toneVariantCount >= kToneVariantMax) {
        ++g_toneVariantOverflow;
        return;
    }
    g_toneVariants[g_toneVariantCount++] = {vs, ps};
    const char* which = (vs == kToneVsMeasured && ps == kTonePsMeasured) ? "the measured pair"
                        : (vs == kToneVsEdhm && ps == kTonePsMeasured)   ? "the EDHM swap"
                                                                           : "NEW";
    Log::get().note(
        "hud layer census: gb tonemap variant: vs=%016llX ps=%016llX (%s)%s: rtv %ux%u %s "
        "res=%p, b2=%u, vs t0=%p (exposure), ps t0=%p (lut, %s), ps t%d=%p %ux%u %s (hdr) "
        "hdr=family-target:%s",
        static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps), which,
        ti.full ? " [full match]" : " [shape only -- srv shape fails]",
        ti.rtvW, ti.rtvH, viewName(ti.rtvFmt), ti.rtvRes, ti.b2, ti.vsT0Res, ti.psT0Res,
        ti.psT0Is3D ? "3D" : "not 3D", ti.hdrSlotRead >= 0 ? ti.hdrSlotRead : 1, ti.hdrRes,
        ti.hdrW, ti.hdrH, viewName(ti.hdrFmt),
        famTargeted(ti.hdrRes) ? "yes" : "no");
}

// The design doc's bloom question: vs 953C8123AD8DC13B, believed there to
// be the cockpit's bloom composite, said by the repo's evidence to be
// FSS-only. Sightings are counted always and detailed once in a frame the
// HUD families drew in -- a cockpit sighting settles it.
bool g_bloomNoted = false;
uint32_t g_bloomSightingsSession = 0;
uint32_t g_bloomInFamilyFramesSession = 0;

void noteComposite(ID3D11DeviceContext* ctx, uint64_t ps, char kind, uint32_t count,
                   uint32_t instances, uint32_t ord, bool familyThisFrame) {
    ++g_win.bloomSightings;
    ++g_bloomSightingsSession;
    if (familyThisFrame) {
        ++g_win.bloomInFamilyFrames;
        ++g_bloomInFamilyFramesSession;
    }
    if (g_bloomNoted || !familyThisFrame) return;
    g_bloomNoted = true;
    const TargetCache& tc = targetOf();
    Log::get().note(
        "hud layer census: gb: vs 953C8123AD8DC13B SIGHTED in a cockpit frame (the HUD families "
        "drew): ord=%u ps=%016llX kind=%c count=%u instances=%u target=%ux%u %s res=%p -- the "
        "design doc's bloom-composite candidate; the repo's evidence says FSS-only, and this "
        "sighting says where it actually runs.",
        ord, static_cast<unsigned long long>(ps), kind, count, instances, tc.w, tc.h,
        viewName(tc.view), tc.resource);
}

// -------------------------------------------------- session reset / release

void releaseGpu() {
    g_gcStage.Reset();
    for (auto& s : g_gcSlots) s.pending = false;
    for (auto& p : g_gdPairs) {
        p.a.Reset();
        p.b.Reset();
        p.pending = false;
    }
    for (auto& e : g_gdDerived) e = GdDerived{};
    g_ge = GeSample{};
}

void resetSession() {
    releaseGpu();
    g_win = Window{};
    g_winStartMs = 0;
    g_frame = FrameScratch{};
    g_tc = TargetCache{};
    for (int i = 0; i < kFamilies; ++i) {
        g_famFpCount[i] = 0;
        g_famFpFullNoted[i] = false;
        g_gcSmallCbNoted[i] = false;
        for (int e = 0; e < 2; ++e) g_gcLast[i][e] = kGcNoData;
    }
    g_gcSkipped = 0;
    g_gdDisabled = false;
    for (auto& d : g_gdDecl) d = 0;
    g_gdDropped = 0;
    g_gdPendingFam = -1;
    g_gdPendingEye = -1;
    g_openGameQueryCount = 0;
    g_openGameQueryOverflow = false;
    for (auto& q : g_openGameQueries) q = nullptr;
    g_geLastQpc = LARGE_INTEGER{};
    g_geExposureNoted = false;
    g_geLumaFmtNoted = false;
    g_geHaveLast = false;
    g_toneVariantCount = 0;
    g_toneVariantOverflow = 0;
    for (auto& v : g_toneVariants) v = ToneVariant{};
    g_bloomNoted = false;
    g_bloomSightingsSession = 0;
    g_bloomInFamilyFramesSession = 0;
    g_gcWaitFrames = 0;
}

// ------------------------------------------------------------- the window

void logWindow(double secs) {
    const Window& w = g_win;
    uint32_t famSeen = 0;
    for (const auto& f : w.fam)
        if (f.draws) ++famSeen;
    if (!famSeen && !w.tonemaps) {
        Log::get().note(
            "hud layer census: window %.0fs: frames=%u (eye-draws/f<=%u) -- no HUD family draws "
            "and no tonemap (menu, loading, or on foot).",
            secs, w.frames, w.eyeDrawsMax);
        return;
    }
    Log::get().note("hud layer census: window %.0fs: frames=%u eye-draws/f<=%u.",
                    secs, w.frames, w.eyeDrawsMax);
    for (int i = 0; i < kFamilies; ++i) {
        const FamWindow& f = w.fam[i];
        if (!f.draws) continue;
        Log::get().note(
            "hud layer census: ga %s: %.1f draws/f (%u..%u) in %u/%u frames, eyes %.1f/%.1f, "
            "ord %u..%u.",
            kFamNames[i], static_cast<double>(f.draws) / f.framesWithDraws, f.minPerFrame,
            f.maxPerFrame, f.framesWithDraws, w.frames,
            static_cast<double>(f.eyeDraws[0]) / f.framesWithDraws,
            static_cast<double>(f.eyeDraws[1]) / f.framesWithDraws,
            f.firstOrdMin == ~0u ? 0 : f.firstOrdMin, f.lastOrdMax);
        const uint64_t gcN = f.gcJittered + f.gcUnjittered + f.gcOther + f.gcNotScene + f.gcAbstain;
        if (gcN) {
            const double jitteredShare =
                (f.gcJittered + f.gcUnjittered)
                    ? 100.0 * static_cast<double>(f.gcJittered) /
                          static_cast<double>(f.gcJittered + f.gcUnjittered)
                    : 0.0;
            Log::get().note(
                "hud layer census: gc %s: jitter-carried %.1f%% of decided (n=%llu: jittered "
                "%llu unjittered %llu other %llu not-scene %llu no-reference %llu).",
                kFamNames[i], jitteredShare, static_cast<unsigned long long>(gcN),
                static_cast<unsigned long long>(f.gcJittered),
                static_cast<unsigned long long>(f.gcUnjittered),
                static_cast<unsigned long long>(f.gcOther),
                static_cast<unsigned long long>(f.gcNotScene),
                static_cast<unsigned long long>(f.gcAbstain));
        }
        for (int e = 0; e < 2; ++e) {
            if (!f.gdPairs[e]) continue;
            const double rejected =
                f.gdOff[e] ? 100.0 * (1.0 - static_cast<double>(f.gdOn[e]) /
                                                static_cast<double>(f.gdOff[e]))
                           : 0.0;
            Log::get().note(
                "hud layer census: gd %s eye=%d: depth-rejected %.1f%% (on %llu, off %llu, "
                "pairs %u).",
                kFamNames[i], e, rejected, static_cast<unsigned long long>(f.gdOn[e]),
                static_cast<unsigned long long>(f.gdOff[e]), f.gdPairs[e]);
        }
    }
    Log::get().note(
        "hud layer census: gb tonemap: %u (%.2f/f), ord %u..%u, hdr=family-target %u times, "
        "srv-shape rejects %u, variants %u%s, bloom 953C8123AD8DC13B sightings %u (%u in "
        "family frames). gd dropped %u; gc skipped %u.",
        w.tonemaps, w.frames ? static_cast<double>(w.tonemaps) / w.frames : 0.0,
        w.tonemapOrdMin == ~0u ? 0 : w.tonemapOrdMin, w.tonemapOrdMax, w.tonemapHdrIsFam,
        w.tonemapRejected, g_toneVariantCount,
        g_toneVariantOverflow ? " (table full)" : "", w.bloomSightings, w.bloomInFamilyFrames,
        w.gdDropped, w.gcSkipped);
    uint32_t gdDeclTotal = 0;
    for (uint32_t d : w.gdDecl) gdDeclTotal += d;
    if (gdDeclTotal) {
        Log::get().note(
            "hud layer census: gd declines: predication %u, sample-counting query open %u, "
            "ring full %u, ps uav %u, stream-out bound %u.",
            w.gdDecl[kGdDeclPredication], w.gdDecl[kGdDeclQuery], w.gdDecl[kGdDeclRing],
            w.gdDecl[kGdDeclUav], w.gdDecl[kGdDeclSo]);
    }
    if (g_geHaveLast) {
        Log::get().note("hud layer census: ge last: eye=%d exposure=%.6g hud_luma=%.6g (max %.6g).",
                        g_geLastEye, static_cast<double>(g_geLastExposure),
                        static_cast<double>(g_geLastLuma), static_cast<double>(g_geLastLumaMax));
    }
}

}  // namespace

void hudLayerCensusConfigure(Config& cfg) {
    const std::string v = cfg.getString("advanced.hud_census", "off");
    const bool on = v == "on" || v == "1" || v == "true" || v == "yes";
    if (on == detail::g_hudLayerCensusOn) return;
    if (on) {
        resetSession();
        detail::g_hudLayerCensusOn = true;
        Log::get().note(
            "hud layer census: armed (advanced.hud_census) -- Phase 0 gates G-A..G-E of "
            "docs/cockpit-hud-layer-design-2026-09-27.md. Observation only: the game's draws, "
            "bindings and buffers are unchanged (the G-D pair re-issues colourlessly, writes "
            "masked, and restores everything).");
    } else {
        detail::g_hudLayerCensusOn = false;
        releaseGpu();
        Log::get().note("hud layer census: disarmed.");
    }
}

bool hudLayerCensusEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances,
                           uint32_t eyeDrawIndex, const DrawArgs& args) {
    if (!ctx) return false;
    FrameScratch& f = g_frame;
    if (eyeDrawIndex > f.eyeDrawsMax) f.eyeDrawsMax = eyeDrawIndex;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    const int fam = familyOfVs(vs);
    if (fam >= 0) {
        f.anyFamily = true;
        FrameScratch::Fam& fs = f.fam[fam];
        ++fs.draws;
        if (eyeDrawIndex < fs.firstOrd) fs.firstOrd = eyeDrawIndex;
        if (eyeDrawIndex > fs.lastOrd) fs.lastOrd = eyeDrawIndex;
        const TargetCache& tc = targetOf();
        int eye = -1;
        if (tc.ok) eye = uiDepthEyeOfTarget(tc.resource, tc.w, tc.h, tc.fmt);
        if (eye >= 0 && eye < 2) ++fs.eyeDraws[eye];
        if (tc.ok) noteFamTarget(tc.resource);
        const uint64_t ps = bindingShaderHash(BindSlot::Ps);
        noteFamilyState(ctx, fam, eye, eyeDrawIndex, tc, vs, ps);
        gcOnFamilyDraw(ctx, fam, eye, eyeDrawIndex);
        if (!g_gdDisabled && eye >= 0 && eye < 2 && f.gdBudget[fam][eye] > 0) {
            --f.gdBudget[fam][eye];
            g_gdPendingFam = fam;
            g_gdPendingEye = eye;
            return true;
        }
        return false;
    }
    if (vs == kCompositeVs) {
        noteComposite(ctx, bindingShaderHash(BindSlot::Ps), kind, count, instances, eyeDrawIndex,
                      f.anyFamily);
    }
    // The tonemap, structure-first: the measured pair, the EDHM swap and
    // anything of the same shape all land here, and the variant line says
    // which. The 3-vertex prefilter is register-cheap; the state reads
    // happen for the handful of full-screen triangles a frame that pass it.
    if ((kind == 'D' || kind == 'N') && count == 3 && instances == 1 && args.startInstance == 0) {
        const uint64_t ps = bindingShaderHash(BindSlot::Ps);
        ToneInfo ti;
        if (toneInspect(ctx, ps, ti)) {
            noteToneVariant(vs, ps, ti);
            if (ti.full) {
                int eye = -1;
                if (ti.rtvRes) {
                    ResourceInfo ri;
                    if (bindingResolveResource(ti.rtvRes, &ri) && ri.isTexture2D) {
                        eye = uiDepthEyeOfTarget(ri.resource, ri.a, ri.b, ri.fmt);
                    }
                }
                if (f.tonemapCount < 4) {
                    const uint32_t t = f.tonemapCount++;
                    f.tonemapOrd[t] = eyeDrawIndex;
                    f.tonemapEye[t] = eye;
                    f.tonemapHdrIsFam[t] = famTargeted(ti.hdrRes);
                }
                geOnTonemap(ctx, eye);
            } else {
                ++g_win.tonemapRejected;
            }
        }
    }
    return false;
}

bool hudLayerCensusGdBegin(ID3D11DeviceContext* ctx, HudCensusGdSave& save) {
    if (g_gdDisabled || !ctx) return false;
    const int fam = g_gdPendingFam, eye = g_gdPendingEye;
    g_gdPendingFam = -1;
    g_gdPendingEye = -1;
    if (fam < 0) return false;
    bool ok = false;
    const bool ran = guarded("hudCensus.gdBegin", [&] {
        // A predicated draw: the re-issue would count the predicate's
        // verdict, not the family's (night_vision.cpp's own decline).
        Ptr<ID3D11Predicate> pred;
        BOOL predValue = FALSE;
        ctx->GetPredication(&pred, &predValue);
        if (pred) {
            ++g_gdDecl[kGdDeclPredication];
            return;
        }
        // A sample-counting game query open on this context: the re-issue
        // would feed its counter, and changing what the game measures
        // changes what it draws a frame later.
        if (g_openGameQueryCount || g_openGameQueryOverflow) {
            ++g_gdDecl[kGdDeclQuery];
            return;
        }
        int slot = -1;
        for (uint32_t i = 0; i < kGdPairRing; ++i) {
            if (g_gdPairs[i].pending) continue;
            if (!g_gdPairs[i].a) {
                D3D11_QUERY_DESC qd{};
                qd.Query = D3D11_QUERY_OCCLUSION;
                Ptr<ID3D11Device> dev;
                ctx->GetDevice(&dev);
                if (!dev || FAILED(dev->CreateQuery(&qd, &g_gdPairs[i].a)) ||
                    FAILED(dev->CreateQuery(&qd, &g_gdPairs[i].b))) {
                    gdDisable("occlusion query creation failed");
                    return;
                }
            }
            slot = static_cast<int>(i);
            break;
        }
        if (slot < 0) {
            ++g_gdDecl[kGdDeclRing];  // every pair still awaits its GPU result
            return;
        }
        ID3D11RenderTargetView* rawRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rawRtvs, &save.dsv);
        for (uint32_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) save.rtvs[i].Attach(rawRtvs[i]);
        // A PS UAV would still be written with the colour targets unbound;
        // the families have none measured, but decline rather than presume.
        ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0,
                                                       D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
        bool anyUav = false;
        for (auto* u : uavs) {
            if (u) {
                anyUav = true;
                u->Release();
            }
        }
        if (anyUav) {
            ++g_gdDecl[kGdDeclUav];
            gdReleaseSave(save);
            return;
        }
        // A stream-out binding would take the re-issue's vertices a second
        // time; the families have none measured, but decline rather than
        // presume (the same terms as the PS UAV check above).
        ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT] = {};
        ctx->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so);
        bool anySo = false;
        for (auto* b : so) {
            if (b) {
                anySo = true;
                b->Release();
            }
        }
        if (anySo) {
            ++g_gdDecl[kGdDeclSo];
            gdReleaseSave(save);
            return;
        }
        ctx->OMGetDepthStencilState(&save.dss, &save.stencilRef);
        GdDerived* der = gdDerivedFor(ctx, save.dss.Get(), save.stencilRef);
        if (!der) {
            gdDisable("a depth-stencil state clone or a fifth distinct state failed");
            gdReleaseSave(save);
            return;
        }
        // No colour target, the game's own depth view kept: the re-issue's
        // depth test runs against the same buffer, its colour lands
        // nowhere, and the write-masked clone writes nothing even there.
        vScreenSetRenderTargetsRaw(ctx, 0, nullptr, save.dsv.Get());
        ctx->OMSetDepthStencilState(der->writeMasked.Get(), save.stencilRef);
        ctx->Begin(g_gdPairs[slot].a.Get());
        save.slot = slot;
        save.aOpen = true;
        g_gdPairs[slot].fam = fam;
        g_gdPairs[slot].eye = eye;
        ok = true;
    });
    if (!ran) {
        hudLayerCensusGdEnd(ctx, save);  // restore whatever Begin touched, first
        gdDisable("a fault while setting up an occlusion pair");
        return false;
    }
    if (!ok) save.slot = -1;
    return ok;
}

void hudLayerCensusGdSwapToDepthOff(ID3D11DeviceContext* ctx, HudCensusGdSave& save) {
    if (save.slot < 0) return;
    const int slot = save.slot;
    const bool ran = guarded("hudCensus.gdSwap", [&] {
        GdDerived* der = gdDerivedFor(ctx, save.dss.Get(), save.stencilRef);
        // Null would mean the entry Begin made is gone -- it cannot be, and
        // a null STATE here would be D3D11's default (depth on, writing),
        // so the pair simply does not continue; GdEnd restores.
        if (!der) return;
        ctx->End(g_gdPairs[slot].a.Get());
        save.aOpen = false;
        ctx->OMSetDepthStencilState(der->testsOff.Get(), save.stencilRef);
        ctx->Begin(g_gdPairs[slot].b.Get());
        save.bOpen = true;
    });
    if (!ran) gdDisable("a fault mid occlusion pair");
}

void hudLayerCensusGdEnd(ID3D11DeviceContext* ctx, HudCensusGdSave& save) {
    if (save.slot < 0) {
        gdReleaseSave(save);
        return;
    }
    const int slot = save.slot;
    GdPair& pair = g_gdPairs[slot];
    bool complete = false;
    const bool ran = guarded("hudCensus.gdEnd", [&] {
        if (save.aOpen) {
            ctx->End(pair.a.Get());
            save.aOpen = false;
        }
        if (save.bOpen) {
            ctx->End(pair.b.Get());
            save.bOpen = false;
            complete = true;
        }
        // Restore everything the pair touched, always: the game's own draw
        // runs next and must find its bindings exactly as they were.
        ctx->OMSetDepthStencilState(save.dss.Get(), save.stencilRef);
        uint32_t n = 0;
        for (uint32_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            if (save.rtvs[i]) n = i + 1;
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        for (uint32_t i = 0; i < n; ++i) rtvs[i] = save.rtvs[i].Get();
        vScreenSetRenderTargetsRaw(ctx, n, rtvs, save.dsv.Get());
    });
    if (!ran) gdDisable("a fault restoring state after an occlusion pair");
    if (complete) {
        pair.pending = true;
        pair.waitFrames = 0;
    } else {
        ++g_gdDropped;
    }
    save.slot = -1;
    gdReleaseSave(save);
}

void hudLayerCensusNoteGameQuery(bool begin, void* async) {
    if (!async || isOwnQuery(async)) return;
    if (begin) {
        for (uint32_t i = 0; i < g_openGameQueryCount; ++i)
            if (g_openGameQueries[i] == async) return;
        // Only sample-counting types reach the open list (gdQueryTypeBlocks
        // says why); an End for an ignored type simply finds nothing below.
        // A query whose type cannot be read blocks: the wrong direction to
        // guess is the one that feeds the game's counter.
        bool blocks = true;
        Ptr<ID3D11Query> query;
        if (SUCCEEDED(reinterpret_cast<ID3D11Asynchronous*>(async)->QueryInterface(
                IID_PPV_ARGS(&query))) &&
            query) {
            D3D11_QUERY_DESC qd{};
            query->GetDesc(&qd);
            blocks = gdQueryTypeBlocks(qd.Query);
        }
        if (!blocks) return;
        if (g_openGameQueryCount < 16) {
            g_openGameQueries[g_openGameQueryCount++] = async;
        } else {
            g_openGameQueryOverflow = true;
        }
    } else {
        for (uint32_t i = 0; i < g_openGameQueryCount; ++i)
            if (g_openGameQueries[i] == async) {
                g_openGameQueries[i] = g_openGameQueries[--g_openGameQueryCount];
                g_openGameQueries[g_openGameQueryCount] = nullptr;
                return;
            }
    }
}

void hudLayerCensusFrameBoundary(ID3D11DeviceContext* ctx) {
    if (!detail::g_hudLayerCensusOn) return;
    if (ctx) {
        gdPoll(ctx);
        gcPoll(ctx);
        geResolve(ctx);
    }
    Window& w = g_win;
    ++w.frames;
    if (g_frame.eyeDrawsMax > w.eyeDrawsMax) w.eyeDrawsMax = g_frame.eyeDrawsMax;
    for (int i = 0; i < kFamilies; ++i) {
        const FrameScratch::Fam& fs = g_frame.fam[i];
        if (!fs.draws) continue;
        FamWindow& fw = w.fam[i];
        fw.draws += fs.draws;
        ++fw.framesWithDraws;
        if (fs.draws < fw.minPerFrame) fw.minPerFrame = fs.draws;
        if (fs.draws > fw.maxPerFrame) fw.maxPerFrame = fs.draws;
        if (fs.firstOrd < fw.firstOrdMin) fw.firstOrdMin = fs.firstOrd;
        if (fs.lastOrd > fw.lastOrdMax) fw.lastOrdMax = fs.lastOrd;
        fw.eyeDraws[0] += fs.eyeDraws[0];
        fw.eyeDraws[1] += fs.eyeDraws[1];
    }
    for (uint32_t t = 0; t < g_frame.tonemapCount; ++t) {
        ++w.tonemaps;
        if (g_frame.tonemapOrd[t] < w.tonemapOrdMin) w.tonemapOrdMin = g_frame.tonemapOrd[t];
        if (g_frame.tonemapOrd[t] > w.tonemapOrdMax) w.tonemapOrdMax = g_frame.tonemapOrd[t];
        if (g_frame.tonemapHdrIsFam[t]) ++w.tonemapHdrIsFam;
    }
    for (uint32_t i = 0; i < kGdDeclCount; ++i) {
        w.gdDecl[i] += g_gdDecl[i];
        g_gdDecl[i] = 0;
    }
    w.gdDropped += g_gdDropped;
    g_gdDropped = 0;
    w.gcSkipped += g_gcSkipped;
    g_gcSkipped = 0;
    g_frame = FrameScratch{};
    const uint64_t now = GetTickCount64();
    if (!g_winStartMs) g_winStartMs = now;
    if (now - g_winStartMs < kTotalsMs) return;
    logWindow(static_cast<double>(now - g_winStartMs) / 1000.0);
    g_win = Window{};
    g_winStartMs = now;
}

void hudLayerCensusShutdown() {
    const bool measured = g_win.frames > 0 || g_toneVariantCount > 0 || g_bloomSightingsSession > 0;
    if (measured) {
        Log::get().note(
            "hud layer census: session over: %u tonemap variants, bloom sightings %u (%u in "
            "family frames).",
            g_toneVariantCount, g_bloomSightingsSession, g_bloomInFamilyFramesSession);
    }
    detail::g_hudLayerCensusOn = false;
    releaseGpu();
    resetSession();
}

}  // namespace edvr

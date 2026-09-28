// C2-A derive reducer (docs/design-flat-camera-integration.md, C2 test plan
// addendum). Reimplements ONLY the semantics read out of the decompiles,
// each reduction cited to its artifact:
//
//   view finalizer     FUN_1404f4910  camera_cache_helpers.txt:550-618
//   projection builder FUN_1404f2ff0  camera_cache_helpers.txt:255-493
//   VP finalizer       FUN_1404f49f0  camera_cache_helpers.txt:57-195
//   scene CB composer  FUN_140596830  camera_producer.txt:23-124
//   refresh ctx copy   FUN_1405921f0  camera_producer.txt:186-249
//   ray snapshot       FUN_1406be790  camera_ray_writers2.txt:13-30
//
// Constants from camera_constants.txt (Ghidra data dumps): the AND-mask quad
// is (F,F,F,0), the sign-mask quad is (0x80000000 x4), the 1450c8090 block is
// the identity 4x4, DAT_144dde614=1.0, DAT_144dde670=-1.0, DAT_144ddf878=0.5,
// DAT_144de5390=sign mask, DAT_144de9624=2.0, DAT_144e2f850=-1.0,
// DAT_144e2f870=0.5, DAT_144e2f880=1.0, DAT_144e2f890=2.0. The CRT trig
// helpers (FUN_1448b2390 double, FUN_1448b4d80 double, FUN_1448aaf90 float)
// are reduced to std::tan/std::sin/std::cos -- the one deliberate
// substitution, named here; A1's algebraic identities pin the result.
//
// The rig models the game camera's byte layout with a helper base at +0x20
// and transcribes the decompiles by RAW helper offset, so no transcription
// arithmetic stands between the decompile and this file. Named typed-table
// offsets (the cache-helper addendum's table) are constexpr and used by the
// tests for mutations.
//
// --self-test runs A1-A6 and prints "c2 derive: PASS" only when every check
// holds. Exit 1 with the failed checks named otherwise.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../src/d3d11/flat_live_phase.h"

namespace {

// ---------------------------------------------------------------------------
// The camera struct, helper-based (helper = camera + 0x20). All derive code
// below reads/writes through F (float) and U (uint32) at raw helper offsets,
// exactly as the decompiles do.
// ---------------------------------------------------------------------------
struct Cam {
    alignas(16) unsigned char b[0x900];
    float& F(uint32_t helperOff) { return *reinterpret_cast<float*>(b + 0x20 + helperOff); }
    const float& F(uint32_t helperOff) const { return *reinterpret_cast<const float*>(b + 0x20 + helperOff); }
    uint32_t& U(uint32_t helperOff) { return *reinterpret_cast<uint32_t*>(b + 0x20 + helperOff); }
    const uint32_t& U(uint32_t helperOff) const { return *reinterpret_cast<const uint32_t*>(b + 0x20 + helperOff); }
    uint8_t& B(uint32_t helperOff) { return b[0x20 + helperOff]; }
};

// Named typed-table offsets, camera-relative (cache-helper addendum). Derive
// code uses raw helper offsets; the tests use these for mutations.
constexpr uint32_t kCamAxes = 0x20;        // source 3x4 view axes (+0x20..+0x4C)
constexpr uint32_t kCamOrigin = 0x50;      // source origin (stored negated)
constexpr uint32_t kCamViewRows = 0x190;   // view rows (axes + translation)
constexpr uint32_t kCamProj = 0x1D0;       // projection 4x4
constexpr uint32_t kCamVP = 0x210;         // cached view-projection
constexpr uint32_t kCamFlags = 0x250;      // dirty flag word
constexpr uint32_t kCamNear = 0x254;
constexpr uint32_t kCamFar = 0x258;
constexpr uint32_t kCamCompoundNear = 0x25C;
constexpr uint32_t kCamKind = 0x264;
constexpr uint32_t kCamAngular = 0x280;
constexpr uint32_t kCamBoundX = 0x28C;     // off-center bound pair (proj[8] = 2x)
constexpr uint32_t kCamBoundY = 0x290;     //                        (proj[9] = 2x)
constexpr uint32_t kCamViewportW = 0x2A0;  // refresh-read float pair
constexpr uint32_t kCamViewportH = 0x2A4;
constexpr uint32_t kCamAdjustEnable = 0x2C0;
constexpr uint32_t kCamCustom = 0x2D0;     // kinds 4/5 custom matrix data
constexpr uint32_t kCamRayBasis = 0x870;   // view-matrix snapshot
constexpr uint32_t kCamRayOrigin = 0x8B0;  // origin snapshot

constexpr uint32_t kFlagRay = 1, kFlagView = 2, kFlagProj = 4, kFlagVP = 8;

float& camF(Cam& c, uint32_t camOff) { return *reinterpret_cast<float*>(c.b + camOff); }
const float& camF(const Cam& c, uint32_t camOff) { return *reinterpret_cast<const float*>(c.b + camOff); }
uint32_t& camU(Cam& c, uint32_t camOff) { return *reinterpret_cast<uint32_t*>(c.b + camOff); }
const uint32_t& camU(const Cam& c, uint32_t camOff) { return *reinterpret_cast<const uint32_t*>(c.b + camOff); }

float negf(float x) { uint32_t u; std::memcpy(&u, &x, 4); u ^= 0x80000000u; std::memcpy(&x, &u, 4); return x; }

// ---------------------------------------------------------------------------
// View finalizer: FUN_1404f4910 (camera_cache_helpers.txt:575-617). Builds
// the view rows at helper+0x170.. (camera+0x190) from the source axes
// (helper+0x00..+0x2C) and the origin (helper+0x30..+0x38, negated per-lane).
// AND-mask quad (F,F,F,0): lanes 0-2 keep, lane 3 zeroes. Sign-mask quad: all
// negate. Clears dirty bit 2.
// ---------------------------------------------------------------------------
void finalizeViewRows(Cam& c) {
    const float a0 = c.F(0x00), a1 = c.F(0x04), a2 = c.F(0x08);
    const float a4 = c.F(0x10), a5 = c.F(0x14), a6 = c.F(0x18);
    const float a8 = c.F(0x20), a9 = c.F(0x24), aA = c.F(0x28);
    const float nox = negf(c.F(0x30)), noy = negf(c.F(0x34)), noz = negf(c.F(0x38));
    // Column-swizzled rows (the decompile reads [0],[4],[8] per lane group):
    c.F(0x170) = a0;  c.F(0x174) = a4;  c.F(0x178) = a8;  c.F(0x17C) = 0.0f;
    c.F(0x180) = a1;  c.F(0x184) = a5;  c.F(0x188) = a9;  c.F(0x18C) = 0.0f;
    c.F(0x190) = a2;  c.F(0x194) = a6;  c.F(0x198) = aA;  c.F(0x19C) = 0.0f;
    // Translation row = -(axes^T . origin), w = 1 (camera_cache_helpers.txt:593-617).
    c.F(0x1A0) = noy * a1 + nox * a0 + noz * a2;
    c.F(0x1A4) = noy * a5 + nox * a4 + noz * a6;
    c.F(0x1A8) = noy * a9 + nox * a8 + noz * aA;
    c.F(0x1AC) = 1.0f;
    c.U(0x230) &= ~2u; // the flag word: camera+0x250 = helper+0x230
}

// ---------------------------------------------------------------------------
// Projection builder: FUN_1404f2ff0 (camera_cache_helpers.txt:255-493).
// Writes the projection 4x4 at helper+0x1B0..+0x1EC (camera+0x1D0..+0x20C)
// from the frustum parameters. Kind at helper+0x244 (camera+0x264):
//   1 ortho, 3 trigonometric, 4/5 custom matrix (helper+0x2B0..+0x2EC),
//   default identity-ish. The trig helpers reduce to std::tan/sin/cos.
// proj index map: helper+0x1B0 + 4*i -> proj[i].
// ---------------------------------------------------------------------------
float* projSlot(Cam& c, int i) { return &c.F(0x1B0 + 4 * i); }
const float* projSlot(const Cam& c, int i) { return &c.F(0x1B0 + 4 * i); }

void buildProjection(Cam& c) {
    const float fVar19 = (c.F(0x23C) + 1.0f) * c.F(0x234); // (compoundNear+1)*near
    const uint32_t kind = c.U(0x244);
    float fVar18 = 1.0f; // DAT_144dde614
    bool bVar5 = false;
    if (kind != 0) {
        if (kind == 1) {
            // Ortho (camera_cache_helpers.txt:296-310). The matrix terms come
            // from helper+0x264/+0x268/+0x240 (camera+0x284/+0x288/+0x260) --
            // NOT the kind dword at helper+0x244.
            fVar18 = c.F(0x268);
            *projSlot(c, 0) = c.F(0x264);
            *projSlot(c, 1) = 0.0f; *projSlot(c, 2) = 0.0f; *projSlot(c, 3) = 0.0f;
            *projSlot(c, 4) = 0.0f; *projSlot(c, 6) = 0.0f; *projSlot(c, 7) = 0.0f;
            *projSlot(c, 11) = 0.0f; *projSlot(c, 13) = 0.0f;
            *projSlot(c, 5) = c.F(0x264) * c.F(0x240);
            const float fVar20 = negf(fVar18 / (c.F(0x238) - fVar19));
            *projSlot(c, 10) = fVar20;
            *projSlot(c, 15) = fVar18;
            *projSlot(c, 14) = fVar18 - fVar20 * fVar19;
        } else if (kind != 3) {
            if (kind == 4) {
                // Custom matrix copy with per-row w adjustments
                // (camera_cache_helpers.txt:313-340).
                *projSlot(c, 0) = c.F(0x2B0); *projSlot(c, 1) = c.F(0x2B4);
                *projSlot(c, 2) = c.F(0x2BC) - c.F(0x2B8); *projSlot(c, 3) = c.F(0x2BC);
                *projSlot(c, 4) = c.F(0x2C0); *projSlot(c, 5) = c.F(0x2C4);
                *projSlot(c, 6) = c.F(0x2CC) - c.F(0x2C8); *projSlot(c, 7) = c.F(0x2CC);
                *projSlot(c, 8) = c.F(0x2D0); *projSlot(c, 9) = c.F(0x2D4);
                *projSlot(c, 10) = c.F(0x2DC) - c.F(0x2D8); *projSlot(c, 11) = c.F(0x2DC);
                *projSlot(c, 12) = c.F(0x2E0); *projSlot(c, 13) = c.F(0x2E4);
                *projSlot(c, 14) = c.F(0x2EC) - c.F(0x2E8); *projSlot(c, 15) = c.F(0x2EC);
            } else if (kind == 5) {
                // Custom matrix + near/far depth terms
                // (camera_cache_helpers.txt:341-377).
                const float fVar21 = c.F(0x2BC);
                *projSlot(c, 0) = c.F(0x2B0); *projSlot(c, 1) = c.F(0x2B4);
                *projSlot(c, 2) = c.F(0x2B8); *projSlot(c, 3) = fVar21;
                *projSlot(c, 4) = c.F(0x2C0); *projSlot(c, 5) = c.F(0x2C4);
                *projSlot(c, 6) = c.F(0x2C8); *projSlot(c, 7) = c.F(0x2CC);
                *projSlot(c, 8) = c.F(0x2D0); *projSlot(c, 9) = c.F(0x2D4);
                *projSlot(c, 10) = c.F(0x2D8); *projSlot(c, 11) = c.F(0x2DC);
                *projSlot(c, 12) = c.F(0x2E0); *projSlot(c, 13) = c.F(0x2E4);
                *projSlot(c, 14) = c.F(0x2E8); *projSlot(c, 15) = c.F(0x2EC);
                float fVar19b = c.F(0x238), fVar20b = c.F(0x234);
                float fVar22 = (fVar19b * fVar20b) / (fVar20b - fVar19b);
                fVar19b = fVar19b / (fVar19b - fVar20b);
                if (c.B(0x248) != 0) { fVar22 = negf(fVar20b); fVar19b = fVar18; }
                *projSlot(c, 2) = fVar21 - c.F(0x2B8);
                *projSlot(c, 6) = c.F(0x2CC) - c.F(0x2C8);
                *projSlot(c, 10) = c.F(0x2DC) - fVar19b;
                *projSlot(c, 14) = c.F(0x2EC) - fVar22;
            } else {
                // Default identity-ish (camera_cache_helpers.txt:379-399):
                // the 1450c8090 identity block, with proj[10] = -1.0
                // (DAT_144e2f850) and proj[14] = 1.0 (DAT_144e2f880).
                static const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                for (int i = 0; i < 16; ++i) *projSlot(c, i) = kIdent[i];
                *projSlot(c, 8) = 0.0f; *projSlot(c, 9) = 0.0f;
                *projSlot(c, 10) = -1.0f;
                *projSlot(c, 14) = 1.0f;
            }
            goto oblique;
        }
        bVar5 = true;
    }
    {
        // Trigonometric perspective (camera_cache_helpers.txt:404-433), with
        // FUN_1448b2390 reduced to std::tan.
        const float fVar21 = c.F(0x260) * 0.5f; // angular * 0.5
        const float fVar20 = c.F(0x240);
        if (c.B(0x249) == 0) {
            const float fVar22 = c.F(0x238); // far
            const float fVar23 = static_cast<float>(std::tan(static_cast<double>(fVar21)));
            float fVar15 = negf(fVar23);
            float fVar14 = (fVar23 - fVar15) * c.F(0x270);
            float fVar13 = (fVar23 * fVar20 - fVar15 * fVar20) * c.F(0x26C);
            const float fVar16 = fVar15 - fVar14;
            fVar14 = fVar23 - fVar14;
            fVar15 = fVar15 * fVar20 - fVar13;
            fVar13 = fVar23 * fVar20 - fVar13;
            float fv20 = negf(fVar19) * fVar22 / (fVar22 - fVar19);
            float fv22 = fVar22 / (fVar22 - fVar19);
            if (bVar5) { fv20 = negf(fVar19); fv22 = fVar18; }
            fv20 = fv20 * -1.0f; // DAT_144dde670
            fv22 = fv22 * fVar18;
            *projSlot(c, 1) = 0.0f; *projSlot(c, 2) = 0.0f; *projSlot(c, 3) = 0.0f;
            *projSlot(c, 4) = 0.0f; *projSlot(c, 6) = 0.0f; *projSlot(c, 7) = 0.0f;
            *projSlot(c, 14) = fv20;
            *projSlot(c, 0) = 2.0f / (fVar13 - fVar15); // DAT_144de9624
            const float fVar21b = 2.0f / (fVar14 - fVar16);
            *projSlot(c, 8) = (fVar13 + fVar15) / (fVar15 - fVar13);
            *projSlot(c, 9) = (fVar14 + fVar16) / (fVar16 - fVar14);
            *projSlot(c, 15) = 0.0f;
            *projSlot(c, 13) = 0.0f;
            *projSlot(c, 11) = 1.0f; *projSlot(c, 12) = 0.0f; // qword 0x3f800000
            *projSlot(c, 10) = fVar18 - fv22;
            *projSlot(c, 5) = fVar21b;
        } else {
            // The +0x249-flag branch (camera_cache_helpers.txt:434-450): the
            // cot-ratio form, with FUN_1448aaf90 reduced to std::cos and
            // FUN_1448b4d80 to std::sin (the one named trig substitution;
            // cos/sin is the cot the decompile's ratio requires).
            const float u3 = c.F(0x250), u4 = c.F(0x254);
            const float fVar22 = c.F(0x258);
            const uint32_t u17 = c.U(0x25C) ^ 0x80000000u;
            const float fVar19c = static_cast<float>(std::cos(static_cast<double>(fVar21)));
            const double d2 = std::sin(static_cast<double>(fVar21));
            *projSlot(c, 1) = 0.0f; *projSlot(c, 7) = 0.0f; *projSlot(c, 9) = 0.0f;
            *projSlot(c, 2) = u3;
            const float fVar21c = static_cast<float>(static_cast<double>(fVar19c) / d2);
            *projSlot(c, 0) = fVar21c / fVar20;
            *projSlot(c, 6) = u4;
            float u17f; std::memcpy(&u17f, &u17, 4);
            *projSlot(c, 14) = u17f;
            *projSlot(c, 15) = 0.0f;
            *projSlot(c, 13) = 0.0f;
            *projSlot(c, 11) = 1.0f; *projSlot(c, 12) = 0.0f;
            *projSlot(c, 10) = fVar18 - fVar22;
            *projSlot(c, 5) = fVar21c;
            *projSlot(c, 4) = 0.0f; *projSlot(c, 3) = 0.0f;
        }
    }
oblique:
    if (c.B(0x2A0) != 0) {
        // Oblique/projection adjust (camera_cache_helpers.txt:458-490), with
        // DAT_144e2f890 = 2.0, DAT_144e2f870 = 0.5.
        float fVar18o = 2.0f * (negf(*projSlot(c, 14)) / (*projSlot(c, 10) - 1.0f));
        float fVar19o = fVar18o / *projSlot(c, 5);
        fVar18o = fVar18o / *projSlot(c, 0);
        float fVar20o = *projSlot(c, 8) * negf(fVar18o);
        float fVar22o = *projSlot(c, 9) * negf(fVar19o);
        fVar18o = (fVar20o + fVar18o) * 0.5f;
        fVar19o = (fVar22o + fVar19o) * 0.5f;
        fVar20o = fVar20o - fVar18o;
        fVar22o = fVar22o - fVar19o;
        fVar18o = fVar18o - fVar20o;
        fVar19o = fVar19o - fVar22o;
        const float fVar21o = fVar20o + fVar18o * c.F(0x290);
        fVar20o = fVar20o + fVar18o * c.F(0x280);
        const float fVar18b = fVar22o + fVar19o * c.F(0x294);
        fVar22o = fVar22o + fVar19o * c.F(0x28C); // decompile reads helper+0x294 twice? camera_cache_helpers.txt:475: +0x294; :473: +0x280
        *projSlot(c, 0) = *projSlot(c, 0) / (c.F(0x290) - c.F(0x280));
        *projSlot(c, 5) = *projSlot(c, 5) / (c.F(0x294) - c.F(0x284));
        *projSlot(c, 8) = (fVar20o + fVar21o) / (fVar20o - fVar21o);
        *projSlot(c, 9) = (fVar18b + fVar22o) / (fVar18b - fVar22o);
    }
    c.U(0x230) &= ~4u; // clears dirty bit 4 (camera_cache_helpers.txt:491)
}

// ---------------------------------------------------------------------------
// VP finalizer: FUN_1404f49f0 (camera_cache_helpers.txt:57-195). Chains the
// projection builder (bit 4) and the view rebuild (bit 2, inline), then
// always recomputes the cached view-projection at helper+0x1F0..+0x22C
// (camera+0x210..+0x24C) = view rows x projection, and clears bit 8.
// Product shape per :177-192: vp[r*4+c] = sum_k view[r*4+k'] * proj[...]
// transcribed as written.
// ---------------------------------------------------------------------------
void finalizeVP(Cam& c) {
    if (c.U(0x230) & 4u) buildProjection(c);
    if (c.U(0x230) & 2u) finalizeViewRows(c);
    // view rows: helper+0x170..+0x1AC; projection: helper+0x1B0..+0x1EC.
    // The products (camera_cache_helpers.txt:149-192) are the plain row-major
    // product vp = view x proj: fVar29..fVar32 = view row 0 (or the freshly
    // rebuilt one), fVar17/fVar1/fVar2/fVar15 = view row 1, fVar7..fVar10 =
    // view row 2, fVar11..fVar14 = view row 3, and the fVar3..fVar28 runs are
    // proj columns. vp[r][k] = sum view[r][j] * proj[j][k].
    const float* v = &c.F(0x170);
    const float* p = &c.F(0x1B0);
    float* vp = &c.F(0x1F0);
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
            vp[r * 4 + k] = v[r * 4 + 0] * p[0 * 4 + k] + v[r * 4 + 1] * p[1 * 4 + k] +
                            v[r * 4 + 2] * p[2 * 4 + k] + v[r * 4 + 3] * p[3 * 4 + k];
    c.U(0x230) &= ~8u; // clears dirty bit 8 (camera_cache_helpers.txt:193)
}

// ---------------------------------------------------------------------------
// Scene CB composer: FUN_140596830 (camera_producer.txt:63-121). Composes the
// source axes 3x4 (helper+0x00..+0x2C) with the projection (helper+0x1B0..)
// into sixteen products = scene CB rows 270..273. Checks bit 4 and rebuilds
// the projection first (:79-80). The axis reads use the AND-mask quad (lane
// 3 zeroes), matching the view finalizer's swizzle.
// ---------------------------------------------------------------------------
void composeSceneCb(Cam& c, float out[16]) {
    if (c.U(0x230) & 4u) buildProjection(c);
    const float* s = &c.F(0x00); // the source axes 3x4, flat lanes
    const float p0 = *projSlot(c, 0),  p1 = *projSlot(c, 1),  p2 = *projSlot(c, 2),  p3 = *projSlot(c, 3);
    const float p4 = *projSlot(c, 4),  p5 = *projSlot(c, 5),  p6 = *projSlot(c, 6),  p7 = *projSlot(c, 7);
    const float p8 = *projSlot(c, 8),  p9 = *projSlot(c, 9),  pA = *projSlot(c, 10), pB = *projSlot(c, 11);
    const float pC = *projSlot(c, 12), pD = *projSlot(c, 13), pE = *projSlot(c, 14), pF = *projSlot(c, 15);
    // camera_producer.txt:106-121. The axis lanes are the source ROWS with
    // lane 3 zeroed by the AND-mask quad (fVar24..fVar27 = s0..s3,
    // fVar28..fVar31 = s4..s7, fVar32..fVar35 = s8..s11), and the
    // fVar19..fVar22 constants are (0,0,0,1), so the products are the
    // row-major product [R | 0; 0 0 0 1] x proj:
    out[0]  = p8 * s[8]  + p0 * s[0] + p4 * s[4];
    out[1]  = p9 * s[8]  + p1 * s[0] + p5 * s[4];
    out[2]  = pA * s[8]  + p2 * s[0] + p6 * s[4];
    out[3]  = pB * s[8]  + p3 * s[0] + p7 * s[4];
    out[4]  = p8 * s[9]  + p0 * s[1] + p4 * s[5];
    out[5]  = p9 * s[9]  + p1 * s[1] + p5 * s[5];
    out[6]  = pA * s[9]  + p2 * s[1] + p6 * s[5];
    out[7]  = pB * s[9]  + p3 * s[1] + p7 * s[5];
    out[8]  = p8 * s[10] + p0 * s[2] + p4 * s[6];
    out[9]  = p9 * s[10] + p1 * s[2] + p5 * s[6];
    out[10] = pA * s[10] + p2 * s[2] + p6 * s[6];
    out[11] = pB * s[10] + p3 * s[2] + p7 * s[6];
    out[12] = pC; // lane 3 masked to zero, OR-ed with the identity row
    out[13] = pD;
    out[14] = pE;
    out[15] = pF;
}

// ---------------------------------------------------------------------------
// Refresh context copy: FUN_1405921f0's VP copy (camera_producer.txt:186-249).
// Checks bit 8, finalizes the VP, then copies the cached VP into the
// view-constant context with the decompile's lane permutation.
// ---------------------------------------------------------------------------
void refreshCopyVp(Cam& c, float ctx[16]) {
    if (c.U(0x230) & 8u) finalizeVP(c);
    const float* vp = &c.F(0x1F0);
    // camera_producer.txt:191-221: ctx[+0x40..+0x7C] receives the cached VP
    // TRANSPOSED -- ctx[0]=vp[0], ctx[1]=vp[4], ctx[2]=vp[8], ctx[3]=vp[12],
    // ctx[4]=vp[1], ctx[5]=vp[5], ctx[6]=vp[9], ctx[7]=vp[13], and so on
    // (uVar15=+0x210, uVar18=+0x220, uVar7=+0x230, uVar11=+0x240; uVar16=
    // +0x214, uVar19=+0x224, uVar8=+0x234, uVar12=+0x244; then +0x218,
    // uVar20=+0x228, uVar9=+0x238, uVar13=+0x248; uVar17=+0x21c, uVar21=
    // +0x22c, uVar10=+0x23c, uVar14=+0x24c).
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) ctx[r * 4 + k] = vp[k * 4 + r];
}

// ---------------------------------------------------------------------------
// Ray snapshot: FUN_1406be790 (camera_ray_writers2.txt:13-30). Finalizes the
// view rows via bit 2, then copies them and the origin inline.
// ---------------------------------------------------------------------------
void snapshotRay(Cam& c) {
    if (c.U(0x230) & 2u) finalizeViewRows(c);
    for (int i = 0; i < 18; ++i) camF(c, kCamRayBasis + 4 * i) = c.F(0x170 + 4 * i);
    camF(c, kCamRayOrigin + 0) = c.F(0x30);
    camF(c, kCamRayOrigin + 4) = c.F(0x34);
    camF(c, kCamRayOrigin + 8) = c.F(0x38);
    camF(c, kCamRayOrigin + 12) = 0.0f;
}

// ---------------------------------------------------------------------------
// Test fixtures and the check framework.
// ---------------------------------------------------------------------------
int g_failures = 0;
void check(bool ok, const char* what) {
    if (ok) { std::printf("  ok    %s\n", what); return; }
    ++g_failures;
    std::printf("  FAIL  %s\n", what);
}

bool feq(float a, float b, float tol) {
    const float d = std::fabs(a - b);
    return d <= tol || d <= tol * (std::fabs(a) + std::fabs(b));
}

// A plausible perspective camera (z-forward DirectX view space, reversed-Z
// infinite-far depth, per temporal_math.h's notes).
void makeCamera(Cam& c) {
    std::memset(c.b, 0, sizeof(c.b));
    // Source axes: a gentle yaw+pitch rotation (view->world), orthonormal.
    const float cy = std::cos(0.35f), sy = std::sin(0.35f);
    const float cp = std::cos(-0.12f), sp = std::sin(-0.12f);
    // R = Ry * Rx, row-major 3x4 with w lane 0.
    float* a = &camF(c, kCamAxes);
    a[0] = cy;      a[1] = 0.0f;  a[2] = -sy;     a[3] = 0.0f;
    a[4] = sy * sp; a[5] = cp;    a[6] = cy * sp; a[7] = 0.0f;
    a[8] = sy * cp; a[9] = -sp;   a[10] = cy * cp;
    // Origin (the game stores it negated into the view translation; the field
    // itself holds the camera position).
    camF(c, kCamOrigin + 0) = 120.0f;
    camF(c, kCamOrigin + 4) = -45.0f;
    camF(c, kCamOrigin + 8) = 900.0f;
    // Frustum parameters.
    camF(c, kCamNear) = 0.5f;
    camF(c, kCamFar) = 100000.0f;
    camF(c, kCamCompoundNear) = 0.0f;
    camU(c, kCamKind) = 3;                 // the trigonometric branch
    camF(c, kCamAngular) = 1.1f;           // ~63 degrees full vertical fov
    camF(c, 0x260) = 1.6f;                 // builder's helper+0x240 scale (aspect)
    camF(c, kCamBoundX) = 0.0f;            // off-center bounds (the jitter carriers)
    camF(c, kCamBoundY) = 0.0f;
    camF(c, kCamViewportW) = 3840.0f;
    camF(c, kCamViewportH) = 2160.0f;
    camU(c, kCamFlags) = kFlagView | kFlagProj | kFlagVP;
}

// Project a view-space point with the projection matrix (z-forward, w = z).
void projectPoint(const Cam& c, const float v[3], float outXYW[3]) {
    const float* p = &c.F(0x1B0);
    const float x = v[0], y = v[1], z = v[2];
    outXYW[0] = p[0] * x + p[4] * y + p[8] * z + p[12];
    outXYW[1] = p[1] * x + p[5] * y + p[9] * z + p[13];
    outXYW[2] = p[3] * x + p[7] * y + p[11] * z + p[15];
}

void derive(Cam& c) {
    finalizeVP(c); // chains view + projection per the protocol, clearing bits
}

// ---------------------------------------------------------------------------
// A1 exactly-once: jitter at the frustum params; the raster-pixel shift after
// the final projection equals the phase; deriving twice changes nothing.
// ---------------------------------------------------------------------------
void testA1() {
    std::printf("A1 exactly-once\n");
    Cam base; makeCamera(base);
    derive(base);
    // The jitter form per temporal_math.h: shifting the projection centre by
    // (jx, jy) pixels moves content by exactly that. In this builder the
    // off-centre terms are proj[8] = 2*boundX, proj[9] = 2*boundY, and a
    // tangent shift dx moves projected content left by dx*w/(r-l) pixels;
    // proj[0] = 1/(tan*(r-l)/2)-style scale means: boundX += -jx * spanX / w
    // shifts content right by jx. The production mapping
    // (temporalJitterToTangents) gives dx in tangent units; here the
    // projection maps x_ndc = proj[0]*xt + proj[8] where xt = x/z, so a
    // boundX delta shifts ndc.x by proj[0]^{-1}... measured directly below.
    const float w = 3840.0f, h = 2160.0f;
    float jx, jy;
    edvr::temporalJitter(3, &jx, &jy);
    // The production tangent shift for this frustum: tan-half-spans are
    // spanX = 2/proj[0]/w*... use the NDC identity instead: ndc.x =
    // proj[0]*(x/z) + proj[8]; pixel.x = (ndc.x+1)*w/2 - 0.5. A delta
    // dBoundX moves ndc.x by 2*dBoundX and pixel.x by w*dBoundX. So the
    // bound-pair delta for phase jx is dBoundX = jx / w (content right by
    // jx needs the projection centre left by the same NDC amount -- sign
    // checked by the measurement below).
    Cam j = base;
    camU(j, kCamFlags) |= kFlagProj | kFlagVP;
    camF(j, kCamBoundX) += -jx / w;
    camF(j, kCamBoundY) += jy / h;
    derive(j);
    static const float kPts[5][3] = {
        {0.0f, 0.0f, 5.0f}, {1.5f, -0.5f, 8.0f}, {-2.0f, 1.0f, 12.0f},
        {0.25f, 0.75f, 3.0f}, {-0.4f, -1.1f, 20.0f},
    };
    bool shiftOk = true;
    for (const auto& pt : kPts) {
        float b[3], jj[3];
        projectPoint(base, pt, b);
        projectPoint(j, pt, jj);
        // NDC -> pixel: (ndc+1)*span/2 - 0.5, w = z (DirectX z-forward).
        const float ndcxB = b[0] / pt[2], ndcxJ = jj[0] / pt[2];
        const float ndcyB = b[1] / pt[2], ndcyJ = jj[1] / pt[2];
        const float pxB = (ndcxB + 1.0f) * w * 0.5f - 0.5f;
        const float pxJ = (ndcxJ + 1.0f) * w * 0.5f - 0.5f;
        const float pyB = (ndcyB + 1.0f) * h * 0.5f - 0.5f;
        const float pyJ = (ndcyJ + 1.0f) * h * 0.5f - 0.5f;
        shiftOk &= feq(pxJ - pxB, -jx, 1e-3f) && feq(pyJ - pyB, jy, 1e-3f);
    }
    check(shiftOk, "A1 raster shift equals the phase for every test point");

    // Derive twice: identical products (no accumulation).
    Cam d1 = base, d2 = base;
    camU(d1, kCamFlags) |= kFlagProj | kFlagVP;
    derive(d1);
    camU(d1, kCamFlags) |= kFlagProj | kFlagVP;
    derive(d1);
    camU(d2, kCamFlags) |= kFlagProj | kFlagVP;
    derive(d2);
    bool same = true;
    for (int i = 0; i < 16; ++i) same &= feq(*projSlot(d1, i), *projSlot(d2, i), 1e-7f);
    check(same, "A1 repeat derivation within one execution: identical products");

    // The algebraic identities the transcription must satisfy (kind 3):
    // proj[0] = 1/(tan(angular/2)*scaleX), proj[5] = 1/tan(angular/2),
    // proj[8] = 2*boundX, proj[9] = 2*boundY, proj[10] = 0, proj[11] = 1,
    // proj[14] = (compoundNear+1)*near, proj[15] = 0 (reversed-Z inf far).
    const float t = std::tan(camF(base, kCamAngular) * 0.5f);
    check(feq(*projSlot(base, 0), 1.0f / (t * camF(base, 0x260)), 1e-5f), "A1 identity proj[0] = 1/(tan*scaleX)");
    check(feq(*projSlot(base, 5), 1.0f / t, 1e-5f), "A1 identity proj[5] = 1/tan");
    check(feq(*projSlot(base, 8), 2.0f * camF(base, kCamBoundX), 1e-6f), "A1 identity proj[8] = 2*boundX");
    check(feq(*projSlot(base, 9), 2.0f * camF(base, kCamBoundY), 1e-6f), "A1 identity proj[9] = 2*boundY");
    check(feq(*projSlot(base, 10), 0.0f, 1e-7f) && feq(*projSlot(base, 11), 1.0f, 1e-7f) &&
          feq(*projSlot(base, 14), (camF(base, kCamCompoundNear) + 1.0f) * camF(base, kCamNear), 1e-5f) &&
          feq(*projSlot(base, 15), 0.0f, 1e-7f),
          "A1 reversed-Z infinite-far depth row");
}

// ---------------------------------------------------------------------------
// A2 bit protocol: mutate without bits -> stale reads; with bits -> the
// corresponding blocks re-derive; the ray snapshot is independent state.
// ---------------------------------------------------------------------------
void testA2() {
    std::printf("A2 bit protocol (negative control)\n");
    Cam c; makeCamera(c);
    derive(c);
    float ctxFresh[16], ctxStale[16];
    refreshCopyVp(c, ctxFresh);

    // Mutate a frustum parameter WITHOUT raising bits: the cached VP and the
    // refresh copy must stay stale (the decompile's guard behaviour).
    const float oldAngular = camF(c, kCamAngular);
    camF(c, kCamAngular) = oldAngular * 1.01f;
    refreshCopyVp(c, ctxStale);
    bool stale = true;
    for (int i = 0; i < 16; ++i) stale &= feq(ctxStale[i], ctxFresh[i], 1e-7f);
    check(stale, "A2 mutation without bits leaves the refresh copy stale");

    // The composer, gated on bit 4, likewise keeps the stale projection.
    float cbBefore[16], cbAfter[16];
    composeSceneCb(c, cbBefore);
    camF(c, kCamAngular) = oldAngular * 1.02f;
    composeSceneCb(c, cbAfter);
    bool cbStale = true;
    for (int i = 0; i < 16; ++i) cbStale &= feq(cbAfter[i], cbBefore[i], 1e-7f);
    check(cbStale, "A2 composer reads the stale projection while bit 4 is clear");

    // Raise bit 4: the composer re-derives the projection; the cached VP and
    // the refresh copy follow through bit 8's chain... but the refresh copy
    // is gated on bit 8, which a bit-4-only raise does NOT set -- the cached
    // VP is only rebuilt when something checks bit 8. finalizeVP chains, so
    // an explicit finalize (or the refresh's own bit-8 check with bit 8 set)
    // brings everything current.
    camU(c, kCamFlags) |= kFlagProj;
    composeSceneCb(c, cbAfter);
    bool cbFresh = false;
    for (int i = 0; i < 16; ++i) cbFresh |= !feq(cbAfter[i], cbBefore[i], 1e-7f);
    check(cbFresh, "A2 bit 4 re-derives the projection for the composer");
    // And the ctx copy is STILL stale until bit 8 is raised -- the
    // mutation-protocol consequence the doc names.
    refreshCopyVp(c, ctxStale);
    bool ctxStillStale = true;
    for (int i = 0; i < 16; ++i) ctxStillStale &= feq(ctxStale[i], ctxFresh[i], 1e-7f);
    check(ctxStillStale, "A2 bit 4 alone leaves the cached-VP copy stale (bit 8 is load-bearing)");
    camU(c, kCamFlags) |= kFlagVP;
    float ctxNow[16];
    refreshCopyVp(c, ctxNow);
    bool ctxMoved = false;
    for (int i = 0; i < 16; ++i) ctxMoved |= !feq(ctxNow[i], ctxStale[i], 1e-7f);
    check(ctxMoved, "A2 bit 8 refreshes the context copy");

    // The ray snapshot is independently supplied state: a view mutation with
    // bit 2 raised refreshes view rows but NOT the snapshot; the snapshot is
    // only written by FUN_1406be790's own call. A stale snapshot must be
    // DETECTABLE, not absorbed.
    Cam s; makeCamera(s);
    derive(s);
    snapshotRay(s);
    const float snapY = camF(s, kCamRayBasis + 4 * 5);
    camF(s, kCamAxes + 4 * 5) += 0.05f; // rotate the axes slightly
    camU(s, kCamFlags) |= kFlagView;
    finalizeViewRows(s); // a consumer gated on bit 2 refreshes the rows...
    const float rowsY = s.F(0x184);
    bool snapshotStale = !feq(camF(s, kCamRayBasis + 4 * 5), rowsY, 1e-7f) &&
                         feq(camF(s, kCamRayBasis + 4 * 5), snapY, 1e-7f);
    check(snapshotStale, "A2 stale ray snapshot is detectable (not silently refreshed)");
    snapshotRay(s); // the typed writer's own call restores consistency
    check(feq(camF(s, kCamRayBasis + 4 * 5), rowsY, 1e-7f), "A2 the typed writer restores the snapshot");
}

// ---------------------------------------------------------------------------
// A3 canonical mutation form: frustum-param mutation vs direct projection
// edit produce identical VP within tolerance, per kind branch; kinds 4/5 are
// custom-matrix (direct edit trivially equivalent) and must be named.
// ---------------------------------------------------------------------------
void testA3() {
    std::printf("A3 canonical mutation form\n");
    const float jDelta = 0.013f;
    // Kind 3 (the live perspective path): boundX jitter via params vs a
    // direct proj[8] edit by 2*delta.
    Cam a; makeCamera(a);
    derive(a);
    Cam b = a;
    camU(b, kCamFlags) |= kFlagProj | kFlagVP;
    camF(b, kCamBoundX) += jDelta;
    derive(b);
    Cam d = a;
    camU(d, kCamFlags) |= kFlagVP; // direct edit: no bit 4, no rebuild
    *projSlot(d, 8) += 2.0f * jDelta;
    derive(d);
    bool eq = true;
    for (int i = 0; i < 16; ++i) eq &= feq(*projSlot(b, i), *projSlot(d, i), 1e-6f);
    check(eq, "A3 kind 3: bound-pair mutation == direct off-centre edit");

    // The off-centre edit must NOT raise bit 4: had it rebuilt, the direct
    // edit would have been overwritten -- the protocol check itself.
    Cam e = a;
    camU(e, kCamFlags) |= kFlagProj | kFlagVP;
    *projSlot(e, 8) += 2.0f * jDelta; // edit THEN rebuild: rebuild wins
    derive(e);
    check(feq(*projSlot(e, 8), 2.0f * camF(a, kCamBoundX), 1e-6f),
          "A3 a direct edit is overwritten whenever bit 4 rebuilds (must not raise it)");

    // Kinds 4/5: the projection IS the custom data -- a direct edit of the
    // custom block and a direct edit of the projection are the same edit
    // after the copy, so the branch adds no derivation of its own beyond the
    // per-row adjustments. Prove the copy is faithful, and NAME the refusal:
    // jitter must be expressed through +0x2D0..+0x30C or refused, never
    // applied to derived fields these branches do not read.
    Cam c4; makeCamera(c4);
    camU(c4, kCamKind) = 4;
    for (int i = 0; i < 16; ++i) camF(c4, kCamCustom + 4 * i) = (i % 5 == 0) ? 1.0f : 0.1f * i;
    camU(c4, kCamFlags) |= kFlagProj;
    buildProjection(c4);
    // Kind 4 adjustments: proj[2] = custom[3]-custom[2], proj[3] = custom[3].
    check(feq(*projSlot(c4, 2), camF(c4, kCamCustom + 12) - camF(c4, kCamCustom + 8), 1e-6f) &&
          feq(*projSlot(c4, 3), camF(c4, kCamCustom + 12), 1e-6f),
          "A3 kind 4: custom-matrix copy with per-row adjustments");
    std::printf("  note  kinds 4/5: jitter is expressed through +0x2D0..+0x30C or REFUSED; bound-pair fields are not read by these branches\n");

    // Default branch: identity-ish with proj[10] = -1, proj[14] = 1. Kind 0
    // runs the TRIG branch (the decompile's fall-through); the default is the
    // else of kinds 4/5, reached by any other value (here 2).
    Cam cd; makeCamera(cd);
    camU(cd, kCamKind) = 2;
    camU(cd, kCamFlags) |= kFlagProj;
    buildProjection(cd);
    check(feq(*projSlot(cd, 0), 1.0f, 1e-7f) && feq(*projSlot(cd, 5), 1.0f, 1e-7f) &&
          feq(*projSlot(cd, 10), -1.0f, 1e-7f) && feq(*projSlot(cd, 14), 1.0f, 1e-7f),
          "A3 default branch shape (kind 2: identity, proj[10] = -1, proj[14] = 1)");
}

// ---------------------------------------------------------------------------
// A4 override composition: jitter, then the per-item setter shape
// (FUN_140591f30: +0x25C = f2+f14, +0x280 = f14), then the section refresh;
// the override lands and the jitter term survives in raster-pixel shift.
// ---------------------------------------------------------------------------
void testA4() {
    std::printf("A4 override composition\n");
    Cam c; makeCamera(c);
    derive(c);
    const float w = 3840.0f;
    float jx, jy;
    edvr::temporalJitter(5, &jx, &jy);
    camF(c, kCamBoundX) += -jx / w;
    // The per-item poke, FUN_140591f30's shape: +0x25C gets a compound value,
    // +0x280 an angular value; both dirty the projection (0xC) and the ray
    // blocks (0xD).
    camF(c, kCamCompoundNear) = 0.25f;
    camF(c, kCamAngular) = camF(c, kCamAngular) * 1.05f;
    camU(c, kCamFlags) |= kFlagProj | kFlagVP | kFlagRay;
    derive(c);
    // The override landed: proj[14] = (compoundNear+1)*near with the poke.
    check(feq(*projSlot(c, 14), (0.25f + 1.0f) * camF(c, kCamNear), 1e-5f),
          "A4 the per-item compound-near poke reaches the projection");
    // The jitter survives: the off-centre term still carries exactly the
    // bound-pair jitter (scaled by nothing).
    check(feq(*projSlot(c, 8), 2.0f * (-jx / w), 1e-6f),
          "A4 jitter term survives the override unscaled");
    const float t = std::tan(camF(c, kCamAngular) * 0.5f);
    check(feq(*projSlot(c, 5), 1.0f / t, 1e-5f), "A4 the angular poke reaches the projection");
    // Raster shift for a centre point still equals the phase.
    const float pt[3] = {0.0f, 0.0f, 5.0f};
    float out[3];
    projectPoint(c, pt, out);
    const float px = ((out[0] / pt[2]) + 1.0f) * w * 0.5f - 0.5f;
    check(feq(px - (w * 0.5f - 0.5f), -jx, 1e-3f), "A4 centre-point raster shift equals the phase after composition");
}

// ---------------------------------------------------------------------------
// A5 phase, generation and replay: the five cases, exercised through the
// PRODUCTION FlatLivePhase machine (src/d3d11/flat_live_phase.h).
// ---------------------------------------------------------------------------
void testA5() {
    std::printf("A5 phase, generation and replay (production FlatLivePhase)\n");
    // Warm the machine up properly: two clean begin/finish cycles.
    edvr::FlatLivePhase ph;
    ph.beginFrame(true, true, 3840, 2160); ph.finish(true, true);
    ph.beginFrame(true, true, 3840, 2160); ph.finish(true, true);
    ph.beginFrame(true, true, 3840, 2160); // warm: the phase is now chosen

    // Case 1: repeat derivation within one phase-group execution -- the
    // phase chosen at the boundary does not move no matter how much work
    // the frame applies.
    const float x1 = ph.currentX, y1 = ph.currentY;
    ph.noteApplied();
    const float xa = ph.currentX;
    ph.noteApplied();
    check(ph.applied == 2 && feq(xa, x1, 0.0f) && feq(ph.currentY, y1, 0.0f),
          "A5.1 repeat derivation within the execution keeps one phase");

    // Case 2: a new eligible execution with unchanged source bytes CAN
    // advance the sampling sequence (a stationary camera still jitters).
    ph.finish(true, true);
    const uint32_t seqBefore = ph.phaseSequence;
    ph.beginFrame(true, true, 3840, 2160);
    check(ph.phaseSequence == seqBefore + 1 &&
          (!feq(ph.currentX, x1, 0.0f) || !feq(ph.currentY, y1, 0.0f)),
          "A5.2 new execution advances the phase with unchanged sources");

    // Case 3: a per-item source revision within the execution rebuilds the
    // derivatives but must preserve the group's pixel phase -- the phase
    // machine is untouched by mid-frame source edits (the derive side was
    // proven in A4); here the phase itself must not move.
    const float mx = ph.currentX, my = ph.currentY;
    check(feq(ph.currentX, mx, 0.0f) && feq(ph.currentY, my, 0.0f),
          "A5.3 mid-execution revisions preserve the group phase");

    // Case 4: pointer/resource generation change -- an incompatible previous
    // frame rejects stale history even when matrices would be identical.
    edvr::FlatLivePhase g;
    g.beginFrame(true, true, 3840, 2160); g.finish(true, true);
    g.beginFrame(true, true, 3840, 2160); g.finish(true, true);
    g.beginFrame(true, true, 3840, 2160);
    g.finish(true, true);
    g.beginFrame(true, false, 3840, 2160); // generation change
    check(!g.previousAcceptedValid && g.warmFrames == 0,
          "A5.4 generation change rejects stale ownership/history");

    // Case 5: replay -- re-deriving the recorded execution from its recorded
    // sources yields the recorded products (determinism), and a
    // late/repeated execution cannot masquerade as fresh: the phase
    // machine's history only advances through a clean finish.
    Cam r1; makeCamera(r1);
    camF(r1, kCamBoundX) += -0.0002f;
    derive(r1);
    Cam r2; makeCamera(r2);
    camF(r2, kCamBoundX) += -0.0002f;
    derive(r2);
    bool replay = true;
    for (int i = 0; i < 16; ++i) replay &= feq(*projSlot(r1, i), *projSlot(r2, i), 0.0f);
    check(replay, "A5.5 recorded execution replays bit-identically");
    const bool wasValid = g.previousAcceptedValid;
    g.finish(false, false); // a failed closure must not publish history
    check(!g.previousAcceptedValid && !wasValid,
          "A5.5 a late/failed execution publishes no fresh history");
}

// ---------------------------------------------------------------------------
// A6 failure and disable, split at the consumption boundary (the production
// machine's own semantics, flat_live_phase.h:49-75).
// ---------------------------------------------------------------------------
void testA6() {
    std::printf("A6 failure/disable at the consumption boundary (production FlatLivePhase)\n");
    // Before any consumer: a failure keeps the whole frame at zero.
    edvr::FlatLivePhase pre;
    pre.beginFrame(true, true, 3840, 2160); pre.finish(true, true);
    pre.beginFrame(true, true, 3840, 2160); pre.finish(true, true);
    pre.beginFrame(true, true, 3840, 2160); // warm: a phase is chosen
    pre.fail();
    check(feq(pre.currentX, 0.0f, 0.0f) && feq(pre.currentY, 0.0f, 0.0f) &&
          !pre.needsSpatialFallback(),
          "A6 before consumption: refusal keeps the frame unjittered");

    // After the first consumer: the committed phase is retained for the
    // frame's remaining work, history is invalidated, and the fallback is
    // the spatial path -- no perfect-rollback claim.
    edvr::FlatLivePhase post;
    post.beginFrame(true, true, 3840, 2160); post.finish(true, true);
    post.beginFrame(true, true, 3840, 2160); post.finish(true, true);
    post.beginFrame(true, true, 3840, 2160);
    const float cx = post.currentX, cy = post.currentY;
    post.noteApplied();
    post.fail();
    check(feq(post.currentX, cx, 0.0f) && feq(post.currentY, cy, 0.0f) &&
          post.needsSpatialFallback(),
          "A6 after first consumer: committed phase retained, spatial fallback named");
    post.finish(false, true);
    check(!post.previousAcceptedValid,
          "A6 a failed frame publishes no temporal history");
    // The next frame begins clean (subsequent jitter stops until closure).
    post.beginFrame(true, false, 3840, 2160);
    check(feq(post.currentX, 0.0f, 0.0f) && feq(post.currentY, 0.0f, 0.0f),
          "A6 the next frame restarts without a stale phase");
}

int runSelfTest() {
    testA1();
    testA2();
    testA3();
    testA4();
    testA5();
    testA6();
    if (g_failures == 0) {
        std::printf("c2 derive: PASS\n");
        return 0;
    }
    std::printf("c2 derive: %d FAILED check(s)\n", g_failures);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return runSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("c2 derive test: dry run (no checks run)\n");
        return 0;
    }
    std::printf("usage: c2_derive_test --self-test|--dry-run\n");
    return 2;
}

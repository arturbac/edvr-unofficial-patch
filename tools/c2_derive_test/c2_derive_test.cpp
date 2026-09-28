// C2-A derive reducer (docs/design-flat-camera-integration.md, C2 test plan
// addendum). Reimplements ONLY the semantics read out of the decompiles; the
// derive half lives in c2_derive_model.h (shared with c2_warp_test), whose
// header comment names the artifacts:
//
//   view finalizer     FUN_1404f4910  camera_cache_helpers.txt:550-618
//   projection builder FUN_1404f2ff0  camera_cache_helpers.txt:255-493
//   VP finalizer       FUN_1404f49f0  camera_cache_helpers.txt:57-195
//   scene CB composer  FUN_140596830  camera_producer.txt:23-124
//   refresh ctx copy   FUN_1405921f0  camera_producer.txt:186-249
//   ray snapshot       FUN_1406be790  camera_ray_writers2.txt:13-30
//
// --self-test runs A1-A6 and prints "c2 derive: PASS" only when every check
// holds. Exit 1 with the failed checks named otherwise.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "c2_derive_model.h"
#include "../../src/d3d11/flat_live_phase.h"

namespace {

using namespace c2derive;

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

// ---------------------------------------------------------------------------
// A1 exactly-once: jitter at the frustum params; the raster-pixel shift after
// the final projection equals the phase; deriving twice changes nothing.
// ---------------------------------------------------------------------------
void testA1() {
    std::printf("A1 exactly-once\n");
    Cam base; makeCamera(base);
    derive(base);
    const float w = 3840.0f, h = 2160.0f;
    float jx, jy;
    edvr::temporalJitter(3, &jx, &jy);
    // The bound-pair delta for the phase, in the D3D rasterizer's
    // coordinates (ndc.y = +1 is the TOP, pixel row 0 is the top):
    // content right by jx needs boundX += jx/w; content down by jy needs
    // boundY += -jy/h. ndc.x = proj[0]*(x/z) + proj[8], and pixel.y runs
    // (1 - ndc.y)*h/2 - 0.5.
    Cam j = base;
    camU(j, kCamFlags) |= kFlagProj | kFlagVP;
    camF(j, kCamBoundX) += jx / w;
    camF(j, kCamBoundY) += -jy / h;
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
        const float ndcxB = b[0] / pt[2], ndcxJ = jj[0] / pt[2];
        const float ndcyB = b[1] / pt[2], ndcyJ = jj[1] / pt[2];
        const float pxB = (ndcxB + 1.0f) * w * 0.5f - 0.5f;
        const float pxJ = (ndcxJ + 1.0f) * w * 0.5f - 0.5f;
        const float pyB = (1.0f - ndcyB) * h * 0.5f - 0.5f;
        const float pyJ = (1.0f - ndcyJ) * h * 0.5f - 0.5f;
        shiftOk &= feq(pxJ - pxB, jx, 1e-3f) && feq(pyJ - pyB, jy, 1e-3f);
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

    // The algebraic identities the transcription must satisfy (kind 3).
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

    const float oldAngular = camF(c, kCamAngular);
    camF(c, kCamAngular) = oldAngular * 1.01f;
    refreshCopyVp(c, ctxStale);
    bool stale = true;
    for (int i = 0; i < 16; ++i) stale &= feq(ctxStale[i], ctxFresh[i], 1e-7f);
    check(stale, "A2 mutation without bits leaves the refresh copy stale");

    float cbBefore[16], cbAfter[16];
    composeSceneCb(c, cbBefore);
    camF(c, kCamAngular) = oldAngular * 1.02f;
    composeSceneCb(c, cbAfter);
    bool cbStale = true;
    for (int i = 0; i < 16; ++i) cbStale &= feq(cbAfter[i], cbBefore[i], 1e-7f);
    check(cbStale, "A2 composer reads the stale projection while bit 4 is clear");

    camU(c, kCamFlags) |= kFlagProj;
    composeSceneCb(c, cbAfter);
    bool cbFresh = false;
    for (int i = 0; i < 16; ++i) cbFresh |= !feq(cbAfter[i], cbBefore[i], 1e-7f);
    check(cbFresh, "A2 bit 4 re-derives the projection for the composer");
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

    Cam s; makeCamera(s);
    derive(s);
    snapshotRay(s);
    const float snapY = camF(s, kCamRayBasis + 4 * 5);
    camF(s, kCamAxes + 4 * 5) += 0.05f;
    camU(s, kCamFlags) |= kFlagView;
    finalizeViewRows(s);
    const float rowsY = s.F(0x184);
    bool snapshotStale = !feq(camF(s, kCamRayBasis + 4 * 5), rowsY, 1e-7f) &&
                         feq(camF(s, kCamRayBasis + 4 * 5), snapY, 1e-7f);
    check(snapshotStale, "A2 stale ray snapshot is detectable (not silently refreshed)");
    snapshotRay(s);
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
    Cam a; makeCamera(a);
    derive(a);
    Cam b = a;
    camU(b, kCamFlags) |= kFlagProj | kFlagVP;
    camF(b, kCamBoundX) += jDelta;
    derive(b);
    Cam d = a;
    camU(d, kCamFlags) |= kFlagVP;
    *projSlot(d, 8) += 2.0f * jDelta;
    derive(d);
    bool eq = true;
    for (int i = 0; i < 16; ++i) eq &= feq(*projSlot(b, i), *projSlot(d, i), 1e-6f);
    check(eq, "A3 kind 3: bound-pair mutation == direct off-centre edit");

    Cam e = a;
    camU(e, kCamFlags) |= kFlagProj | kFlagVP;
    *projSlot(e, 8) += 2.0f * jDelta;
    derive(e);
    check(feq(*projSlot(e, 8), 2.0f * camF(a, kCamBoundX), 1e-6f),
          "A3 a direct edit is overwritten whenever bit 4 rebuilds (must not raise it)");

    Cam c4; makeCamera(c4);
    camU(c4, kCamKind) = 4;
    for (int i = 0; i < 16; ++i) camF(c4, kCamCustom + 4 * i) = (i % 5 == 0) ? 1.0f : 0.1f * i;
    camU(c4, kCamFlags) |= kFlagProj;
    buildProjection(c4);
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
    camF(c, kCamBoundX) += jx / w;
    camF(c, kCamCompoundNear) = 0.25f;
    camF(c, kCamAngular) = camF(c, kCamAngular) * 1.05f;
    camU(c, kCamFlags) |= kFlagProj | kFlagVP | kFlagRay;
    derive(c);
    check(feq(*projSlot(c, 14), (0.25f + 1.0f) * camF(c, kCamNear), 1e-5f),
          "A4 the per-item compound-near poke reaches the projection");
    check(feq(*projSlot(c, 8), 2.0f * (jx / w), 1e-6f),
          "A4 jitter term survives the override unscaled");
    const float t = std::tan(camF(c, kCamAngular) * 0.5f);
    check(feq(*projSlot(c, 5), 1.0f / t, 1e-5f), "A4 the angular poke reaches the projection");
    const float pt[3] = {0.0f, 0.0f, 5.0f};
    float out[3];
    projectPoint(c, pt, out);
    const float px = ((out[0] / pt[2]) + 1.0f) * w * 0.5f - 0.5f;
    check(feq(px - (w * 0.5f - 0.5f), jx, 1e-3f), "A4 centre-point raster shift equals the phase after composition");
}

// ---------------------------------------------------------------------------
// A5 phase, generation and replay: the five cases, exercised through the
// PRODUCTION FlatLivePhase machine (src/d3d11/flat_live_phase.h).
// ---------------------------------------------------------------------------
void testA5() {
    std::printf("A5 phase, generation and replay (production FlatLivePhase)\n");
    edvr::FlatLivePhase ph;
    ph.beginFrame(true, true, 3840, 2160); ph.finish(true, true);
    ph.beginFrame(true, true, 3840, 2160); ph.finish(true, true);
    ph.beginFrame(true, true, 3840, 2160);

    const float x1 = ph.currentX, y1 = ph.currentY;
    ph.noteApplied();
    const float xa = ph.currentX;
    ph.noteApplied();
    check(ph.applied == 2 && feq(xa, x1, 0.0f) && feq(ph.currentY, y1, 0.0f),
          "A5.1 repeat derivation within the execution keeps one phase");

    ph.finish(true, true);
    const uint32_t seqBefore = ph.phaseSequence;
    ph.beginFrame(true, true, 3840, 2160);
    check(ph.phaseSequence == seqBefore + 1 &&
          (!feq(ph.currentX, x1, 0.0f) || !feq(ph.currentY, y1, 0.0f)),
          "A5.2 new execution advances the phase with unchanged sources");

    const float mx = ph.currentX, my = ph.currentY;
    check(feq(ph.currentX, mx, 0.0f) && feq(ph.currentY, my, 0.0f),
          "A5.3 mid-execution revisions preserve the group phase");

    edvr::FlatLivePhase g;
    g.beginFrame(true, true, 3840, 2160); g.finish(true, true);
    g.beginFrame(true, true, 3840, 2160); g.finish(true, true);
    g.beginFrame(true, true, 3840, 2160);
    g.finish(true, true);
    g.beginFrame(true, false, 3840, 2160);
    check(!g.previousAcceptedValid && g.warmFrames == 0,
          "A5.4 generation change rejects stale ownership/history");

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
    g.finish(false, false);
    check(!g.previousAcceptedValid && !wasValid,
          "A5.5 a late/failed execution publishes no fresh history");
}

// ---------------------------------------------------------------------------
// A6 failure and disable, split at the consumption boundary (the production
// machine's own semantics, flat_live_phase.h:49-75).
// ---------------------------------------------------------------------------
void testA6() {
    std::printf("A6 failure/disable at the consumption boundary (production FlatLivePhase)\n");
    edvr::FlatLivePhase pre;
    pre.beginFrame(true, true, 3840, 2160); pre.finish(true, true);
    pre.beginFrame(true, true, 3840, 2160); pre.finish(true, true);
    pre.beginFrame(true, true, 3840, 2160);
    pre.fail();
    check(feq(pre.currentX, 0.0f, 0.0f) && feq(pre.currentY, 0.0f, 0.0f) &&
          !pre.needsSpatialFallback(),
          "A6 before consumption: refusal keeps the frame unjittered");

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

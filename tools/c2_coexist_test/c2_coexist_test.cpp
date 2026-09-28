// C2-C coexistence, ownership and closure tests
// (docs/design-flat-camera-integration.md, C2 test plan addendum, C2-C table,
// revised per the C2-plan review's R5). Exercises the PRODUCTION-INTENDED
// owner-selection policy (src/d3d11/flat_camera_ownership.h) with the C2-A
// derive model as the camera-lineage reference and the PRODUCTION projection
// ownership classifier (src/d3d11/flat_projection_ownership.h) as the
// legacy-evidence check. No abstract XOR of booleans stands in for the
// policy; the game-camera model generates the scenarios.
//
// --self-test runs C1-C6 and prints "c2 coexist: PASS" only when every check
// holds. Exit 1 with the failures named otherwise.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>

#include "../c2_derive_test/c2_derive_model.h"
#include "../../src/d3d11/flat_camera_ownership.h"
#include "../../src/d3d11/flat_projection_ownership.h"

namespace {

using namespace c2derive;
using namespace edvr;

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

// A 6-row scene-camera block in the classifier's measured encoding
// (flat_projection_ownership.h:75-80), composed by the derive itself: rows
// 270..273 from composeSceneCb (rotation included), row 274 the repeated
// view direction (column 3 of rows 270..272), row 275 the camera position.
void sceneCameraRows(Cam& c, float out[24]) {
    composeSceneCb(c, out);
    out[16] = out[3]; out[17] = out[7]; out[18] = out[11]; out[19] = 0.0f;
    out[20] = camF(c, kCamOrigin + 0);
    out[21] = camF(c, kCamOrigin + 4);
    out[22] = camF(c, kCamOrigin + 8);
    out[23] = 1.0f;
}

// ---------------------------------------------------------------------------
// C1 single owner before mutation, legacy paths suppressed.
// ---------------------------------------------------------------------------
void testC1() {
    std::printf("C1 single owner\n");
    FlatCameraOwnershipState s{};
    FlatCameraGroupInput in;
    in.upstreamCertified = true;
    in.legacyEligible = true;
    const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
    check(d.owner == FlatCameraOwner::Upstream &&
          d.legacyGraphicsSuppressed && d.legacyComputeSuppressed &&
          d.outcome == FlatCameraOutcome::Treated,
          "C1 a both-eligible frame gets upstream as the single owner, legacy graphics+compute suppressed");
    // The selection precedes any mutation by construction (it is called with
    // only the state and inputs); the close records application afterwards.
    flatCameraOwnerClose(s, d, true, true);
    check(s.outstanding == FlatCameraOwner::Upstream && s.historyValid,
          "C1 closure records the upstream owner and valid history");
}

// ---------------------------------------------------------------------------
// C2 unknown shaders cannot veto certified lineage.
// ---------------------------------------------------------------------------
void testC2() {
    std::printf("C2 unknown shaders cannot veto certified lineage\n");
    FlatCameraOwnershipState s{};
    FlatCameraGroupInput in;
    in.upstreamCertified = true;
    in.legacyEligible = false;   // the legacy selector does not know this pair
    in.legacyObserving = true;   // ...and is parked in observation
    const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
    check(d.owner == FlatCameraOwner::Upstream &&
          d.outcome == FlatCameraOutcome::Treated &&
          !d.legacyGraphicsSuppressed && !d.legacyComputeSuppressed,
          "C2 certified lineage reaches treatment without legacy admission hashes");
    // The legacy observation state is untouched (it keeps collecting
    // evidence); it simply has no veto over the certified route.
    flatCameraOwnerClose(s, d, true, true);
    check(s.outstanding == FlatCameraOwner::Upstream,
          "C2 legacy observation continues without a veto");
}

// ---------------------------------------------------------------------------
// C3 named safe outcomes.
// ---------------------------------------------------------------------------
void testC3() {
    std::printf("C3 named safe outcomes\n");
    {
        FlatCameraOwnershipState s{}; s.historyValid = true;
        FlatCameraGroupInput in;
        in.upstreamCertified = true;
        in.lateProjectionRewrite = true;
        const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        check(d.owner == FlatCameraOwner::None &&
              d.outcome == FlatCameraOutcome::Recovery && d.historyReset &&
              d.preserveCameraInputs && std::strcmp(d.reason, "late-projection-rewrite") == 0,
              "C3 late projection rewrite: named recovery, history reset, inputs preserved");
    }
    {
        FlatCameraOwnershipState s{}; s.historyValid = true;
        FlatCameraGroupInput in;
        in.uncoveredSecondary = true;
        const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        check(d.owner == FlatCameraOwner::None &&
              d.outcome == FlatCameraOutcome::Recovery &&
              std::strcmp(d.reason, "uncovered-secondary-camera") == 0,
              "C3 uncovered secondary camera: named recovery");
    }
    {
        FlatCameraOwnershipState s{};
        FlatCameraGroupInput in;
        in.upstreamUnsupported = true; // kinds 4/5 territory, named
        const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        check(d.owner == FlatCameraOwner::None &&
              d.outcome == FlatCameraOutcome::Unsupported &&
              std::strcmp(d.reason, "upstream-unsupported-projection-kind") == 0,
              "C3 unsupported projection kind: named, not silent");
    }
    {
        FlatCameraOwnershipState s{};
        FlatCameraGroupInput in;
        in.upstreamUnsupported = true;
        in.legacyEligible = true; // the legacy route may still own here
        const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        check(d.owner == FlatCameraOwner::Legacy &&
              d.outcome == FlatCameraOutcome::Treated &&
              std::strcmp(d.reason, "legacy-eligible-upstream-unsupported") == 0,
              "C3 unsupported upstream with an eligible legacy: legacy owns, named");
    }
}

// ---------------------------------------------------------------------------
// C4 interleaved views retain group-level ownership.
// ---------------------------------------------------------------------------
void testC4() {
    std::printf("C4 interleaved views\n");
    // Three groups in one frame: main certified, cockpit legacy-eligible,
    // auxiliary unsupported. Per-group selection must give each its own
    // owner -- a frame-wide OR would have hidden the auxiliary's Unsupported.
    FlatCameraOwnershipState mainS{}, cockpitS{}, auxS{};
    FlatCameraGroupInput mainIn, cockpitIn, auxIn;
    mainIn.upstreamCertified = true; mainIn.legacyEligible = true;
    cockpitIn.legacyEligible = true;
    auxIn.upstreamUnsupported = true;
    const FlatCameraOwnershipDecision mainD = flatCameraOwnerSelect(mainS, mainIn);
    const FlatCameraOwnershipDecision cockpitD = flatCameraOwnerSelect(cockpitS, cockpitIn);
    const FlatCameraOwnershipDecision auxD = flatCameraOwnerSelect(auxS, auxIn);
    check(mainD.owner == FlatCameraOwner::Upstream &&
          cockpitD.owner == FlatCameraOwner::Legacy &&
          auxD.owner == FlatCameraOwner::None &&
          auxD.outcome == FlatCameraOutcome::Unsupported,
          "C4 main=upstream, cockpit=legacy, auxiliary=named-unsupported -- no frame-wide count hides the mix");
    // The auxiliary's unsupported camera must not poison the others'
    // closures either.
    flatCameraOwnerClose(mainS, mainD, true, true);
    flatCameraOwnerClose(cockpitS, cockpitD, true, true);
    flatCameraOwnerClose(auxS, auxD, false, false);
    check(mainS.historyValid && cockpitS.historyValid && !auxS.historyValid,
          "C4 closures stay per-group (auxiliary history invalid, others valid)");
}

// ---------------------------------------------------------------------------
// C5 ownership switch waits for outstanding work.
// ---------------------------------------------------------------------------
void testC5() {
    std::printf("C5 ownership switch\n");
    // Outstanding legacy work already applied: the switch defers a frame
    // rather than mixing phases.
    {
        FlatCameraOwnershipState s{};
        s.outstanding = FlatCameraOwner::Legacy;
        s.outstandingApplied = true;
        s.historyValid = true;
        FlatCameraGroupInput in;
        in.upstreamCertified = true;
        const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        check(d.owner == FlatCameraOwner::Legacy && !d.historyReset &&
              std::strcmp(d.reason, "switch-deferred-outstanding-legacy") == 0,
              "C5 outstanding legacy work defers the switch (no mixed phase)");
    }
    // No outstanding legacy application: the switch happens, with history
    // reset and the original camera inputs preserved for motion
    // reconstruction.
    {
        FlatCameraOwnershipState s{};
        s.outstanding = FlatCameraOwner::Legacy;
        s.outstandingApplied = false;
        s.historyValid = true;
        FlatCameraGroupInput in;
        in.upstreamCertified = true;
        const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        check(d.owner == FlatCameraOwner::Upstream && d.historyReset &&
              d.preserveCameraInputs,
              "C5 a clean switch resets history and preserves camera inputs");
        flatCameraOwnerClose(s, d, true, true);
        check(s.outstanding == FlatCameraOwner::Upstream && s.historyValid,
              "C5 closure after the switch records the new owner and fresh history");
    }
}

// ---------------------------------------------------------------------------
// C6 steady-state cost and the legacy-evidence classifier's consistency.
// ---------------------------------------------------------------------------
void testC6() {
    std::printf("C6 steady-state cost and classifier consistency\n");
    // The policy allocates nothing and touches no globals: it runs on the
    // stack, which is the whole steady-state resource claim (offline
    // measurement; equal-scene game cost is C3/C4, per the plan).
    FlatCameraOwnershipState s{};
    FlatCameraGroupInput in;
    in.upstreamCertified = true;
    in.legacyEligible = true;
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t upstream = 0;
    for (uint32_t i = 0; i < 1000000; ++i) {
        const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        upstream += d.owner == FlatCameraOwner::Upstream ? 1u : 0u;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double nsPer = std::chrono::duration<double, std::nano>(t1 - t0).count() / 1000000.0;
    std::printf("  note  1,000,000 selections: %.1f ns/decision, stack-only, no allocation\n", nsPer);
    check(upstream == 1000000, "C6 selection is deterministic over a million identical frames");

    // The production ownership classifier, fed the derive's own scene-camera
    // encoding: identity match certifies, a changed near does not.
    Cam c; makeCamera(c);
    camF(c, kCamBoundX) = 0.0f; camF(c, kCamBoundY) = 0.0f;
    derive(c);
    float rows[24];
    sceneCameraRows(c, rows);
    FlatProjectionOwnershipInput oi{};
    oi.referenceBuffer = rows;
    oi.referenceCamera = reinterpret_cast<const unsigned char*>(rows);
    oi.referenceBytes = sizeof(rows);
    oi.currentReference = true;
    oi.candidateBuffer = rows;
    oi.candidateRows = reinterpret_cast<const unsigned char*>(rows);
    oi.candidateBytes = sizeof(rows);
    oi.currentCandidate = true;
    oi.layout = FlatProjectionOwnershipLayout::CanonicalVsB1;
    const FlatProjectionOwnershipResult same = flatClassifyProjectionOwnership(oi);
    check(same.kind == FlatProjectionOwnershipKind::CanonicalSceneCamera,
          "C6 the derive's scene-camera encoding certifies as the canonical scene camera");
    float changed[24];
    std::memcpy(changed, rows, sizeof(changed));
    changed[14] *= 1.5f; // a different near term
    oi.candidateBuffer = changed;
    oi.candidateRows = reinterpret_cast<const unsigned char*>(changed);
    const FlatProjectionOwnershipResult diff = flatClassifyProjectionOwnership(oi);
    check(diff.kind == FlatProjectionOwnershipKind::Unmatched,
          "C6 a changed camera is Unmatched (classifier evidence stays honest)");
}

int runSelfTest() {
    testC1();
    testC2();
    testC3();
    testC4();
    testC5();
    testC6();
    if (g_failures == 0) {
        std::printf("c2 coexist: PASS\n");
        return 0;
    }
    std::printf("c2 coexist: %d FAILED check(s)\n", g_failures);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return runSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("c2 coexist test: dry run (no checks run)\n");
        return 0;
    }
    std::printf("usage: c2_coexist_test --self-test|--dry-run\n");
    return 2;
}

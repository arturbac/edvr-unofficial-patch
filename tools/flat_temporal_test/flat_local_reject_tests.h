#pragma once

#include <cstdio>
#include "../../src/d3d11/flat_local_reject.h"

inline int flatLocalRejectTests() {
    using edvr::flatLocalRefusalReason;
    using edvr::flatObservationClears;
    using edvr::flatObservationToggle;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: local reject %s\n", name); ++failures; }
    };

    expect(flatLocalRefusalReason("unknown-scene-projection-recipe") &&
           flatLocalRefusalReason("unchanged-shader-mismatch") &&
           flatLocalRefusalReason("actual-shader-mismatch") &&
           flatLocalRefusalReason("projection-viewport-mismatch") &&
           flatLocalRefusalReason("projection-preparation-refused") &&
           flatLocalRefusalReason("draw-binding-refused"),
           "the six per-draw reasons are locally refusable");
    expect(!flatLocalRefusalReason("render-extent-changed") &&
           !flatLocalRefusalReason("invalid-render-extent") &&
           !flatLocalRefusalReason("scene-projection-depth-unassociated") &&
           !flatLocalRefusalReason("scene-depth-changed") &&
           !flatLocalRefusalReason("no-raster-application") &&
           !flatLocalRefusalReason("compute-binding-refused") &&
           !flatLocalRefusalReason("compute-source-invalidated") &&
           !flatLocalRefusalReason("unknown-context-state") &&
           !flatLocalRefusalReason("already-treated-this-frame") &&
           !flatLocalRefusalReason("producer-source-identity-mismatch") &&
           !flatLocalRefusalReason(nullptr),
           "frame-global reasons and a null reason are never locally refusable");

    // reviews/flat-temporal-main-review-2026-09-26.md: observation exit
    // requires a positively qualified handoff on a completely covered frame;
    // empty, failed, uncertain or foreign-work frames keep observing.
    expect(!flatObservationClears(true, false, true) &&
           !flatObservationClears(true, true, false) &&
           !flatObservationClears(true, false, false) &&
           flatObservationClears(true, true, true) &&
           !flatObservationClears(false, true, true),
           "observation clears only on a qualified, completely covered frame");
    expect(!flatObservationToggle(true, true, false) &&
           flatObservationToggle(true, true, true) &&
           flatObservationToggle(true, false, true) &&
           flatObservationToggle(true, false, false) &&
           !flatObservationToggle(false, true, false),
           "only the on-to-off toggle ends observation explicitly");

    return failures;
}

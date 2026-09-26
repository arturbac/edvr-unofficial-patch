#pragma once

#include <cstdio>
#include "../../src/d3d11/flat_local_reject.h"

inline int flatLocalRejectTests() {
    using edvr::flatLocalRefusalReason;
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

    return failures;
}

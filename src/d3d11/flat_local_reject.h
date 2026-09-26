#pragma once
#include <cstring>

// Local refusal ("partial" temporal AA), redesigned 2026-09-26 after
// docs/review-flat-temporal-aa-2026-09-26.md findings 1, 4 and 5: the
// stamp-mask/mixed-phase form of partial AA never enabled in the flat
// profile and was unproven where it mixed unjittered geometry into a
// jittered scene. The semantics now: one raster phase across shared scene
// depth/colour. A scene draw that cannot be jittered for a reason specific
// to that one draw still goes out unjittered (the proxy cannot stop the
// game's own draw); the frame's history is invalidated and the runtime
// returns to observation until a refusal-free frame requalifies the
// contract. It is never claimed as treated. Pure, header-only policy shared
// by flat_runtime.cpp and tools/flat_temporal_test so the exact
// locally-refusable-reason set is one tested table instead of two
// hand-matched copies. See docs/design-flat-temporal-aa-2026-09-23.md.
namespace edvr {

// The exact per-draw failPhase reasons local refusal turns into
// invalidate-and-observe. Every other reason stays frame-global by calling
// failPhase directly: render-extent-changed, invalid-render-extent,
// scene-projection-depth-unassociated, scene-depth-changed,
// no-raster-application, anything reached only from the compute/dispatch
// path (qualifyProjection's cs!=0 calls, compute-binding-refused,
// compute-source-invalidated, unknown-context-state), and every
// selector/model refusal downstream of the resolve
// (already-treated-this-frame, producer-source-identity-mismatch,
// actual-handoff-or-depth-view-refused, incomplete-jitter-frame,
// engine-source-not-ready, and the resolver's own failure reasons).
inline bool flatLocalRefusalReason(const char* reason) {
    if (!reason) return false;
    return std::strcmp(reason, "unknown-scene-projection-recipe") == 0 ||
        std::strcmp(reason, "unchanged-shader-mismatch") == 0 ||
        std::strcmp(reason, "actual-shader-mismatch") == 0 ||
        std::strcmp(reason, "projection-viewport-mismatch") == 0 ||
        std::strcmp(reason, "projection-preparation-refused") == 0 ||
        std::strcmp(reason, "draw-binding-refused") == 0;
}

} // namespace edvr

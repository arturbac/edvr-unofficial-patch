#pragma once
#include <cstring>

// Partial temporal AA ("local refusal"): pure, header-only policy shared by
// flat_runtime.cpp and tools/flat_temporal_test so the exact stampable-kind
// and locally-refusable-reason sets are one tested table instead of two
// hand-matched copies. See docs/design-flat-temporal-aa-2026-09-23.md.
namespace edvr {

// Kinds vscreen.cpp's hookedDraw/hookedDrawIndexed/hookedDrawInstanced/
// hookedDrawIndexedInstanced stash a full re-issue recipe for (count, start,
// base, startInstance, instances -- see FlatRuntimeDrawScope's ctor args at
// each call site). DrawAuto ('A'), both indirect kinds ('Y' DrawInstanced-
// Indirect, 'Z' DrawIndexedInstancedIndirect) and the ctor's own '?' default
// carry no such captured arguments, so a per-draw reason on one of those
// cannot be locally refused -- it keeps failing the whole frame's phase
// exactly as before partial AA existed.
constexpr bool flatDrawKindStampable(char kind) {
    return kind == 'D' || kind == 'I' || kind == 'N' || kind == 'X';
}

// The exact per-draw failPhase reasons partial AA may turn into a local
// refusal (stamp this one draw's pixels into the reject mask and leave the
// rest of the frame's jitter alone) instead of failing the whole frame's
// phase. Every other reason stays frame-global by calling failPhase
// directly: render-extent-changed, invalid-render-extent, scene-projection-
// depth-unassociated, scene-depth-changed, no-raster-application, anything
// reached only from the compute/dispatch path (qualifyProjection's cs!=0
// calls, compute-binding-refused, compute-source-invalidated, unknown-
// context-state), and every selector/model refusal downstream of the
// resolve (already-treated-this-frame, producer-source-identity-mismatch,
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

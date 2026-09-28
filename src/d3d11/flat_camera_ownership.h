#pragma once

// Owner selection between the legacy qualified-jitter path and the upstream
// camera injector, per docs/design-flat-camera-integration.md's C2-C table
// (revised per the C2-plan review's R5). One owner is selected BEFORE any
// mutation in the frame; the losing route's graphics AND compute mutations
// are suppressed; safe outcomes are named; ownership switches wait for
// outstanding work; per-view-group ownership never lets frame-wide counts
// hide a mixed frame.
//
// This header is the production-intended policy: C3 wires it into
// flat_runtime.cpp where refuseDraw/observing decide today
// (flat_runtime.cpp:970-986, 1830). The legacy semantics those lines encode
// are the inputs' contract, cited where they apply. Header-only and pure:
// the policy allocates nothing and performs no I/O, so it can run on the
// render thread and in the offline coexistence rig unchanged.

#include <cstdint>

namespace edvr {

// The routes that can jitter a frame.
enum class FlatCameraOwner : uint8_t { None, Legacy, Upstream };

// What a frame gets when no route may mutate it, or when mutation completes.
// All are valid states; a stable supported scene must make bounded progress
// out of the non-terminal ones.
enum class FlatCameraOutcome : uint8_t {
    Treated,     // an owner was selected and mutation proceeded
    Observing,   // collecting evidence; refusal-free frames requalify
    Unprepared,  // evidence complete, resources not yet negotiated
    Unsupported, // a named-unproven domain (e.g. projection kinds 4/5)
    Recovery,    // history invalidated; bounded re-warming in progress
};

// One view-group's inputs for the selection. Groups: main, cockpit,
// auxiliary (shadow/reflection/env passes). Per the setter map, auxiliary
// domains own and re-dirty their own camera structs per pass; they are
// separate groups, never summed into frame-wide counts.
struct FlatCameraGroupInput {
    bool upstreamCertified = false;   // the producer chain for this group's camera is
                                      // the mapped refresh and its projection kind is
                                      // in the proven set (1 ortho, 3 trig, default)
    bool upstreamUnsupported = false; // an explicitly named unproven domain (kinds 4/5)
    bool legacyEligible = false;      // the legacy selector would treat this group
    bool legacyObserving = false;     // the legacy selector is in observation here
    bool lateProjectionRewrite = false; // a true late rewrite was observed this frame
    bool uncoveredSecondary = false;  // an uncovered secondary camera was observed
};

struct FlatCameraOwnershipDecision {
    FlatCameraOwner owner = FlatCameraOwner::None;
    bool legacyGraphicsSuppressed = false; // legacy draw-path scope mutation off
    bool legacyComputeSuppressed = false;  // legacy dispatch-path scope mutation off
    FlatCameraOutcome outcome = FlatCameraOutcome::Observing;
    const char* reason = "";
    bool historyReset = false;          // closure/history invalidated this decision
    bool preserveCameraInputs = false;  // original inputs kept for motion reconstruction
};

// Cross-frame ownership state, per view-group.
struct FlatCameraOwnershipState {
    FlatCameraOwner outstanding = FlatCameraOwner::None; // owner with in-flight work
    bool outstandingApplied = false;  // that owner's mutation already reached a draw
    bool historyValid = false;
};

// The selection, called once per group per frame BEFORE any mutation (the
// consumption boundary the C2 plan's A6 split names). Pure: no allocation,
// no globals, no side effects beyond the returned decision.
inline FlatCameraOwnershipDecision flatCameraOwnerSelect(
    const FlatCameraOwnershipState& s, const FlatCameraGroupInput& in) {
    FlatCameraOwnershipDecision d;

    // A true late rewrite or an uncovered secondary camera invalidates
    // closure/history and produces a named safe outcome, never an owner
    // change riding on poisoned evidence (flat_runtime.cpp:970-986's
    // refuseDraw semantics: the frame fails coherently and observation
    // resumes).
    if (in.lateProjectionRewrite || in.uncoveredSecondary) {
        d.outcome = FlatCameraOutcome::Recovery;
        d.reason = in.lateProjectionRewrite ? "late-projection-rewrite"
                                            : "uncovered-secondary-camera";
        d.historyReset = true;
        d.preserveCameraInputs = true;
        return d;
    }

    // An explicitly named unproven domain: upstream never owns, by name;
    // the legacy route may still own if its own selector is eligible.
    if (in.upstreamUnsupported) {
        if (in.legacyEligible && s.outstanding != FlatCameraOwner::Upstream) {
            d.owner = FlatCameraOwner::Legacy;
            d.outcome = FlatCameraOutcome::Treated;
            d.reason = "legacy-eligible-upstream-unsupported";
        } else {
            d.outcome = FlatCameraOutcome::Unsupported;
            d.reason = "upstream-unsupported-projection-kind";
        }
        return d;
    }

    // Certified upstream lineage: the migration prefers it whenever the
    // legacy route is also present, and legacy observation cannot veto it
    // (the C2-plan review's R5: an unknown or color-only shader variant
    // must not park a certified lineage in observation).
    if (in.upstreamCertified) {
        // An ownership switch waits for outstanding work: if the legacy
        // route already applied a mutation this frame, the frame keeps the
        // old owner for coherence -- switching mid-frame would mix phases.
        if (s.outstanding == FlatCameraOwner::Legacy && s.outstandingApplied) {
            d.owner = FlatCameraOwner::Legacy;
            d.outcome = FlatCameraOutcome::Treated;
            d.reason = "switch-deferred-outstanding-legacy";
            return d;
        }
        d.owner = FlatCameraOwner::Upstream;
        d.outcome = FlatCameraOutcome::Treated;
        d.reason = in.legacyEligible ? "upstream-certified-legacy-suppressed"
                                     : "upstream-certified-legacy-not-eligible";
        if (in.legacyEligible) {
            // Both the draw and the dispatch mutation paths are suppressed
            // (flat_runtime.cpp:1820-1823 and its compute siblings).
            d.legacyGraphicsSuppressed = true;
            d.legacyComputeSuppressed = true;
        }
        if (s.outstanding == FlatCameraOwner::Legacy) {
            // A clean switch (no outstanding legacy work): history resets,
            // and the original camera inputs are preserved so motion
            // reconstruction still has an unjittered reference.
            d.historyReset = true;
            d.preserveCameraInputs = true;
        }
        return d;
    }

    // No certified lineage: the legacy route owns when its selector is
    // eligible; otherwise the frame observes (with bounded progress
    // required of a stable supported scene).
    if (in.legacyEligible) {
        d.owner = FlatCameraOwner::Legacy;
        d.outcome = FlatCameraOutcome::Treated;
        d.reason = "legacy-eligible-no-upstream";
        return d;
    }
    d.outcome = FlatCameraOutcome::Observing;
    d.reason = in.legacyObserving ? "legacy-observing-no-upstream"
                                  : "no-route-eligible";
    return d;
}

// The frame close for one group: fold the decision into the cross-frame
// state. Applied is recorded only after a mutation actually reached a draw
// (the same noteApplied discipline as flat_live_phase.h:49-52).
inline void flatCameraOwnerClose(FlatCameraOwnershipState& s,
                                 const FlatCameraOwnershipDecision& d, bool applied,
                                 bool cleanClosure) {
    if (d.historyReset) s.historyValid = false;
    if (d.owner != FlatCameraOwner::None) {
        s.outstanding = d.owner;
        s.outstandingApplied = applied;
        if (cleanClosure) s.historyValid = true;
    } else {
        s.outstanding = FlatCameraOwner::None;
        s.outstandingApplied = false;
    }
}

} // namespace edvr

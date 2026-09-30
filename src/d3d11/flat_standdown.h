// Flat runtime stand-down: stop doing per-draw and per-call temporal work while
// every frame is refused for a reason that cannot change by itself.
//
// WHY. Two rc.4 users had a temporal mode selected and every frame refused for
// the shape of the post chain (`no-known-tone-pass`: game AA, bloom or DoF put
// passes between the tone pass and the final copy). The refusal is decided in
// one place, at the game's final copy, but everything before it ran anyway on
// every draw and every constant-buffer write, and user 1 presented 60 fps
// against 130-270 with anti-aliasing off. A refused frame should cost almost
// nothing.
//
// WHAT COUNTS. Only the chain-shape reasons (flatMonoReasonStructural): the
// selector could not find or trust the tone pass or the output copy. Warming,
// resets, truncation, missing HDR writes and conflicting HDR are transient or
// diagnostic and never start a stand-down. A frame the runtime watched that
// reached no recognised output copy at all counts as no-known-output-copy: it
// could not have been treated either.
//
// WHAT IT DOES. A run of structurally refused frames lasting kFlatStandDownTriggerMs
// stands the work down (the runtime pauses the pieces; see flat_runtime.cpp
// applyWork). While stood down the runtime re-probes: every kFlatStandDownProbeMs
// one whole frame is watched with only the contract observation on (the online
// prefix model and the selector, the same code and inputs as an active frame).
// A probe frame the selector selects ends the stand-down. Anything else keeps it,
// and the next probe is due kFlatStandDownProbeMs later. A settings change in the
// game or the end of a loading screen therefore resumes within about two seconds.
//
// WHAT IT NEVER DOES. It never changes what a treated frame does: a session whose
// frames are selected stays in FlatWork::Full for good, and the selector and the
// online prefix model are the same functions they were. This header holds the
// policy only (pure, no clock of its own, no I/O) so tools\flat_temporal_test
// drives the very state machine the DLL runs.
#pragma once
#include "flat_mono_frame.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

// The trigger, the probe cadence and the report cadence, in one place.
constexpr uint64_t kFlatStandDownTriggerMs = 5000;   // structural refusal, unbroken, before the work stands down
constexpr uint64_t kFlatStandDownProbeMs = 1500;     // one whole frame watched this often while stood down
constexpr uint64_t kFlatStandDownReportMs = 30000;   // the "still stood down" line
constexpr uint64_t kFlatStandDownWarnMs = 2000;      // the F8 warning waits this long: a loading screen is not news

// The chain-shape refusals: what the selector says when the frame's post chain is
// not the one it recognises. These do not clear by themselves inside a scene: only
// a settings change, a mod change or a new scene changes the chain. Everything
// else is transient (warming, resets, truncation, no HDR yet) or a diagnostic
// conflict, and keeps the runtime doing its work. flat_temporal.cpp's chain dump
// asks the same question and uses this function.
inline bool flatMonoReasonStructural(FlatMonoReason reason) {
    switch (reason) {
    case FlatMonoReason::NoTonePass:
    case FlatMonoReason::AmbiguousTonePass:
    case FlatMonoReason::InvalidTonePass:
    case FlatMonoReason::NoOutputCopy:
    case FlatMonoReason::AmbiguousOutputCopy:
    case FlatMonoReason::InvalidOutputCopy:
    case FlatMonoReason::BrokenLineage:
    case FlatMonoReason::WrongOrder:
        return true;
    default:
        return false;
    }
}

// What the runtime does in the frame that starts now.
//   Full    everything, exactly as before the stand-down existed
//   Probe   one whole frame with the contract observation only (a stand-down probe)
//   Paused  nothing per draw or per call beyond the O(1) state trackers
enum class FlatWork : uint8_t { Full, Probe, Paused };
inline const char* flatWorkName(FlatWork work) {
    switch (work) {
    case FlatWork::Full: return "full";
    case FlatWork::Probe: return "probe";
    case FlatWork::Paused: return "paused";
    }
    return "?";
}

// What one frame showed about its chain. Ordered: within a frame the higher value
// dominates (a copy draw that selected makes the frame treatable whatever another
// copy draw said).
enum class FlatFrameSeen : uint8_t { None, Structural, Transient, Treatable };
inline FlatFrameSeen flatFrameSeenFor(bool selected, FlatMonoReason reason) {
    if (selected) return FlatFrameSeen::Treatable;
    return flatMonoReasonStructural(reason) ? FlatFrameSeen::Structural : FlatFrameSeen::Transient;
}

enum class FlatStandDownEvent : uint8_t { None, Entered, Resumed };

// Everything the runtime does when it stands down, as one string, for the log line
// that announces it. The runtime's applyWork is the code behind each entry and the
// rig scans the runtime source for the gate of each.
inline constexpr const char* kFlatStandDownPausedWork =
    "per-draw coverage classification and legacy projection readiness "
    "(shader/viewport/binding checks, jitter preparation), constant-buffer shadow "
    "tracking on every Map/Unmap/Update, the camera-write witness, the camera "
    "refresh hook, engine-motion hooks and shader/render-target substitution, the "
    "discovery observers, the draw-capture and audit probes; kept: the O(1) state "
    "trackers (viewport, constant-buffer binds, ClearState)";

struct FlatStandDown {
    // ---- state ---------------------------------------------------------------
    bool standing = false;
    FlatWork current = FlatWork::Full;   // the mode of the frame in flight
    // The unbroken run of structurally refused frames (Full frames only).
    uint64_t runSinceMs = 0, runFrames = 0;
    FlatMonoReason runReason = FlatMonoReason::NoTonePass;
    // Stood-down bookkeeping, valid while standing (and kept for the resume line).
    uint64_t standSinceMs = 0, nextProbeMs = 0, nextReportMs = 0;
    uint64_t pausedFrames = 0, probes = 0;
    uint64_t enteredAfterMs = 0, enteredAfterFrames = 0;
    FlatMonoReason enteredReason = FlatMonoReason::NoTonePass;
    FlatMonoReason probeReason = FlatMonoReason::NoTonePass;
    FlatFrameSeen probeSeen = FlatFrameSeen::None;
    // Session totals for the census line.
    uint64_t entries = 0, resumes = 0, totalStoodDownMs = 0;
    // What the last resume or wake reported (for its line).
    uint64_t lastStoodDownMs = 0, lastProbes = 0;

    // ---- the frame boundary ----------------------------------------------------
    // Called once per completed frame on the owner thread. `observed` is whether the
    // runtime watched the frame's draws (Full and Probe frames are, Paused frames are
    // not); `seen` and `reason` are the frame's verdict on its chain (None when no
    // copy draw was reached).
    FlatStandDownEvent frameEnded(FlatFrameSeen seen, FlatMonoReason reason, bool observed,
                                  uint64_t nowMs) {
        if (!observed) {
            if (standing && current == FlatWork::Paused) ++pausedFrames;
            // A probe frame nothing watched found nothing out: the next one is not due
            // before the cadence says so.
            if (standing && current == FlatWork::Probe) nextProbeMs = nowMs + kFlatStandDownProbeMs;
            return FlatStandDownEvent::None;
        }
        if (seen == FlatFrameSeen::None) {
            // A watched frame that never reached a recognised output copy: the same
            // finding the selector reports when the copy is not there.
            seen = FlatFrameSeen::Structural;
            reason = FlatMonoReason::NoOutputCopy;
        }
        if (standing) {
            if (current != FlatWork::Probe) return FlatStandDownEvent::None;   // not a frame this reads
            ++probes;
            probeSeen = seen;
            probeReason = reason;
            if (seen == FlatFrameSeen::Treatable) return resume(nowMs);
            nextProbeMs = nowMs + kFlatStandDownProbeMs;
            return FlatStandDownEvent::None;
        }
        if (seen != FlatFrameSeen::Structural) {
            runSinceMs = 0;
            runFrames = 0;
            return FlatStandDownEvent::None;
        }
        if (!runSinceMs) {
            runSinceMs = nowMs ? nowMs : 1;
            runFrames = 0;
        }
        ++runFrames;
        runReason = reason;
        if (nowMs - runSinceMs >= kFlatStandDownTriggerMs) return enter(nowMs);
        return FlatStandDownEvent::None;
    }

    // The mode of the frame that starts now. Called once per frame, after
    // frameEnded, on the owner thread.
    FlatWork nextFrame(uint64_t nowMs) {
        if (!standing) return current = FlatWork::Full;
        if (nowMs >= nextProbeMs) return current = FlatWork::Probe;
        return current = FlatWork::Paused;
    }

    // A change that invalidates what the stand-down learned (the AA mode changed, a
    // device or swap-chain reset, an F10 audit asked for everything): forget the run
    // and go back to Full at once. True when it ended an active stand-down.
    bool wake(uint64_t nowMs) {
        const bool was = standing;
        if (was) {
            lastStoodDownMs = nowMs > standSinceMs ? nowMs - standSinceMs : 0;
            lastProbes = probes;
            totalStoodDownMs += lastStoodDownMs;
            ++resumes;
        }
        standing = false;
        current = FlatWork::Full;
        runSinceMs = 0;
        runFrames = 0;
        return was;
    }

    // Whether the "still stood down" line is due; advances its own clock.
    bool reportDue(uint64_t nowMs) {
        if (!standing || nowMs < nextReportMs) return false;
        nextReportMs = nowMs + kFlatStandDownReportMs;
        return true;
    }

    // The reason the current refusal names: the run's while active, the one that
    // started the stand-down while stood down.
    FlatMonoReason reason() const { return standing ? enteredReason : runReason; }

    // The F8 warning's gate: a structural refusal in force -- stood down, or a run
    // long enough to be more than a loading screen. The caller has already checked
    // that a temporal mode is selected.
    bool warningActive(uint64_t nowMs) const {
        if (standing) return true;
        return runSinceMs && nowMs > runSinceMs && nowMs - runSinceMs >= kFlatStandDownWarnMs;
    }

    // How long the current structural run has lasted, ms (0 when none).
    uint64_t runMs(uint64_t nowMs) const { return runSinceMs && nowMs > runSinceMs ? nowMs - runSinceMs : 0; }

private:
    FlatStandDownEvent enter(uint64_t nowMs) {
        standing = true;
        current = FlatWork::Paused;
        standSinceMs = nowMs;
        nextProbeMs = nowMs + kFlatStandDownProbeMs;
        nextReportMs = nowMs + kFlatStandDownReportMs;
        enteredAfterMs = nowMs - runSinceMs;
        enteredAfterFrames = runFrames;
        enteredReason = runReason;
        probes = 0;
        pausedFrames = 0;
        probeSeen = FlatFrameSeen::None;
        runSinceMs = 0;
        runFrames = 0;
        ++entries;
        return FlatStandDownEvent::Entered;
    }
    FlatStandDownEvent resume(uint64_t nowMs) {
        lastStoodDownMs = nowMs > standSinceMs ? nowMs - standSinceMs : 0;
        lastProbes = probes;
        totalStoodDownMs += lastStoodDownMs;
        ++resumes;
        standing = false;
        current = FlatWork::Full;
        runSinceMs = 0;
        runFrames = 0;
        return FlatStandDownEvent::Resumed;
    }
};

// ---- the log lines, built here so the rig prints exactly what the DLL writes ----
// Three lines, each with a prefix a reader can search for. "flat stand-down:" never
// appears in a session that never stood down; the periodic line repeats every 30 s
// for as long as it lasts, so "never ran" and "stood down" cannot be confused.
inline int flatStandDownFormatEntered(char* out, size_t size, uint64_t frame, const FlatStandDown& s) {
    return std::snprintf(out, size,
        "flat stand-down: entered at frame=%llu: every frame for %.1f s (%llu frames) was refused for %s, "
        "none treated; paused: %s; re-probing one whole frame every %.1f s, resuming when a frame is "
        "treatable (a settings change in the game, or a loading screen ending)",
        static_cast<unsigned long long>(frame), static_cast<double>(s.enteredAfterMs) / 1000.0,
        static_cast<unsigned long long>(s.enteredAfterFrames), flatMonoReasonName(s.enteredReason),
        kFlatStandDownPausedWork, static_cast<double>(kFlatStandDownProbeMs) / 1000.0);
}
// `why` is null for the ordinary resume (a probe frame was treatable); a wake names its cause.
inline int flatStandDownFormatResumed(char* out, size_t size, uint64_t frame, const FlatStandDown& s,
                                      const char* why) {
    if (why)
        return std::snprintf(out, size,
            "flat stand-down: ended at frame=%llu after %.1f s stood down (%llu probes): %s; all work restarts",
            static_cast<unsigned long long>(frame), static_cast<double>(s.lastStoodDownMs) / 1000.0,
            static_cast<unsigned long long>(s.lastProbes), why);
    return std::snprintf(out, size,
        "flat stand-down: resumed at frame=%llu after %.1f s stood down (%llu probes): a probe frame's "
        "chain was recognised and selected; all work restarts",
        static_cast<unsigned long long>(frame), static_cast<double>(s.lastStoodDownMs) / 1000.0,
        static_cast<unsigned long long>(s.lastProbes));
}
inline int flatStandDownFormatStill(char* out, size_t size, uint64_t frame, const FlatStandDown& s,
                                    uint64_t nowMs) {
    return std::snprintf(out, size,
        "flat stand-down: still stood down at frame=%llu after %.0f s: refused for %s, %llu probes so far "
        "(last probe: %s), %llu frames skipped",
        static_cast<unsigned long long>(frame),
        static_cast<double>(nowMs > s.standSinceMs ? nowMs - s.standSinceMs : 0) / 1000.0,
        flatMonoReasonName(s.enteredReason), static_cast<unsigned long long>(s.probes),
        s.probeSeen == FlatFrameSeen::None ? "none yet"
        : s.probeSeen == FlatFrameSeen::Treatable ? "treatable"
        : flatMonoReasonName(s.probeReason),
        static_cast<unsigned long long>(s.pausedFrames));
}

}  // namespace edvr

#pragma once

// The flat runtime's stand-down state machine (src\d3d11\flat_standdown.h), driven
// on a mock clock at 60 fps: the trigger, the probe cadence, the re-probe latency,
// the frames that must not start it, the wake, the report cadence, the warning gate
// and the three log lines. The model-driven cases (real refused chains through the
// online prefix model) and the source-scan wiring pins live in flat_temporal_test.cpp,
// which owns the captured-frame fixture.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "../../src/d3d11/flat_standdown.h"

namespace stand_down_test {

// One Present-to-Present cadence of the runtime, without the runtime: the frame in
// flight runs in `work`, shows a verdict only if it was watched, and ends 16 ms later.
struct Sim {
    edvr::FlatStandDown machine;
    uint64_t now = 1000;
    edvr::FlatWork work = edvr::FlatWork::Full;
    uint64_t frames = 0, watched = 0;
    edvr::FlatStandDownEvent frame(edvr::FlatFrameSeen seenIfWatched,
                                   edvr::FlatMonoReason reason = edvr::FlatMonoReason::NoTonePass,
                                   bool observedOverride = true) {
        now += 16;
        ++frames;
        const bool isWatched = observedOverride && work != edvr::FlatWork::Paused;
        if (isWatched) ++watched;
        const auto event = machine.frameEnded(isWatched ? seenIfWatched : edvr::FlatFrameSeen::None,
                                              reason, isWatched, now);
        work = machine.nextFrame(now);
        return event;
    }
};

}  // namespace stand_down_test

inline int flatStandDownTests() {
    using namespace edvr;
    using stand_down_test::Sim;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: stand-down %s\n", name); ++failures; }
    };

    // ---- the structural set: exactly the chain-shape reasons ----------------------
    {
        const FlatMonoReason all[] = {
            FlatMonoReason::Selected, FlatMonoReason::InvalidInput, FlatMonoReason::UnknownOutput,
            FlatMonoReason::Truncated, FlatMonoReason::ForeignWork, FlatMonoReason::NoOutputCopy,
            FlatMonoReason::AmbiguousOutputCopy, FlatMonoReason::InvalidOutputCopy,
            FlatMonoReason::NoTonePass, FlatMonoReason::AmbiguousTonePass, FlatMonoReason::InvalidTonePass,
            FlatMonoReason::BrokenLineage, FlatMonoReason::WrongOrder, FlatMonoReason::MissingCamera,
            FlatMonoReason::InvalidCamera, FlatMonoReason::NoHdr, FlatMonoReason::ConflictingHdr,
            FlatMonoReason::NoHdrCamera, FlatMonoReason::NoSupportedSource, FlatMonoReason::AmbiguousSource,
            FlatMonoReason::InvalidSource};
        bool table = true;
        unsigned structural = 0;
        for (FlatMonoReason r : all) {
            const bool want = r == FlatMonoReason::NoTonePass || r == FlatMonoReason::AmbiguousTonePass ||
                r == FlatMonoReason::InvalidTonePass || r == FlatMonoReason::NoOutputCopy ||
                r == FlatMonoReason::AmbiguousOutputCopy || r == FlatMonoReason::InvalidOutputCopy ||
                r == FlatMonoReason::BrokenLineage || r == FlatMonoReason::WrongOrder;
            if (flatMonoReasonStructural(r) != want) table = false;
            structural += flatMonoReasonStructural(r) ? 1u : 0u;
        }
        expect(table && structural == 8 && sizeof(all) / sizeof(all[0]) == 21,
               "the structural reasons are exactly the eight chain-shape ones, of all 21");
        expect(flatFrameSeenFor(true, FlatMonoReason::Selected) == FlatFrameSeen::Treatable &&
               flatFrameSeenFor(false, FlatMonoReason::NoTonePass) == FlatFrameSeen::Structural &&
               flatFrameSeenFor(false, FlatMonoReason::Truncated) == FlatFrameSeen::Transient &&
               flatFrameSeenFor(false, FlatMonoReason::NoHdr) == FlatFrameSeen::Transient,
               "a frame's verdict: selected is treatable, a chain-shape refusal structural, the rest transient");
        expect(FlatFrameSeen::None < FlatFrameSeen::Structural && FlatFrameSeen::Structural < FlatFrameSeen::Transient &&
               FlatFrameSeen::Transient < FlatFrameSeen::Treatable,
               "the merge order within a frame: treatable dominates transient dominates structural");
    }

    // ---- the trigger: an unbroken 5 s run of structural frames ---------------------
    {
        Sim sim;
        uint64_t firstStructural = 0, enteredAt = 0;
        for (int i = 0; i < 1000 && !enteredAt; ++i) {
            const auto event = sim.frame(FlatFrameSeen::Structural);
            if (!firstStructural) firstStructural = sim.now;
            if (event == FlatStandDownEvent::Entered) enteredAt = sim.now;
            else expect(!sim.machine.standing, "not stood down before the event");
        }
        const uint64_t after = enteredAt - firstStructural;
        expect(enteredAt && after >= kFlatStandDownTriggerMs && after < kFlatStandDownTriggerMs + 32,
               "a 5 s unbroken structural run stands the work down, not before it and not long after");
        expect(sim.machine.standing && sim.work == FlatWork::Paused && sim.machine.entries == 1 &&
               sim.machine.enteredReason == FlatMonoReason::NoTonePass &&
               sim.machine.enteredAfterMs == after && sim.machine.enteredAfterFrames > 300,
               "entering records the reason, the run length and the frames, and pauses");
    }
    {   // 4.9 s of structural frames then one transient frame: the run never reaches 5 s, twice over.
        Sim sim;
        bool ever = false;
        for (int round = 0; round < 3; ++round) {
            for (int i = 0; i < 300; ++i)   // 4.8 s
                if (sim.frame(FlatFrameSeen::Structural) == FlatStandDownEvent::Entered) ever = true;
            if (sim.frame(FlatFrameSeen::Transient, FlatMonoReason::Truncated) == FlatStandDownEvent::Entered) ever = true;
        }
        expect(!ever && !sim.machine.standing,
               "a transient frame (warming, truncation, missing HDR) breaks the run: no stand-down");
    }
    {   // The same with a treatable frame breaking it.
        Sim sim;
        bool ever = false;
        for (int round = 0; round < 3; ++round) {
            for (int i = 0; i < 300; ++i)
                if (sim.frame(FlatFrameSeen::Structural) == FlatStandDownEvent::Entered) ever = true;
            if (sim.frame(FlatFrameSeen::Treatable, FlatMonoReason::Selected) == FlatStandDownEvent::Entered) ever = true;
        }
        expect(!ever && !sim.machine.standing, "a treatable frame breaks the run: no stand-down");
    }
    {   // A treated session: 10 minutes of selected frames never leave Full.
        Sim sim;
        bool ever = false;
        for (int i = 0; i < 60 * 600; ++i) {
            if (sim.frame(FlatFrameSeen::Treatable, FlatMonoReason::Selected) != FlatStandDownEvent::None) ever = true;
            if (sim.work != FlatWork::Full) ever = true;
        }
        expect(!ever && sim.machine.entries == 0 && sim.machine.probes == 0,
               "a session whose frames are selected stays in Full for good");
    }
    {   // Frames nothing watched neither start nor break a run.
        Sim sim;
        for (int i = 0; i < 100; ++i) sim.frame(FlatFrameSeen::Structural);
        const uint64_t frames = sim.machine.runFrames, since = sim.machine.runSinceMs;
        for (int i = 0; i < 50; ++i) sim.frame(FlatFrameSeen::Treatable, FlatMonoReason::Selected, /*observed=*/false);
        expect(frames == 100 && sim.machine.runFrames == frames && sim.machine.runSinceMs == since && !sim.machine.standing,
               "an unwatched frame is no evidence either way: it neither breaks nor extends the run");
    }
    {   // A watched frame that reached no recognised copy draw is a no-known-output-copy refusal.
        Sim sim;
        bool entered = false;
        for (int i = 0; i < 1000 && !entered; ++i)
            entered = sim.frame(FlatFrameSeen::None) == FlatStandDownEvent::Entered;
        expect(entered && sim.machine.enteredReason == FlatMonoReason::NoOutputCopy &&
               flatMonoReasonStructural(sim.machine.enteredReason),
               "watched frames with no output copy at all count as no-known-output-copy and stand down");
    }
    {   // The run's reason is the last structural one: a chain that changes shape keeps counting.
        Sim sim;
        FlatStandDownEvent last = FlatStandDownEvent::None;
        for (int i = 0; i < 1000 && last != FlatStandDownEvent::Entered; ++i)
            last = sim.frame(FlatFrameSeen::Structural, i % 2 ? FlatMonoReason::NoTonePass : FlatMonoReason::InvalidTonePass);
        expect(last == FlatStandDownEvent::Entered, "structural reasons that alternate still make one run");
    }

    // ---- stood down: paused frames, then a probe every 1.5 s -----------------------
    {
        Sim sim;
        while (sim.frame(FlatFrameSeen::Structural) != FlatStandDownEvent::Entered) {}
        const uint64_t enteredAt = sim.now;
        uint64_t firstProbeAt = 0, pausedBefore = 0;
        while (!firstProbeAt) {
            sim.frame(FlatFrameSeen::Structural);
            if (sim.work == FlatWork::Probe) firstProbeAt = sim.now;
            else { expect(sim.work == FlatWork::Paused, "between probes the frame is Paused"); ++pausedBefore; }
            if (sim.now - enteredAt > 5000) break;
        }
        expect(firstProbeAt >= enteredAt + kFlatStandDownProbeMs && firstProbeAt < enteredAt + kFlatStandDownProbeMs + 32,
               "the first probe frame starts 1.5 s after the stand-down");
        // Every frame that ended before the probe frame started was a Paused one, and counted.
        expect(pausedBefore > 80 && sim.machine.pausedFrames == pausedBefore + 1,
               "the frames before it are counted as paused");
        // The probe frame: watched, structural again -> stays, and the next probe is 1.5 s after IT ends.
        const auto event = sim.frame(FlatFrameSeen::Structural);
        expect(event == FlatStandDownEvent::None && sim.machine.standing && sim.machine.probes == 1 &&
               sim.machine.probeSeen == FlatFrameSeen::Structural && sim.work == FlatWork::Paused,
               "a structurally refused probe keeps the stand-down and counts as a probe");
        uint64_t secondProbeAt = 0, probeEnd = sim.now;
        while (!secondProbeAt && sim.now - probeEnd < 4000) {
            sim.frame(FlatFrameSeen::Structural);
            if (sim.work == FlatWork::Probe) secondProbeAt = sim.now;
        }
        expect(secondProbeAt >= probeEnd + kFlatStandDownProbeMs && secondProbeAt < probeEnd + kFlatStandDownProbeMs + 32,
               "probes repeat every 1.5 s");
        // A transient probe (the scene is loading) is not a resume either.
        sim.frame(FlatFrameSeen::Transient, FlatMonoReason::NoHdr);
        expect(sim.machine.standing && sim.machine.probeSeen == FlatFrameSeen::Transient,
               "a transient probe frame keeps the stand-down");
    }

    // ---- resume: a treatable probe, within about two seconds of the scene changing ----
    {
        // The settings change (or the loading screen ends) at a moment of the test's choosing.
        for (uint64_t offsetMs : {0ull, 300ull, 700ull, 1100ull, 1499ull, 2400ull, 3777ull}) {
            Sim sim;
            while (sim.frame(FlatFrameSeen::Structural) != FlatStandDownEvent::Entered) {}
            const uint64_t enteredAt = sim.now;
            const uint64_t treatableFrom = enteredAt + offsetMs;
            uint64_t resumedAt = 0;
            for (int i = 0; i < 1000 && !resumedAt; ++i) {
                const bool treatable = sim.now >= treatableFrom;
                const auto event = sim.frame(treatable ? FlatFrameSeen::Treatable : FlatFrameSeen::Structural,
                                             treatable ? FlatMonoReason::Selected : FlatMonoReason::NoTonePass);
                if (event == FlatStandDownEvent::Resumed) resumedAt = sim.now;
            }
            expect(resumedAt != 0, "a treatable scene resumes the work");
            // The frame that shows it must be a probe frame; the worst case is one cadence and a frame or two.
            expect(resumedAt >= treatableFrom && resumedAt - treatableFrom <= 2000,
                   "the re-probe picks up a treatable scene within two seconds");
            expect(!sim.machine.standing && sim.work == FlatWork::Full && sim.machine.resumes == 1 &&
                   sim.machine.runSinceMs == 0 && sim.machine.totalStoodDownMs == sim.machine.lastStoodDownMs &&
                   sim.machine.lastProbes >= 1,
                   "after the resume the machine is active, clean, and counted");
        }
    }
    {   // After a resume the same 5 s rule applies afresh (a second cycle).
        Sim sim;
        while (sim.frame(FlatFrameSeen::Structural) != FlatStandDownEvent::Entered) {}
        while (sim.frame(FlatFrameSeen::Treatable, FlatMonoReason::Selected) != FlatStandDownEvent::Resumed) {}
        bool secondEntry = false;
        for (int i = 0; i < 200; ++i)   // 3.2 s: not enough
            if (sim.frame(FlatFrameSeen::Structural) == FlatStandDownEvent::Entered) secondEntry = true;
        expect(!secondEntry, "a resumed session needs the full 5 s again");
        for (int i = 0; i < 200; ++i)
            if (sim.frame(FlatFrameSeen::Structural) == FlatStandDownEvent::Entered) secondEntry = true;
        expect(secondEntry && sim.machine.entries == 2, "and stands down again after it");
    }

    // ---- wake: the mode changed, the device was reset, an audit asked for everything ----
    {
        Sim sim;
        while (sim.frame(FlatFrameSeen::Structural) != FlatStandDownEvent::Entered) {}
        for (int i = 0; i < 20; ++i) sim.frame(FlatFrameSeen::Structural);
        expect(sim.machine.wake(sim.now), "a wake reports that it ended a stand-down");
        sim.work = sim.machine.nextFrame(sim.now);
        expect(!sim.machine.standing && sim.work == FlatWork::Full && sim.machine.resumes == 1 &&
               sim.machine.lastStoodDownMs > 0,
               "a wake returns to Full at once and counts");
        expect(!sim.machine.wake(sim.now), "waking an active machine reports nothing");
    }

    // ---- the periodic line: every 30 s while stood down, never otherwise ----------
    {
        Sim sim;
        unsigned dueBefore = 0;
        for (int i = 0; i < 250; ++i) { sim.frame(FlatFrameSeen::Structural); dueBefore += sim.machine.reportDue(sim.now); }
        expect(dueBefore == 0, "no periodic line while the work is active");
        while (!sim.machine.standing) sim.frame(FlatFrameSeen::Structural);
        const uint64_t enteredAt = sim.now;
        unsigned due = 0;
        uint64_t firstDue = 0, secondDue = 0;
        while (sim.now - enteredAt < 65000) {
            sim.frame(FlatFrameSeen::Structural);
            if (sim.machine.reportDue(sim.now)) { ++due; (firstDue ? secondDue : firstDue) = sim.now; }
        }
        expect(due == 2 && firstDue >= enteredAt + kFlatStandDownReportMs &&
               firstDue < enteredAt + kFlatStandDownReportMs + 32 &&
               secondDue >= firstDue + kFlatStandDownReportMs && secondDue < firstDue + kFlatStandDownReportMs + 32,
               "the still-stood-down line is due every 30 s while stood down");
    }

    // ---- the F8 warning gate: stood down, or a run longer than a loading blink -------
    {
        Sim sim;
        expect(!sim.machine.warningActive(sim.now), "no warning before any refusal");
        sim.frame(FlatFrameSeen::Structural);
        const uint64_t runStart = sim.now;
        while (sim.now - runStart < kFlatStandDownWarnMs - 40) sim.frame(FlatFrameSeen::Structural);
        expect(!sim.machine.warningActive(sim.now), "a structural run under 2 s does not warn");
        while (sim.now - runStart < kFlatStandDownWarnMs + 20) sim.frame(FlatFrameSeen::Structural);
        expect(sim.machine.warningActive(sim.now) && !sim.machine.standing, "a run of 2 s warns before the stand-down");
        sim.frame(FlatFrameSeen::Treatable, FlatMonoReason::Selected);
        expect(!sim.machine.warningActive(sim.now), "a treatable frame ends the warning");
        while (!sim.machine.standing) sim.frame(FlatFrameSeen::Structural);
        expect(sim.machine.warningActive(sim.now), "stood down always warns");
        while (sim.frame(FlatFrameSeen::Treatable, FlatMonoReason::Selected) != FlatStandDownEvent::Resumed) {}
        expect(!sim.machine.warningActive(sim.now), "resumed: the warning ends");
    }

    // ---- the log lines ------------------------------------------------------------
    {
        Sim sim;
        while (sim.frame(FlatFrameSeen::Structural, FlatMonoReason::NoTonePass) != FlatStandDownEvent::Entered) {}
        char line[2048];
        int n = flatStandDownFormatEntered(line, sizeof(line), 4242, sim.machine);
        const std::string entered(line);
        expect(n > 0 && entered.rfind("flat stand-down: entered at frame=4242", 0) == 0 &&
               entered.find("no-known-tone-pass") != std::string::npos &&
               entered.find("5.0 s") != std::string::npos && entered.find("paused:") != std::string::npos &&
               entered.find("every 1.5 s") != std::string::npos,
               "the entered line names the frame, the reason, how long frames were refused, what was paused, and the cadence");
        // What was paused is enumerated in the line, family by family.
        const char* families[] = {"coverage classification", "projection readiness", "constant-buffer shadow",
                                  "camera-write witness", "camera refresh hook", "engine-motion hooks",
                                  "discovery observers", "state trackers"};
        bool all = true;
        for (const char* f : families) if (entered.find(f) == std::string::npos) all = false;
        expect(all, "the entered line lists every paused piece and the trackers it keeps");
        // The log truncates at about 1160 characters after its timestamp; the line must fit whole.
        expect(entered.size() < 1100, "the entered line fits the log's line limit without truncation");

        n = flatStandDownFormatStill(line, sizeof(line), 4999, sim.machine, sim.now + 31000);
        const std::string still(line);
        expect(n > 0 && still.rfind("flat stand-down: still stood down at frame=4999", 0) == 0 &&
               still.find("no-known-tone-pass") != std::string::npos && still.find("none yet") != std::string::npos &&
               still.find("31 s") != std::string::npos,
               "the periodic line says it is still stood down, for how long, why, and the probes so far");

        while (sim.frame(FlatFrameSeen::Treatable, FlatMonoReason::Selected) != FlatStandDownEvent::Resumed) {}
        n = flatStandDownFormatResumed(line, sizeof(line), 5100, sim.machine, nullptr);
        const std::string resumed(line);
        expect(n > 0 && resumed.rfind("flat stand-down: resumed at frame=5100", 0) == 0 &&
               resumed.find("probe frame") != std::string::npos && resumed.find("all work restarts") != std::string::npos,
               "the resume line says a probe frame was recognised");
        n = flatStandDownFormatResumed(line, sizeof(line), 5200, sim.machine, "the anti-aliasing mode changed");
        const std::string woken(line);
        expect(n > 0 && woken.rfind("flat stand-down: ended at frame=5200", 0) == 0 &&
               woken.find("the anti-aliasing mode changed") != std::string::npos,
               "a wake's line names its cause");
        // The three lines are distinguishable from one another and from the census: "resumed" only
        // in the resume line, "still stood down" only in the periodic one.
        expect(entered.find("resumed") == std::string::npos && still.find("resumed") == std::string::npos &&
               resumed.find("still stood down") == std::string::npos,
               "the three lines cannot be confused with each other");
    }
    return failures;
}

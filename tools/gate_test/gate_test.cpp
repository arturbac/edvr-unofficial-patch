// gate_test -- replays frame sequences through the head-offset gate.
//
// WHY
//
// The gate decides whether to move the viewpoint of a headset somebody is
// wearing, and its failure class is "the offset applied in the cockpit". It had
// no automated coverage at all: the ~500-line decision machine was only ever
// exercised by flying the game and reading a log afterwards.
//
// Six defects in it were found that way, at roughly one test flight each, and
// two more were found by a code review that the flights had already passed.
// Every one of them is a replayable sequence of (panel draws, eye draws, key
// press, view index) -- which is the whole input surface. The gate is pure:
// counters in, one published bit out.
//
// WHAT EACH SCENARIO IS
//
// The named ones below are not invented cases. They are the situations that
// have actually gone wrong, kept as tests so they cannot go wrong quietly again.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/guard.h"
#include "../../src/common/periodic_work.h"
#include "../../src/common/timing.h"
#include "../../src/common/log.h"
// Header-only, so this adds no link dependency: the grouping is pure pointer
// arithmetic and lives in the header precisely so it can be asserted here.
#include "../../src/d3d11/camera_view.h"
#include "../../src/d3d11/head_offset_gate.h"
// Linked into this fixture already; the two pure decisions it exposes --
// journalPickNewest and cameraViewRebuildBackoff -- had no coverage at all
// until the review rounds on issue #19 found three arithmetic bugs between
// them, every one of which is one line of table here.
#include "../../src/d3d11/journal_watch.h"
// Same reason: eyeShapedAtScale is the recogniser rule that decides whether
// the gate is fed at all, and it is inline in the header so this file can
// assert it against the sizes a real rig produced.
#include "../../src/d3d11/vscreen.h"

using namespace edvr;

namespace {

int g_bad = 0;
uint32_t g_frame = 0;

// A FAKE CLOCK, ADVANCED ONE FRAME PERIOD PER FRAME.
//
// The gate's thresholds are durations now (see src/common/timing.h), so a
// fixture that steps frames without time passing would find every one of them
// permanently unelapsed -- a gate that can never drop a latch, never end a
// panel run, never close a grace window. The clock has to move with the
// frames, and moving it HERE rather than sleeping keeps the suite as fast as
// it was.
//
// The rate is a variable, not a constant, because that is the point: the same
// scenarios run at 72, 90 and 120Hz and must reach the same verdicts. That is
// the property the conversion exists to give, so it is the property the suite
// asserts, rather than being taken on the strength of the arithmetic.
uint64_t g_fakeMs = 0;
uint32_t g_rateHz = 90;

uint64_t fakeClock() { return g_fakeMs; }

// One frame of wall clock at the current rate. Accumulated in microseconds so
// that 72 and 120 do not drift: a whole-millisecond step would be 13ms at 72Hz
// against 13.888 real, losing a second every sixteen, which across fixtures
// that run thousands of frames is the difference between passing and failing.
// The microsecond accumulator leaves 0.006% instead.
uint64_t g_fakeUs = 0;
void advanceOneFrame() {
    g_fakeUs += 1000000ull / g_rateHz;
    g_fakeMs = g_fakeUs / 1000ull;
}

// The view the SHIPPED ini asks for, rather than a number written here.
//
// The scenarios are about the gate's logic, not about which view somebody has
// tuned their offsets for -- that changes, and it just did, from 1 to 2. A test
// that hardcodes it fails for the one reason that is not a bug, which trains
// people to edit the test until it goes quiet.
//
// Reading it from the file also means these scenarios run against the
// configuration that ships, so a default nobody can reach is a test failure
// rather than a surprise in a headset.
int g_wantView = 2;
int g_otherView = 3;   // any view that is not the wanted one

// A frame with the flat on-foot panel composited: first person.
void panelFrame(uint32_t n = 1) {
    for (uint32_t i = 0; i < n; ++i) {
        advanceOneFrame();
        headOffsetGateFrame(g_frame++, 4, 120);
    }
}

// A frame with a full stereo scene and no panel: the external camera, the
// cockpit, or anything else that draws the world into both eyes.
void sceneFrame(uint32_t n = 1) {
    for (uint32_t i = 0; i < n; ++i) {
        advanceOneFrame();
        headOffsetGateFrame(g_frame++, 0, 2500);
    }
}

// THE SAME TWO ON A RIG WHERE THE RECOGNISER IS BARELY SEEING ANYTHING.
//
// Not an invented shape: a Steam install running EDHM and a dxgi.dll wrapper
// reported eye-draw peaks of 18 and 20 for whole sessions of real play
// (2026-08-19, two logs), against 975 and 1074 measured here. The panel is
// still recognised and the void still clears twice a frame -- eye-sized
// targets exist -- but the world is drawn into something else and only a
// dozen passes a frame land on an eye texture. Everything the gate reads
// except the draw count is intact, which is exactly what makes it hard to
// see from a log.
void starvedPanelFrame(uint32_t n = 1) {
    for (uint32_t i = 0; i < n; ++i) {
        advanceOneFrame();
        headOffsetGateFrame(g_frame++, 4, 12);
    }
}
void starvedSceneFrame(uint32_t n = 1) {
    for (uint32_t i = 0; i < n; ++i) {
        advanceOneFrame();
        headOffsetGateFrame(g_frame++, 0, 12);
    }
}

// Neither: a menu, a loading screen, a mode change we cannot see.
void idleFrame(uint32_t n = 1) {
    for (uint32_t i = 0; i < n; ++i) {
        advanceOneFrame();
        headOffsetGateFrame(g_frame++, 0, 3);
    }
}

// THE SAME THREE, IN MILLISECONDS.
//
// A fixture that says sceneFrame(90) because a measurement said "+90 frames"
// has hidden a duration inside a frame count exactly the way the code used to,
// and it fails at the other two rates for that reason and no other. Where a
// scenario is about how LONG something lasted -- a status sample arriving on
// the game's ~1 Hz cadence, a grace window expiring, a player taking three
// seconds to press a key after boarding -- it says so here.
//
// Frame-count helpers are kept for the scenarios that really are about frames:
// a two-frame settle, a single dropped panel composite.
void panelFor(uint64_t ms) { const uint64_t t = g_fakeMs + ms;
                             while (g_fakeMs < t) panelFrame(); }
void starvedPanelFor(uint64_t ms) { const uint64_t t = g_fakeMs + ms;
                                    while (g_fakeMs < t) starvedPanelFrame(); }
void starvedSceneFor(uint64_t ms) { const uint64_t t = g_fakeMs + ms;
                                    while (g_fakeMs < t) starvedSceneFrame(); }
void sceneFor(uint64_t ms) { const uint64_t t = g_fakeMs + ms;
                             while (g_fakeMs < t) sceneFrame(); }
void idleFor(uint64_t ms)  { const uint64_t t = g_fakeMs + ms;
                             while (g_fakeMs < t) idleFrame(); }

bool offsetOn() { return externalCameraOnFoot(); }

// Counted here rather than written into the summary by hand. That number has
// been wrong twice already -- it is exactly the kind of thing that drifts
// silently, and a test that misreports how much it checked is halfway to a test
// that checks nothing.
int g_checks = 0;

void check(bool want, const char* what) {
    ++g_checks;
    if (offsetOn() == want) return;
    printf("  FAIL  %s -- offset is %s, expected %s\n", what,
           offsetOn() ? "ON" : "off", want ? "ON" : "off");
    ++g_bad;
}

// The counted view itself, for the cases where what matters is the number
// rather than whether the offset happens to be on at it.
void checkView(int want, const char* what) {
    ++g_checks;
    const int got = headOffsetGateCountedView();
    if (got == want) return;
    printf("  FAIL  %s -- counted view is %d, expected %d\n", what, got, want);
    ++g_bad;
}

// Every scenario starts from a clean gate with the shipped configuration --
// except that begin(false) scenarios are exercising the PARKED keyless path
// (experimental.keyless_camera, default off since the 2026-08-16 pivot to keyed
// entries), so they switch it on explicitly. The shipped default gets its
// own fixture below.
void begin(bool keyBound) {
    Config::get().set("experimental.keyless_camera", keyBound ? "0" : "1");
    headOffsetGateReset();
    headOffsetGateConfigure();
    headOffsetGateSetKeyBound(keyBound);
    headOffsetGateSetView(-1);   // nothing supplying an index unless said
    g_frame = 0;
}

// Enter the camera the way a set-up player does: on foot, press the key, the
// panel stops and the scene appears a couple of frames later.
//
// The scene run is 12 frames, not 4, and the difference is the test being
// realistic rather than the code being lenient. The gate now requires the panel
// to have been gone for several consecutive frames before arming -- one dropped
// frame used to be enough, which is a one-frame pose jump on any hitch. Nobody
// enters a camera for four frames; the mode change alone takes 25 to 86
// (6ac.6c), so a helper that fed four was encoding an entry that cannot happen.
void enterCamera() {
    panelFrame(200);
    headOffsetGateKeyPressed();
    panelFrame(2);          // the game takes a few frames to change mode
    sceneFrame(12);
}

// ------------------------------------------------------- grouping scan matches
//
// The other half of the feature, and the half that failed in the field first.
//
// The gate scenarios above all begin by being TOLD a view index. Supplying one
// means picking the camera settings array out of a heap full of objects of the
// same type, and the picking is pure address arithmetic -- so it is testable
// here, without a game, and it is where the first user-reported bug was.
//
// ------------------------------------------------- what counts as an eye
//
// The third thing the gate depends on and does not own: whether a draw is
// going into an eye texture at all. It cannot arm if the count it reads is
// starved, and on 2026-08-19 a rig starved it by rendering the world at a
// scale -- 1626x1774 into a 2112x2304 the headset was handed. eyeShapedAtScale
// is the rule that recognises that case, and it is a shape test with a
// tolerance, which is exactly the kind of thing that drifts.
//
// Every size below is from that session's own log, so this is the real list a
// real rig offered: one of them is the world and six are not.
int eyeShapeChecks() {
    struct Case { uint32_t w, h; bool want; const char* what; };
    const uint32_t eyeW = 2112, eyeH = 2304;
    const Case cases[] = {
        {1626, 1774, true,  "the world, rendered at 77% and scaled up (the field case)"},
        {2048, 2048, false, "a square atlas, 9% off the eye's shape"},
        {1024, 1024, false, "a smaller square, same 9%"},
        {1920, 1080, false, "16:9 -- the panel's stock size"},
        {1791, 1007, false, "16:9 again, at an odd size"},
        {1280,  768, false, "5:3"},
        {1024,  512, false, "2:1"},
        // The bounds themselves, which the list above does not reach.
        {2112, 2304, true,  "the published size, which the exact test takes first"},
        // The band's edges, named as edges. 40% and 250% are IN, because a
        // comment that says "40% to 250%" is read as inclusive by whoever
        // changes this next, and a boundary nobody asserts is a boundary that
        // moves.
        { 845,  922, true,  "the eye's shape at exactly 40% -- the bottom of the band"},
        {5280, 5760, true,  "the eye's shape at exactly 250% -- the top of it"},
        { 803,  875, false, "the eye's shape at 38% -- under the band"},
        {5491, 5990, false, "the eye's shape at 260% -- over it"},
        {   0,    0, false, "nothing"},
    };
    int bad = 0;
    for (const Case& c : cases) {
        const bool got = eyeShapedAtScale(c.w, c.h, eyeW, eyeH);
        ++g_checks;
        if (got == c.want) continue;
        printf("  FAIL  %ux%u %s -- eyeShapedAtScale said %s\n", c.w, c.h, c.what,
               got ? "yes" : "no");
        ++bad;
    }
    // A headset that has published nothing yet cannot answer this, and must
    // not answer it by dividing by zero.
    ++g_checks;
    if (eyeShapedAtScale(1626, 1774, 0, 0)) {
        printf("  FAIL  eyeShapedAtScale answered yes with no published eye size\n");
        ++bad;
    }
    if (!bad) printf("  ok    the world at a render scale is an eye; six other shapes are not\n");
    return bad;
}

// These are not invented shapes. The first is the exact layout from issue #2,
// which cost that player Explorer Cam for the last twenty-four minutes of a
// session; the rest are the neighbouring cases that a fix for it must not break.
// THE MID-REBUILD BACKOFF, as a table.
//
// Six lines of arithmetic that had three bugs across two review rounds, none of
// them visible by reading it. Each group below is one of them.
int rebuildBackoffChecks() {
    int bad = 0;
    auto want = [&](const char* what, CameraViewBackoff got, uint32_t runs,
                    uint64_t waitMs) {
        ++g_checks;
        if (got.runs == runs && got.waitMs == waitMs) return;
        printf("  FAIL  %s -- got runs=%u wait=%llums, expected runs=%u "
               "wait=%llums\n",
               what, got.runs, (unsigned long long)got.waitMs, runs,
               (unsigned long long)waitMs);
        ++bad;
    };

    // A SINGLE GENUINE MOVE IS UNTOUCHED. The first mid-rebuild answer of an
    // episode waits the flat cooldown, which is the case the rescan budget is
    // deliberately generous for. dueMs = 0 is "no episode yet".
    want("first answer waits the flat cooldown",
         cameraViewRebuildBackoff(0, 0, 100000), 1, 2700);

    // A RUN OF THEM DOUBLES, walked forward the way a station visit does, with
    // each answer's dueMs carried into the next.
    uint32_t runs = 0;
    uint64_t due = 0, now = 100000;
    {
        const uint64_t expect[] = {2700, 5400, 10800, 21600, 43200};
        for (int i = 0; i < 5; ++i) {
            const CameraViewBackoff bo = cameraViewRebuildBackoff(runs, due, now);
            char what[96];
            _snprintf_s(what, _TRUNCATE, "consecutive answer %d doubles the wait",
                        i + 1);
            want(what, bo, static_cast<uint32_t>(i + 1), expect[i]);
            runs = bo.runs;
            due = bo.dueMs;
            now = due + 1500;   // the wait, then a 1.5s heap walk
        }
    }

    // AND THEN STOPS DOUBLING. kRebuildBackoffMax caps the shift at 4, so 43.2s
    // is the ceiling however long the episode runs. The cap constant (45s) is a
    // guard against a future edit to the shift and must never be what binds --
    // if this ever reads 45000, the two constants have swapped roles.
    for (int i = 0; i < 3; ++i) {
        const CameraViewBackoff bo = cameraViewRebuildBackoff(runs, due, now);
        want("a long episode holds at the ceiling, never 86400", bo, runs + 1,
             43200);
        runs = bo.runs;
        due = bo.dueMs;
        now = due + 1500;
    }

    // THE RECOVERY, which is review finding #1. The run count was reset at two
    // of the seven paths that end an episode, so it stayed high, and the next
    // single ordinary array move waited 43 seconds -- the forty-second dead
    // window the rescan budget exists to prevent. A quiet gap must bring it
    // back to the flat cooldown whichever path ended the episode.
    want("a quiet gap ends the episode and restores the flat cooldown",
         cameraViewRebuildBackoff(5, 1000000, 1000000 + 60001), 1, 2700);

    // The boundary, pinned: expiry is strictly greater, so exactly the grace
    // period is still the same episode.
    want("exactly the grace period is still the same episode",
         cameraViewRebuildBackoff(4, 1000000, 1000000 + 60000), 5, 43200);

    // THE WALK MUST NOT COUNT, which is review finding V2. The gap between two
    // answers is the wait PLUS a heap walk, and the walk's length is a user
    // setting: at camera_index_mb_per_frame = 8 a 14 GB walk takes 20 seconds.
    // Measured from the ANSWER, 43.2s + 20s exceeded the 60s grace, so every
    // episode expired and the backoff silently switched itself off -- the exact
    // drain it exists to prevent, reintroduced by its own guard. Measured from
    // the DUE time, the walk is out of the comparison.
    want("a slow heap walk is not mistaken for a quiet gap",
         cameraViewRebuildBackoff(4, 1000000, 1000000 + 20000), 5, 43200);

    // dueMs is the caller's carry, and must be now + wait.
    {
        const CameraViewBackoff bo = cameraViewRebuildBackoff(0, 0, 500000);
        ++g_checks;
        if (bo.dueMs != 500000 + 2700) {
            printf("  FAIL  dueMs must be now + wait, got %llu\n",
                   (unsigned long long)bo.dueMs);
            ++bad;
        }
    }

    if (!bad) {
        printf("  ok    the mid-rebuild backoff doubles, caps, recovers after a "
               "quiet gap, and never counts the heap walk as quiet\n");
    }
    return bad;
}

// WHICH JOURNAL IS OURS, as a table.
//
// The decision that read a crashed session's journal as this session's and
// announced gameplay 0.1s into a process still sitting at the launcher
// (issue #19). Times are FILETIME units, stated relative to notBefore because
// that is what the comparison is against.
int journalPickChecks() {
    int bad = 0;
    constexpr uint64_t kSec = 10000000ull;
    constexpr uint64_t N = 1000000ull * kSec;   // notBefore: our start, less slack

    auto want = [&](const char* what, const uint64_t* cre, const uint64_t* wr,
                    size_t count, int index, bool ours) {
        ++g_checks;
        const JournalPick got = journalPickNewest(cre, wr, count, N);
        if (got.index == index && got.ours == ours) return;
        printf("  FAIL  %s -- got index=%d ours=%d, expected index=%d ours=%d\n",
               what, got.index, got.ours ? 1 : 0, index, ours ? 1 : 0);
        ++bad;
    };

    // Nothing at all, and nothing live: both resolve to "do not know" rather
    // than to a file.
    want("an empty folder picks nothing", nullptr, nullptr, 0, -1, false);
    {
        const uint64_t cre[] = {N - 3600 * kSec};
        const uint64_t wr[] = {N - 60 * kSec};
        want("a journal untouched since we started is not a candidate", cre, wr, 1,
             -1, false);
    }

    // THE ISSUE #19 CASE. [0] is the crashed session's journal: created 27
    // minutes ago, written 40 seconds after notBefore because the crash landed
    // inside the slack. [1] is ours: created moments ago and barely written, so
    // it LOSES on recency and must win anyway.
    {
        const uint64_t cre[] = {N - 1620 * kSec, N + 32 * kSec};
        const uint64_t wr[] = {N + 40 * kSec, N + 33 * kSec};
        want("our journal beats a crashed session's, despite the write times", cre,
             wr, 2, 1, true);

        // The same two in the order the walk might equally have found them.
        // Provenance must not depend on what FindFirstFile returns first.
        const uint64_t creR[] = {N + 32 * kSec, N - 1620 * kSec};
        const uint64_t wrR[] = {N + 33 * kSec, N + 40 * kSec};
        want("...and the same whichever order the walk found them in", creR, wrR, 2,
             0, true);
    }

    // BEFORE OURS EXISTS the foreign one is still adopted -- live events from it
    // are worth having -- but `ours` is false, which is what makes the caller
    // tail it from the end rather than replay its history.
    {
        const uint64_t cre[] = {N - 1620 * kSec};
        const uint64_t wr[] = {N + 40 * kSec};
        want("a foreign journal is adopted, but not as ours", cre, wr, 1, 0, false);
    }

    // THE RUNNING BEST WRITE TIME GOES BACKWARDS when a born file displaces a
    // non-born one, and this is what proves that is harmless: the non-born file
    // carries an enormous write time, and the two born files must still be
    // compared against each other rather than against it.
    {
        const uint64_t cre[] = {N - 1620 * kSec, N + 5 * kSec, N + 6 * kSec};
        const uint64_t wr[] = {N + 9000 * kSec, N + 10 * kSec, N + 20 * kSec};
        want("a stale write time from the other class is never consulted", cre, wr,
             3, 2, true);
    }

    // Equal provenance falls back to recency, and that comparison is strict, so
    // a tie keeps the first -- a stable answer rather than one that flips
    // between reglobs.
    {
        const uint64_t cre[] = {N + 5 * kSec, N + 6 * kSec};
        const uint64_t wr[] = {N + 20 * kSec, N + 20 * kSec};
        want("an exact tie on write time keeps the first, stably", cre, wr, 2, 0,
             true);
    }

    if (!bad) {
        printf("  ok    the journal pick prefers provenance over recency, in any "
               "order, and a crashed session's journal never wins\n");
    }
    return bad;
}

// PHASE-0 TIMING FOR PERIODIC WORK (src/common/periodic_work.h), as a table.
//
// It is tested here because this rig already links the biggest operation it
// times (journal_watch.cpp), the real log and the guard, and is where the
// cadence arithmetic (timing.h) is already asserted -- so no build step changes.
// What is under test is the window and threshold logic, which is pure: the
// clock, the local time of day and the log all arrive as arguments, so a fake
// clock walks whole 30 s windows in microseconds and the rig can count exactly
// how often the local clock was read.
int periodicWorkChecks() {
    int bad = 0;
    auto verify = [&](bool ok, const char* what) {
        ++g_checks;
        if (ok) return;
        printf("  FAIL  %s\n", what);
        ++bad;
    };
    auto verifyText = [&](const std::string& got, const char* want, const char* what) {
        ++g_checks;
        if (got == want) return;
        printf("  FAIL  %s\n          got:      %s\n          expected: %s\n", what,
               got.c_str(), want);
        ++bad;
    };

    constexpr int64_t kFreq = 10000000;   // 10 MHz, a usual QueryPerformanceFrequency
    std::vector<std::string> lines;       // everything the operation wrote
    int wallCalls = 0;                    // how often the local clock was read
    PeriodicWallClock wall;               // what the fake local clock answers
    auto setWall = [&](unsigned h, unsigned m, unsigned s, unsigned ms) {
        wall.hour = h;
        wall.minute = m;
        wall.second = s;
        wall.millis = ms;
    };
    auto reset = [&] {
        lines.clear();
        wallCalls = 0;
    };
    // One finished run of `ticks` (10,000 ticks = 1 ms here), completed at t.
    auto run = [&](PeriodicWork& w, int64_t ticks, double atSeconds, uint64_t context = 0) {
        w.record(ticks, static_cast<int64_t>(atSeconds * static_cast<double>(kFreq)), kFreq,
                 context, [&] { ++wallCalls; return wall; },
                 [&](const char* line) { lines.push_back(line); });
    };

    // A QUIET OPERATION writes one summary per 30 s window and nothing before it.
    {
        reset();
        PeriodicWork w("op_quiet");
        setWall(12, 34, 56, 789);
        for (int i = 0; i < 30; ++i) run(w, 5000, i);   // 0.5 ms once a second, t = 0..29 s
        verify(lines.empty(), "nothing is written while the window is still open");
        run(w, 5000, 30);   // the 31st run, exactly 30 s after the window's first
        verify(lines.size() == 1, "the first run at or past 30 s closes the window with one summary");
        if (lines.size() == 1) {
            verifyText(lines[0],
                       "periodic work: op_quiet n=31 total=15.500 max=0.500 at 12:34:56.789 "
                       "slow=0",
                       "the summary's exact form");
        }
        verify(wallCalls == 1,
               "31 unremarkable runs read the local clock once, for the window's first");
        verify(w.runs() == 0, "a closed window starts empty");
    }

    // THE LOCAL CLOCK IS READ ONLY FOR A NEW MAXIMUM OR A SLOW RUN, and once
    // when a run is both. The threshold is exact: 20,000 ticks is 2.0 ms.
    {
        reset();
        PeriodicWork w("op_wall");
        struct Step {
            int64_t ticks;
            int calls;   // total local-clock reads expected after this run
            const char* why;
        };
        const Step steps[] = {
            {10000, 1, "the first run of a window is its maximum, so it reads the clock"},
            {9000, 1, "a shorter run does not"},
            {8000, 1, "nor does another"},
            {15000, 2, "a new maximum does"},
            {15000, 2, "a tie is not a new maximum"},
            {19999, 3, "1.9999 ms is a new maximum, and is not slow"},
            {20000, 4, "2.0000 ms is slow AND a new maximum, and reads the clock once for both"},
            {20000, 5, "a slow run that is not a maximum still needs its time of day"},
        };
        double at = 1.0;
        for (const Step& step : steps) {
            run(w, step.ticks, at);
            at += 1.0;
            verify(wallCalls == step.calls, step.why);
        }
        verify(w.slowRuns() == 2, "2.0000 ms is slow and 1.9999 ms is not");
        verify(lines.size() == 1,
               "the second slow run, a second after the first, is counted but not written");
    }

    // SLOW RUNS: written at once, at most one line per operation per 10 s, each
    // carrying its own context; and the summary's time and context are the
    // SLOWEST run's, not the last one's.
    {
        reset();
        PeriodicWork w("journal_reglob", "files");
        setWall(10, 0, 0, 100);
        run(w, 34120, 100.0, 1893);   // 3.412 ms: the window's maximum
        setWall(10, 0, 5, 100);
        run(w, 25000, 105.0, 9);      // 2.5 ms, five seconds on: counted, not written
        setWall(10, 0, 9, 900);
        run(w, 25000, 109.9, 9);      // 9.9 s after the last WRITTEN line: still not
        setWall(10, 0, 10, 100);
        run(w, 25000, 110.0, 9);      // exactly 10 s after it: written
        verify(lines.size() == 2, "slow lines are limited to one per 10 s, and 10 s is enough");
        if (lines.size() == 2) {
            verifyText(lines[0], "periodic work: journal_reglob SLOW ms=3.412 at 10:00:00.100 files=1893",
                       "the first slow line, with its own context");
            verifyText(lines[1], "periodic work: journal_reglob SLOW ms=2.500 at 10:00:10.100 files=9",
                       "the next one, ten seconds later");
        }
        setWall(10, 0, 30, 0);
        run(w, 1000, 130.0, 9);   // 0.1 ms, thirty seconds after the window opened
        verify(lines.size() == 3, "the window closes on the first run 30 s after it opened");
        if (lines.size() == 3) {
            verifyText(lines[2],
                       "periodic work: journal_reglob n=5 total=11.012 max=3.412 at "
                       "10:00:00.100 slow=4 files=1893",
                       "the summary names the slowest run's time and context, and counts every "
                       "slow run including the withheld ones");
        }

        // ONLY IF IT RAN: the closed window is empty, and the next opens with the
        // next run, so the next summary is 30 s after THAT, not after this one.
        verify(w.runs() == 0, "the window is closed and empty after its summary");
        lines.clear();
        setWall(11, 11, 11, 111);
        run(w, 1000, 131.0, 1);
        run(w, 1000, 160.9, 1);
        verify(lines.empty(), "29.9 s into the next window: nothing");
        run(w, 1000, 161.0, 1);
        verify(lines.size() == 1, "30 s into it: the summary");
        if (lines.size() == 1) {
            verifyText(lines[0],
                       "periodic work: journal_reglob n=3 total=0.300 max=0.100 at "
                       "11:11:11.111 slow=0 files=1",
                       "the second window is its own, not a continuation");
        }
    }

    // STATE IS PER INSTANCE: two operations do not share a slow-line limit or a
    // window.
    {
        reset();
        PeriodicWork a("op_a"), b("op_b");
        setWall(9, 0, 0, 0);
        run(a, 30000, 0.0);   // 3 ms: slow, written
        run(b, 30000, 1.0);   // 3 ms on ANOTHER operation a second later: written too
        verify(lines.size() == 2, "each operation has its own slow-line limit");
        verify(a.runs() == 1 && b.runs() == 1 && a.slowRuns() == 1 && b.slowRuns() == 1,
               "and its own window");
    }

    // A clock that misbehaves must not crash it or manufacture a slow run.
    {
        reset();
        PeriodicWork w("op_odd");
        w.record(-5, 0, 0, 0, [&] { ++wallCalls; return wall; },
                 [&](const char* line) { lines.push_back(line); });
        verify(w.runs() == 1 && w.slowRuns() == 0 && lines.empty(),
               "a negative duration and a zero clock rate are clamped, not believed");
    }

    // THE PRODUCTION BINDING reads the real clock in real milliseconds. With no
    // log open the line it writes goes nowhere, which is fine here: what is
    // asserted is the unit, the mistake a fake clock cannot catch (ticks read
    // as milliseconds looks fine until a flight).
    {
        PeriodicWork real("gate_test_real");
        {
            PeriodicWorkScope scope(real);
            Sleep(20);
        }
        const double gotMs = static_cast<double>(real.maxTicks()) * 1000.0 /
                             static_cast<double>(qpcFrequency());
        verify(real.runs() == 1, "a scope records exactly one run");
        verify(gotMs >= 10.0 && gotMs < 5000.0,
               "a scope around a 20 ms sleep reads about 20, in milliseconds and not ticks");
    }

    if (!bad) {
        printf("  ok    periodic-work timing: a summary per 30 s window and only if it ran, "
               "slow runs written at once and limited to one per 10 s, the local clock read "
               "only for a new maximum or a slow run, and 2.0000 ms exactly slow\n");
    }
    return bad;
}

std::string readWholeFile(const std::wstring& path) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(f);
    return out;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        size_t stop = end;
        if (stop > start && text[stop - 1] == '\r') --stop;
        out.push_back(text.substr(start, stop - start));
        start = end + 1;
    }
    return out;
}

// This executable's own file name, which is the module a fault inside the rig's
// own code must be attributed to.
std::string ownModuleLeaf() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* leaf = slash ? slash + 1 : path;
    std::string out;
    for (; *leaf; ++leaf) out.push_back(*leaf < 0x80 ? static_cast<char>(*leaf) : '?');
    return out;
}

// " at HH:MM:SS.mmm" in a periodic-work line, zero-padded as the log's own
// prefix is, so the two can be read side by side.
bool hasTimeOfDay(const std::string& line) {
    const size_t at = line.find(" at ");
    if (at == std::string::npos) return false;
    const std::string t = line.substr(at + 4, 12);
    if (t.size() != 12) return false;
    for (size_t i = 0; i < t.size(); ++i) {
        if (i == 2 || i == 5) {
            if (t[i] != ':') return false;
        } else if (i == 8) {
            if (t[i] != '.') return false;
        } else if (t[i] < '0' || t[i] > '9') {
            return false;
        }
    }
    return true;
}

// THE NOTES AS THEY REACH A REAL LOG FILE: where a caught fault happened
// (src/common/guard.h), and the periodic-work lines written through the
// production binding (src/common/periodic_work.h).
//
// The fault half is the case R4 / A-9 asked for. A site is a budget's name, and
// deviceHook.frameBoundary covers the whole frame boundary, so a fault anywhere
// in it used to say only that name. The rig faults on purpose inside guarded()
// -- the real template, the real filter, the real Log -- closes the log and
// reads the file back, so what is asserted is the text a reporter would paste.
int loggedNotesChecks() {
    int bad = 0;
    auto verify = [&](bool ok, const char* what) {
        ++g_checks;
        if (ok) return;
        printf("  FAIL  %s\n", what);
        ++bad;
    };

    wchar_t tmp[MAX_PATH];
    const DWORD tn = GetTempPathW(MAX_PATH, tmp);
    if (tn == 0 || tn >= MAX_PATH) {
        printf("  FAIL  no temp folder to write the notes' log in\n");
        return 1;
    }
    const std::wstring dir =
        std::wstring(tmp) + L"edvr_gate_test_notes_" + std::to_wstring(GetCurrentProcessId());
    // A log.enabled = 0 in whatever ini this rig was pointed at would turn every
    // assertion below into "the file is empty", which reads as a failure of the
    // code and not of the setup.
    Config::get().set("log.enabled", "1");
    if (!Log::get().open(dir, L"gatenotes")) {
        printf("  FAIL  could not open a log in %ls to read the notes back\n", dir.c_str());
        return 1;
    }

    // One pointer per site: sites are compared by address, as they are in the
    // product, where each is a literal at one call.
    static const char kOk[] = "gate_test/no_fault";
    static const char kAv[] = "gate_test/fault_note";
    static const char kPrivate[] = "gate_test/fault_private";
    static const char kLegacy[] = "gate_test/legacy";

    const bool okRan = guarded(kOk, [] {});
    const uintptr_t lowAddress = 0x10;
    auto writeLow = [lowAddress] { *reinterpret_cast<volatile int*>(lowAddress) = 1; };
    const bool av1 = guarded(kAv, writeLow);
    const bool av2 = guarded(kAv, writeLow);
    const bool av3 = guarded(kAv, writeLow);

    // A fault in code that is in no module: a page EDVR-style code would sit in,
    // holding one illegal instruction. Written, then made executable, so this
    // does not ask for a page that is writable and executable at once.
    bool privateRan = true;
    if (void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
        const unsigned char ud2[] = {0x0F, 0x0B};
        memcpy(page, ud2, sizeof(ud2));
        DWORD previous = 0;
        VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &previous);
        privateRan = guarded(kPrivate, [page] { reinterpret_cast<void (*)()>(page)(); });
        VirtualFree(page, 0, MEM_RELEASE);
    }

    // A caller that reaches the filter with only a code, as the three rigs that
    // stub it do not but a future caller might.
    const int legacyVerdict = guardFilter(0xC0000005UL, kLegacy);

    // The periodic-work line through the production binding: a real clock, the
    // real local time of day, the real log. A 50 ms window instead of 30 s so
    // the summary can be reached without a half-minute sleep; the slow-run line
    // needs no such help.
    PeriodicWorkPolicy quick;
    quick.summaryEveryMs = 50;
    PeriodicWork realWork("gate_test_e2e", "ctx", quick);
    {
        PeriodicWorkScope scope(realWork, 5);
        Sleep(10);   // >= 2 ms: written the moment the scope closes
    }
    Sleep(60);
    {
        PeriodicWorkScope scope(realWork, 6);   // past the window: the summary
    }

    Log::get().close();

    std::string text;
    {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((dir + L"\\edvr_gatenotes_*.log").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring path = dir + L"\\" + fd.cFileName;
                text += readWholeFile(path);
                DeleteFileW(path.c_str());
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }
    RemoveDirectoryW(dir.c_str());
    const std::vector<std::string> log = splitLines(text);
    auto linesWith = [&](const char* a, const char* b) {
        std::vector<const std::string*> found;
        for (const std::string& line : log) {
            if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) {
                found.push_back(&line);
            }
        }
        return found;
    };

    verify(!log.empty(), "the log file was written and read back");
    verify(okRan, "a block that does not fault runs to the end and reports true");
    verify(!av1 && !av2 && !av3, "a faulting block is absorbed and reports false");
    verify(!privateRan, "so is a fault in code that is in no module");
    verify(legacyVerdict == EXCEPTION_EXECUTE_HANDLER, "the filter's verdict is unchanged");

    const std::string exe = ownModuleLeaf();
    verify(!exe.empty(), "this executable's own name could be read");

    // The access violation: three faults at one site.
    const auto absorbed = linesWith("FAULT ABSORBED", "site=gate_test/fault_note");
    verify(absorbed.size() == 1, "three faults at one site write one FAULT ABSORBED line, as ever");
    if (absorbed.size() == 1) {
        const std::string& n = *absorbed[0];
        verify(n.find("exception=0xC0000005 site=gate_test/fault_note at=0x") != std::string::npos,
               "the note gives the faulting address right after the site");
        verify(n.find(exe + "+0x") != std::string::npos,
               "...names the module the fault is in and an offset into it");
        verify(n.find("access=write data=0x0000000000000010 (no module: not mapped)") !=
                   std::string::npos,
               "...and, for an access violation, what was touched and how");
        verify(n.find("[truncated]") == std::string::npos,
               "...without the line being clipped");
        verify(n.find("THIS DID NOT CRASH THE GAME") != std::string::npos,
               "...and the verdict text is still there");
    }
    const auto totals = linesWith("FAULT TOTAL", "site=gate_test/fault_note");
    verify(totals.size() == 1, "the total is restated at 2 and not at 3");
    if (totals.size() == 1) {
        const std::string& n = *totals[0];
        verify(n.find(": 2 absorbed so far") != std::string::npos, "...and it is the count of 2");
        verify(n.find("Latest fault at=0x") != std::string::npos &&
                   n.find(exe + "+0x") != std::string::npos,
               "...carrying the latest fault's location, so a shared budget shows where it moved");
    }

    // The fault in no module.
    const auto priv = linesWith("FAULT ABSORBED", "site=gate_test/fault_private");
    verify(priv.size() == 1, "a fault in a private page is reported once");
    if (priv.size() == 1) {
        const std::string& n = *priv[0];
        verify(n.find("exception=0xC000001D") != std::string::npos, "...as an illegal instruction");
        verify(n.find("no module: private region 0x") != std::string::npos,
               "...saying there is no module and giving the region it is in");
        verify(n.find("access=") == std::string::npos, "...with no data address, it is not a memory fault");
    }

    // A code-only caller keeps the note it always had.
    const auto legacy = linesWith("FAULT ABSORBED", "site=gate_test/legacy");
    verify(legacy.size() == 1 && legacy[0]->find(" at=") == std::string::npos,
           "a caller with only a code gets the note it always did, without a location");

    // The periodic-work lines, through the real binding and the real log.
    const auto slowLine = linesWith("periodic work: gate_test_e2e SLOW ms=", " ctx=5");
    verify(slowLine.size() == 1, "a slow run through the production binding is written at once");
    if (slowLine.size() == 1) {
        verify(hasTimeOfDay(*slowLine[0]), "...with a zero-padded time of day");
    }
    // Only the shape here: which run is the slowest, and so whose context the
    // summary carries, is decided by real sleeps on a loaded machine. The fake
    // clock cases above pin that exactly.
    const auto summary = linesWith("periodic work: gate_test_e2e n=2 total=", " slow=");
    verify(summary.size() == 1, "the window's summary reaches the log through the real binding");
    if (summary.size() == 1) {
        verify(hasTimeOfDay(*summary[0]), "...with a zero-padded time of day");
        verify(summary[0]->find(" ctx=") != std::string::npos, "...and the operation's context");
    }

    if (!bad) {
        printf("  ok    a caught fault's note names its address, module and offset (and what "
               "an access violation touched), once per site and restated with the latest "
               "location; periodic-work lines reach the log through the real binding\n");
    }
    return bad;
}

int cameraRunChecks() {
    // Real storage, so the addresses are real addresses. The grouping never
    // dereferences them, but arithmetic on invented pointers is a bad habit to
    // teach a file that other people will copy from.
    static uint8_t arena[0x8000];
    constexpr size_t S = 0x18;      // record stride (6ad.7a)
    constexpr size_t kGap = 2;      // what camera_view.cpp passes
    constexpr size_t kOrd = 11;     // the ordinal (6ad.8b)
    int bad = 0;
    auto slot = [&](size_t i) -> const uint8_t* { return arena + 0x400 + i * S; };

    // THE FIELD CASE. Slots 0-9 and 11-12 hold records; slot 10 has stopped
    // carrying the type pointer while the game rebuilds it. The answer lives at
    // slot 11 and must still be found there.
    //
    // Before the fix this produced "10 record(s) -- too short for the ordinal"
    // and "2 record(s) -- too short for the ordinal", and the feature was over
    // for the session.
    {
        std::vector<const uint8_t*> recs;
        for (size_t i = 0; i < 10; ++i) recs.push_back(slot(i));
        recs.push_back(slot(11));
        recs.push_back(slot(12));
        const std::vector<CameraViewRun> runs = cameraViewGroupRuns(recs, S, kGap);
        ++g_checks;
        if (runs.size() != 1 || runs[0].slots <= kOrd ||
            runs[0].base + kOrd * S != slot(kOrd)) {
            printf("  FAIL  one empty slot split the array and lost the ordinal: "
                   "%zu run(s)", runs.size());
            for (size_t i = 0; i < runs.size(); ++i) {
                printf("%s %zu slot(s)/%zu filled", i ? "," : "",
                       runs[i].slots, runs[i].present);
            }
            printf("\n");
            ++bad;
        } else {
            ++g_checks;
            if (runs[0].present != 12) {
                printf("  FAIL  the bridged slot was counted as a record: %zu "
                       "filled, expected 12\n", runs[0].present);
                ++bad;
            }
        }

        // AND THE SAME LAYOUT MUST STILL SPLIT WITH NO TOLERANCE.
        //
        // Without this the case above passes whether or not the tolerance does
        // any work -- which is exactly how the first version of this file came
        // to pass with the arming rule reverted. This pins the failure to the
        // thing that was changed: gap 0 is the old code, and the old code has
        // to be seen losing the ordinal.
        const std::vector<CameraViewRun> old = cameraViewGroupRuns(recs, S, 0);
        ++g_checks;
        if (old.size() != 2 || old[0].slots != 10 || old[1].slots != 2) {
            printf("  FAIL  this layout does not reproduce the reported split, "
                   "so the case above proves nothing: %zu run(s)\n", old.size());
            ++bad;
        }
    }

    // A WHOLE ARRAY still groups as one run, and bridging must not inflate it.
    {
        std::vector<const uint8_t*> recs;
        for (size_t i = 0; i < 13; ++i) recs.push_back(slot(i));
        const std::vector<CameraViewRun> runs = cameraViewGroupRuns(recs, S, kGap);
        ++g_checks;
        if (runs.size() != 1 || runs[0].slots != 13 || runs[0].present != 13) {
            printf("  FAIL  an intact array of 13 grouped as %zu run(s)\n",
                   runs.size());
            ++bad;
        }
    }

    // A GAP TOO WIDE IS STILL A BOUNDARY. This is what stops the tolerance
    // becoming "sweep up every object of this type in address order", which is
    // the bug the run grouping was introduced to fix in the first place (EDVR-118,
    // where global index 11 read 2210427397).
    {
        std::vector<const uint8_t*> recs;
        for (size_t i = 0; i < 6; ++i) recs.push_back(slot(i));
        recs.push_back(slot(6 + kGap + 1));      // one slot beyond the tolerance
        const std::vector<CameraViewRun> runs = cameraViewGroupRuns(recs, S, kGap);
        ++g_checks;
        if (runs.size() != 2) {
            printf("  FAIL  a gap of %zu slots was bridged; %zu run(s), expected 2\n",
                   kGap + 1, runs.size());
            ++bad;
        }
    }

    // AN UNRELATED OBJECT far away stays its own run, and one that is close but
    // not on the stride is not a member either -- a heap neighbour at some
    // arbitrary offset is not the thirteenth camera preset.
    {
        std::vector<const uint8_t*> recs;
        for (size_t i = 0; i < 13; ++i) recs.push_back(slot(i));
        recs.push_back(slot(13) + 4);            // on no stride boundary
        recs.push_back(slot(200));               // elsewhere entirely
        const std::vector<CameraViewRun> runs = cameraViewGroupRuns(recs, S, kGap);
        ++g_checks;
        if (runs.size() != 3 || runs[0].slots != 13) {
            printf("  FAIL  neighbours were absorbed into the array: %zu run(s), "
                   "first spans %zu slot(s)\n", runs.size(),
                   runs.empty() ? 0u : runs[0].slots);
            ++bad;
        }
    }

    // A GENUINELY SHORT RUN STAYS SHORT. Bridging must not manufacture the
    // length that qualifies a run to answer -- "too short for the ordinal" is a
    // correct refusal and has to survive.
    {
        std::vector<const uint8_t*> recs;
        for (size_t i = 0; i < 4; ++i) recs.push_back(slot(i));
        const std::vector<CameraViewRun> runs = cameraViewGroupRuns(recs, S, kGap);
        ++g_checks;
        if (runs.size() != 1 || runs[0].slots > kOrd) {
            printf("  FAIL  four records qualified to answer for ordinal %zu\n", kOrd);
            ++bad;
        }
    }

    // NO MATCHES AT ALL is not a crash.
    {
        const std::vector<const uint8_t*> none;
        const std::vector<CameraViewRun> runs = cameraViewGroupRuns(none, S, kGap);
        ++g_checks;
        if (!runs.empty()) {
            printf("  FAIL  an empty match list produced %zu run(s)\n", runs.size());
            ++bad;
        }
    }

    if (bad == 0)
        printf("  ok    a rebuilt slot does not cost the array its ordinal\n");
    return bad;
}

// THE SENTINEL, PINNED.
//
// elapsedMs and dueMs differ only in what they answer for a stamp of 0, and
// that difference silently disabled four subsystems in one review: the
// journal watcher, config reloading in both DLLs, and the camera-view
// candidate poll. Each had been a countdown initialised to 0 meaning "due
// now"; each became elapsedMs, which answers false for 0; and because the
// only write to each stamp was inside the branch it gated, none of them ever
// ran again. Nothing crashed and no log line changed -- the features just
// were not there.
//
// So the two are asserted apart here. Anyone who "simplifies" one into the
// other, or makes elapsedMs treat 0 as the epoch, fails this rather than
// shipping four dead subsystems.
int timingChecks() {
    int bad = 0;
    const uint64_t saveMs = g_fakeMs;
    edvr::g_clockForTest = &fakeClock;
    g_fakeMs = 5000;

    if (edvr::elapsedMs(0, 100)) {
        printf("  FAIL  elapsedMs(0, ...) must be false\n");
        ++bad;
    }
    if (!edvr::dueMs(0, 100)) {
        printf("  FAIL  dueMs(0, ...) must be true\n");
        ++bad;
    }
    if (edvr::elapsedMs(4950, 100) || edvr::dueMs(4950, 100)) {
        printf("  FAIL  50 ms into a 100 ms window, neither should fire\n");
        ++bad;
    }
    if (!edvr::elapsedMs(4900, 100) || !edvr::dueMs(4900, 100)) {
        printf("  FAIL  exactly at the window, both should fire\n");
        ++bad;
    }
    // stampMs never hands back the sentinel, or the run it marks would read
    // as one that never started.
    g_fakeMs = 0;
    if (edvr::stampMs() == 0) {
        printf("  FAIL  stampMs() returned 0, which means never started\n");
        ++bad;
    }

    g_fakeMs = saveMs;
    if (bad == 0)
        printf("  ok    elapsedMs and dueMs disagree about 0, as they must\n");
    return bad;
}

// Every scenario, run at one refresh rate. See g_rateHz.
void runScenarios() {

    // ---------------------------------------------------------------- arming
    //
    // BOARDING A SHIP MUST NOT ARM IT. This is the one that matters most. The
    // panel stops and a full scene is drawn -- which is exactly what entering
    // the external camera looks like, and exactly what walking into your own
    // ship looks like (EVIDENCE 6ac.6b). With no camera key bound the gate has
    // no way to tell them apart, so it must do nothing.
    begin(/*keyBound=*/false);
    panelFrame(200);
    sceneFrame(600);
    check(false, "boarding a ship with no key bound");

    // ...and it must still be off much later, because a ship's cockpit draws a
    // full scene forever and the panel never comes back.
    sceneFrame(5000);
    check(false, "still in the ship 5600 frames later");

    // THE SAME, WITH THE VIEW ALREADY ON THE WANTED ONE.
    //
    // This scenario is why the two above are not enough, and writing it is what
    // showed the first version of this test did not discriminate: with the view
    // index at its default 0, the VIEW gate rejected the boarding case and the
    // arming rule was never consulted. Reverting the arming rule to its old
    // shape still passed.
    //
    // The game REMEMBERS the camera view across uses, so a player who cycled to
    // view 1 earlier has the live index reading 1 while they walk around. The
    // view gate then passes, and the arming rule is the ONLY thing left between
    // walking into your own ship and the offset applying in your cockpit.
    begin(/*keyBound=*/false);
    headOffsetGateSetView(g_wantView);
    panelFrame(200);
    sceneFrame(600);
    check(false, "boarding a ship, view already the wanted one, no key bound");
    sceneFrame(3000);
    check(false, "...and still off deep into the flight");

    // A KEY BOUND BUT NEVER PRESSED is the same situation. gateHaveKey used to
    // be set by the first press, so a correctly configured player ran the weak
    // path until they happened to press it -- which is when they needed it.
    begin(/*keyBound=*/true);
    headOffsetGateSetView(g_wantView);
    panelFrame(200);
    sceneFrame(600);
    check(false, "key bound but never pressed, wanted view, boarding a ship");

    // THE HAPPY PATH. Key bound, key pressed, panel stops, scene appears.
    begin(true);
    headOffsetGateSetView(g_wantView);        // the game says view 1
    enterCamera();
    check(true, "entering the camera on the wanted view");

    // A STARVED EYE-DRAW COUNT, WHICH IS A REAL ENTRY THE GATE CANNOT SEE.
    //
    // The field case of 2026-08-19: the player pressed their bound key on the
    // wanted view, the flat panel genuinely stopped for twenty-six seconds
    // while they sat in the camera, and the panel came back when they pressed
    // again -- all of it in the log. The gate did not arm, because sceneNow
    // wants more than 50 draws into an eye texture in one frame and that rig
    // never produced 20 in a whole session.
    //
    // The assertion records TODAY's behaviour, not a desired one. Arming here
    // would mean arming on evidence the gate has no way to tell from boarding
    // a ship, which is the failure this whole module exists to prevent -- so
    // the count is not something to loosen. What the session cost instead was
    // the DIAGNOSIS: the line that names the four numbers used to sit behind
    // sceneNow, so the one failure it was written for was the one it could not
    // report. That line now fires from the panel-run expiry, which this
    // sequence reaches, and where a fix for the starvation itself belongs is
    // in the recogniser (vscreen.cpp), not here.
    begin(true);
    headOffsetGateSetView(g_wantView);
    starvedPanelFor(3000);
    headOffsetGateKeyPressed();
    starvedPanelFrame(2);            // the game takes a few frames to change mode
    starvedSceneFor(4000);
    check(false, "a real entry whose eye-draw count never reaches the gate's floor");

    // ...and the entry that follows a NORMAL count still works, on the same
    // gate, so nothing above has been made permanently suspicious.
    panelFor(1000);
    headOffsetGateKeyPressed();
    panelFrame(2);
    sceneFrame(12);
    check(true, "a countable entry right after a starved one");

    // ------------------------------------------------------------------ views
    //
    // The wrong view must not arm, even with everything else right. The default
    // camera view faces back at the commander, and placing the viewpoint at
    // their head there means facing the wrong way.
    begin(true);
    headOffsetGateSetView(g_otherView);
    enterCamera();
    check(false, "in the camera on view 0 when the offset is for view 1");

    // ...and it engages the moment the view becomes the wanted one, with no
    // further keypress.
    headOffsetGateSetView(g_wantView);
    sceneFrame(2);
    check(true, "the view changed to the wanted one while in the camera");

    // ...and disengages again on the way past.
    headOffsetGateSetView(g_otherView);
    sceneFrame(2);
    check(false, "the view changed away again while in the camera");

    // ----------------------------------------------------------- view bridge
    //
    // The read DYING mid-camera is routine near a planet: the game rebuilds
    // its camera records every ten to thirty seconds (6ar-6at), and the strict
    // drop landed exactly when the player was sitting still in the wanted
    // view, supplying none of the presses re-certification needs. The bridge
    // holds the last confirmed view for a bounded window; the first re-read
    // corrects it that frame; expiry restores the strict drop; and 0 in the
    // config restores the old rule entirely. Each property pinned. The hold is
    // the half that fails against the pre-bridge build.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "in the camera on the wanted view, read alive");
    headOffsetGateSetView(-1);            // the rebuild takes the read away
    sceneFrame(200);                      // well inside the bridge window
    check(true, "the read died mid-use and the offset held: the bridge");
    headOffsetGateSetView(g_otherView);   // it returns saying the player moved
    sceneFrame(2);
    check(false, "the returning read named a different view and won at once");
    headOffsetGateSetView(g_wantView);
    sceneFrame(2);
    check(true, "back on the wanted view");
    headOffsetGateSetView(-1);
    sceneFrame(2800);                     // the old TTL would have expired here
    check(true, "the hold has no clock: a long dead-read stretch stays on");

    Config::get().set("fix.head_offset_view_bridge", "0");
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "in the camera, bridge configured off");
    headOffsetGateSetView(-1);
    sceneFrame(2);
    check(false, "with the bridge off, losing the read drops the offset at once");
    Config::get().set("fix.head_offset_view_bridge", "1");

    // THE RELANDING CASE, corrected twice by the field (sixth and ninth
    // flights of 2026-08-15). The first version held the old view across the
    // whole absence, on "the game freezes the view while the camera is
    // closed" -- and the ninth flight showed the offset applied on preset 0
    // at re-entry, which was read as the game RESETTING its view across a
    // vehicle leg.
    //
    // THAT READING WAS WRONG, and the field said so on 2026-09-02:
    // disembarking and re-embarking leave the on-foot preset exactly where
    // it was. What the ninth flight almost certainly saw was a BACKWARD
    // cycle press, which nothing counted until VanityCameraScrollLeft was
    // wired up -- one uncounted press looks identical to a reset if you only
    // ever check where you ended up.
    //
    // So a landing holds the view, and a LOW OR HIGH WAKE is the boundary
    // that really zeroes it: entering supercruise or jumping rebuilds the
    // camera and the preset goes with it. Both halves are asserted below.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "in the camera on the wanted view");
    headOffsetGateSetView(-1);            // the rebuild takes the read away
    sceneFrame(20);
    check(true, "holding on the bridge");
    headOffsetGateKeyPressed();           // leave the camera for the ship
    sceneFrame(1);
    check(false, "left the camera");
    sceneFrame(6000);                     // the ship leg: vehicle scene
    panelFrame(200);                      // relanded, on foot: NEW session
    headOffsetGateKeyPressed();           // re-enter the camera
    panelFrame(2);
    sceneFrame(12);
    check(true, "a landing keeps the camera preset, so the held view arms "
                "again at re-entry");

    // AND THE HOLD HAS NO CLOCK AT ALL: staying in the camera on the held
    // view for as long as the player wishes is the product requirement
    // (2026-08-15) -- the wall-clock TTL greeted every relanding with a dead
    // bridge, and the in-camera budget contradicted indefinite stays. An
    // hour of frames on the hold stays on.
    sceneFrame(324000);
    check(true, "an hour in the camera on the held view is still on");

    // NOW THE BOUNDARY THAT DOES RESET IT. A first sample is not an edge --
    // arriving already in supercruise tells you nothing about a transition --
    // so establish "not in a wake" before raising it.
    headOffsetGateSetWakeLive(true, false, false);
    sceneFrame(1);
    check(true, "a first wake sample on its own is not an edge and changes "
                "nothing");
    headOffsetGateSetWakeLive(true, true, false);   // low wake: supercruise
    sceneFrame(2);
    check(false, "a low wake puts the counted view back to 0, and the offset "
                 "comes off with it");
    headOffsetGateViewBumped();           // cycle: 0 -> 1
    sceneFrame(2);
    check(false, "view 1 is not the wanted one either");
    headOffsetGateViewBumped();           // cycle: 1 -> 2
    sceneFrame(2);
    check(true, "two presses reach the wanted view and the offset arms, "
                "read or no read");

    // LEAVING SUPERCRUISE IS A BOUNDARY TOO (field, 2026-09-02). Dropping out
    // rebuilds the scene as surely as entering does, so the edge counts in
    // both directions and this is where rising-edge-only would have been
    // silently wrong -- it would have carried the old count through every
    // arrival.
    // The count is already on the wanted view from the two presses above,
    // and we are still inside supercruise.
    check(true, "still on the wanted view inside supercruise");
    headOffsetGateSetWakeLive(true, false, false);   // drop out
    sceneFrame(2);
    check(false, "dropping OUT of supercruise resets it as well");

    // A HIGH WAKE IS THE SAME BOUNDARY by a different signal: the jump
    // tunnel, which the witchspace fix already watches.
    headOffsetGateViewBumped();
    headOffsetGateViewBumped();
    sceneFrame(2);
    check(true, "back on the wanted view in normal space");
    headOffsetGateSetWakeLive(true, false, true);   // high wake: the tunnel
    sceneFrame(2);
    check(false, "a high wake resets it too");

    // A CAMERA TOGGLE WITHIN a session keeps the count: leaving the camera
    // to on-foot and coming straight back is the case the game genuinely
    // remembers across, and no vehicle scene intervenes.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    headOffsetGateSetView(-1);
    sceneFrame(20);
    check(true, "in the camera, read dead, holding");
    headOffsetGateKeyPressed();           // out to on-foot
    panelFrame(60);                       // walking about: panel, no vehicle
    headOffsetGateKeyPressed();           // straight back in
    panelFrame(2);
    sceneFrame(12);
    check(true, "a same-session toggle keeps the held view and re-arms");

    // THE ON-FOOT RING IS 0..5 AND ROLLS OVER AT BOTH ENDS. The gate cares
    // about no other context: an SRV's ring is 8 and a ship's up to 11, and a
    // count that walked off either end of this one would name a preset the
    // player cannot reach on foot.
    begin(true);
    enterCamera();
    checkView(0, "the ring starts at 0");
    for (int i = 0; i < 5; ++i) headOffsetGateViewBumped();
    checkView(5, "five forward presses reach the end of the ring");
    headOffsetGateViewBumped();
    checkView(0, "and the sixth rolls over to the start");
    headOffsetGateViewUnbumped();
    checkView(5, "backward from the start rolls over to the end");
    for (int i = 0; i < 5; ++i) headOffsetGateViewUnbumped();
    checkView(0, "and five more backward presses come back to 0");

    // STEPPING OUT OF A VEHICLE: PAST THE END BECOMES 0, INSIDE IT IS KEPT.
    // The game's rule, and it is a clamp rather than a fold (field,
    // 2026-09-02): 8 in a ship becomes 0 on foot, not 8 mod 6.
    begin(true);
    headOffsetGateSetOnFootLive(true, false, 1);   // in a vehicle
    for (int i = 0; i < 8; ++i) headOffsetGateViewBumped();
    checkView(8, "in a vehicle the count runs on past the on-foot cycle");
    headOffsetGateSetOnFootLive(true, true, 2);    // step out
    checkView(0, "and stepping out on 8 puts it back to 0");

    // The other half, which a plain reset-to-0 would get wrong: a vehicle
    // preset the on-foot cycle also has is KEPT.
    begin(true);
    headOffsetGateSetOnFootLive(true, false, 1);
    for (int i = 0; i < 3; ++i) headOffsetGateViewBumped();
    checkView(3, "three presses in a vehicle");
    headOffsetGateSetOnFootLive(true, true, 2);
    checkView(3, "and stepping out on 3 keeps it -- the on-foot cycle has a 3");

    // On foot the ring still wraps, which is what makes 6 unreachable there
    // and the clamp above a vehicle-only event.
    begin(true);
    headOffsetGateSetOnFootLive(true, true, 1);
    for (int i = 0; i < 8; ++i) headOffsetGateViewBumped();
    checkView(2, "on foot, eight presses wrap twice and land on 2");

    // A READ FROM A LONGER RING IS FOLDED, NOT TAKEN RAW. 8 is an SRV index;
    // on foot it can only mean 2. Clamping would have said 5, which is a real
    // preset and the wrong one -- the failure mode worth a test of its own.
    begin(true);
    enterCamera();
    headOffsetGateSetView(8);
    sceneFrame(2);
    checkView(2, "a read of 8 folds into the on-foot ring as 2");
    headOffsetGateSetView(11);
    sceneFrame(2);
    checkView(5, "and 11 folds to 5");

    // THE JOURNAL'S BOUNDARY AND THE HEURISTIC'S ARE ONE EVENT. Disembark
    // (wired from device_hook) and the panel-return heuristic mark the same
    // landing seconds apart, and the dedupe means it is announced once rather
    // than twice. Neither touches the counted view any more -- what they still
    // do is retire the bridge, which is about the READ dying across a landing
    // and was never about the count.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    headOffsetGateSetView(-1);
    sceneFrame(20);
    check(true, "in the camera, read dead, holding");
    headOffsetGateKeyPressed();               // out to the ship
    sceneFrame(2000);                         // the leg
    headOffsetGateNewFootSession("test: journal Disembark");
    panelFrame(200);                          // panel returns: the heuristic
    headOffsetGateKeyPressed();               // would fire here -- deduped
    panelFrame(2);
    sceneFrame(12);
    check(true, "the journal's landing keeps the view, so the offset arms "
                "again");
    headOffsetGateViewBumped();
    sceneFrame(2);
    check(false, "one press off the wanted view drops it");
    headOffsetGateViewUnbumped();
    sceneFrame(2);
    check(true, "and cycling BACK onto it re-arms -- the backward key counts");

    // AN IDLE STRETCH (map, menu) is not a vehicle leg and must not reset:
    // neither panel nor scene accrues toward the session boundary.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    headOffsetGateSetView(-1);
    sceneFrame(20);
    headOffsetGateKeyPressed();           // out to on-foot
    panelFrame(30);
    idleFrame(2000);                      // a long map session
    panelFrame(60);                       // back on foot
    headOffsetGateKeyPressed();           // into the camera again
    panelFrame(2);
    sceneFrame(12);
    check(true, "a long menu stretch does not start a new session, and the "
                "held view still arms");

    // --------------------------------------------------------- keyless mode
    //
    // No camera key bound, and the game's own status standing in for it
    // (6bb: the OnFoot flags HOLD through the whole camera window and drop
    // on boarding). The sample counter models Status.json's ~1 Hz cadence:
    // keyless arming requires an on-foot sample taken AFTER the panel
    // stopped, so boarding's stale second of "on foot" cannot arm the
    // offset into the boarding animation.
    begin(false);
    headOffsetGateSetView(g_wantView);
    headOffsetGateSetOnFootLive(true, true, 1);
    panelFrame(200);
    sceneFrame(6);                            // panel stops: entering the camera
    headOffsetGateSetOnFootLive(true, true, 2);   // a fresh on-foot sample
    sceneFrame(12);
    check(true, "keyless: on foot per the game, panel gone, scene up -- the "
                "external camera, no key needed");

    // Boarding from the camera: the flag drops, the offset must beat the
    // cockpit.
    headOffsetGateSetOnFootLive(true, false, 3);
    sceneFrame(2);
    check(false, "keyless: the game says not on foot, so the camera is over");

    // THE CONFIRMATION ARRIVES ON THE FILE'S SCHEDULE, NOT THE WINDOW'S.
    // Measured 2026-08-16 (11:27:10): the fresh sample landed ~90 frames
    // after the panel stopped -- Status.json polls plus the game's ~1 Hz
    // write cadence -- and the 60-frame entry window had already closed, so
    // a certified entry with the read alive and view 2 on screen never
    // latched. The keyless window must outlast the cadence it waits on.
    begin(false);
    headOffsetGateSetView(g_wantView);
    headOffsetGateSetOnFootLive(true, true, 1);
    panelFrame(200);
    sceneFor(1000);      // in the camera, sample pending: the measured +90
                         // frames at 90Hz, said as the 1000 ms it actually was
    headOffsetGateSetOnFootLive(true, true, 2);   // the poll finally lands
    sceneFrame(12);
    check(true, "keyless: a fresh sample on the file's own schedule still "
                "latches the entry");

    // Boarding INSTEAD of the camera: the panel stops, the scene appears,
    // and the only on-foot samples are from BEFORE the panel stopped --
    // stale. No fresh sample, no arming, however on-foot the old one says.
    begin(false);
    headOffsetGateSetView(g_wantView);
    headOffsetGateSetOnFootLive(true, true, 1);
    panelFrame(200);
    sceneFrame(30);                           // boarding: no fresh sample yet
    check(false, "keyless: a stale on-foot sample does not arm into a "
                 "boarding animation");

    // No live context at all (no Status.json, watcher off): keyless stays
    // the dead configuration it always was.
    begin(false);
    headOffsetGateSetView(g_wantView);
    panelFrame(200);
    sceneFrame(30);
    check(false, "keyless with no live context: nothing can arm, as before");

    // THE SHIPPED DEFAULT: keyless parked (experimental.keyless_camera=0). No
    // key bound means nothing arms, however alive the on-foot status is --
    // the 6bf copy circus showed the view cannot be supplied without
    // presses, so an entry with no view source is a latch with no payoff.
    Config::get().set("experimental.keyless_camera", "0");
    headOffsetGateReset();
    headOffsetGateConfigure();
    headOffsetGateSetKeyBound(false);
    headOffsetGateSetView(g_wantView);
    g_frame = 0;
    headOffsetGateSetOnFootLive(true, true, 1);
    panelFrame(200);
    sceneFrame(30);
    headOffsetGateSetOnFootLive(true, true, 2);
    sceneFrame(12);
    check(false, "shipped default: keyless is parked, so no key means no "
                 "arming even with the status alive");

    // --------------------------------- the disembark's stale-status window
    //
    // Measured 2026-08-16, both field sessions: after the journal's
    // Disembark, Status.json keeps answering "not on foot" for ~6 seconds
    // while the airlock animation runs. A player entering the camera inside
    // that window is on foot by the game's own declaration -- and the
    // boarding-exit firing on the stale flag killed the latch six frames
    // running (10:57:12). Until the flag has been seen TRUE this foot
    // session, false describes the PREVIOUS leg, not a boarding.
    begin(true);
    headOffsetGateSetView(g_wantView);
    headOffsetGateSetOnFootLive(true, false, 1);   // in the ship
    sceneFrame(2000);                              // the leg
    headOffsetGateNewFootSession("test: journal Disembark");
    panelFrame(60);                                // standing, panel up
    headOffsetGateKeyPressed();                    // straight into the camera
    panelFrame(2);
    headOffsetGateSetOnFootLive(true, false, 2);   // stale: still "in ship"
    sceneFrame(12);
    headOffsetGateViewBumped();
    headOffsetGateViewBumped();
    sceneFrame(2);
    check(true, "a stale not-on-foot sample straight after disembarking does "
                "not kill the camera the player is standing in");
    headOffsetGateSetOnFootLive(true, true, 3);    // the flag catches up
    sceneFrame(30);
    check(true, "the flag catching up changes nothing");
    headOffsetGateSetOnFootLive(true, false, 4);   // NOW false means boarded
    sceneFrame(2);
    check(false, "false after true is a boarding and exits");

    // The other side of the same window: KEYLESS arming during it. The
    // journal has declared the foot session; demanding a fresh Status
    // sample agree forfeits every fast entry (11:02:28 armed only because
    // the player took 6.7 s to reach the camera).
    begin(false);
    headOffsetGateSetView(g_wantView);
    headOffsetGateSetOnFootLive(true, false, 1);   // in the ship
    sceneFrame(2000);                              // the leg
    headOffsetGateNewFootSession("test: journal Disembark", true);
    headOffsetGateSetOnFootLive(true, false, 2);   // stale through the airlock
    panelFrame(60);                                // standing, panel up
    sceneFrame(12);                                // straight into the camera
    check(true, "keyless: the journal's disembark stands in while the status "
                "file catches up, so a fast entry is not forfeit");

    // The grace is a window, not a licence: expired with the status never
    // confirming, entries revert to needing the fresh sample.
    begin(false);
    headOffsetGateSetView(g_wantView);
    headOffsetGateSetOnFootLive(true, false, 1);
    sceneFrame(2000);
    headOffsetGateNewFootSession("test: journal Disembark", true);
    headOffsetGateSetOnFootLive(true, false, 2);
    sceneFor(11000);                               // grace expires unconfirmed
    panelFrame(60);
    sceneFrame(12);
    check(false, "keyless: the disembark grace expires and the old rule "
                 "stands");

    // Embark cancels the grace: boarding again is not a camera entry.
    begin(false);
    headOffsetGateSetView(g_wantView);
    headOffsetGateSetOnFootLive(true, false, 1);
    sceneFrame(2000);
    headOffsetGateNewFootSession("test: journal Disembark", true);
    headOffsetGateNoteEmbark();
    headOffsetGateSetOnFootLive(true, false, 2);
    panelFrame(60);
    sceneFrame(12);
    check(false, "keyless: an embark cancels the grace");

    // The grace opens even when the reset itself dedupes: the panel
    // heuristic spoke first, the journal's echo is a duplicate reset but
    // not duplicate news about the status file lagging.
    begin(false);
    headOffsetGateSetView(g_wantView);
    headOffsetGateSetOnFootLive(true, false, 1);
    sceneFrame(2000);                              // the leg
    panelFrame(30);                                // heuristic fires the reset
    headOffsetGateNewFootSession("test: journal Disembark", true);  // deduped
    headOffsetGateSetOnFootLive(true, false, 2);   // still stale
    panelFrame(30);
    sceneFrame(12);
    check(true, "keyless: a deduped journal echo still opens the grace");

    // ------------------------------------------------------- certification
    //
    // The pure judgement both flight-caught certification bugs lived in,
    // replayed on a desk instead of a planet: 6au (rebuild noise counted as
    // behaviour) and 6aw (a counter steps like a player). The scan machinery
    // is not needed to prove the rules, which is the whole point of the seam.
    {
        int cbad = 0;

        // Legacy bar (no next-view key): three sequential in-camera steps.
        CameraViewVote legacy{};
        cameraViewCertStep(&legacy, 0, true, false, false);   // primes
        bool early = cameraViewCertStep(&legacy, 1, true, false, false);
        early = early || cameraViewCertStep(&legacy, 2, true, false, false);
        const bool third = cameraViewCertStep(&legacy, 3, true, false, false);
        if (early || !third) {
            printf("  FAIL  the legacy bar did not certify on exactly three "
                   "sequential in-camera steps\n");
            ++cbad;
        }

        // 6au's shape: arbitrary rebuild writes reset and never accumulate.
        CameraViewVote noise{};
        cameraViewCertStep(&noise, 6, true, false, false);
        cameraViewCertStep(&noise, 0, true, false, false);    // 6->0 resets
        cameraViewCertStep(&noise, 3, true, false, false);    // 0->3 resets
        if (cameraViewCertStep(&noise, 4, true, false, false)) {
            printf("  FAIL  rebuild noise accumulated toward certification\n");
            ++cbad;
        }

        // 6aw's shape, out of camera: a counter climbing 0,1,2,3,4 with the
        // player elsewhere certifies nothing, however sequential it is.
        CameraViewVote counter{};
        cameraViewCertStep(&counter, 0, false, false, false);
        bool out = false;
        for (uint32_t v = 1; v <= 4; ++v) {
            out = out || cameraViewCertStep(&counter, v, false, false, false);
        }
        if (out) {
            printf("  FAIL  a counter certified while the player was out of "
                   "the camera\n");
            ++cbad;
        }

        // Witnessed bar: two steps landing beside real presses certify...
        CameraViewVote witnessed{};
        cameraViewCertStep(&witnessed, 0, true, false, true);
        const bool w1 = cameraViewCertStep(&witnessed, 1, true, true, true);
        const bool w2 = cameraViewCertStep(&witnessed, 2, true, true, true);
        if (w1 || !w2) {
            printf("  FAIL  the witnessed bar did not certify on exactly two "
                   "press-coincident steps\n");
            ++cbad;
        }

        // ...and the 6aw hole the legacy bar still has is CLOSED by it: an
        // in-camera counter climbing sequentially with no press near any
        // step never certifies, however long it runs.
        CameraViewVote inCam{};
        cameraViewCertStep(&inCam, 0, true, false, true);
        bool climbed = false;
        for (uint32_t v = 1; v <= 5; ++v) {
            climbed = climbed || cameraViewCertStep(&inCam, v, true, false, true);
        }
        if (climbed) {
            printf("  FAIL  a counter certified under the witnessed bar "
                   "without a single press\n");
            ++cbad;
        }

        // The anchored two-step (keyless): primed at the value the count
        // predicted, two sequential in-camera steps certify -- the player's
        // own walk from the opening view to their preset.
        CameraViewVote anchored{};
        cameraViewCertStep(&anchored, 0, true, false, false);   // primes
        anchored.anchored = true;   // primed at the predicted entry view
        const bool a1 = cameraViewCertStep(&anchored, 1, true, false, false);
        const bool a2 = cameraViewCertStep(&anchored, 2, true, false, false);
        if (a1 || !a2) {
            printf("  FAIL  the anchored bar did not certify on exactly two "
                   "steps from the predicted view\n");
            ++cbad;
        }

        // ...and a broken sequence forfeits the anchor: after noise, the
        // same candidate is back to the unanchored three-step bar.
        CameraViewVote forfeited{};
        cameraViewCertStep(&forfeited, 0, true, false, false);
        forfeited.anchored = true;
        cameraViewCertStep(&forfeited, 1, true, false, false);
        cameraViewCertStep(&forfeited, 5, true, false, false);  // noise: reset
        cameraViewCertStep(&forfeited, 6, true, false, false);
        const bool f2 = cameraViewCertStep(&forfeited, 7, true, false, false);
        if (f2 || forfeited.anchored) {
            printf("  FAIL  a broken sequence kept its anchor or certified "
                   "on two post-noise steps\n");
            ++cbad;
        }

        if (cbad == 0) {
            printf("  ok    certification needs the player's finger: presses "
                   "certify, counters and noise cannot\n");
        }
        g_bad += cbad;
    }

    // THE POISONED READER (6aw): the array contains a counter that certifies
    // under shape rules and supplies garbage. A read that DISAGREES while the
    // player is OUT of the camera is impossible for the real preset -- the
    // game freezes the view there -- so the gate must keep the confirmed
    // value and distrust the reader, and the next entry must arm on the held
    // view, not the poison.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "in the camera on the wanted view, read alive");
    headOffsetGateKeyPressed();           // leave the camera
    sceneFrame(1);
    check(false, "left the camera");
    headOffsetGateSetView(0);             // a suspect reader says 0 out here
    sceneFrame(50);                       // refused: the view cannot change here
    headOffsetGateSetView(-1);            // the poisoned reader dies (6aw did)
    sceneFrame(50);
    panelFrame(200);
    headOffsetGateKeyPressed();           // re-enter
    panelFrame(2);
    sceneFrame(12);
    check(true, "an out-of-camera read naming another view was refused, and "
                "the entry armed on the confirmed view instead of the poison");
    // In the camera a live reader is believed again -- the player can
    // genuinely cycle here -- so a disagreeing in-camera read syncs and the
    // wrong view correctly drops the offset.
    headOffsetGateSetView(0);
    sceneFrame(2);
    check(false, "the same read in the camera syncs, and the wrong view "
                 "drops the offset as ever");
    headOffsetGateSetView(g_wantView);
    sceneFrame(2);
    check(true, "and back on the wanted view it returns");

    // ------------------------------------------------------------------ exits
    //
    // THE KEYED EXIT, which is the only exit render state cannot supply:
    // leaving the camera for a ship produces no panel frame ever.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "in the camera");
    headOffsetGateKeyPressed();
    sceneFrame(1);
    check(false, "the camera key was pressed again");
    sceneFrame(3000);
    check(false, "and it stays off in the ship afterwards");

    // THE SECOND ENTRY OF A SESSION. This failed in every session until the
    // intent age was reset on a keypress: the age accrued for the whole first
    // camera session, so the next press was born already past its grace period
    // and was discarded on the frame it was made.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "first entry");
    headOffsetGateKeyPressed();      // leave
    sceneFrame(2000);                // spend a while in the ship
    check(false, "left the camera");
    panelFrame(200);                 // disembark: first person again
    headOffsetGateKeyPressed();      // enter a second time
    panelFrame(2);
    sceneFrame(12);
    check(true, "SECOND entry in the same session");

    // THE PANEL COMING BACK is first person again, so the offset comes off.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "in the camera");
    panelFrame(3);
    check(false, "the flat panel came back");
    // ...and the intent went with it, so the NEXT press is a fresh entry rather
    // than a toggle back off.
    headOffsetGateKeyPressed();
    panelFrame(2);
    sceneFrame(12);
    check(true, "the press after a panel-return exit is a fresh entry");

    // NEITHER PANEL NOR SCENE for a long stretch -- a menu, a load screen --
    // drops the latch rather than carrying it into whatever comes back.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "in the camera");
    idleFrame(400);
    check(false, "400 frames of neither panel nor scene");

    // A SECOND PRESS WHILE STILL IN FIRST PERSON is still an entry.
    //
    // Measured in a real session: two presses 1.2 s apart while entering the
    // camera, the panel not yet stopped, and the blind toggle read the second
    // as leaving. Every later entry then needed an even number of presses.
    // The panel being up is proof the player is not in the camera, so there is
    // nothing to toggle out of.
    begin(true);
    headOffsetGateSetView(g_wantView);
    panelFrame(200);
    headOffsetGateKeyPressed();      // the player presses...
    panelFrame(1);
    headOffsetGateKeyPressed();      // ...and presses again, still on the panel
    panelFrame(2);
    sceneFrame(12);
    check(true, "two presses while the panel was still up, then the camera");

    // ...and three presses is no different from one, for the same reason.
    begin(true);
    headOffsetGateSetView(g_wantView);
    panelFrame(200);
    headOffsetGateKeyPressed();
    headOffsetGateKeyPressed();
    headOffsetGateKeyPressed();
    panelFrame(2);
    sceneFrame(12);
    check(true, "three presses while the panel was up");

    // But IN the camera, a second press still means leave -- that is the case
    // the panel cannot answer, and the toggle has to survive there.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "in the camera");
    headOffsetGateKeyPressed();
    sceneFrame(2);
    check(false, "pressing again IN the camera still leaves");

    // ONE DROPPED PANEL FRAME MUST NOT ARM ANYTHING.
    //
    // A hitch, a stutter, one composite missed -- sincePanel >= 1 satisfied the
    // window, so the gate armed for exactly as long as it took the panel to
    // come back. That is a one-frame pose jump of the whole offset, and the
    // panel's return then ate the pending intent as well, so the entry the
    // player actually asked for was discarded too.
    begin(true);
    headOffsetGateSetView(g_wantView);
    panelFrame(200);
    headOffsetGateKeyPressed();      // a real entry press, pending
    sceneFrame(1);                   // one frame without the panel
    check(false, "a single dropped panel frame did not arm");
    panelFrame(30);                  // ...and the panel comes back
    check(false, "still off after the panel returned");
    // The press must SURVIVE that, and the real entry still work.
    sceneFrame(12);
    check(true, "the real entry still works after the hitch");

    // THE SHIP VANITY CAMERA MUST NOT GET THE ON-FOOT OFFSET.
    //
    // The same Elite binding opens the ship's camera. On-foot panel credit used
    // to outlive the panel by 300 frames, so boarding and pressing the key
    // within about three seconds armed the on-foot offset on the ship camera --
    // the offset applied in a cockpit, which is the outcome this gate exists to
    // prevent.
    begin(true);
    headOffsetGateSetView(g_wantView);
    panelFrame(200);                 // on foot
    sceneFor(1333);                  // board the ship: a full scene, no panel.
                                     // "within about three seconds" is the
                                     // hazard; this is the 120 frames at 90Hz
                                     // the fixture was written with, in ms.
    headOffsetGateKeyPressed();      // check the ship camera
    sceneFrame(20);
    check(false, "the ship vanity camera did not get the on-foot offset");
    sceneFrame(600);
    check(false, "...and still not, later in the flight");

    // -------------------------------------------------------------- intent
    //
    // A PRESS THAT DID SOMETHING ELSE expires. A camera key pressed in a menu,
    // or that the game ignored, must not latch: it would invert the next real
    // press and the offset would never arm again that session.
    begin(true);
    panelFrame(200);
    headOffsetGateKeyPressed();
    headOffsetGateSetView(g_wantView);
    panelFrame(400);                 // the panel never stops: it did not enter
    sceneFrame(12);
    check(false, "a press that never entered the camera expired");

    // -------------------------------------------------------------- the gate
    //
    // SWITCHING THE GATE OFF must clear the latch, not freeze it. It used to
    // take a bare early return, leaving gateInCamera true -- so re-enabling it
    // republished a stale latch wherever the player had gone by then.
    begin(true);
    headOffsetGateSetView(g_wantView);
    enterCamera();
    check(true, "in the camera");
    {
        // Same effect as fix.head_offset_gate = 0 arriving on a config reload.
        Config::get().set("fix.head_offset_gate", "0");
        headOffsetGateConfigure();
        sceneFrame(1);
        check(false, "the gate was switched off");
        // Now the player boards a ship while it is off...
        sceneFrame(2000);
        // ...and switches it back on. The old code resumed the stale latch here.
        Config::get().set("fix.head_offset_gate", "1");
        headOffsetGateConfigure();
        sceneFrame(12);
        check(false, "the gate was switched back on somewhere else");
    }

    // ------------------------------------------------------------------ done
    Config::get().set("fix.head_offset_gate", "1");
    headOffsetGateReset();

    g_bad += cameraRunChecks();
    g_bad += eyeShapeChecks();
}

}  // namespace

int main(int argc, char** argv) {
    // The real ini, so the scenarios run against the shipped defaults rather
    // than against numbers this file made up. A test that invents its own
    // configuration cannot tell you the configuration you ship is safe.
    const std::string dir = argc > 1 ? argv[1] : ".";
    Config::get().init(std::wstring(dir.begin(), dir.end()));

    g_wantView = Config::get().getIntInRange("advanced.head_offset_view", 2, -1, 63);
    // A shipped -1 means "any view", which would make every view scenario below
    // vacuous rather than failing -- so it is refused here. -1 is a legitimate
    // thing for a USER to set; it is not a legitimate thing to ship, because it
    // arms in the view that faces back at the commander.
    if (g_wantView < 0) {
        printf("  FAIL  edvr.ini ships advanced.head_offset_view = %d (any view), so "
               "the offset would arm in the front-facing view and every view "
               "scenario here would pass without testing anything.\n", g_wantView);
        return 1;
    }
    g_otherView = g_wantView > 0 ? g_wantView - 1 : g_wantView + 1;
    printf("edvr gate frame-feed test -- edvr.ini wants view %d\n", g_wantView);

    // THE SAME SCENARIOS AT ALL THREE SUPPORTED RATES.
    //
    // Elite in VR runs at 72, 90 or 120Hz depending on the headset, and until
    // 2026-08-17 every threshold in the gate was a frame count -- so each of
    // them silently meant a different duration at each rate, and this suite,
    // which steps frames, could not have noticed. Running the whole body three
    // times is what makes "rate-invariant" a tested claim rather than an
    // argument about arithmetic: a regression that reintroduces a frame count
    // fails here at 72 or 120 while still passing at 90.
    edvr::g_clockForTest = &fakeClock;
    g_bad += timingChecks();
    // Pure decisions over numbers, so they run once here rather than inside
    // the 72/90/120Hz loop below -- there is no frame rate in either.
    g_bad += rebuildBackoffChecks();
    g_bad += journalPickChecks();
    g_bad += periodicWorkChecks();
    g_bad += loggedNotesChecks();
    const uint32_t rates[] = {72, 90, 120};
    for (uint32_t hz : rates) {
        const int before = g_bad;
        const int checksBefore = g_checks;
        g_rateHz = hz;
        g_fakeUs = 0;
        g_fakeMs = 0;
        runScenarios();
        printf("  %-4s  %d assertion(s) at %uHz\n",
               g_bad == before ? "ok" : "FAIL", g_checks - checksBefore, hz);
    }
    edvr::g_clockForTest = nullptr;

    if (g_bad) {
        printf("\nGATE TEST FAILED (%d)\n", g_bad);
        return 1;
    }
    printf("  ok    %d assertion(s) total: the offset arms only where it "
           "should, the view index survives the array being rebuilt, and every "
           "verdict is identical at 72, 90 and 120Hz\n", g_checks);
    printf("\nGATE TEST PASSED\n");
    return 0;
}

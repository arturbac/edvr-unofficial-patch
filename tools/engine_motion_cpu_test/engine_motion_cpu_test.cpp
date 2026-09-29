// Engine motion's CPU instrument (src\d3d11\engine_motion_cpu.h): the accumulator
// and the fold, with a fake clock, so every figure is exact.
//
// What it holds to, each a way the first flight of this instrument could have
// lied:
//   - exclusive time: a scope's figure is its own EDVR work; a forward pause
//     leaves the game's code out, a scope nested in EDVR's work is subtracted from
//     its parent, a scope nested in the paused forward is nobody's but its own
//   - no lost update, no contended atomic: four threads write their own slots
//     while another cuts frames; the deltas add back up to the exact call count
//   - thread attribution: the thread that cuts is the render thread, the rest are
//     "other", counted as threads; a frame with no other activity counts none
//   - the window's percentiles, per part and total, against known samples in a
//     hostile order
//   - "the code never ran" is "-", never 0.00; a part that ran and rounded to
//     nothing is 0.00; the unclocked evaluator says it is not clocked
//   - the report's three lines at their worst stay under the log line's limit
//   - the first frame primes: the session's earlier work is never one frame's
//   - the window closes at 30 s and when full, and the per-call maxima are the
//     window's own
//   - the instrument's clock floor is measured, on the real clock, and printed
#include <windows.h>

#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rig {
thread_local bool t_fake = false;
thread_local int64_t t_now = 0;
thread_local int64_t t_step = 0;   // ticks each reading advances the fake clock by (0: it only moves when told)
// A preemption, simulated: after the reading with this ordinal (counted from fake()), the clock jumps a million ticks.
thread_local int64_t t_reads = 0, t_spikeA = -1, t_spikeB = -1;
inline int64_t now() {
    if (t_fake) {
        const int64_t v = t_now;
        ++t_reads;
        if (t_reads == t_spikeA || t_reads == t_spikeB) t_now += 1000000;
        t_now += t_step;
        return v;
    }
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
}  // namespace rig
#define EDVR_EMCPU_NOW() (::rig::now())
#include "../../src/d3d11/engine_motion_cpu.h"

using namespace edvr::emcpu;

namespace {
unsigned g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}
bool nearly(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

void fake(int64_t at) {
    rig::t_fake = true;
    rig::t_now = at;
    rig::t_step = 0;
    rig::t_reads = 0;
    rig::t_spikeA = rig::t_spikeB = -1;
}
uint64_t ticksOf(const Slot* s, unsigned p) { return s->cell[p].ticks.load(); }
uint64_t callsOf(const Slot* s, unsigned p) { return s->cell[p].calls.load(); }
unsigned popcount(const FrameCut& c) {
    unsigned n = 0;
    for (unsigned w = 0; w < kMaskWords; ++w)
        for (uint64_t m = c.otherMask[w]; m; m >>= 1) n += static_cast<unsigned>(m & 1u);
    return n;
}

// One worker thread that runs what it is told, on its own slot and its own fake
// clock, one job at a time (a new thread per job would use up the registry).
class Worker {
public:
    Worker() : thread_([this] { loop(); }) {}
    ~Worker() {
        {
            std::lock_guard<std::mutex> l(m_);
            quit_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    void run(std::function<void()> job) {
        std::unique_lock<std::mutex> l(m_);
        job_ = std::move(job);
        busy_ = true;
        cv_.notify_all();
        cv_.wait(l, [this] { return !busy_; });
    }

private:
    void loop() {
        std::unique_lock<std::mutex> l(m_);
        for (;;) {
            cv_.wait(l, [this] { return busy_ || quit_; });
            if (quit_) return;
            job_();
            busy_ = false;
            cv_.notify_all();
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::function<void()> job_;
    bool busy_ = false, quit_ = false;
    std::thread thread_;
};

// ---- 1: exclusive nesting -------------------------------------------------------
void nesting() {
    resetForTest();
    fake(0);
    Token outer, inner;
    enter(outer, kDraw);
    rig::t_now = 10;
    enter(inner, kPatch);
    rig::t_now = 30;
    leave(inner);
    rig::t_now = 100;
    leave(outer);
    const Slot* s = t_ctx.slot;
    check(ticksOf(s, kPatch) == 20 && callsOf(s, kPatch) == 1, "nesting: the inner scope is its own 20 ticks, one call");
    check(ticksOf(s, kDraw) == 80 && callsOf(s, kDraw) == 1,
          "nesting: the outer scope is 100 less the inner's 20: the parts partition");

    // Two levels deep: each level subtracts only its direct child's whole wall time.
    resetForTest();
    fake(0);
    Token a, b, c;
    enter(a, kDraw);
    rig::t_now = 5;
    enter(b, kApply);
    rig::t_now = 8;
    enter(c, kPatch);
    rig::t_now = 18;
    leave(c);   // patch 10
    rig::t_now = 25;
    leave(b);   // apply: 20 wall - 10 = 10
    rig::t_now = 40;
    leave(a);   // draw: 40 wall - 20 = 20
    s = t_ctx.slot;
    check(ticksOf(s, kPatch) == 10 && ticksOf(s, kApply) == 10 && ticksOf(s, kDraw) == 20,
          "nesting: three deep, 10 + 10 + 20 = the 40 ticks of wall time, none twice");
    check(t_ctx.child == 40, "nesting: the top scope leaves its wall time as the (unused) child of no one");
}

// ---- 2: the forward pause ----------------------------------------------------------
void pausing() {
    resetForTest();
    fake(0);
    Token k;
    enter(k, kEmit);
    rig::t_now = 10;
    const int64_t at = pause(k);
    check(at == 10, "pause: returns the reading it took");
    rig::t_now = 60;   // the game's code runs
    const int64_t back = resume(k);
    check(back == 60, "resume: returns the reading it took");
    rig::t_now = 75;
    leave(k);
    const Slot* s = t_ctx.slot;
    check(ticksOf(s, kEmit) == 25 && callsOf(s, kEmit) == 1,
          "pause: 10 before the forward plus 15 after, the 50 in the game's code excluded");

    // A scope opened inside the paused forward (a relay the game's code called) is its
    // own part's time and is not taken from the outer's.
    resetForTest();
    fake(0);
    Token outer, nested;
    enter(outer, kBuilder);
    rig::t_now = 10;
    pause(outer);
    rig::t_now = 20;
    enter(nested, kRigid);
    rig::t_now = 40;
    leave(nested);
    rig::t_now = 60;
    resume(outer);
    rig::t_now = 75;
    leave(outer);
    s = t_ctx.slot;
    check(ticksOf(s, kRigid) == 20 && ticksOf(s, kBuilder) == 25,
          "pause: a nested relay's 20 is its own, and the outer stays 10 + 15");

    // A scope opened after the resume, inside EDVR's own post-forward work, IS subtracted.
    resetForTest();
    fake(0);
    enter(outer, kBuilder);
    rig::t_now = 10;
    pause(outer);
    rig::t_now = 60;
    resume(outer);
    rig::t_now = 62;
    enter(nested, kPatch);
    rig::t_now = 70;
    leave(nested);
    rig::t_now = 80;
    leave(outer);
    s = t_ctx.slot;
    check(ticksOf(s, kPatch) == 8 && ticksOf(s, kBuilder) == 10 + (80 - 60) - 8,
          "pause: a scope in the post-forward work is taken from the outer: 10 + 20 - 8");

    // Leaving while paused (the forward was the last thing): only the head counts.
    resetForTest();
    fake(0);
    enter(k, kClear);
    rig::t_now = 10;
    pause(k);
    rig::t_now = 50;
    leave(k);
    s = t_ctx.slot;
    check(ticksOf(s, kClear) == 10 && callsOf(s, kClear) == 1, "pause: a scope closed while paused keeps only its head");

    // The RAII form, the same arithmetic.
    resetForTest();
    fake(0);
    {
        Scope scope(kJobs);
        rig::t_now = 7;
        scope.pause();
        rig::t_now = 107;
        scope.resume();
        rig::t_now = 110;
    }
    s = t_ctx.slot;
    check(ticksOf(s, kJobs) == 10 && callsOf(s, kJobs) == 1, "Scope: the RAII form pauses and resumes the same way");

    // A backwards clock never records a negative: it clamps to zero.
    resetForTest();
    fake(100);
    enter(k, kTee);
    rig::t_now = 90;
    leave(k);
    s = t_ctx.slot;
    check(ticksOf(s, kTee) == 0 && callsOf(s, kTee) == 1, "clock: a backwards reading records 0 ticks, one call");
}

// ---- 3: counted, not clocked --------------------------------------------------------
void unclocked() {
    resetForTest();
    fake(0);
    for (int i = 0; i < 5; ++i) count(kEval);
    {
        Scope probe(kEval, false);   // the probe branch: clocked, not counted again
        rig::t_now = 12;
    }
    const Slot* s = t_ctx.slot;
    check(callsOf(s, kEval) == 5 && ticksOf(s, kEval) == 12,
          "eval: five calls counted, the probe branch's 12 ticks added without a sixth call");
}

// ---- 4: the per-call maximum belongs to its window ------------------------------------
void callMaxima() {
    resetForTest();
    fake(0);
    FrameCut cut;
    cutFrame(cut);   // this thread is the render thread
    for (int64_t w : {30, 50, 20}) {
        Token k;
        enter(k, kPatch);
        rig::t_now += w;
        leave(k);
    }
    uint64_t renderMax[kParts], otherMax[kParts];
    collectCallMax(renderMax, otherMax);
    check(renderMax[kPatch] == 50 && otherMax[kPatch] == 0, "max: the longest of 30/50/20 is 50, on the render thread");
    Token k;
    enter(k, kPatch);
    rig::t_now += 20;
    leave(k);
    collectCallMax(renderMax, otherMax);
    check(renderMax[kPatch] == 20, "max: the next window starts over (20, not the old 50)");
    collectCallMax(renderMax, otherMax);
    check(renderMax[kPatch] == 0 && renderMax[kDraw] == 0, "max: a window with no scope reports 0, not a stale figure");
}

// ---- 5: thread attribution -------------------------------------------------------------
void attribution() {
    resetForTest();
    fake(0);
    FrameCut cut;
    cutFrame(cut);   // the priming cut: this thread is the render thread
    {
        Token k;
        enter(k, kDraw);
        rig::t_now = 40;
        leave(k);   // render: draw 40
    }
    Worker w1, w2, w3;
    w1.run([] { fake(0); Token k; enter(k, kEmit); rig::t_now = 100; leave(k); });
    w2.run([] { fake(0); Token k; enter(k, kEmit); rig::t_now = 50; leave(k); enter(k, kRigid); rig::t_now = 80; leave(k); });
    w3.run([] { fake(0); count(kEval); count(kEval); });
    cutFrame(cut);
    check(cut.renderTicks[kDraw] == 40 && cut.renderCalls[kDraw] == 1, "attribution: the cutting thread's scope is the render thread's");
    check(cut.renderTicks[kEmit] == 0 && cut.renderCalls[kEmit] == 0, "attribution: no worker's emit is counted as the render thread's");
    check(cut.otherTicks[kEmit] == 150 && cut.otherCalls[kEmit] == 2, "attribution: two workers' emit: 100 + 50 ticks, two calls");
    check(cut.otherTicks[kRigid] == 30 && cut.otherCalls[kEval] == 2, "attribution: rigid 30 ticks, and w3's two counted evals");
    check(popcount(cut) == 3, "attribution: three other threads were active");

    // The next frame: only w1 works. Deltas, not totals; one other thread.
    w1.run([] { Token k; enter(k, kEmit); rig::t_now += 25; leave(k); });
    cutFrame(cut);
    check(cut.otherTicks[kEmit] == 25 && cut.otherCalls[kEmit] == 1 && popcount(cut) == 1 && cut.renderCalls[kDraw] == 0,
          "attribution: the next cut is a delta (25 ticks, one thread), the render thread idle");

    // A worker that appears mid-session starts from zero, not from the session's baseline.
    Worker w4;
    w4.run([] { fake(0); Token k; enter(k, kCopier); rig::t_now = 9; leave(k); });
    cutFrame(cut);
    check(cut.otherTicks[kCopier] == 9 && popcount(cut) == 1, "attribution: a thread first seen mid-session reports its own 9 ticks");
}

// ---- 6: no lost update, real clock, four writers and a cutter ------------------------------
void concurrency() {
    resetForTest();
    rig::t_fake = false;
    FrameCut cut;
    cutFrame(cut);
    constexpr unsigned kThreads = 4, kIterations = 200000;
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load()) {
            }
            for (unsigned i = 0; i < kIterations; ++i) {
                Token k;
                enter(k, t & 1 ? kEmit : kCopier);
                pause(k);
                resume(k);
                leave(k);
                count(kEval);
            }
        });
    }
    uint64_t emit = 0, copier = 0, eval = 0, cuts = 0;
    go.store(true);
    for (;;) {
        cutFrame(cut);
        ++cuts;
        emit += cut.otherCalls[kEmit] + cut.renderCalls[kEmit];
        copier += cut.otherCalls[kCopier] + cut.renderCalls[kCopier];
        eval += cut.otherCalls[kEval];
        // Done when the writers have made all their calls: read their running totals.
        uint64_t total = 0;
        const unsigned n = slotCount();
        for (unsigned i = 0; i < n; ++i) total += g_slots[i].cell[kEval].calls.load();
        if (total >= uint64_t(kThreads) * kIterations) break;
        std::this_thread::yield();
    }
    for (auto& th : threads) th.join();
    cutFrame(cut);
    emit += cut.otherCalls[kEmit];
    copier += cut.otherCalls[kCopier];
    eval += cut.otherCalls[kEval];
    check(emit == 2ull * kIterations && copier == 2ull * kIterations && eval == 4ull * kIterations,
          "concurrency: every call of four writers is in the cuts' deltas exactly (no lost update)");
    std::printf("engine_motion_cpu_test: %llu concurrent cuts saw %llu emit, %llu copier calls\n",
                static_cast<unsigned long long>(cuts), static_cast<unsigned long long>(emit),
                static_cast<unsigned long long>(copier));
}

// ---- 7: the window's percentiles ---------------------------------------------------------------
Window& sharedWindow() {
    static Window w;   // 300 KB: not for the stack
    return w;
}
void percentileFold() {
    Window& w = sharedWindow();
    w.reset();
    // Render totals 1..100 ms, fed in a hostile order; the draw side is half of each.
    std::vector<unsigned> order;
    for (unsigned i = 0; i < 100; ++i) order.push_back((i * 37 + 11) % 100);
    for (unsigned v : order) {
        FrameCut c;
        std::memset(&c, 0, sizeof(c));
        const uint64_t ticks = (v + 1) * 1000;   // (v+1) ms at 1000 ticks a ms
        c.renderTicks[kDraw] = ticks / 2;
        c.renderTicks[kApply] = ticks - ticks / 2;
        c.renderCalls[kDraw] = 3;
        c.renderCalls[kApply] = 1;
        c.otherTicks[kEmit] = 2000;
        c.otherCalls[kEmit] = 10;
        c.otherMask[0] = (v % 2) ? 0x5ull : 0x2ull;   // slots 0 and 2 on odd frames, slot 1 on even: three threads
        w.add(c, 0.001);
    }
    uint64_t renderMax[kParts] = {}, otherMax[kParts] = {};
    renderMax[kDraw] = 60000;
    otherMax[kEmit] = 4000;
    WindowReport r;
    w.finish(r, 0.001, renderMax, otherMax);
    check(r.valid && r.frames == 100, "fold: 100 frames");
    check(nearly(r.totalP50, 51.0) && nearly(r.totalP95, 96.0) && nearly(r.totalMax, 100.0),
          "fold: totals 1..100 ms: p50 51 (n/2), p95 96, max 100, whatever the order");
    check(nearly(r.part[kDraw].renderMax, 50.0) && nearly(r.part[kApply].renderMax, 50.0),
          "fold: the draw side and apply split each frame in half; max 50 each");
    check(r.part[kDraw].renderCalls == 300 && r.part[kApply].renderCalls == 100, "fold: render calls summed over the window");
    check(r.part[kEmit].otherCalls == 1000 && nearly(r.part[kEmit].otherMs, 200.0), "fold: other threads: 1000 calls, 100 x 2 ms");
    check(nearly(r.part[kDraw].renderCallMaxMs, 60.0) && nearly(r.part[kEmit].otherCallMaxMs, 4.0),
          "fold: the per-call maxima come through in ms");
    check(nearly(r.otherMs, 200.0), "fold: the other-thread total is the sum of its parts");
    check(r.otherThreads == 3, "fold: slots 0, 1 and 2 were active: three other threads");
    check(w.frames() == 0, "fold: finishing empties the window");

    // A window of one frame and of none.
    FrameCut c;
    std::memset(&c, 0, sizeof(c));
    c.renderTicks[kTee] = 7000;
    c.renderCalls[kTee] = 1;
    w.add(c, 0.001);
    w.finish(r, 0.001, renderMax, otherMax);
    check(r.frames == 1 && nearly(r.totalP50, 7.0) && nearly(r.totalP95, 7.0) && nearly(r.totalMax, 7.0),
          "fold: one frame is its own p50, p95 and max");
    w.finish(r, 0.001, renderMax, otherMax);
    check(!r.valid && r.frames == 0, "fold: an empty window is not a report");
}

// ---- 8: text ---------------------------------------------------------------------------------------
WindowReport sampleReport() {
    WindowReport r;
    r.valid = true;
    r.seconds = 30.0;
    r.frames = 2700;
    r.renderTid = 4321;
    r.totalP50 = 0.31;
    r.totalP95 = 0.52;
    r.totalMax = 1.94;
    r.otherThreads = 7;
    r.floor.measured = true;
    r.floor.pairs = 512;
    r.floor.batches = 4;
    r.floor.plainRecordedNs = 34.0;
    r.floor.plainCostNs = 41.0;
    r.floor.pausedRecordedNs = 60.0;
    r.floor.pausedCostNs = 79.0;
    PartWindow& draw = r.part[kDraw];
    draw.renderCalls = 140400;
    draw.renderP50 = 0.20;
    draw.renderP95 = 0.35;
    draw.renderMax = 1.20;
    draw.renderCallMaxMs = 0.21;
    draw.renderMs = 540.0;
    PartWindow& apply = r.part[kApply];
    apply.renderCalls = 5400;
    apply.renderP50 = 0.08;
    apply.renderP95 = 0.12;
    apply.renderMax = 0.60;
    apply.renderCallMaxMs = 0.5;
    PartWindow& patch = r.part[kPatch];
    patch.renderCalls = 3;
    patch.renderMs = 4.2;
    patch.renderCallMaxMs = 2.1;
    PartWindow& emit = r.part[kEmit];
    emit.otherCalls = 2450ull * 2700;
    emit.otherMs = 36000.0;
    emit.otherCallMaxMs = 0.04;
    r.otherMs = 36000.0;
    PartWindow& eval = r.part[kEval];
    eval.otherCalls = 5400ull * 2700;
    return r;
}
void neverRanIsNotZero() {
    char line[1400];
    WindowReport r = sampleReport();
    formatRenderParts(line, sizeof(line), r);
    const std::string render = line;
    check(render.find("draw side 0.20/0.35/1.20 (52.0; 0.21)") != std::string::npos,
          "text: the draw side line: p50/p95/max, calls per frame, longest call");
    check(render.find("shader patch 0.00/0.00/0.00 (0.001; 2.10)") != std::string::npos,
          "text: a rare event (three shader patches in 2700 frames) shows 0.001 calls per frame, not 0.0");
    check(render.find("emit -") != std::string::npos && render.find("jobs -") != std::string::npos,
          "text: a part that never ran on the render thread prints -");
    check(render.find("eval -") != std::string::npos, "text: an evaluator that never ran here prints -");
    check(render.find("shader patches this window on every thread: 3, total 4.20 ms, longest 2.10 ms") != std::string::npos,
          "text: shader patches: count, total, longest");

    // Ran, and cost nothing measurable: 0.00, not -.
    r.part[kTee].renderCalls = 2700;
    formatRenderParts(line, sizeof(line), r);
    check(std::string(line).find("tees 0.00/0.00/0.00 (1.0; 0.00)") != std::string::npos,
          "text: a part that ran and rounds to nothing is 0.00, not -");

    formatOtherParts(line, sizeof(line), r);
    const std::string other = line;
    check(other.find("other threads (7)") != std::string::npos, "text: the other threads' count");
    check(other.find("emit 1200.00 (2450.0; 0.04)") != std::string::npos, "text: emit on other threads: ms per s, calls per frame, longest call");
    check(other.find("draw side -") != std::string::npos, "text: draw side never ran off the render thread: -");
    check(other.find("eval 5400.0 calls per frame, not clocked (a pass-through)") != std::string::npos,
          "text: the evaluator is counted and says it is not clocked");
    r.part[kEval].otherMs = 2700.0 * 0.05;
    formatOtherParts(line, sizeof(line), r);
    check(std::string(line).find("probe work clocked 0.05 ms per frame") != std::string::npos,
          "text: the evaluator's clocked probe work is named separately");

    formatSummary(line, sizeof(line), r);
    const std::string summary = line;
    check(summary.find("30 s, 2700 frames") != std::string::npos && summary.find("Render thread 4321") != std::string::npos,
          "text: the summary names the window and the render thread");
    check(summary.find("total p50 0.31 / p95 0.52 / max 1.94 ms per frame") != std::string::npos, "text: the summary's render total");
    check(summary.find("Other threads: 1200.00 ms per s across 7 threads over 2450.0 clocked calls per frame") != std::string::npos,
          "text: the summary's other threads, and its call rate counts the clocked scopes only (the evaluator's 5400 counted calls are not in it)");
    check(summary.find("ms per frame over 55.0 clocked calls per frame") != std::string::npos,
          "text: the summary's render thread call rate: 52 draw side + 2 apply + 1 tee + a patch's 0.001");
    check(summary.find("records 34 ns and costs 41 ns (60 and 79 ns with a forward pause; the fastest of 4 batches of 512 null pairs, "
                       "measured as this window closed)") != std::string::npos,
          "text: the summary states the clock floor both shapes");
    double spent = 0, recorded = 0;
    instrumentMsPerFrame(r, &spent, &recorded);
    // 52+1+... clocked calls per frame at the plain shape, emit 2450 at the paused shape.
    const double perFramePlain = (140400.0 + 5400.0 + 2700.0 + 3.0) / 2700.0;
    const double perFramePaused = 2450.0;
    check(nearly(spent, (perFramePlain * 41.0 + perFramePaused * 79.0) * 1e-6, 1e-9),
          "text: the instrument's cost is calls per frame x the calibrated cost, by shape, clocked parts only");
    check(nearly(recorded, (perFramePlain * 34.0 + perFramePaused * 60.0) * 1e-6, 1e-9),
          "text: the floor the figures include is calls per frame x the recorded floor");

    // Nothing ever ran: every part is - and the summary says zero calls, not a fabricated figure.
    WindowReport idle;
    idle.valid = true;
    idle.seconds = 30.0;
    idle.frames = 2700;
    formatRenderParts(line, sizeof(line), idle);
    const std::string idleRender = line;
    bool anyNumbers = false;
    for (unsigned p = 0; p < kParts; ++p) {
        const std::string want = std::string(kInfo[p].name) + " -";
        if (idleRender.find(want) == std::string::npos) anyNumbers = true;
    }
    check(!anyNumbers && idleRender.find("shader patches this window: none") != std::string::npos,
          "text: a window in which nothing ran prints - for every part and says there were no patches");
    formatSummary(line, sizeof(line), idle);
    check(std::string(line).find("over 0.0 clocked calls per frame") != std::string::npos &&
              std::string(line).find("Clock floor: not measured.") != std::string::npos,
          "text: nothing ran, no floor: zero calls per frame and an unmeasured floor, both said");
}

void worstCaseLengths() {
    WindowReport r;
    r.valid = true;
    r.seconds = 99999.0;
    r.frames = 999999;
    r.renderTid = 4294967295u;
    r.totalP50 = r.totalP95 = r.totalMax = 99999.99;
    r.otherMs = 99999999.99;
    r.otherThreads = 128;
    r.overflowThreads = 4294967295u;
    r.floor.measured = true;
    r.floor.pairs = 4294967295u;
    r.floor.batches = 4294967295u;
    r.floor.plainRecordedNs = r.floor.plainCostNs = r.floor.pausedRecordedNs = r.floor.pausedCostNs = 99999.0;
    for (unsigned p = 0; p < kParts; ++p) {
        PartWindow& w = r.part[p];
        w.renderCalls = w.otherCalls = 99999999999ull;
        w.renderP50 = w.renderP95 = w.renderMax = 99999.99;
        w.renderCallMaxMs = w.otherCallMaxMs = 99999.99;
        w.renderMs = w.otherMs = 99999999.99;
    }
    char line[2000];
    const size_t a = formatSummary(line, sizeof(line), r);
    const size_t b = formatRenderParts(line, sizeof(line), r);
    const size_t c = formatOtherParts(line, sizeof(line), r);
    std::printf("engine_motion_cpu_test: worst-case line lengths: summary %zu, render parts %zu, other parts %zu (limit 1150)\n", a, b, c);
    check(a < 1150 && b < 1150 && c < 1150, "text: each of the three lines at its worst fits the log line (about 1166 characters)");
    check(std::strlen(line) == c && line[c - 1] == '.', "text: the last line ends whole, not cut");
    char tiny[24];
    check(formatSummary(tiny, sizeof(tiny), r) < sizeof(tiny), "text: a short buffer is cut, never overrun");
}

// ---- 9: the recorder, end to end -----------------------------------------------------------------------
Recorder& sharedRecorder() {
    static Recorder rec;
    return rec;
}
void recorderEndToEnd() {
    resetForTest();
    fake(0);
    Recorder& rec = sharedRecorder();
    constexpr int64_t kFreq = 10000000;   // 10 MHz: a tick is 100 ns
    // The session's earlier work, before the first frame: never one frame's.
    {
        Token k;
        enter(k, kDraw);
        rig::t_now += 999999;
        leave(k);
    }
    Worker worker;
    worker.run([] { fake(0); });
    Figures f = rec.onFrame(kFreq, 1000);
    check(!f.measured, "recorder: the priming frame reports nothing");
    check(!rec.floor().measured, "recorder: no floor yet at the first frame: it is measured as a window closes");

    WindowReport report;
    bool closed = false;
    uint64_t nowMs = 1000;
    for (int frame = 0; frame < 2700 && !closed; ++frame) {
        // The render thread: draw side 0.20 ms in 3 calls, a 0.01 ms patch on frame 10.
        for (int i = 0; i < 3; ++i) {
            Token k;
            enter(k, kDraw);
            rig::t_now += 700;
            leave(k);
        }
        // (3 x 0.07 ms = 0.21 ms)
        if (frame == 10) {
            Token k;
            enter(k, kPatch);
            rig::t_now += 100;
            leave(k);
        }
        worker.run([] {
            Token k;
            enter(k, kEmit);
            rig::t_now += 1500;   // 0.15 ms
            pause(k);
            rig::t_now += 90000;  // the game's code
            resume(k);
            rig::t_now += 500;    // 0.05 ms
            leave(k);
        });
        nowMs += 11;   // 90 Hz, near enough
        f = rec.onFrame(kFreq, nowMs);
        if (frame == 0)
            check(f.measured && nearly(f.renderMs, 0.21) && f.renderCalls == 3 && nearly(f.otherMs, 0.20) && f.otherCalls == 1,
                  "recorder: frame 1's figures are exact: render 0.21 ms in 3 calls, other 0.20 ms in 1");
        if (frame == 10)
            check(nearly(f.renderMs, 0.22) && f.renderCalls == 4, "recorder: the frame with a patch is 0.22 ms in 4 calls, exactly");
        closed = rec.takeReport(report);
    }
    check(!closed, "recorder: the window did not close before 30 s (2700 frames of 11 ms is 29.7 s)");
    // Run on, with no work, to the deadline: 28 more frames of 11 ms make the 2728th.
    while (!closed) {
        nowMs += 11;
        rec.onFrame(kFreq, nowMs);
        closed = rec.takeReport(report);
    }
    check(report.valid && report.frames == 2728 && nearly(report.seconds, 30.008),
          "recorder: the window closes at 30 s, at its 2728th frame");
    check(report.renderTid == GetCurrentThreadId(), "recorder: the report names the render thread's id");
    check(report.otherThreads == 1, "recorder: one other thread");
    check(report.part[kPatch].renderCalls == 1 && nearly(report.part[kPatch].renderMs, 0.01) && nearly(report.part[kPatch].renderCallMaxMs, 0.01),
          "recorder: exactly one shader patch this window, 0.01 ms, longest 0.01");
    check(report.part[kEmit].otherCalls == 2700 && nearly(report.part[kEmit].otherMs, 2700 * 0.20, 1e-6),
          "recorder: the worker's 2700 emit calls, 0.20 ms each, the game's 9 ms in the forward excluded");
    check(nearly(report.part[kEmit].otherCallMaxMs, 0.20), "recorder: the emit's longest call is 0.20 ms");
    check(report.part[kDraw].renderCalls == 8100, "recorder: three draw-side calls a frame, 2700 frames");
    check(nearly(report.part[kDraw].renderP50, 0.21) && nearly(report.totalP50, 0.21) && nearly(report.totalMax, 0.22),
          "recorder: draw side p50 0.21 ms; the total's max is the patch frame's 0.22");
    check(report.floor.measured && report.floor.pairs == 512 && report.floor.batches == 4 && nearly(report.floor.plainCostNs, 0.0) &&
              rec.floor().measured,
          "recorder: the window's report carries the floor measured as it closed (zero on a clock that never moves)");
    check(!rec.takeReport(report), "recorder: a report is handed over once");

    // Nothing at all ran for a window: the report exists and every part reads -.
    resetForTest();
    fake(0);
    Recorder& idle = *new Recorder();
    idle.onFrame(kFreq, 5000);
    nowMs = 5000;
    WindowReport quiet;
    bool got = false;
    while (!got) {
        nowMs += 11;
        idle.onFrame(kFreq, nowMs);
        got = idle.takeReport(quiet);
    }
    bool anyCalls = false;
    for (unsigned p = 0; p < kParts; ++p) anyCalls = anyCalls || quiet.part[p].renderCalls || quiet.part[p].otherCalls;
    check(!anyCalls && quiet.valid && quiet.frames > 2000,
          "recorder: a window in which no hook ran is a full report of zero calls, not silence");
    delete &idle;
}

void windowFull() {
    resetForTest();
    fake(0);
    Recorder& rec = *new Recorder();
    rec.onFrame(10000000, 0);
    WindowReport report;
    bool closed = false;
    unsigned frames = 0;
    while (!closed && frames < 7000) {
        rec.onFrame(10000000, 0);   // the clock never advances: only a full window can close it
        ++frames;
        closed = rec.takeReport(report);
    }
    check(closed && report.frames == Window::kMaxFrames && frames == Window::kMaxFrames,
          "window: a full window closes early, at exactly its capacity, however the clock stands");
    delete &rec;
}

// ---- 10: more threads than slots ---------------------------------------------------------------------
void slotOverflow() {
    resetForTest();
    fake(0);
    FrameCut cut;
    cutFrame(cut);
    for (unsigned i = 0; i < kMaxSlots + 3; ++i) {
        std::thread([] {
            Token k;
            enter(k, kJobs);
            leave(k);
        }).join();
    }
    check(g_overflowThreads.load() == 4, "slots: the threads past the registry (127 free slots + the render thread's) are counted");
    cutFrame(cut);
    check(cut.otherCalls[kJobs] == kMaxSlots + 3, "slots: an overflowing thread's calls still count, in the shared slot");
    WindowReport r;
    r.valid = true;
    r.overflowThreads = g_overflowThreads.load();
    char line[1400];
    formatSummary(line, sizeof(line), r);
    check(std::string(line).find("share one counter: their figures are approximate") != std::string::npos,
          "slots: the report says the overflow figures are approximate");
}

// ---- 11: the real clock's floor -------------------------------------------------------------------------
void realFloor() {
    resetForTest();
    rig::t_fake = false;
    const Floor f = calibrate(qpcFrequency());
    check(f.measured && f.pairs == 512 && f.batches == 4, "floor: measured on the real clock, four batches of 512 pairs");
    check(f.plainCostNs > 0.0 && f.plainCostNs < 20000.0 && f.pausedCostNs > 0.0 && f.pausedCostNs < 40000.0,
          "floor: a null scope costs a plausible number of nanoseconds (0 < cost < 20 us)");
    check(f.plainRecordedNs >= 0.0 && f.pausedRecordedNs >= 0.0, "floor: what a null scope records is never negative");
    std::printf("engine_motion_cpu_test: clock floor on this machine: plain scope records %.1f ns, costs %.1f ns; "
                "with a forward pause records %.1f ns, costs %.1f ns (fastest of %u batches of %u pairs, QPC %lld Hz)\n",
                f.plainRecordedNs, f.plainCostNs, f.pausedRecordedNs, f.pausedCostNs, f.batches, f.pairs,
                static_cast<long long>(qpcFrequency()));
    const Floor none = calibrate(0);
    check(!none.measured, "floor: no clock rate, no measurement");
    // And on a fake clock everything is exactly zero.
    fake(0);
    const Floor fk = calibrate(10000000);
    check(fk.measured && fk.plainCostNs == 0.0 && fk.plainRecordedNs == 0.0, "floor: a fake clock that never moves measures zero");

    // The arithmetic, exactly: a clock that advances 3 ticks (300 ns at 10 MHz) on every reading. A plain
    // pair reads twice: it records 3 ticks (enter to leave) and costs 6 (both readings' advance). A pair
    // with a forward pause reads four times: it records 3 + 3 (the head and the tail, the pause between
    // them left out) and costs 12. The loop's own two bracketing readings add 3 ticks to the total.
    fake(0);
    rig::t_step = 3;
    const Floor step = calibrate(10000000);
    rig::t_step = 0;
    const double pairs = 512.0;
    check(step.measured && nearly(step.plainRecordedNs, 300.0) && nearly(step.plainCostNs, (3.0 + 6.0 * pairs) * 100.0 / pairs),
          "floor: a plain null scope records 300 ns and costs 600 ns (plus the loop's own reading) on a 3-tick-a-reading clock");
    check(nearly(step.pausedRecordedNs, 600.0) && nearly(step.pausedCostNs, (3.0 + 12.0 * pairs) * 100.0 / pairs),
          "floor: a null scope with a forward pause records 600 ns and costs 1200 ns: two more readings, the gap between them out");

    // A batch a preemption inflated is not the floor: the fastest of the four is kept. The clock jumps a million
    // ticks (100 ms) once inside the first plain batch (reading 300) and once inside the first paused batch
    // (reading 5000); the answer is the clean one, not an average and not the first batch.
    fake(0);
    rig::t_step = 3;
    rig::t_spikeA = 300;
    rig::t_spikeB = 5000;
    const Floor spiked = calibrate(10000000);
    rig::t_step = 0;
    rig::t_spikeA = rig::t_spikeB = -1;
    check(nearly(spiked.plainRecordedNs, 300.0) && nearly(spiked.plainCostNs, (3.0 + 6.0 * pairs) * 100.0 / pairs) &&
              nearly(spiked.pausedRecordedNs, 600.0) && nearly(spiked.pausedCostNs, (3.0 + 12.0 * pairs) * 100.0 / pairs),
          "floor: a preempted batch (a 100 ms jump in the first batch of each shape) does not inflate the floor: the fastest batch is kept");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--dry-run")) {
        std::puts("engine_motion_cpu_test: dry-run (no threads, no clock, no files)");
        return 0;
    }
    if (argc != 2 || wcscmp(argv[1], L"--self-test")) {
        std::fputs("usage: --self-test | --dry-run\n", stderr);
        return 2;
    }
    nesting();
    pausing();
    unclocked();
    callMaxima();
    attribution();
    concurrency();
    percentileFold();
    neverRanIsNotZero();
    worstCaseLengths();
    recorderEndToEnd();
    windowFull();
    slotOverflow();
    realFloor();
    std::printf("engine_motion_cpu_test: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

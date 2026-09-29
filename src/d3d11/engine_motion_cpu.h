#pragma once
// Engine motion's CPU time on the game's threads, per frame, every call clocked
// (docs\openxr-performance-review-2026-09-14.md, the 2026-09-29 carrier entry).
//
// WHY. At Sean's busy fleet carrier Elite's render thread runs 11.6-12.4 ms a
// cycle with fix.temporal_aa on and about 5 ms with it off, and two things the
// AA path costs were not measured: the GPU it spends inside Elite's own draws
// (gpu_census.cpp's altered-draw sections) and the CPU it spends in hooks on
// Elite's own code, which the frame tick chain never saw (it covers the Present
// hook and, sampled, the Direct3D draw hooks). Engine motion has fifteen relays
// into game code, and the emit hook alone ran 1,801-2,930 calls a frame there.
//
// WHAT IS TIMED. EDVR's own work in each hook, never the game function the hook
// calls through: a relay that forwards is bracketed enter / pause / (game code) /
// resume / leave, so the forward is excluded and a nested EDVR scope opened
// inside it (a relay called by game code the outer relay forwarded to) is its
// own part's time and no one else's. A scope nested in EDVR's OWN work (the
// shader patch inside the draw side's slow half) is subtracted from its parent,
// so the parts partition: no nanosecond is in two of them.
//
// EVERY CALL, NO STRIDE, NO CONTENDED ATOMIC. Each thread owns one Slot of
// counters, registered on its first scope (one fetch_add, once per thread) and
// written only by that thread: a running total is a relaxed load, an add and a
// relaxed store, plain moves on x64, no lock prefix, no shared cache line. The
// render thread's per-frame cut reads every slot's totals and takes the deltas.
//
// THE ONE THING NOT CLOCKED, and the report says so: the two evaluator relays
// (Part kEval) forward straight through unless a diagnostic is attached, so
// their own work is two loads and a call, about 5 ns. A clock read alone costs
// more than that (the floor below), so every call is COUNTED and only the probe
// branches, which have real work, are clocked. The count bounds the cost.
//
// THREAD ATTRIBUTION. "Render thread" is the thread that calls Present: the
// Present hook calls cutFrame, and the slot of the thread that cut is the render
// slot from then on. Every other slot is "other threads".
//
// THE INSTRUMENT'S OWN COST is calibrated the way the GPU census states its
// timer floor: null scopes on the render thread at the first frame, both shapes
// (plain, and with a forward pause), what a null scope RECORDS (it inflates every
// figure by that much per call) and what it COSTS (wall time per call), and the
// report multiplies them by the window's call rates.
//
// "THE CODE NEVER RAN" IS NEVER 0.00. A part with no calls in the window on a
// class of thread prints "-"; a part with calls whose time rounds to nothing
// prints 0.00. The LONG FRAME clause says "none this frame" for no calls.
//
// Header only, so tools\engine_motion_cpu_test can drive every function with a
// fake clock (define EDVR_EMCPU_NOW before including it).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef EDVR_EMCPU_NOW
#define EDVR_EMCPU_NOW() (::edvr::emcpu::qpcNow())
#endif

namespace edvr {
namespace emcpu {

inline int64_t qpcNow() noexcept {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
inline int64_t qpcFrequency() noexcept {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

// ---- the parts ---------------------------------------------------------------
enum Part : unsigned {
    kEmit = 0,   // FUN_144312E00's bracket (EDVR's work around the forward) and the emit observer
    kRigid,      // FUN_1442B4130, the primary rigid emit: relay + observer
    kCopier,     // FUN_144C81BE0, the engine's pool copier: relay + observer
    kMerge,      // FUN_14434E740, the list merge: begin and end
    kClear,      // FUN_1436819D0, the typed dictionary clear: the observer
    kEval,       // the evaluator and rig-eval relays: counted; only probe work is clocked
    kJobs,       // the six job-body brackets (UpdateRenderDataJob and its siblings)
    kBuilder,    // the draw-item builder bracket, the second direct producer, the part test, the LOD setter
    kDraw,       // the pool-family draw side: the slow half of engineVelocityBeforeDraw
    kApply,      // primaryCopy::apply inside the draw side (nested in it, not part of it)
    kTee,        // the Map/Unmap/write tees on watched resources
    kPatch,      // the lazy shader patch on a cache miss (nested in the draw side)
    kParts
};
constexpr unsigned kCal = kParts;          // the calibration cell, never reported
constexpr unsigned kCells = kParts + 1;

struct PartInfo {
    const char* name;
    bool clocked;   // false: counted, and only the work behind a diagnostic is clocked
    bool paused;    // the scope's shape: enter / pause / resume / leave (four clock reads)
};
inline constexpr PartInfo kInfo[kParts] = {
    {"emit", true, true},        {"rigid emit", true, true}, {"copier", true, true},
    {"merge", true, true},       {"clear", true, false},     {"eval", false, false},
    {"jobs", true, true},        {"builder", true, true},    {"draw side", true, false},
    {"apply", true, false},      {"tees", true, false},      {"shader patch", true, false},
};
// Report order: the render thread's own parts first.
inline constexpr unsigned kOrder[kParts] = {kDraw, kApply, kTee,     kPatch, kEmit,    kRigid,
                                            kCopier, kMerge, kClear, kJobs,  kBuilder, kEval};
constexpr bool orderIsPermutation() {
    for (unsigned p = 0; p < kParts; ++p) {
        unsigned seen = 0;
        for (unsigned i = 0; i < kParts; ++i) seen += kOrder[i] == p ? 1u : 0u;
        if (seen != 1) return false;
    }
    return true;
}
static_assert(orderIsPermutation(), "every part is reported exactly once");

// ---- the per-thread counters --------------------------------------------------
struct alignas(32) Cell {
    std::atomic<uint64_t> ticks{0};     // exclusive time, running total
    std::atomic<uint64_t> calls{0};     // running total
    std::atomic<uint64_t> maxTicks{0};  // the longest single call of maxEpoch's window
    uint64_t spare_ = 0;                // a cell is one 32-byte half of a cache line
};
struct alignas(64) Slot {
    Cell cell[kCells];
    std::atomic<uint32_t> tid{0};
    std::atomic<uint32_t> maxEpoch{0};
    uint64_t spare_[3] = {};            // fills the slot to seven cache lines
};
static_assert(sizeof(Cell) == 32 && sizeof(Slot) == 448, "the layout the comments describe");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "the counters are plain moves on x64");

constexpr unsigned kMaxSlots = 128;
inline Slot g_slots[kMaxSlots];
inline Slot g_overflow;   // threads past kMaxSlots share this one: approximate, counted, said
inline std::atomic<unsigned> g_slotCount{0};
inline std::atomic<unsigned> g_overflowThreads{0};
inline std::atomic<uint32_t> g_epoch{1};   // the window the per-call maxima belong to
inline std::atomic<Slot*> g_renderSlot{nullptr};

struct ThreadCtx {
    Slot* slot;
    int64_t child;   // the direct children's wall time inside the scope now open on this thread
};
inline thread_local ThreadCtx t_ctx{nullptr, 0};

__declspec(noinline) inline Slot* registerSlot() noexcept {
    ThreadCtx& c = t_ctx;
    if (c.slot) return c.slot;
    const unsigned i = g_slotCount.fetch_add(1, std::memory_order_acq_rel);
    Slot* s = nullptr;
    if (i < kMaxSlots) {
        s = &g_slots[i];
    } else {
        g_overflowThreads.fetch_add(1, std::memory_order_relaxed);
        s = &g_overflow;
    }
    s->tid.store(static_cast<uint32_t>(GetCurrentThreadId()), std::memory_order_relaxed);
    s->maxEpoch.store(g_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
    c.slot = s;
    return s;
}
inline unsigned slotCount() noexcept {
    return (std::min)(g_slotCount.load(std::memory_order_acquire), kMaxSlots);
}

inline void record(Slot* s, unsigned part, int64_t excl, bool counts) noexcept {
    if (excl < 0) excl = 0;
    const uint64_t e = static_cast<uint64_t>(excl);
    Cell& c = s->cell[part];
    c.ticks.store(c.ticks.load(std::memory_order_relaxed) + e, std::memory_order_relaxed);
    if (counts) c.calls.store(c.calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    const uint32_t epoch = g_epoch.load(std::memory_order_relaxed);
    if (s->maxEpoch.load(std::memory_order_relaxed) != epoch) {
        for (unsigned p = 0; p < kCells; ++p) s->cell[p].maxTicks.store(0, std::memory_order_relaxed);
        s->maxEpoch.store(epoch, std::memory_order_relaxed);
    }
    if (e > c.maxTicks.load(std::memory_order_relaxed)) c.maxTicks.store(e, std::memory_order_relaxed);
}

// One counted call with no clock read (Part kEval): a thread-local load, the
// slot, an increment.
inline void count(unsigned part) noexcept {
    Slot* s = t_ctx.slot;
    if (!s) s = registerSlot();
    Cell& c = s->cell[part];
    c.calls.store(c.calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// ---- the scope ----------------------------------------------------------------
// A POD token, for functions with __try/__finally (C2712: no object with a
// destructor there), and the RAII Scope below for the rest. The clock is read
// LAST in enter/pause/resume/leave-before-bookkeeping and FIRST in the closing
// calls, so the bookkeeping is outside the timed span: what a null scope
// records is about one clock read's latency, not the whole call.
struct Token {
    ThreadCtx* ctx;
    Slot* slot;
    int64_t begin, seg, excl, savedChild;
    uint8_t part;
    bool counts, paused;
};
inline void enter(Token& k, unsigned part, bool counts = true) noexcept {
    ThreadCtx& c = t_ctx;
    Slot* s = c.slot;
    if (!s) s = registerSlot();
    k.ctx = &c;
    k.slot = s;
    k.part = static_cast<uint8_t>(part);
    k.counts = counts;
    k.paused = false;
    k.excl = 0;
    k.savedChild = c.child;
    c.child = 0;
    const int64_t now = EDVR_EMCPU_NOW();
    k.begin = k.seg = now;
}
// Before calling into game code. Returns the reading, so a caller that already
// clocked the game call (kinematic_eval_hook's job bracket) shares it.
inline int64_t pause(Token& k) noexcept {
    const int64_t now = EDVR_EMCPU_NOW();
    if (!k.paused) {
        k.excl += (now - k.seg) - k.ctx->child;
        k.paused = true;
    }
    k.ctx->child = 0;
    return now;
}
inline int64_t resume(Token& k) noexcept {
    const int64_t now = EDVR_EMCPU_NOW();
    k.seg = now;
    k.paused = false;
    k.ctx->child = 0;
    return now;
}
inline void leave(Token& k) noexcept {
    const int64_t now = EDVR_EMCPU_NOW();
    if (!k.paused) k.excl += (now - k.seg) - k.ctx->child;
    record(k.slot, k.part, k.excl, k.counts);
    // The parent sees this scope's whole wall time (its forward included) as
    // its child, whether or not the parent is paused: a paused parent resets it.
    k.ctx->child = k.savedChild + (now - k.begin);
}

class Scope {
public:
    explicit Scope(unsigned part, bool counts = true) noexcept { enter(k_, part, counts); }
    ~Scope() { leave(k_); }
    int64_t pause() noexcept { return emcpu::pause(k_); }
    int64_t resume() noexcept { return emcpu::resume(k_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Token k_;
};

// ---- the render thread's per-frame cut ------------------------------------------
struct CutState {
    uint64_t ticks[kParts];
    uint64_t calls[kParts];
};
inline CutState g_cut[kMaxSlots + 1];
constexpr unsigned kMaskWords = (kMaxSlots + 1 + 63) / 64;

struct FrameCut {
    uint64_t renderTicks[kParts];
    uint64_t renderCalls[kParts];
    uint64_t otherTicks[kParts];
    uint64_t otherCalls[kParts];
    uint64_t otherMask[kMaskWords];   // the slots (threads) with any activity this frame
};

// Called by the thread that calls Present, once per frame: that thread is the
// render thread. Deltas since the previous cut, split render / other.
inline void cutFrame(FrameCut& out) noexcept {
    std::memset(&out, 0, sizeof(out));
    Slot* me = t_ctx.slot;
    if (!me) me = registerSlot();
    g_renderSlot.store(me, std::memory_order_relaxed);
    const unsigned n = slotCount();
    for (unsigned i = 0; i <= n; ++i) {
        Slot* s = i < n ? &g_slots[i] : &g_overflow;
        const unsigned index = i < n ? i : kMaxSlots;
        CutState& st = g_cut[index];
        const bool render = s == me;
        bool active = false;
        for (unsigned p = 0; p < kParts; ++p) {
            const uint64_t t = s->cell[p].ticks.load(std::memory_order_relaxed);
            const uint64_t k = s->cell[p].calls.load(std::memory_order_relaxed);
            const uint64_t dt = t - st.ticks[p], dk = k - st.calls[p];
            st.ticks[p] = t;
            st.calls[p] = k;
            if (render) {
                out.renderTicks[p] += dt;
                out.renderCalls[p] += dk;
            } else {
                out.otherTicks[p] += dt;
                out.otherCalls[p] += dk;
                if (dt | dk) active = true;
            }
        }
        if (active) out.otherMask[index / 64] |= uint64_t(1) << (index % 64);
    }
}

// The rig only: forget every slot. Not safe with other threads alive.
inline void resetForTest() noexcept {
    for (unsigned i = 0; i < kMaxSlots; ++i) {
        Slot& s = g_slots[i];
        for (unsigned p = 0; p < kCells; ++p) {
            s.cell[p].ticks.store(0);
            s.cell[p].calls.store(0);
            s.cell[p].maxTicks.store(0);
        }
        s.tid.store(0);
        s.maxEpoch.store(0);
    }
    for (unsigned p = 0; p < kCells; ++p) {
        g_overflow.cell[p].ticks.store(0);
        g_overflow.cell[p].calls.store(0);
        g_overflow.cell[p].maxTicks.store(0);
    }
    std::memset(g_cut, 0, sizeof(g_cut));
    g_slotCount.store(0);
    g_overflowThreads.store(0);
    g_epoch.store(1);
    g_renderSlot.store(nullptr);
    t_ctx.slot = nullptr;
    t_ctx.child = 0;
}

// ---- the clock floor -------------------------------------------------------------
struct Floor {
    bool measured = false;
    unsigned pairs = 0;
    double plainRecordedNs = 0, plainCostNs = 0;    // enter / leave
    double pausedRecordedNs = 0, pausedCostNs = 0;  // enter / pause / resume / leave
};
// Null scopes on the calling thread, into the calibration cell (never reported).
// Recorded: what a scope with nothing in it writes into its part's ticks, which
// every real figure includes once per call. Cost: wall time per call, the
// instrument's own price.
inline Floor calibrate(int64_t freq, unsigned pairs = 2048) noexcept {
    Floor f;
    if (freq <= 0 || pairs == 0) return f;
    Token k;
    Slot* s = t_ctx.slot;
    if (!s) s = registerSlot();
    for (unsigned i = 0; i < 64; ++i) {   // warm: the slot, the thread-local, the code
        enter(k, kCal, false);
        leave(k);
    }
    const double perNs = 1e9 / static_cast<double>(freq) / static_cast<double>(pairs);
    {
        const uint64_t r0 = s->cell[kCal].ticks.load(std::memory_order_relaxed);
        const int64_t t0 = EDVR_EMCPU_NOW();
        for (unsigned i = 0; i < pairs; ++i) {
            enter(k, kCal, false);
            leave(k);
        }
        const int64_t t1 = EDVR_EMCPU_NOW();
        const uint64_t r1 = s->cell[kCal].ticks.load(std::memory_order_relaxed);
        f.plainCostNs = static_cast<double>(t1 - t0) * perNs;
        f.plainRecordedNs = static_cast<double>(r1 - r0) * perNs;
    }
    {
        const uint64_t r0 = s->cell[kCal].ticks.load(std::memory_order_relaxed);
        const int64_t t0 = EDVR_EMCPU_NOW();
        for (unsigned i = 0; i < pairs; ++i) {
            enter(k, kCal, false);
            pause(k);
            resume(k);
            leave(k);
        }
        const int64_t t1 = EDVR_EMCPU_NOW();
        const uint64_t r1 = s->cell[kCal].ticks.load(std::memory_order_relaxed);
        f.pausedCostNs = static_cast<double>(t1 - t0) * perNs;
        f.pausedRecordedNs = static_cast<double>(r1 - r0) * perNs;
    }
    f.pairs = pairs;
    f.measured = true;
    return f;
}

// ---- one window ------------------------------------------------------------------
struct PartWindow {
    uint64_t renderCalls = 0, otherCalls = 0;
    double renderMs = 0;                                   // window total
    double renderP50 = 0, renderP95 = 0, renderMax = 0;    // ms per frame, over every frame
    double renderCallMaxMs = 0;                            // the longest single call
    double otherMs = 0;                                    // window total
    double otherCallMaxMs = 0;
};
struct WindowReport {
    bool valid = false;
    double seconds = 0;
    unsigned frames = 0;
    unsigned renderTid = 0;
    PartWindow part[kParts];
    double totalP50 = 0, totalP95 = 0, totalMax = 0;       // render thread, every part, ms per frame
    double otherMs = 0;                                    // window total, every part, other threads
    unsigned otherThreads = 0;
    unsigned overflowThreads = 0;
    Floor floor;
};

inline void percentiles(float* a, unsigned n, double* p50, double* p95, double* mx) noexcept {
    *p50 = *p95 = *mx = 0.0;
    if (!n) return;
    float* end = a + n;
    *mx = static_cast<double>(*std::max_element(a, end));
    unsigned i95 = static_cast<unsigned>(0.95 * static_cast<double>(n));
    if (i95 >= n) i95 = n - 1;
    const unsigned i50 = n / 2;
    std::nth_element(a, a + i95, end);
    *p95 = static_cast<double>(a[i95]);
    if (i50 >= i95) {
        *p50 = static_cast<double>(a[i95]);
    } else {
        std::nth_element(a, a + i50, a + i95);
        *p50 = static_cast<double>(a[i50]);
    }
}

class Window {
public:
    static constexpr unsigned kMaxFrames = 6000;   // 200 Hz for 30 s; a full window closes early

    void reset() noexcept {
        frames_ = 0;
        std::memset(renderCalls_, 0, sizeof(renderCalls_));
        std::memset(otherCalls_, 0, sizeof(otherCalls_));
        std::memset(renderTicks_, 0, sizeof(renderTicks_));
        std::memset(otherTicks_, 0, sizeof(otherTicks_));
        std::memset(mask_, 0, sizeof(mask_));
    }
    unsigned frames() const noexcept { return frames_; }
    bool full() const noexcept { return frames_ >= kMaxFrames; }

    void add(const FrameCut& c, double msPerTick) noexcept {
        if (frames_ >= kMaxFrames) return;
        double total = 0.0;
        for (unsigned p = 0; p < kParts; ++p) {
            const double ms = static_cast<double>(c.renderTicks[p]) * msPerTick;
            ms_[p][frames_] = static_cast<float>(ms);
            total += ms;
            renderCalls_[p] += c.renderCalls[p];
            otherCalls_[p] += c.otherCalls[p];
            renderTicks_[p] += c.renderTicks[p];
            otherTicks_[p] += c.otherTicks[p];
        }
        ms_[kParts][frames_] = static_cast<float>(total);
        for (unsigned w = 0; w < kMaskWords; ++w) mask_[w] |= c.otherMask[w];
        ++frames_;
    }

    // Folds the window into out and empties it. renderMax/otherMax are the
    // per-call maxima in ticks (from the slots, collectCallMax).
    void finish(WindowReport& out, double msPerTick, const uint64_t renderMax[kParts],
                const uint64_t otherMax[kParts]) noexcept {
        out = WindowReport{};
        out.valid = frames_ > 0;
        out.frames = frames_;
        for (unsigned p = 0; p < kParts; ++p) {
            PartWindow& w = out.part[p];
            w.renderCalls = renderCalls_[p];
            w.otherCalls = otherCalls_[p];
            w.renderMs = static_cast<double>(renderTicks_[p]) * msPerTick;
            w.otherMs = static_cast<double>(otherTicks_[p]) * msPerTick;
            w.renderCallMaxMs = static_cast<double>(renderMax[p]) * msPerTick;
            w.otherCallMaxMs = static_cast<double>(otherMax[p]) * msPerTick;
            percentiles(ms_[p], frames_, &w.renderP50, &w.renderP95, &w.renderMax);
            out.otherMs += w.otherMs;
        }
        percentiles(ms_[kParts], frames_, &out.totalP50, &out.totalP95, &out.totalMax);
        for (unsigned w = 0; w < kMaskWords; ++w) {
            uint64_t m = mask_[w];
            while (m) {
                out.otherThreads += static_cast<unsigned>(m & 1u);
                m >>= 1;
            }
        }
        reset();
    }

private:
    unsigned frames_ = 0;
    uint64_t renderCalls_[kParts] = {}, otherCalls_[kParts] = {};
    uint64_t renderTicks_[kParts] = {}, otherTicks_[kParts] = {};
    uint64_t mask_[kMaskWords] = {};
    float ms_[kParts + 1][kMaxFrames];
};

// The per-call maxima of the window that just ended, from the slots that
// recorded anything in it; then the next window starts.
inline void collectCallMax(uint64_t renderMax[kParts], uint64_t otherMax[kParts]) noexcept {
    std::memset(renderMax, 0, sizeof(uint64_t) * kParts);
    std::memset(otherMax, 0, sizeof(uint64_t) * kParts);
    const uint32_t epoch = g_epoch.load(std::memory_order_relaxed);
    const Slot* render = g_renderSlot.load(std::memory_order_relaxed);
    const unsigned n = slotCount();
    for (unsigned i = 0; i <= n; ++i) {
        const Slot* s = i < n ? &g_slots[i] : &g_overflow;
        if (s->maxEpoch.load(std::memory_order_relaxed) != epoch) continue;
        uint64_t* dst = s == render ? renderMax : otherMax;
        for (unsigned p = 0; p < kParts; ++p)
            dst[p] = (std::max)(dst[p], s->cell[p].maxTicks.load(std::memory_order_relaxed));
    }
    g_epoch.fetch_add(1, std::memory_order_relaxed);
}

// ---- what one frame looked like, for the LONG FRAME clause -------------------------
struct Figures {
    bool measured = false;         // false on the priming frame and with no clock rate
    double renderMs = 0;           // EDVR's engine-motion hook time on the render thread this frame
    uint64_t renderCalls = 0;
    double otherMs = 0;            // the same on every other thread (thread-ms, summed)
    uint64_t otherCalls = 0;
};

class Recorder {
public:
    static constexpr uint64_t kWindowMs = 30000;

    // Once per frame, from the Present hook (the render thread). The first call
    // primes the baselines and calibrates the clock floor; it reports nothing.
    Figures onFrame(int64_t freq, uint64_t nowMs) noexcept {
        Figures fig;
        FrameCut cut;
        cutFrame(cut);
        if (!primed_) {
            primed_ = true;
            floor_ = calibrate(freq);
            windowStartMs_ = nowMs;
            window_.reset();
            return fig;
        }
        const double msPerTick = freq > 0 ? 1000.0 / static_cast<double>(freq) : 0.0;
        window_.add(cut, msPerTick);
        fig.measured = freq > 0;
        for (unsigned p = 0; p < kParts; ++p) {
            fig.renderMs += static_cast<double>(cut.renderTicks[p]) * msPerTick;
            fig.renderCalls += cut.renderCalls[p];
            fig.otherMs += static_cast<double>(cut.otherTicks[p]) * msPerTick;
            fig.otherCalls += cut.otherCalls[p];
        }
        if (nowMs - windowStartMs_ >= kWindowMs || window_.full()) {
            uint64_t renderMax[kParts], otherMax[kParts];
            collectCallMax(renderMax, otherMax);
            window_.finish(report_, msPerTick, renderMax, otherMax);
            report_.seconds = static_cast<double>(nowMs - windowStartMs_) / 1000.0;
            const Slot* render = g_renderSlot.load(std::memory_order_relaxed);
            report_.renderTid = render ? render->tid.load(std::memory_order_relaxed) : 0;
            report_.overflowThreads = g_overflowThreads.load(std::memory_order_relaxed);
            report_.floor = floor_;
            ready_ = report_.valid;
            windowStartMs_ = nowMs;
        }
        return fig;
    }
    bool takeReport(WindowReport& out) noexcept {
        if (!ready_) return false;
        out = report_;
        ready_ = false;
        return true;
    }
    const Floor& floor() const noexcept { return floor_; }

private:
    bool primed_ = false, ready_ = false;
    uint64_t windowStartMs_ = 0;
    Floor floor_;
    Window window_;
    WindowReport report_;
};

// ---- text ---------------------------------------------------------------------------
inline void appendf(char* buf, size_t cap, size_t& len, const char* fmt, ...) noexcept {
    if (!buf || len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    len = static_cast<size_t>(n) < cap - len ? len + static_cast<size_t>(n) : cap - 1;
}

// Calls per frame: one decimal, three when it is under 0.1 so a rare event (a shader
// patch) does not read as 0.0, which would look like code that never ran.
inline void callsText(char* out, size_t cap, double perFrame) noexcept {
    std::snprintf(out, cap, perFrame > 0.0 && perFrame < 0.1 ? "%.3f" : "%.1f", perFrame);
}

// The instrument's own price at this window's call rates, thread-ms per frame:
// what its clock reads cost (spent), and what they add to the figures (recorded).
inline void instrumentMsPerFrame(const WindowReport& r, double* spent, double* recorded) noexcept {
    *spent = *recorded = 0.0;
    if (!r.floor.measured || !r.frames) return;
    for (unsigned p = 0; p < kParts; ++p) {
        if (!kInfo[p].clocked) continue;
        const double perFrame =
            static_cast<double>(r.part[p].renderCalls + r.part[p].otherCalls) / static_cast<double>(r.frames);
        *spent += perFrame * (kInfo[p].paused ? r.floor.pausedCostNs : r.floor.plainCostNs) * 1e-6;
        *recorded += perFrame * (kInfo[p].paused ? r.floor.pausedRecordedNs : r.floor.plainRecordedNs) * 1e-6;
    }
}

inline size_t formatSummary(char* buf, size_t cap, const WindowReport& r) noexcept {
    size_t len = 0;
    if (!buf || !cap) return 0;
    buf[0] = 0;
    uint64_t renderCalls = 0, otherCalls = 0;   // the clocked scopes; the evaluator's counted calls are on the parts lines
    for (unsigned p = 0; p < kParts; ++p) {
        if (!kInfo[p].clocked) continue;
        renderCalls += r.part[p].renderCalls;
        otherCalls += r.part[p].otherCalls;
    }
    const double frames = r.frames ? static_cast<double>(r.frames) : 1.0;
    const double seconds = r.seconds > 0.0 ? r.seconds : 1.0;
    appendf(buf, cap, len,
            "engine motion CPU: %.0f s, %u frames; EDVR's own work in its hooks on the game's code, every call "
            "clocked, the game's forwarded code excluded. Render thread %u: total p50 %.2f / p95 %.2f / max %.2f "
            "ms per frame over %.1f clocked calls per frame. Other threads: %.2f ms per s across %u threads over %.1f "
            "clocked calls per frame. ",
            r.seconds, r.frames, r.renderTid, r.totalP50, r.totalP95, r.totalMax,
            static_cast<double>(renderCalls) / frames, r.otherMs / seconds, r.otherThreads,
            static_cast<double>(otherCalls) / frames);
    if (r.floor.measured) {
        double spent = 0.0, recorded = 0.0;
        instrumentMsPerFrame(r, &spent, &recorded);
        appendf(buf, cap, len,
                "Clock floor: a timed scope records %.0f ns and costs %.0f ns (%.0f and %.0f ns with a forward "
                "pause; %u null pairs), so these figures include about %.3f ms per frame of floor and the "
                "instrument costs about %.3f ms per frame.",
                r.floor.plainRecordedNs, r.floor.plainCostNs, r.floor.pausedRecordedNs, r.floor.pausedCostNs,
                r.floor.pairs, recorded, spent);
    } else {
        appendf(buf, cap, len, "Clock floor: not measured.");
    }
    if (r.overflowThreads)
        appendf(buf, cap, len, " %u threads past %u share one counter: their figures are approximate.",
                r.overflowThreads, kMaxSlots);
    return len;
}

// The shader patches across both classes of thread: count, total and longest.
struct PatchTotals {
    uint64_t count;
    double totalMs, longestMs;
};
inline PatchTotals patchTotals(const WindowReport& r) noexcept {
    const PartWindow& w = r.part[kPatch];
    return {w.renderCalls + w.otherCalls, w.renderMs + w.otherMs, (std::max)(w.renderCallMaxMs, w.otherCallMaxMs)};
}

// kEval: every call counted, none clocked but the probe branches. The words say
// so; a bare "eval 0.00" would read as measured.
inline void appendEvalNote(char* buf, size_t cap, size_t& len, uint64_t calls, double frames, double msPerFrame) noexcept {
    if (!calls) {
        appendf(buf, cap, len, "eval -");
        return;
    }
    char perFrame[24];
    callsText(perFrame, sizeof(perFrame), static_cast<double>(calls) / frames);
    if (msPerFrame > 0.0)
        appendf(buf, cap, len, "eval %s calls per frame, not clocked (a pass-through; probe work clocked %.2f ms per frame)",
                perFrame, msPerFrame);
    else
        appendf(buf, cap, len, "eval %s calls per frame, not clocked (a pass-through)", perFrame);
}

inline size_t formatRenderParts(char* buf, size_t cap, const WindowReport& r) noexcept {
    size_t len = 0;
    if (!buf || !cap) return 0;
    buf[0] = 0;
    const double frames = r.frames ? static_cast<double>(r.frames) : 1.0;
    appendf(buf, cap, len,
            "engine motion CPU, render thread, ms per frame p50/p95/max (calls per frame; longest call ms), "
            "\"-\" = the code never ran on this thread in the window: ");
    for (unsigned i = 0; i < kParts; ++i) {
        const unsigned p = kOrder[i];
        const PartWindow& w = r.part[p];
        if (i) appendf(buf, cap, len, ", ");
        if (p == kEval) {
            appendEvalNote(buf, cap, len, w.renderCalls, frames, w.renderMs / frames);
        } else if (!w.renderCalls) {
            appendf(buf, cap, len, "%s -", kInfo[p].name);
        } else {
            char perFrame[24];
            callsText(perFrame, sizeof(perFrame), static_cast<double>(w.renderCalls) / frames);
            appendf(buf, cap, len, "%s %.2f/%.2f/%.2f (%s; %.2f)", kInfo[p].name, w.renderP50, w.renderP95, w.renderMax,
                    perFrame, w.renderCallMaxMs);
        }
    }
    const PatchTotals pt = patchTotals(r);
    if (pt.count)
        appendf(buf, cap, len, "; shader patches this window on every thread: %llu, total %.2f ms, longest %.2f ms.",
                static_cast<unsigned long long>(pt.count), pt.totalMs, pt.longestMs);
    else
        appendf(buf, cap, len, "; shader patches this window: none (no cache miss).");
    return len;
}

inline size_t formatOtherParts(char* buf, size_t cap, const WindowReport& r) noexcept {
    size_t len = 0;
    if (!buf || !cap) return 0;
    buf[0] = 0;
    const double frames = r.frames ? static_cast<double>(r.frames) : 1.0;
    const double seconds = r.seconds > 0.0 ? r.seconds : 1.0;
    appendf(buf, cap, len,
            "engine motion CPU, other threads (%u), ms per s (calls per frame; longest call ms), \"-\" = the code "
            "never ran off the render thread in the window: ",
            r.otherThreads);
    for (unsigned i = 0; i < kParts; ++i) {
        const unsigned p = kOrder[i];
        const PartWindow& w = r.part[p];
        if (i) appendf(buf, cap, len, ", ");
        if (p == kEval) {
            appendEvalNote(buf, cap, len, w.otherCalls, frames, w.otherMs / frames);
        } else if (!w.otherCalls) {
            appendf(buf, cap, len, "%s -", kInfo[p].name);
        } else {
            char perFrame[24];
            callsText(perFrame, sizeof(perFrame), static_cast<double>(w.otherCalls) / frames);
            appendf(buf, cap, len, "%s %.2f (%s; %.2f)", kInfo[p].name, w.otherMs / seconds, perFrame, w.otherCallMaxMs);
        }
    }
    appendf(buf, cap, len, ".");
    return len;
}

}  // namespace emcpu
}  // namespace edvr

#include "shared_pair.h"

#include <cstdio>

#include "../common/log.h"

namespace edvr {
namespace {

PairTracker g_tracker;

constexpr uint64_t kWindowMs = 30000;

// The references the stencil guard turned away, with how often: a guard that is too strict shows its references
// here (a HUD that leaves 5 or 8 where the rule wants 4). Six distinct values are listed; any further ones are
// counted together.
struct RefCount {
    uint32_t ref = 0;
    uint64_t n = 0;
};
constexpr int kRefSlots = 6;

// One window's counts. min/max are the distances to the nearest mark that made each call (radar family for a
// radar draw, console draw for a pad draw): the evidence the next capture reads the rule's margins from.
struct Window {
    uint64_t startMs = 0;
    bool active = false;             // the rule ran in this window
    uint64_t counts[3] = {};         // radar, pad, world
    uint32_t radarBackMin = 0, radarBackMax = 0;
    uint32_t consoleBackMin = 0, consoleBackMax = 0;
    uint64_t worldWithMarksElsewhere = 0;   // a world call in a frame whose HUD marks sat on another target
    // A NEAR MISS: a world call with a mark of its own on its own target that reached too far back to count. A
    // pad display whose rings stay in the game's frame because the HUD grew past the window reads here.
    uint64_t worldNearMiss = 0;
    uint32_t nearMissMin = 0, nearMissMax = 0;
    // THE STENCIL GUARD: draws the marks called radar or pad that the reference sent to world (a reference that
    // could not be read counts here too, as kPairStencilUnknown).
    uint64_t stencilDenied = 0;
    uint64_t deniedRadar = 0, deniedPad = 0;
    RefCount refs[kRefSlots] = {};
    int refsUsed = 0;
    uint64_t refsOther = 0;          // denied draws whose reference did not fit the table
};
Window g_win;
bool g_firstNoted[3] = {};
bool g_firstDenied = false;

size_t indexOf(PairClass c) noexcept { return c == PairClass::kRadar ? 0u : c == PairClass::kPad ? 1u : 2u; }

void widen(uint32_t& lo, uint32_t& hi, uint32_t v) noexcept {
    if (v == 0) return;
    if (lo == 0 || v < lo) lo = v;
    if (v > hi) hi = v;
}

// The stencil reference as the log words it: the number, or that it could not be read.
void refWord(char* out, size_t n, uint32_t ref) {
    if (ref == kPairStencilUnknown) std::snprintf(out, n, "could not be read");
    else std::snprintf(out, n, "is %u", ref);
}

void countDenied(const PairEvidence& ev) noexcept {
    ++g_win.stencilDenied;
    ++(ev.byMarks == PairClass::kRadar ? g_win.deniedRadar : g_win.deniedPad);
    for (int i = 0; i < g_win.refsUsed; ++i) {
        if (g_win.refs[i].ref == ev.stencilRef) {
            ++g_win.refs[i].n;
            return;
        }
    }
    if (g_win.refsUsed < kRefSlots) {
        g_win.refs[g_win.refsUsed].ref = ev.stencilRef;
        g_win.refs[g_win.refsUsed].n = 1;
        ++g_win.refsUsed;
    } else {
        ++g_win.refsOther;
    }
}

// "0 x 3, 12 x 1, unreadable x 2, other x 1", or "-" when the guard turned nothing away.
void refsText(char* out, size_t n) {
    size_t at = 0;
    out[0] = '\0';
    auto put = [&](const char* what, unsigned long long count) {
        if (at + 1 >= n) return;
        const int w = std::snprintf(out + at, n - at, "%s%s x %llu", at ? ", " : "", what, count);
        if (w > 0) at += static_cast<size_t>(w) < n - at ? static_cast<size_t>(w) : n - at - 1;
    };
    for (int i = 0; i < g_win.refsUsed; ++i) {
        char label[16];
        if (g_win.refs[i].ref == kPairStencilUnknown) std::snprintf(label, sizeof(label), "unreadable");
        else std::snprintf(label, sizeof(label), "%u", g_win.refs[i].ref);
        put(label, static_cast<unsigned long long>(g_win.refs[i].n));
    }
    if (g_win.refsOther) put("other", static_cast<unsigned long long>(g_win.refsOther));
    if (at == 0) std::snprintf(out, n, "-");
}

void noteFirst(PairClass c, uint32_t instances, const PairEvidence& ev) {
    // A draw the guard sent to world is named by the guard's own note; the world class's note waits for a world
    // draw of its own, so it never says "no radar family within" of a draw that had one.
    if (c == PairClass::kWorld && pairStencilDenied(ev)) return;
    const size_t i = indexOf(c);
    if (g_firstNoted[i]) return;
    g_firstNoted[i] = true;
    switch (c) {
        case PairClass::kRadar:
            Log::get().note("shared pair: the first draw of the radar class: %u instances, a radar family drew %u "
                            "draws before it on its target, stencil reference %u.", instances, ev.radarBack,
                            ev.stencilRef);
            break;
        case PairClass::kPad:
            Log::get().note("shared pair: the first draw of the pad class: %u instances, a console draw %u draws "
                            "before it on its target and no radar family within %u, stencil reference %u.",
                            instances, ev.consoleBack, kPairRadarWindow, ev.stencilRef);
            break;
        default: {
            char nearMiss[96] = "";
            if (pairNearMiss(ev))
                std::snprintf(nearMiss, sizeof(nearMiss), "; the nearest mark on its own target is %u draws back, past the window",
                              pairNearestMark(ev));
            char ref[32];
            refWord(ref, sizeof(ref), ev.stencilRef);
            Log::get().note("shared pair: the first draw of the world class: %u instances, no radar family within %u "
                            "and no console draw within %u draws before it on its target%s%s; its stencil reference %s.",
                            instances, kPairRadarWindow, kPairPadWindow,
                            ev.marksElsewhere ? "; the frame's HUD marks are on another target" : "", nearMiss, ref);
            break;
        }
    }
}

// The first draw the guard sent to world, once a session: the marks said radar or pad, the reference disagreed.
void noteFirstDenied(uint32_t instances, const PairEvidence& ev) {
    if (g_firstDenied) return;
    g_firstDenied = true;
    char ref[32];
    refWord(ref, sizeof(ref), ev.stencilRef);
    const bool radar = ev.byMarks == PairClass::kRadar;
    Log::get().note("shared pair: the first draw the stencil guard sent to world: %u instances, the marks said %s (%s "
                    "%u draws back on its target) but its stencil reference %s, and the rule needs %u.",
                    instances, pairClassName(ev.byMarks), radar ? "a radar family" : "a console draw",
                    radar ? ev.radarBack : ev.consoleBack, ref, kPairStencilRef);
}

void range(char* out, size_t n, uint32_t lo, uint32_t hi) {
    if (lo == 0) std::snprintf(out, n, "-");
    else if (lo == hi) std::snprintf(out, n, "%u", lo);
    else std::snprintf(out, n, "%u-%u", lo, hi);
}

void logAndResetWindow(uint64_t nowMs) {
    char radar[24], console[24], miss[24], refs[160];
    range(radar, sizeof(radar), g_win.radarBackMin, g_win.radarBackMax);
    range(console, sizeof(console), g_win.consoleBackMin, g_win.consoleBackMax);
    range(miss, sizeof(miss), g_win.nearMissMin, g_win.nearMissMax);
    refsText(refs, sizeof(refs));
    Log::get().note(
        "shared pair: radar %llu, pad %llu, world %llu (%.0f s; the draws of VS %016llX / PS %016llX by the class "
        "the rule gave each). Nearest mark before the draws it called: radar family %s draws back, console draw %s; "
        "%llu world draws in a frame whose HUD marks were on another target; %llu world draws with a mark on their "
        "own target past the window (%s draws back); stencil guard (the reference must be %u): %llu draws the marks "
        "called radar (%llu) or pad (%llu) went to world, references seen: %s.",
        static_cast<unsigned long long>(g_win.counts[0]), static_cast<unsigned long long>(g_win.counts[1]),
        static_cast<unsigned long long>(g_win.counts[2]),
        static_cast<double>(nowMs - g_win.startMs) / 1000.0, static_cast<unsigned long long>(kSharedPairVs),
        static_cast<unsigned long long>(kSharedPairPs), radar, console,
        static_cast<unsigned long long>(g_win.worldWithMarksElsewhere),
        static_cast<unsigned long long>(g_win.worldNearMiss), miss, kPairStencilRef,
        static_cast<unsigned long long>(g_win.stencilDenied), static_cast<unsigned long long>(g_win.deniedRadar),
        static_cast<unsigned long long>(g_win.deniedPad), refs);
    g_win = Window{};
    g_win.startMs = nowMs;
}

}  // namespace

void sharedPairNoteEyeDraw(uint32_t frame, const void* target, uint32_t ordinal, uint64_t vs) noexcept {
    g_tracker.note(frame, target, ordinal, vs);
}

PairClass sharedPairClassify(uint32_t frame, const void* target, uint32_t ordinal, uint64_t vs, uint64_t ps,
                             uint32_t instances, uint32_t stencilRef) noexcept {
    if (!isSharedPair(vs, ps)) return PairClass::kNotPair;
    PairEvidence ev;
    const PairClass c = g_tracker.classify(frame, target, ordinal, stencilRef, &ev);
    ++g_win.counts[indexOf(c)];
    if (c == PairClass::kRadar) widen(g_win.radarBackMin, g_win.radarBackMax, ev.radarBack);
    else if (c == PairClass::kPad) widen(g_win.consoleBackMin, g_win.consoleBackMax, ev.consoleBack);
    else {
        if (ev.marksElsewhere) ++g_win.worldWithMarksElsewhere;
        if (pairStencilDenied(ev)) {
            countDenied(ev);
            noteFirstDenied(instances, ev);
        } else if (pairNearMiss(ev)) {
            ++g_win.worldNearMiss;
            widen(g_win.nearMissMin, g_win.nearMissMax, pairNearestMark(ev));
        }
    }
    noteFirst(c, instances, ev);
    return c;
}

void sharedPairFrameBoundary(uint64_t nowMs, bool active) noexcept {
    if (g_win.startMs == 0) g_win.startMs = nowMs;
    g_win.active = g_win.active || active;
    if (nowMs - g_win.startMs < kWindowMs) return;
    if (g_win.active) {
        logAndResetWindow(nowMs);
    } else {
        g_win = Window{};
        g_win.startMs = nowMs;
    }
}

void sharedPairShutdown() noexcept {
    g_tracker.reset();
    g_win = Window{};
    for (bool& b : g_firstNoted) b = false;
    g_firstDenied = false;
}

}  // namespace edvr

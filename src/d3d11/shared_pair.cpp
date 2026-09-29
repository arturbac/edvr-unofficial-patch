#include "shared_pair.h"

#include <cstdio>

#include "../common/log.h"

namespace edvr {
namespace {

PairTracker g_tracker;

constexpr uint64_t kWindowMs = 30000;

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
};
Window g_win;
bool g_firstNoted[3] = {};

size_t indexOf(PairClass c) noexcept { return c == PairClass::kRadar ? 0u : c == PairClass::kPad ? 1u : 2u; }

void widen(uint32_t& lo, uint32_t& hi, uint32_t v) noexcept {
    if (v == 0) return;
    if (lo == 0 || v < lo) lo = v;
    if (v > hi) hi = v;
}

void noteFirst(PairClass c, uint32_t instances, const PairEvidence& ev) {
    const size_t i = indexOf(c);
    if (g_firstNoted[i]) return;
    g_firstNoted[i] = true;
    switch (c) {
        case PairClass::kRadar:
            Log::get().note("shared pair: the first draw of the radar class: %u instances, a radar family drew %u "
                            "draws before it on its target.", instances, ev.radarBack);
            break;
        case PairClass::kPad:
            Log::get().note("shared pair: the first draw of the pad class: %u instances, a console draw %u draws "
                            "before it on its target and no radar family within %u.", instances, ev.consoleBack,
                            kPairRadarWindow);
            break;
        default: {
            char nearMiss[96] = "";
            if (pairNearestMark(ev) != 0)
                std::snprintf(nearMiss, sizeof(nearMiss), "; the nearest mark on its own target is %u draws back, past the window",
                              pairNearestMark(ev));
            Log::get().note("shared pair: the first draw of the world class: %u instances, no radar family within %u "
                            "and no console draw within %u draws before it on its target%s%s.", instances,
                            kPairRadarWindow, kPairPadWindow,
                            ev.marksElsewhere ? "; the frame's HUD marks are on another target" : "", nearMiss);
            break;
        }
    }
}

void range(char* out, size_t n, uint32_t lo, uint32_t hi) {
    if (lo == 0) std::snprintf(out, n, "-");
    else if (lo == hi) std::snprintf(out, n, "%u", lo);
    else std::snprintf(out, n, "%u-%u", lo, hi);
}

void logAndResetWindow(uint64_t nowMs) {
    char radar[24], console[24], miss[24];
    range(radar, sizeof(radar), g_win.radarBackMin, g_win.radarBackMax);
    range(console, sizeof(console), g_win.consoleBackMin, g_win.consoleBackMax);
    range(miss, sizeof(miss), g_win.nearMissMin, g_win.nearMissMax);
    Log::get().note(
        "shared pair: radar %llu, pad %llu, world %llu (%.0f s; the draws of VS %016llX / PS %016llX by the class "
        "the rule gave each). Nearest mark before the draws it called: radar family %s draws back, console draw %s; "
        "%llu world draws in a frame whose HUD marks were on another target; %llu world draws with a mark on their "
        "own target past the window (%s draws back).",
        static_cast<unsigned long long>(g_win.counts[0]), static_cast<unsigned long long>(g_win.counts[1]),
        static_cast<unsigned long long>(g_win.counts[2]),
        static_cast<double>(nowMs - g_win.startMs) / 1000.0, static_cast<unsigned long long>(kSharedPairVs),
        static_cast<unsigned long long>(kSharedPairPs), radar, console,
        static_cast<unsigned long long>(g_win.worldWithMarksElsewhere),
        static_cast<unsigned long long>(g_win.worldNearMiss), miss);
    g_win = Window{};
    g_win.startMs = nowMs;
}

}  // namespace

void sharedPairNoteEyeDraw(uint32_t frame, const void* target, uint32_t ordinal, uint64_t vs) noexcept {
    g_tracker.note(frame, target, ordinal, vs);
}

PairClass sharedPairClassify(uint32_t frame, const void* target, uint32_t ordinal, uint64_t vs, uint64_t ps,
                             uint32_t instances) noexcept {
    if (!isSharedPair(vs, ps)) return PairClass::kNotPair;
    PairEvidence ev;
    const PairClass c = g_tracker.classify(frame, target, ordinal, &ev);
    ++g_win.counts[indexOf(c)];
    if (c == PairClass::kRadar) widen(g_win.radarBackMin, g_win.radarBackMax, ev.radarBack);
    else if (c == PairClass::kPad) widen(g_win.consoleBackMin, g_win.consoleBackMax, ev.consoleBack);
    else {
        if (ev.marksElsewhere) ++g_win.worldWithMarksElsewhere;
        if (pairNearestMark(ev) != 0) {
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
}

}  // namespace edvr

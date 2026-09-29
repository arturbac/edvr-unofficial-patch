// The shared pair: one vertex/pixel shader pair that Elite uses for three different things, and the
// rule that tells them apart.
//
//   VS 94D5C556DFD6D705 / PS 912477AEF6958379: an instanced screen-space sprite stamp (six vertices an
//   instance, per-instance records in a vertex buffer, two 2048x1024 BC7 atlas sheets at PS t0/t1). It draws
//     - the radar's contact markers (holo_families.h, kHoloContactE: the last of the radar's families),
//     - the landing-pad display's rings and crosshair, which Elite shows in the radar's place while docking
//       (two draws an eye: about 3400 instances, then about 95), and
//     - the sun's glare train (docs/sun-glare.md: one draw an eye, about 15 instances, more when several
//       bodies contribute).
//   Nothing in the draw's own shape or state tells them apart: all three are `N n=6 i>=2` with the same two
//   BC7 sheets, the same blend (source alpha, one, add) and the same depth test. The glare fix claims by that
//   shape alone (sunglare_fix.h), so it took the radar's and the pad's draws for a sun, and the UI layer took
//   the radar's by the vertex-shader hash alone (a family of holo_families.h's take list), so in stock it took a
//   real glare train as well.
//
// THE RULE (Sean approved it 2026-09-29, docs/openxr-performance-review-2026-09-14.md): a draw of the pair is
//   RADAR if a radar-only family drew within 250 draws before it on the same target,
//   else PAD  if a console draw did within 400,
//   else WORLD;
// and a RADAR or PAD call STANDS only if the draw's stencil reference is 4 (the stencil guard, below). Any other
// reference, or one that could not be read, makes the draw WORLD: the game's frame keeps it, the glare fix judges
// it by its shape as before, the layer leaves it.
// "Draws" are the eye draws of a frame counted through one counter (vscreen's eyeDrawsThisFrame, which is the
// draw census's #N), "the same target" is the bound colour target view, and only draws BEFORE it count: the
// families are drawn in a fixed order and the pair is drawn after them.
//
// THE STENCIL GUARD (Sean approved it 2026-09-29). The reference is the context's, read the way the census reads
// its `st=` column (OMGetDepthStencilState's second result): the census prints the stencil TEST's enable flag and
// then the reference, and the pair's own state has the test OFF (`st=04`: off, reference 4), so the 4 is state the
// HUD section's earlier stencil draws left behind. It says "drawn inside the HUD section's state" and nothing
// finer. It was 4 in all 427 recorded radar and pad draws and 0 in all 10 recorded WORLD draws, so it is the
// fail-safe half of the rule: the marks can call RADAR or PAD only when the reference agrees. It is NOT a proof
// about a glare train in a cockpit frame: a train drawn inside the HUD section's state would carry 4 too, and
// what state the scene pass leaves there has not been measured. The 30 s line counts the draws the guard sent to
// WORLD, with the references seen: a count above 0 with the pad up means the guard is too strict.
//
// WHAT THE CAPTURES SAY (`python tools\pair_class_scan.py --target frontier --all`, and steam: 1,349 graphics
// logs, both installs, to 2026-09-29; the game build of 2026-09-02 and later unless said):
//   RADAR  411 draws with a recorded reference, 4 in every one; the nearest radar-family draw before it is 2 to
//          6 draws back, always (so 250 is generous for the radar; 16 would do). 58 more, in logs from before
//          2026-09-02, whose census has no st= column: no reference recorded.
//   PAD     16 draws, in two logs (flight 132352 and capture 143837), reference 4 in every one: the landing-pad
//          display's own two draws an eye, 3226-3398 instances then 94-182, the nearest console draw 191 to 236
//          back. The pad window is 400 (Sean, 2026-09-29): 1.7x the farthest, where 250 left 14 draws. A busier
//          HUD past it would read WORLD (the rings stay in the game's frame) and the 30 s line counts it as a
//          near miss ("world draws with a mark on their own target past the window").
//   WORLD   18 draws in four logs (a main menu, a loading screen, the 08-28 glare work), every one from a frame
//          with NO radar or console draw anywhere in it, no near miss among them; the 10 that record a
//          reference read 0. So far WORLD only proves that a frame without the HUD's draws is called WORLD.
//   Logs before 2026-09-02 hold 72 more PAD-class calls of 15-98 instances, the console 15-39 draws back on the
//          SAME target, no reference recorded. They are not the pad display: all eight are sessions parked at a
//          star (glare VIVID, the world shader compiled), the instance counts are a glare train's, and there the
//          HUD's console draws shared the train's target, so the marks read a sun as PAD. That is what the
//          marks alone do when the HUD does not have its own target, and no dump exists to confirm it. In the
//          current build the HUD section draws into targets of its own, so the same train would sit on another
//          target than the marks; the guard is a second line for it, unmeasured.
// The console draws (VS 41E245D488BFE83E and 68DDDEF04D9894AF) are ALSO drawn in a plain cockpit frame with the
// radar up, which is why the radar test comes first. The cockpit's HUD section is drawn into its own colour
// target (a capture: scene draws into one target, the holograms into another, one depth buffer for both), and
// the marks are per target, so a glare train drawn in the scene target is WORLD by construction. That is an
// expectation, not a measurement: no capture of the current build holds a glare train in a cockpit frame with
// the radar up.
//
// Pure: no D3D, no globals. shared_pair.cpp keeps the per-frame state, the counts and the 30 s line; vscreen.cpp
// reads the reference (pairReadStencilRef) and feeds the marks; tools\shared_pair_test holds the rule against
// sequences recorded from the census, and tools\pair_class_scan.py runs the same rule over any log.
#pragma once

#include <cstdint>

#include "holo_families.h"

namespace edvr {

constexpr uint64_t kSharedPairVs = kHoloContactE;                  // 94D5C556DFD6D705
constexpr uint64_t kSharedPairPs = 0x912477AEF6958379ull;
// The console's own draws, present in the pad display's frames and in ordinary cockpit frames alike.
constexpr uint64_t kPairConsoleVsA = 0x41E245D488BFE83Eull;
constexpr uint64_t kPairConsoleVsB = 0x68DDDEF04D9894AFull;
// How far back a mark reaches, in eye draws on the same target. The radar's marks are 2-6 draws back (250 is
// generous); the pad's console draw is 191-236 back, and 400 is Sean's window for it (2026-09-29). The rig pins
// both numbers on purpose: a change is a decision, and the rig is where it announces itself.
constexpr uint32_t kPairRadarWindow = 250;
constexpr uint32_t kPairPadWindow = 400;
// The stencil reference a RADAR or PAD draw must carry, and what the draw path passes when it could not read one
// (a game that sets 0xFFFFFFFF itself reads as unreadable too: both are WORLD).
constexpr uint32_t kPairStencilRef = 4;
constexpr uint32_t kPairStencilUnknown = 0xFFFFFFFFu;

// Which marks a draw sets. The radar-only list is the radar's families without the pair's own vertex shader:
// the icon core, its two stalks, the contact markers A to D (holo_families.h). The corona family is not on it
// (the radar's glow and the real sun's corona share it).
enum class PairMark : uint8_t { kNone = 0, kRadar, kConsole };

// The result of the rule for one draw. kNotPair: not this shader pair at all.
enum class PairClass : uint8_t { kNotPair = 0, kRadar, kPad, kWorld };

inline const char* pairClassName(PairClass c) noexcept {
    switch (c) {
        case PairClass::kRadar: return "radar";
        case PairClass::kPad: return "pad";
        case PairClass::kWorld: return "world";
        default: return "not the pair";
    }
}

namespace pair_detail {
// One slot for each mark hash: the low five bits of the hash above bit 7 are all different across the nine.
constexpr uint32_t pairSlot(uint64_t h) { return static_cast<uint32_t>((h >> 7) & 31u); }
struct MarkTable {
    uint64_t hash[32] = {};
    PairMark mark[32] = {};
    bool collided = false;
    constexpr void put(uint64_t h, PairMark m) {
        const uint32_t s = pairSlot(h);
        if (hash[s] != 0) collided = true;
        hash[s] = h;
        mark[s] = m;
    }
    constexpr MarkTable() {
        put(kHoloIconCore, PairMark::kRadar);
        put(kHoloIconStalkA, PairMark::kRadar);
        put(kHoloIconStalkB, PairMark::kRadar);
        put(kHoloContactA, PairMark::kRadar);
        put(kHoloContactB, PairMark::kRadar);
        put(kHoloContactC, PairMark::kRadar);
        put(kHoloContactD, PairMark::kRadar);
        put(kPairConsoleVsA, PairMark::kConsole);
        put(kPairConsoleVsB, PairMark::kConsole);
    }
};
inline constexpr MarkTable kMarks{};
static_assert(!kMarks.collided, "the nine mark hashes must sit in nine slots");
}  // namespace pair_detail

// One table lookup and a compare: called for every eye draw while the rule is wanted.
inline PairMark pairMarkOf(uint64_t vs) noexcept {
    const uint32_t s = pair_detail::pairSlot(vs);
    return pair_detail::kMarks.hash[s] == vs ? pair_detail::kMarks.mark[s] : PairMark::kNone;
}

inline bool isSharedPair(uint64_t vs, uint64_t ps) noexcept { return vs == kSharedPairVs && ps == kSharedPairPs; }

// The marks' verdict alone, on two ordinals: the last radar-family draw and the last console draw on this draw's
// target before it (0: none). Ordinals count from 1. The stencil guard is applied on top (pairGuardStencil).
inline PairClass pairMarksClassFor(uint32_t ordinal, uint32_t lastRadar, uint32_t lastConsole) noexcept {
    if (lastRadar != 0 && lastRadar < ordinal && ordinal - lastRadar <= kPairRadarWindow) return PairClass::kRadar;
    if (lastConsole != 0 && lastConsole < ordinal && ordinal - lastConsole <= kPairPadWindow) return PairClass::kPad;
    return PairClass::kWorld;
}

// The stencil guard on the marks' verdict: RADAR and PAD stand only with the reference the HUD section leaves
// (kPairStencilRef); WORLD is already the floor, and no reference, read or not, can raise a draw above it.
inline PairClass pairGuardStencil(PairClass byMarks, uint32_t stencilRef) noexcept {
    const bool hudClass = byMarks == PairClass::kRadar || byMarks == PairClass::kPad;
    return hudClass && stencilRef != kPairStencilRef ? PairClass::kWorld : byMarks;
}

// The rule itself: the marks' verdict, then the guard. `stencilRef` is kPairStencilUnknown when the draw path
// could not read it.
inline PairClass pairClassFor(uint32_t ordinal, uint32_t lastRadar, uint32_t lastConsole, uint32_t stencilRef) noexcept {
    return pairGuardStencil(pairMarksClassFor(ordinal, lastRadar, lastConsole), stencilRef);
}

// Who may act on a class. The glare fix never sees a radar or pad draw (its claim, its skip in mode off, its
// exposure-damper sun scope: all behind this), and the UI layer takes the pair only as radar or pad, never as
// world, in stock, vivid, realistic and off alike. A draw that is not the pair (kNotPair) is neither's business
// here: the glare fix judges it by its own shape and the layer by its own family table.
constexpr bool pairGlareMayClaim(PairClass c) noexcept { return c != PairClass::kRadar && c != PairClass::kPad; }
constexpr bool pairLayerAdmits(PairClass c) noexcept { return c == PairClass::kRadar || c == PairClass::kPad; }

// What a call was made from, for the log and the scan tool. The distances are to the last mark of each kind on
// the draw's target, whether or not it is inside its window (0: no such mark before the draw), so a draw that
// just missed reads. `byMarks` is the marks' verdict before the stencil guard.
struct PairEvidence {
    uint32_t radarBack = 0;
    uint32_t consoleBack = 0;
    bool marksElsewhere = false;   // the frame has marks on another target (the HUD's own, when this draw is not)
    PairClass byMarks = PairClass::kWorld;
    uint32_t stencilRef = kPairStencilUnknown;
};

// The nearest mark of either kind on the draw's own target, 0 if there is none.
inline uint32_t pairNearestMark(const PairEvidence& ev) noexcept {
    if (ev.radarBack == 0) return ev.consoleBack;
    if (ev.consoleBack == 0) return ev.radarBack;
    return ev.radarBack < ev.consoleBack ? ev.radarBack : ev.consoleBack;
}

// A NEAR MISS: the marks said WORLD, yet a mark of the draw's own was there and reached too far back to count (a
// window that is too short for a busier HUD shows up here first, as a pad's rings staying in the game's frame).
inline bool pairNearMiss(const PairEvidence& ev) noexcept {
    return ev.byMarks == PairClass::kWorld && pairNearestMark(ev) != 0;
}

// The stencil guard changed this draw's class: the marks said RADAR or PAD and the reference disagreed (or was
// unreadable). The count of these is what says the guard is too strict.
inline bool pairStencilDenied(const PairEvidence& ev) noexcept {
    return (ev.byMarks == PairClass::kRadar || ev.byMarks == PairClass::kPad) && ev.stencilRef != kPairStencilRef;
}

// The per-frame state: the last radar and console ordinal on each target drawn into. Four targets are plenty
// (two eyes, and a target the marks were seen on for each); a fifth is not tracked and reads as no marks.
class PairTracker {
public:
    static constexpr int kTargets = 4;

    // A draw into `target` at `ordinal` (1-based, frame-wide) with this vertex shader: sets a mark if it is
    // one. `frame` is any counter that changes every frame: a new value clears the state.
    void note(uint32_t frame, const void* target, uint32_t ordinal, uint64_t vs) noexcept {
        const PairMark m = pairMarkOf(vs);
        if (m == PairMark::kNone) return;
        Slot* s = slotFor(frame, target, true);
        if (!s) return;
        (m == PairMark::kRadar ? s->radar : s->console) = ordinal;
    }

    // The class of a draw of the pair at `ordinal` into `target` whose stencil reference is `stencilRef`
    // (kPairStencilUnknown when it could not be read). There is no default for the reference on purpose: a
    // caller that forgot it would otherwise switch the guard off.
    PairClass classify(uint32_t frame, const void* target, uint32_t ordinal, uint32_t stencilRef,
                       PairEvidence* ev = nullptr) const noexcept {
        uint32_t radar = 0, console = 0;
        bool elsewhere = false;
        if (frame == frame_) {
            for (int i = 0; i < used_; ++i) {
                if (slots_[i].target == target) {
                    radar = slots_[i].radar;
                    console = slots_[i].console;
                } else if (slots_[i].radar || slots_[i].console) {
                    elsewhere = true;
                }
            }
        }
        const PairClass byMarks = pairMarksClassFor(ordinal, radar, console);
        if (ev) {
            ev->radarBack = radar && radar < ordinal ? ordinal - radar : 0;
            ev->consoleBack = console && console < ordinal ? ordinal - console : 0;
            ev->marksElsewhere = elsewhere;
            ev->byMarks = byMarks;
            ev->stencilRef = stencilRef;
        }
        return pairGuardStencil(byMarks, stencilRef);
    }

    void reset() noexcept {
        frame_ = ~0u;
        used_ = 0;
        for (Slot& s : slots_) s = Slot{};
    }

private:
    struct Slot {
        const void* target = nullptr;
        uint32_t radar = 0;
        uint32_t console = 0;
    };
    Slot* slotFor(uint32_t frame, const void* target, bool create) noexcept {
        if (frame != frame_) {
            reset();
            frame_ = frame;
        }
        for (int i = 0; i < used_; ++i)
            if (slots_[i].target == target) return &slots_[i];
        if (!create || used_ == kTargets) return nullptr;
        slots_[used_].target = target;
        return &slots_[used_++];
    }
    uint32_t frame_ = ~0u;
    int used_ = 0;
    Slot slots_[kTargets];
};

// ---------------------------------------------------------------------------------------------------------
// The runtime half (shared_pair.cpp): the one tracker the draw path feeds, the counts, and the 30 s line.

// For every eye draw while the rule is wanted: sets the marks. `ordinal` is the frame-wide eye-draw count,
// `target` the bound colour target view.
void sharedPairNoteEyeDraw(uint32_t frame, const void* target, uint32_t ordinal, uint64_t vs) noexcept;

// For a draw with this vertex and pixel shader: PairClass::kNotPair unless they are the pair's, else the
// rule's answer, counted for the 30 s line and, the first time for each class, named in the log. `stencilRef`
// is the draw's stencil reference, kPairStencilUnknown when the draw path could not read it (WORLD, counted).
PairClass sharedPairClassify(uint32_t frame, const void* target, uint32_t ordinal, uint64_t vs, uint64_t ps,
                             uint32_t instances, uint32_t stencilRef) noexcept;

// Once a frame from vScreenFrameBoundary. `active` says whether the rule ran this frame; a window it never ran
// in prints no line, so its absence means "not running" and a line of zeros means "ran, saw no pair draw".
// Every 30 s it writes: shared pair: radar N, pad N, world N ...
void sharedPairFrameBoundary(uint64_t nowMs, bool active) noexcept;

void sharedPairShutdown() noexcept;

}  // namespace edvr

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
//   else PAD  if a console draw did,
//   else WORLD.
// "Draws" are the eye draws of a frame counted through one counter (vscreen's eyeDrawsThisFrame, which is the
// draw census's #N), "the same target" is the bound colour target view, and only draws BEFORE it count: the
// families are drawn in a fixed order and the pair is drawn after them.
//
// WHAT THE CAPTURES SAY (`python tools\pair_class_scan.py --target frontier --all`, and steam: 1,349 graphics
// logs, both installs, to 2026-09-29; the game build of 2026-09-02 and later unless said):
//   RADAR  469 draws: the nearest radar-family draw before it is 2 to 6 draws back, always (so 250 is generous
//          for the radar; 16 would do).
//   PAD     16 draws, in two logs (flight 132352 and capture 143837): the landing-pad display's own two draws an
//          eye, 3226-3398 instances then 94-182, the nearest console draw 191 to 236 back. THE MARGIN IS 14
//          DRAWS: a busier HUD between the console draws and the pad would put it past the window, and the draw
//          would read WORLD (the pad's rings then stay in the game's frame, as today, and the 30 s line counts
//          it as a near miss: "world draws with a mark on their own target past the window"). A window of 400
//          would leave a margin of 1.7x; the approved rule says 250, so it is 250, and kPairPadWindow is the one
//          number to change (the rig pins both windows at 250 in its rule cases, in
//          constructed_window_edge.txt and in its reference, so a change is a deliberate edit there too).
//   WORLD   18 draws in four logs (a main menu, a loading screen, the 08-28 glare work), every one from a frame
//          with NO radar or console draw anywhere in it, no near miss among them: so far WORLD only proves that
//          a frame without the HUD's draws is called WORLD.
//   Logs before 2026-09-02 (another game build: the radar's families have other vertex shaders there, so no
//   radar mark is ever set) hold 72 more PAD-class calls of 15-98 instances, the console 15-39 draws back:
//   those are not the pad display.
// The console draws (VS 41E245D488BFE83E and 68DDDEF04D9894AF) are ALSO drawn in a plain cockpit frame with the
// radar up, which is why the radar test comes first. The cockpit's HUD section is drawn into its own colour
// target (a capture: scene draws into one target, the holograms into another, one depth buffer for both), and
// the marks are per target, so a glare train drawn in the scene target is WORLD by construction. That is an
// expectation, not a measurement: no capture yet holds a glare train in a cockpit frame with the radar up.
//
// Pure: no D3D, no globals. shared_pair.cpp keeps the per-frame state, the counts and the 30 s line;
// tools\shared_pair_test holds the rule against sequences recorded from the census, and
// tools\pair_class_scan.py runs the same rule over any log.
#pragma once

#include <cstdint>

#include "holo_families.h"

namespace edvr {

constexpr uint64_t kSharedPairVs = kHoloContactE;                  // 94D5C556DFD6D705
constexpr uint64_t kSharedPairPs = 0x912477AEF6958379ull;
// The console's own draws, present in the pad display's frames and in ordinary cockpit frames alike.
constexpr uint64_t kPairConsoleVsA = 0x41E245D488BFE83Eull;
constexpr uint64_t kPairConsoleVsB = 0x68DDDEF04D9894AFull;
// How far back a mark reaches, in eye draws on the same target: the radar's and the console's own numbers (see
// the pad's margin above). 250 each, as approved.
constexpr uint32_t kPairRadarWindow = 250;
constexpr uint32_t kPairPadWindow = 250;

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

// The rule itself, on two ordinals: the last radar-family draw and the last console draw on this draw's target
// before it (0: none). Ordinals count from 1.
inline PairClass pairClassFor(uint32_t ordinal, uint32_t lastRadar, uint32_t lastConsole) noexcept {
    if (lastRadar != 0 && lastRadar < ordinal && ordinal - lastRadar <= kPairRadarWindow) return PairClass::kRadar;
    if (lastConsole != 0 && lastConsole < ordinal && ordinal - lastConsole <= kPairPadWindow) return PairClass::kPad;
    return PairClass::kWorld;
}

// Who may act on a class. The glare fix never sees a radar or pad draw (its claim, its skip in mode off, its
// exposure-damper sun scope: all behind this), and the UI layer takes the pair only as radar or pad, never as
// world, in stock, vivid, realistic and off alike. A draw that is not the pair (kNotPair) is neither's business
// here: the glare fix judges it by its own shape and the layer by its own family table.
constexpr bool pairGlareMayClaim(PairClass c) noexcept { return c != PairClass::kRadar && c != PairClass::kPad; }
constexpr bool pairLayerAdmits(PairClass c) noexcept { return c == PairClass::kRadar || c == PairClass::kPad; }

// How far back the last mark of each kind on the draw's target was, for the log and the scan tool: the distance
// whether or not it is inside the window (0: no such mark before the draw), so a draw that just missed reads.
struct PairEvidence {
    uint32_t radarBack = 0;
    uint32_t consoleBack = 0;
    bool marksElsewhere = false;   // the frame has marks on another target (the HUD's own, when this draw is not)
};

// The nearest mark of either kind on the draw's own target, 0 if there is none. For a WORLD call it is a NEAR
// MISS when nonzero: a mark was there and reached too far back to count (a window that is too short for a
// busier HUD shows up here first, as a pad's rings staying in the game's frame).
inline uint32_t pairNearestMark(const PairEvidence& ev) noexcept {
    if (ev.radarBack == 0) return ev.consoleBack;
    if (ev.consoleBack == 0) return ev.radarBack;
    return ev.radarBack < ev.consoleBack ? ev.radarBack : ev.consoleBack;
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

    // The class of a draw of the pair at `ordinal` into `target`.
    PairClass classify(uint32_t frame, const void* target, uint32_t ordinal, PairEvidence* ev = nullptr) const noexcept {
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
        if (ev) {
            ev->radarBack = radar && radar < ordinal ? ordinal - radar : 0;
            ev->consoleBack = console && console < ordinal ? ordinal - console : 0;
            ev->marksElsewhere = elsewhere;
        }
        return pairClassFor(ordinal, radar, console);
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
// rule's answer, counted for the 30 s line and, the first time for each class, named in the log.
PairClass sharedPairClassify(uint32_t frame, const void* target, uint32_t ordinal, uint64_t vs, uint64_t ps,
                             uint32_t instances) noexcept;

// Once a frame from vScreenFrameBoundary. `active` says whether the rule ran this frame; a window it never ran
// in prints no line, so its absence means "not running" and a line of zeros means "ran, saw no pair draw".
// Every 30 s it writes: shared pair: radar N, pad N, world N ...
void sharedPairFrameBoundary(uint64_t nowMs, bool active) noexcept;

void sharedPairShutdown() noexcept;

}  // namespace edvr

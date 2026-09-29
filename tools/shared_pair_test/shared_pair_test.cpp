// The shared pair's rule (src/d3d11/shared_pair.h) and every place that acts on it.
//
// VS 94D5C556DFD6D705 / PS 912477AEF6958379 draws the radar's contact markers, the landing-pad display's rings
// and the sun's glare train. The rule tells them apart by what was drawn before them on their target, and lets
// RADAR or PAD stand only with stencil reference 4 (the stencil guard); the glare fix never claims a radar or
// pad draw, and the UI layer takes the pair only as radar or pad.
//
// What is held here:
//   1. the mark table, against the take list it is cut from;
//   2. the rule's arithmetic, at its edges (radar 250, pad 400), and the stencil guard: reference 4 or world;
//   3. the per-frame tracker: targets, order, frame reset, capacity, evidence;
//   4. SEQUENCES RECORDED FROM THE DRAW CENSUS (tools\shared_pair_test\fixtures, reduced by
//      tools\pair_class_scan.py): the production tracker, run over each frame's draws in order with the
//      stencil reference the census recorded for each (st=), must give each draw of the pair the class its
//      capture was judged to be (the eye dump named the display: the pad in the hangar, the pad on the
//      approach, the radar in space) -- and tools\pair_class_scan.py, a second implementation of the rule,
//      holds the same fixtures in its own --self-test;
//   5. NEGATIVE CONTROLS: an independent reference implementation of the rule, mutated one way at a time
//      (no radar flag, no console flag, marks on any target, marks after the draw, no window, an unbounded
//      window, the console first, the pad window back at 250 or at 230 -- the recorded pad is 236 draws back --
//      each window one draw short or one draw long, the stencil guard dropped, wanting reference 0, taking any
//      nonzero reference, or letting an unreadable one through); each mutant must disagree with the recordings
//      somewhere, so the recordings are known to be sensitive to every part of the rule, and the controls the
//      approval named are each broken by their own mutant: a pad draw 300 back (pad window at 250), one 401
//      back (a window of 401), a pad-shaped draw with reference 0 and a glare-train draw with reference 0
//      inside the radar window (the guard dropped), an unreadable reference (let through);
//   6. the gates: which classes the glare fix may claim and the layer take, uiLayerFamilyFor with the pair's
//      class, and uiLayerDecide with the pair and a glare verdict, in stock, vivid, realistic and off -- a
//      radar or pad draw must never reach kVerdict ("another fix swallows it"), and the control shows what the
//      old claim did;
//   7. the runtime half (shared_pair.cpp): the counts, the first-seen notes and the 30 s line, and what it
//      prints when the rule never ran;
//   8. the wiring, by scan of src\d3d11\vscreen.cpp and ui_layer_math.h, with mutants of it: the tracker fed
//      from every eye draw, the classification before the glare claim, the claim guarded, the layer's fact
//      read, the gate subscribed, the boundary ticked.
//
// Whitebox, tools\gpu_census_test's convention: shared_pair.cpp is included directly, with a Log that keeps
// its lines. Runs from the repository root (build.bat does), where the fixtures and sources are.
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/common/log.h"

namespace edvr {
std::vector<std::string> g_lines;
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_lines.push_back(buf);
}
} // namespace edvr

#include "../../src/d3d11/shared_pair.cpp"
#include "../../src/d3d11/ui_layer_math.h"

namespace {
unsigned g_checks = 0;
// --keep-going: a failed check is recorded and the run goes on, so a mutated build lists EVERY check it breaks
// (the mutation harness reads that to show a named control fail, not just the first check that noticed).
bool g_keepGoing = false;
std::vector<std::string> g_failures;
void check(bool ok, const std::string& why) {
    ++g_checks;
    if (ok) return;
    if (!g_keepGoing) throw std::runtime_error(why);
    g_failures.push_back(why);
}
} // namespace

namespace edvr {
namespace {

// ---- 1: the mark table ---------------------------------------------------------------------------------
void tableCases() {
    const struct { uint64_t vs; PairMark m; const char* what; } marks[] = {
        {kHoloIconCore, PairMark::kRadar, "the radar's icon core"},
        {kHoloIconStalkA, PairMark::kRadar, "stalk A"},
        {kHoloIconStalkB, PairMark::kRadar, "stalk B"},
        {kHoloContactA, PairMark::kRadar, "contact A"},
        {kHoloContactB, PairMark::kRadar, "contact B"},
        {kHoloContactC, PairMark::kRadar, "contact C"},
        {kHoloContactD, PairMark::kRadar, "contact D"},
        {kPairConsoleVsA, PairMark::kConsole, "console draw A"},
        {kPairConsoleVsB, PairMark::kConsole, "console draw B"},
    };
    for (const auto& e : marks) check(pairMarkOf(e.vs) == e.m, std::string("mark table: ") + e.what);
    unsigned filled = 0;
    for (unsigned i = 0; i < 32; ++i) filled += pair_detail::kMarks.hash[i] != 0;
    check(filled == 9, "mark table: nine marks, nine slots");
    check(!pair_detail::kMarks.collided, "mark table: no two marks share a slot");
    for (uint64_t vs : {kSharedPairVs, kHoloCoronaFamily, kHoloCanopy, kHoloTargetSphere, kHoloWorldMarkerReticle,
                        uint64_t{0}, 0x1234567890ABCDEFull})
        check(pairMarkOf(vs) == PairMark::kNone, "mark table: the pair itself, the corona, the canopy, the sphere, nothing: no mark");
    // The radar marks are the crisp take's eight families without the pair's own vertex shader: the two lists
    // cannot drift apart without this saying so.
    unsigned radarInTake = 0;
    bool consistent = true;
    for (uint64_t h : kHoloFamiliesTake) {
        const bool isPair = h == kSharedPairVs;
        consistent = consistent && (pairMarkOf(h) == PairMark::kRadar) == !isPair;
        radarInTake += pairMarkOf(h) == PairMark::kRadar;
    }
    check(consistent && radarInTake == 7 && kSharedPairVs == kHoloContactE,
          "mark table: the radar marks are the take's eight less the pair (E), which is the contact family E");
    check(kSharedPairPs == 0x912477AEF6958379ull && isSharedPair(kSharedPairVs, kSharedPairPs) &&
              !isSharedPair(kSharedPairVs, 0) && !isSharedPair(kHoloContactD, kSharedPairPs),
          "mark table: the pair is the vertex and pixel shader together");
}

// ---- 2: the rule at its edges -----------------------------------------------------------------------------
void ruleCases() {
    const uint32_t hud = kPairStencilRef;   // the reference the HUD section leaves: what a RADAR or PAD call needs
    check(kPairStencilRef == 4 && kPairStencilUnknown == 0xFFFFFFFFu,
          "guard: the reference is 4 (the census's st=04) and an unreadable one is its own value");
    check(pairClassFor(100, 0, 0, hud) == PairClass::kWorld, "rule: no marks is world");
    check(pairClassFor(100, 98, 0, hud) == PairClass::kRadar, "rule: a radar family two draws back is radar");
    check(pairClassFor(350, 100, 0, hud) == PairClass::kRadar, "rule: 250 draws back is inside the radar's window");
    check(pairClassFor(351, 100, 0, hud) == PairClass::kWorld, "rule: 251 draws back is outside it");
    check(pairClassFor(300, 0, 100, hud) == PairClass::kPad, "rule: a console draw and no radar family is pad");
    check(pairClassFor(400, 0, 100, hud) == PairClass::kPad,
          "rule: a console draw 300 back is pad (the pad window was 250 before, which called it world)");
    check(pairClassFor(500, 0, 100, hud) == PairClass::kPad, "rule: a console draw 400 back is inside the pad's window");
    check(pairClassFor(501, 0, 100, hud) == PairClass::kWorld, "rule: a console draw 401 back is outside it");
    check(pairClassFor(100, 90, 50, hud) == PairClass::kRadar, "rule: both marks: radar wins");
    check(pairClassFor(600, 100, 500, hud) == PairClass::kPad, "rule: a radar family out of the window does not shadow a console draw in it");
    check(pairClassFor(100, 200, 0, hud) == PairClass::kWorld && pairClassFor(100, 0, 200, hud) == PairClass::kWorld,
          "rule: a mark after the draw does not count");
    check(pairClassFor(100, 100, 0, hud) == PairClass::kWorld, "rule: a mark at the draw's own ordinal is not before it");
    // The two windows are the approved 250 and 400 and are pinned here, in constructed_window_edge.txt and in
    // the reference below: a change to either is a decision, and this is where it announces itself.
    check(kPairRadarWindow == 250 && kPairPadWindow == 400, "rule: the windows are 250 (radar) and 400 (pad), as approved");
    // Each window has its own edge, moving with its own constant: the pad's window does not stretch the radar's.
    check(pairClassFor(100 + kPairRadarWindow, 100, 0, hud) == PairClass::kRadar &&
              pairClassFor(101 + kPairRadarWindow, 100, 0, hud) == PairClass::kWorld,
          "rule: the radar's window ends at kPairRadarWindow");
    check(pairClassFor(100 + kPairPadWindow, 0, 100, hud) == PairClass::kPad &&
              pairClassFor(101 + kPairPadWindow, 0, 100, hud) == PairClass::kWorld,
          "rule: the pad's window ends at kPairPadWindow");
    check(pairClassFor(101 + kPairRadarWindow, 100, 101, hud) == PairClass::kPad,
          "rule: a radar family just past its window and a console draw just inside the pad's is pad");
    // THE STENCIL GUARD: a RADAR or PAD call stands only with reference 4; any other reference, or one that
    // could not be read, is world; and world stays world whatever the reference says.
    for (uint32_t ref : {0u, 1u, 3u, 5u, 8u, 255u, 0x104u, kPairStencilUnknown}) {
        const std::string r = std::to_string(ref);
        check(pairClassFor(100, 98, 0, ref) == PairClass::kWorld, "guard: a radar call with reference " + r + " is world");
        check(pairClassFor(300, 0, 100, ref) == PairClass::kWorld, "guard: a pad call with reference " + r + " is world");
        check(pairClassFor(100, 0, 0, ref) == PairClass::kWorld, "guard: no reference raises a draw with no marks above world");
    }
    check(pairClassFor(100, 98, 0, hud) == PairClass::kRadar && pairClassFor(300, 0, 100, hud) == PairClass::kPad,
          "guard: with reference 4 the marks' verdict stands");
    check(pairClassFor(112, 110, 0, 0) == PairClass::kWorld,
          "guard: a glare-train draw two draws after a radar family with reference 0 is world (the case no capture holds)");
    check(pairClassFor(112, 110, 0, hud) == PairClass::kRadar,
          "guard: ...and with reference 4 it reads radar: the guard is a second line, not a proof about a train");
    check(pairGuardStencil(PairClass::kWorld, 0) == PairClass::kWorld && pairGuardStencil(PairClass::kNotPair, 0) == PairClass::kNotPair &&
              pairGuardStencil(PairClass::kRadar, hud) == PairClass::kRadar && pairGuardStencil(PairClass::kPad, 0) == PairClass::kWorld,
          "guard: the guard on a marks' verdict: world and not-the-pair pass through, radar and pad need the reference");
    {
        PairEvidence ev;
        check(pairNearestMark(ev) == 0, "near miss: no marks, no near miss");
        ev.radarBack = 300;
        check(pairNearestMark(ev) == 300, "near miss: the radar family alone");
        ev.consoleBack = 280;
        check(pairNearestMark(ev) == 280, "near miss: the nearer of the two");
        ev.radarBack = 270;
        check(pairNearestMark(ev) == 270, "near miss: the nearer of the two, the other way round");
        ev.radarBack = 0;
        check(pairNearestMark(ev) == 280, "near miss: the console draw alone");
        // a near miss and a stencil denial are different things: the marks said world, or the marks said radar or pad
        PairEvidence miss;
        miss.byMarks = PairClass::kWorld;
        miss.radarBack = 300;
        check(pairNearMiss(miss) && !pairStencilDenied(miss), "near miss: world by the marks with a mark past the window is a near miss, not a denial");
        miss.stencilRef = 0;
        check(pairNearMiss(miss) && !pairStencilDenied(miss), "near miss: a world call by the marks is never a denial, whatever the reference");
        PairEvidence denied;
        denied.byMarks = PairClass::kRadar;
        denied.radarBack = 2;
        denied.stencilRef = 0;
        check(pairStencilDenied(denied) && !pairNearMiss(denied), "guard: radar by the marks with reference 0 is a denial, not a near miss");
        denied.stencilRef = kPairStencilUnknown;
        check(pairStencilDenied(denied), "guard: an unreadable reference is a denial");
        denied.stencilRef = hud;
        check(!pairStencilDenied(denied), "guard: reference 4 is not a denial");
        denied.byMarks = PairClass::kPad;
        denied.stencilRef = 8;
        check(pairStencilDenied(denied), "guard: pad by the marks with reference 8 is a denial");
    }
    check(!pairGlareMayClaim(PairClass::kRadar) && !pairGlareMayClaim(PairClass::kPad) &&
              pairGlareMayClaim(PairClass::kWorld) && pairGlareMayClaim(PairClass::kNotPair),
          "gates: the glare fix never sees a radar or pad draw; it judges a world draw and a non-pair draw as before");
    check(pairLayerAdmits(PairClass::kRadar) && pairLayerAdmits(PairClass::kPad) && !pairLayerAdmits(PairClass::kWorld) &&
              !pairLayerAdmits(PairClass::kNotPair),
          "gates: the layer takes the pair as radar or pad, never as world, and never unclassified");
    check(std::strcmp(pairClassName(PairClass::kRadar), "radar") == 0 && std::strcmp(pairClassName(PairClass::kPad), "pad") == 0 &&
              std::strcmp(pairClassName(PairClass::kWorld), "world") == 0,
          "gates: the class names the log prints");
}

// ---- 3: the per-frame tracker ---------------------------------------------------------------------------
int g_a, g_b, g_c, g_d, g_e;   // targets: only their addresses matter
void trackerCases() {
    const uint32_t hud = kPairStencilRef;
    PairTracker t;
    PairEvidence ev;
    const uint32_t f = 7;
    t.note(f, &g_a, 10, kHoloContactD);
    check(t.classify(f, &g_a, 11, hud) == PairClass::kRadar, "tracker: a draw after a mark and before the next reads that mark (the tracker is a stream: draws are classified as they arrive)");
    t.note(f, &g_a, 12, kHoloIconCore);
    t.note(f, &g_b, 20, kPairConsoleVsA);
    check(t.classify(f, &g_a, 14, hud, &ev) == PairClass::kRadar && ev.radarBack == 2 && ev.consoleBack == 0 && ev.marksElsewhere,
          "tracker: the last radar mark on the draw's target, its distance, and marks elsewhere in the frame");
    check(ev.byMarks == PairClass::kRadar && ev.stencilRef == hud,
          "tracker: the evidence carries the marks' verdict and the reference the call was made with");
    check(t.classify(f, &g_b, 30, hud, &ev) == PairClass::kPad && ev.consoleBack == 10 && ev.radarBack == 0,
          "tracker: another target's own marks: the console draw's distance");
    check(t.classify(f, &g_c, 30, hud, &ev) == PairClass::kWorld && ev.marksElsewhere && ev.radarBack == 0 && ev.consoleBack == 0,
          "tracker: a target with no marks is world, and the evidence says the frame has marks elsewhere");
    check(t.classify(f, &g_a, 12, hud) == PairClass::kWorld, "tracker: a draw at a mark's ordinal is not after it (the mark at 12 is the last noted)");
    // the stencil guard through the tracker: the marks say radar or pad, the reference decides
    check(t.classify(f, &g_a, 14, 0, &ev) == PairClass::kWorld && ev.byMarks == PairClass::kRadar && ev.stencilRef == 0 &&
              pairStencilDenied(ev) && !pairNearMiss(ev),
          "tracker: radar by the marks with reference 0 is world, and the evidence says the guard did it");
    check(t.classify(f, &g_b, 30, kPairStencilUnknown, &ev) == PairClass::kWorld && ev.byMarks == PairClass::kPad && pairStencilDenied(ev),
          "tracker: pad by the marks with an unreadable reference is world, denied");
    check(t.classify(f, &g_a, 14, 8) == PairClass::kWorld && t.classify(f, &g_a, 14, 3) == PairClass::kWorld && t.classify(f, &g_a, 14, hud) == PairClass::kRadar,
          "tracker: only reference 4 lets the marks' verdict stand");
    check(t.classify(f, &g_c, 30, 0, &ev) == PairClass::kWorld && ev.byMarks == PairClass::kWorld && !pairStencilDenied(ev),
          "tracker: world by the marks stays world and is not counted as a denial");
    // frame change: every mark is cleared, even when no new mark has arrived
    check(t.classify(f + 1, &g_a, 14, hud) == PairClass::kWorld, "tracker: a new frame clears the marks");
    t.note(f + 1, &g_a, 5, kHoloContactA);
    check(t.classify(f + 1, &g_a, 6, hud) == PairClass::kRadar && t.classify(f + 1, &g_b, 40, hud) == PairClass::kWorld,
          "tracker: the new frame's marks, and only its own");
    check(t.classify(f, &g_a, 14, hud) == PairClass::kWorld, "tracker: an old frame's draw does not read the new frame's marks");
    // the pair's own vertex shader and non-marks set nothing
    PairTracker u;
    u.note(1, &g_a, 3, kSharedPairVs);
    u.note(1, &g_a, 4, kHoloCoronaFamily);
    u.note(1, &g_a, 5, 0x1111);
    check(u.classify(1, &g_a, 9, hud, &ev) == PairClass::kWorld && !ev.marksElsewhere,
          "tracker: the pair's own shader, the corona and any other draw are no marks");
    // capacity: four targets are tracked, a fifth reads as no marks
    PairTracker c;
    c.note(1, &g_a, 1, kHoloIconCore);
    c.note(1, &g_b, 2, kHoloIconCore);
    c.note(1, &g_c, 3, kHoloIconCore);
    c.note(1, &g_d, 4, kHoloIconCore);
    c.note(1, &g_e, 5, kHoloIconCore);
    check(c.classify(1, &g_d, 9, hud) == PairClass::kRadar && c.classify(1, &g_e, 9, hud) == PairClass::kWorld,
          "tracker: four targets are tracked; a fifth is not, and fails toward world");
    c.reset();
    check(c.classify(1, &g_a, 9, hud) == PairClass::kWorld, "tracker: reset clears everything");
    // the same mark twice on one target keeps the LAST (nearest) one
    PairTracker n;
    n.note(1, &g_a, 10, kHoloContactB);
    n.note(1, &g_a, 400, kHoloContactB);
    check(n.classify(1, &g_a, 500, hud, &ev) == PairClass::kRadar && ev.radarBack == 100,
          "tracker: the nearest radar mark is the one that counts (a stale first mark does not)");
}

// ---- 4: the recorded sequences -------------------------------------------------------------------------
struct Entry {
    char kind = 0;             // 'd' a draw, 'p' a draw of the pair
    uint32_t ordinal = 0;
    uint64_t vs = 0;           // for 'd'
    uint32_t instances = 0;    // for 'p'
    std::string target;
    std::string expected;      // for 'p': radar | pad | world
    // for 'p': the stencil reference the census recorded for the draw (st=N, the number after the enable
    // flag), or kPairStencilUnknown for st=- (not recorded, or could not be read): the guard's input.
    uint32_t stencil = kPairStencilUnknown;
};
struct Frame {
    uint32_t id = 0;
    std::vector<Entry> entries;
};
struct Fixture {
    std::string name;
    std::vector<Frame> frames;
};

std::vector<Fixture> loadFixtures() {
    std::vector<Fixture> out;
    const std::filesystem::path dir = "tools/shared_pair_test/fixtures";
    check(std::filesystem::is_directory(dir), "fixtures: tools/shared_pair_test/fixtures opens (this rig runs from the repository root)");
    std::vector<std::filesystem::path> paths;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().extension() == ".txt") paths.push_back(e.path());
    std::sort(paths.begin(), paths.end());
    for (const auto& p : paths) {
        std::ifstream in(p);
        check(bool(in), "fixtures: " + p.filename().string() + " opens");
        Fixture fx;
        fx.name = p.filename().string();
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            char kind = 0;
            unsigned ordinal = 0, second = 0;
            char a[64] = {}, b[64] = {}, c[64] = {}, s[64] = {};
            if (std::sscanf(line.c_str(), "frame %u", &ordinal) == 1 && line.compare(0, 6, "frame ") == 0) {
                fx.frames.push_back(Frame{ordinal, {}});
            } else if (std::sscanf(line.c_str(), "%c %u %63s %63s", &kind, &ordinal, a, b) == 4 && kind == 'd') {
                check(!fx.frames.empty(), fx.name + ": a draw before any frame");
                Entry e;
                e.kind = 'd';
                e.ordinal = ordinal;
                e.vs = std::strtoull(a, nullptr, 16);
                e.target = b;
                fx.frames.back().entries.push_back(e);
            } else if (std::sscanf(line.c_str(), "%c %u %u %63s %63s %63s", &kind, &ordinal, &second, b, c, s) == 6 && kind == 'p') {
                check(!fx.frames.empty(), fx.name + ": a pair draw before any frame");
                Entry e;
                e.kind = 'p';
                e.ordinal = ordinal;
                e.instances = second;
                e.target = b;
                e.expected = c;
                check(std::strncmp(s, "st=", 3) == 0, fx.name + ": a pair draw needs its stencil reference (st=N, or st=- when it is not known): " + line);
                e.stencil = std::strcmp(s + 3, "-") == 0 ? kPairStencilUnknown : static_cast<uint32_t>(std::strtoul(s + 3, nullptr, 10));
                fx.frames.back().entries.push_back(e);
            } else {
                check(false, fx.name + ": unreadable line: " + line);
            }
        }
        out.push_back(std::move(fx));
    }
    return out;
}

const void* targetPtr(std::map<std::string, int>& ids, const std::string& token) {
    auto it = ids.find(token);
    if (it == ids.end()) it = ids.emplace(token, static_cast<int>(ids.size()) + 1).first;
    return reinterpret_cast<const void*>(static_cast<uintptr_t>(it->second) * 16u);
}

PairClass classFromName(const std::string& s) {
    return s == "radar" ? PairClass::kRadar : s == "pad" ? PairClass::kPad : PairClass::kWorld;
}

// The production tracker over one frame's entries, in order: each pair draw's class.
std::vector<PairClass> productionClasses(const Frame& fr, PairEvidence* last = nullptr) {
    PairTracker t;
    std::map<std::string, int> ids;
    std::vector<PairClass> out;
    for (const Entry& e : fr.entries) {
        const void* target = targetPtr(ids, e.target);
        if (e.kind == 'd') t.note(fr.id, target, e.ordinal, e.vs);
        else out.push_back(t.classify(fr.id, target, e.ordinal, e.stencil, last));
    }
    return out;
}

// The reference: the rule written again, plainly, with switches to break one part of it at a time.
enum class Mutant {
    kNone, kNoRadar, kNoConsole, kAnyTarget, kMarksAfter, kNoWindow, kUnboundedWindow, kConsoleFirst,
    kPadOld, kPadNarrow, kPadShort, kPadLong, kRadarShort, kRadarLong,
    kNoStencil, kStencilZero, kStencilNonZero, kUnknownPasses
};
const char* mutantName(Mutant m) {
    switch (m) {
        case Mutant::kNoRadar: return "no radar flag";
        case Mutant::kNoConsole: return "no console flag";
        case Mutant::kAnyTarget: return "marks on any target";
        case Mutant::kMarksAfter: return "marks after the draw count";
        case Mutant::kNoWindow: return "a window of zero";
        case Mutant::kUnboundedWindow: return "an unbounded window";
        case Mutant::kConsoleFirst: return "the console tested before the radar";
        case Mutant::kPadOld: return "the pad window back at 250";
        case Mutant::kPadNarrow: return "a pad window of 230 (the recorded pad is 236 draws back)";
        case Mutant::kPadShort: return "a pad window of 399";
        case Mutant::kPadLong: return "a pad window of 401";
        case Mutant::kRadarShort: return "a radar window of 249";
        case Mutant::kRadarLong: return "a radar window of 251";
        case Mutant::kNoStencil: return "the stencil guard dropped";
        case Mutant::kStencilZero: return "a guard that wants reference 0";
        case Mutant::kStencilNonZero: return "a guard that takes any nonzero reference";
        case Mutant::kUnknownPasses: return "an unreadable reference passing the guard";
        default: return "none";
    }
}
std::vector<PairClass> referenceClasses(const Frame& fr, Mutant m) {
    std::vector<PairClass> out;
    uint32_t radarWindow = 250u, padWindow = 400u;
    switch (m) {
        case Mutant::kNoWindow: radarWindow = padWindow = 0u; break;
        case Mutant::kUnboundedWindow: radarWindow = padWindow = 0xFFFFFFFFu; break;
        case Mutant::kPadOld: padWindow = 250u; break;
        case Mutant::kPadNarrow: padWindow = 230u; break;
        case Mutant::kPadShort: padWindow = 399u; break;
        case Mutant::kPadLong: padWindow = 401u; break;
        case Mutant::kRadarShort: radarWindow = 249u; break;
        case Mutant::kRadarLong: radarWindow = 251u; break;
        default: break;
    }
    for (size_t i = 0; i < fr.entries.size(); ++i) {
        const Entry& p = fr.entries[i];
        if (p.kind != 'p') continue;
        bool radar = false, console = false;
        for (size_t j = 0; j < fr.entries.size(); ++j) {
            const Entry& d = fr.entries[j];
            if (d.kind != 'd') continue;
            const bool before = m == Mutant::kMarksAfter ? true : d.ordinal < p.ordinal;
            const bool sameTarget = m == Mutant::kAnyTarget ? true : d.target == p.target;
            const uint32_t back = d.ordinal < p.ordinal ? p.ordinal - d.ordinal : d.ordinal - p.ordinal;
            const PairMark mark = pairMarkOf(d.vs);
            const uint32_t window = mark == PairMark::kRadar ? radarWindow : padWindow;
            if (!before || !sameTarget || back > window) continue;
            if (mark == PairMark::kRadar && m != Mutant::kNoRadar) radar = true;
            if (mark == PairMark::kConsole && m != Mutant::kNoConsole) console = true;
        }
        PairClass cls = m == Mutant::kConsoleFirst ? (console ? PairClass::kPad : radar ? PairClass::kRadar : PairClass::kWorld)
                                                   : (radar ? PairClass::kRadar : console ? PairClass::kPad : PairClass::kWorld);
        // the stencil guard, written again: the marks' verdict stands only with reference 4
        if (cls == PairClass::kRadar || cls == PairClass::kPad) {
            bool stands;
            switch (m) {
                case Mutant::kNoStencil: stands = true; break;
                case Mutant::kStencilZero: stands = p.stencil == 0; break;
                case Mutant::kStencilNonZero: stands = p.stencil != 0; break;
                case Mutant::kUnknownPasses: stands = p.stencil == 4 || p.stencil == kPairStencilUnknown; break;
                default: stands = p.stencil == 4; break;
            }
            if (!stands) cls = PairClass::kWorld;
        }
        out.push_back(cls);
    }
    return out;
}

// Where a fixture's pair draw sits: its frame and its index among that frame's pair draws.
struct Located {
    const Frame* fr = nullptr;
    size_t k = 0;
};
Located locate(const std::vector<Fixture>& fixtures, const std::string& name, uint32_t ordinal) {
    for (const Fixture& fx : fixtures) {
        if (fx.name != name) continue;
        for (const Frame& fr : fx.frames) {
            size_t k = 0;
            for (const Entry& e : fr.entries) {
                if (e.kind != 'p') continue;
                if (e.ordinal == ordinal) return {&fr, k};
                ++k;
            }
        }
    }
    check(false, "control: " + name + " has no pair draw at #" + std::to_string(ordinal));
    return {};
}

void fixtureCases() {
    const std::vector<Fixture> fixtures = loadFixtures();
    check(fixtures.size() >= 7, "fixtures: at least seven recorded frames (" + std::to_string(fixtures.size()) + " found)");
    unsigned radar = 0, pad = 0, world = 0, draws = 0;
    for (const Fixture& fx : fixtures) {
        check(!fx.frames.empty(), fx.name + ": has a frame");
        for (const Frame& fr : fx.frames) {
            const std::vector<PairClass> got = productionClasses(fr);
            const std::vector<PairClass> ref = referenceClasses(fr, Mutant::kNone);
            size_t k = 0;
            for (const Entry& e : fr.entries) {
                if (e.kind != 'p') continue;
                check(k < got.size(), fx.name + ": every pair draw was classified");
                check(got[k] == classFromName(e.expected),
                      fx.name + ": draw #" + std::to_string(e.ordinal) + " (" + std::to_string(e.instances) + " instances) is " +
                          e.expected + " in the capture, the tracker says " + pairClassName(got[k]));
                check(ref[k] == got[k], fx.name + ": the reference rule and the tracker agree at #" + std::to_string(e.ordinal));
                radar += got[k] == PairClass::kRadar;
                pad += got[k] == PairClass::kPad;
                world += got[k] == PairClass::kWorld;
                ++draws;
                ++k;
            }
        }
    }
    check(radar >= 4 && pad >= 12 && world >= 4, "fixtures: radar, pad and world draws all recorded");
    std::printf("shared_pair_test: %u recorded draws of the pair over %zu fixtures: %u radar, %u pad, %u world\n", draws, fixtures.size(),
                radar, pad, world);

    // NEGATIVE CONTROLS: break the rule one way at a time; the recordings must notice every one.
    for (Mutant m : {Mutant::kNoRadar, Mutant::kNoConsole, Mutant::kAnyTarget, Mutant::kMarksAfter, Mutant::kNoWindow,
                     Mutant::kUnboundedWindow, Mutant::kConsoleFirst, Mutant::kPadOld, Mutant::kPadNarrow,
                     Mutant::kPadShort, Mutant::kPadLong, Mutant::kRadarShort, Mutant::kRadarLong, Mutant::kNoStencil,
                     Mutant::kStencilZero, Mutant::kStencilNonZero, Mutant::kUnknownPasses}) {
        unsigned disagreements = 0;
        for (const Fixture& fx : fixtures)
            for (const Frame& fr : fx.frames) {
                const std::vector<PairClass> ref = referenceClasses(fr, m);
                size_t k = 0;
                for (const Entry& e : fr.entries)
                    if (e.kind == 'p') disagreements += ref[k++] != classFromName(e.expected);
            }
        char what[160];
        std::snprintf(what, sizeof(what), "negative control: the rule with %s is caught by the recordings (%u disagreements)",
                      mutantName(m), disagreements);
        // The marks-after and any-target mutants only bite where a second target or a later mark exists, and the
        // recordings have both; every mutant must disagree at least once.
        check(disagreements > 0, what);
    }

    // THE CONTROLS THE APPROVAL NAMED, each held by the production tracker AND broken, at that very draw, by
    // its own mutant: it is not enough that some recording notices a mutant, this one must.
    struct Control {
        const char* fixture;
        uint32_t ordinal;
        PairClass expected;
        Mutant breaks;
        const char* what;
    };
    const Control controls[] = {
        {"constructed_window_edge.txt", 405, PairClass::kPad, Mutant::kPadOld,
         "a pad draw 300 back with reference 4 is PAD (a pad window of 250 says world)"},
        {"constructed_window_edge.txt", 500, PairClass::kPad, Mutant::kPadShort, "a pad draw 400 back is PAD (the window's last draw)"},
        {"constructed_window_edge.txt", 502, PairClass::kWorld, Mutant::kPadLong, "a pad draw 401 back is WORLD (one past the window)"},
        {"constructed_window_edge.txt", 353, PairClass::kRadar, Mutant::kRadarShort, "a radar draw 250 back is RADAR"},
        {"constructed_window_edge.txt", 355, PairClass::kWorld, Mutant::kRadarLong, "a radar draw 251 back is WORLD"},
        {"constructed_stencil_guard.txt", 300, PairClass::kPad, Mutant::kStencilZero,
         "the control: a pad-shaped draw with reference 4 is PAD"},
        {"constructed_stencil_guard.txt", 301, PairClass::kWorld, Mutant::kNoStencil,
         "a pad-shaped draw with reference 0 (st=00) is WORLD"},
        {"constructed_stencil_guard.txt", 302, PairClass::kWorld, Mutant::kStencilNonZero,
         "a pad-shaped draw with any other reference (8) is WORLD"},
        {"constructed_stencil_guard.txt", 303, PairClass::kWorld, Mutant::kUnknownPasses,
         "a pad-shaped draw with an unreadable reference is WORLD"},
        {"constructed_stencil_guard.txt", 112, PairClass::kWorld, Mutant::kNoStencil,
         "a glare-train draw with reference 0 two draws after a radar family, inside the radar window, is WORLD"},
        {"constructed_stencil_guard.txt", 122, PairClass::kRadar, Mutant::kStencilZero,
         "the same train-shaped draw with reference 4 reads RADAR: the guard is a second line, not a proof"},
        {"constructed_stencil_guard.txt", 132, PairClass::kWorld, Mutant::kUnknownPasses,
         "a train-shaped draw with an unreadable reference inside the radar window is WORLD"},
        {"constructed_stencil_guard.txt", 142, PairClass::kWorld, Mutant::kStencilNonZero,
         "a train-shaped draw with any other reference (8) inside the radar window is WORLD"},
    };
    for (const Control& c : controls) {
        const Located at = locate(fixtures, c.fixture, c.ordinal);
        if (!at.fr) continue;   // (--keep-going only: locate has already recorded the failure)
        const std::vector<PairClass> got = productionClasses(*at.fr);
        const std::vector<PairClass> ref = referenceClasses(*at.fr, Mutant::kNone);
        const std::vector<PairClass> broken = referenceClasses(*at.fr, c.breaks);
        const std::string tag = std::string(c.fixture) + " #" + std::to_string(c.ordinal) + ": " + c.what;
        check(got[at.k] == c.expected, "control held by the tracker: " + tag + " (got " + pairClassName(got[at.k]) + ")");
        check(ref[at.k] == c.expected, "control held by the reference: " + tag);
        check(broken[at.k] != c.expected, std::string("control broken by its mutant, ") + mutantName(c.breaks) + ": " + tag);
    }
    std::printf("shared_pair_test: %zu named controls, each held by the tracker and broken by its own mutant\n",
                sizeof(controls) / sizeof(controls[0]));
}

// ---- 6: who acts on a class -----------------------------------------------------------------------------
// The layer's decision for a draw of the pair, as vscreen composes it: the glare fix claims a draw by shape
// (every mode but stock) unless pairGlareMayClaim says it may not, the claim is a verdict the layer cannot
// forward, and uiLayerDecide then takes or refuses the draw.
UiLayerDecision layerDecision(PairClass cls, bool glareModeClaims, bool guardOn) {
    UiFamilyFacts f;
    f.targetKind = 1;
    f.vs = kSharedPairVs;
    f.pairClass = cls;
    UiLayerDrawFacts d;
    d.family = uiLayerFamilyFor(f);
    const bool claimed = glareModeClaims && (!guardOn || pairGlareMayClaim(cls));
    d.verdictForwards = !claimed;
    d.eyeTarget = true;
    d.ldrView = false;
    d.crispHdr = true;
    d.eye = 0;
    d.targetMatchesEye = true;
    d.armed = true;
    d.ds.depthTest = true;
    d.blend = UiBlendShape::kScaledAdditive;
    d.layerReady = true;
    return uiLayerDecide(d);
}

void gateCases() {
    // the family rule with the pair's class
    UiFamilyFacts f;
    f.targetKind = 1;
    f.vs = kSharedPairVs;
    for (PairClass c : {PairClass::kRadar, PairClass::kPad}) {
        f.pairClass = c;
        check(uiLayerFamilyFor(f) == UiLayerFamily::kHoloGeneric, "family: the pair as radar or pad is the take's hologram family");
    }
    for (PairClass c : {PairClass::kWorld, PairClass::kNotPair}) {
        f.pairClass = c;
        check(uiLayerFamilyFor(f) == UiLayerFamily::kNone, "family: the pair as world, or never classified, is no family");
    }
    // the class changes nothing for the take's other seven, or for what is not the take
    for (uint64_t h : {kHoloIconCore, kHoloIconStalkA, kHoloIconStalkB, kHoloContactA, kHoloContactB, kHoloContactC, kHoloContactD}) {
        f.vs = h;
        for (PairClass c : {PairClass::kNotPair, PairClass::kRadar, PairClass::kPad, PairClass::kWorld}) {
            f.pairClass = c;
            check(uiLayerFamilyFor(f) == UiLayerFamily::kHoloGeneric, "family: the take's other seven do not read the pair's class");
        }
    }
    f.pairClass = PairClass::kRadar;
    for (uint64_t h : {kHoloCoronaFamily, kHoloCanopy, uint64_t{0}}) {
        f.vs = h;
        check(uiLayerFamilyFor(f) == UiLayerFamily::kNone, "family: a class is not admission for a shader that is not the take's");
    }
    f.vs = kSharedPairVs;
    f.targetKind = 2;   // the post-tonemap target: the pair is no interface composite there
    check(uiLayerFamilyFor(f) == UiLayerFamily::kNone, "family: the pair on the 8-bit post-tonemap target is not a family");

    // uiLayerDecide, in every glare mode. stock: the fix never claims; vivid, realistic and off: it claims by shape.
    struct Mode { const char* name; bool claims; };
    const Mode modes[] = {{"stock", false}, {"vivid", true}, {"realistic", true}, {"off", true}};
    for (const Mode& m : modes) {
        for (PairClass c : {PairClass::kRadar, PairClass::kPad}) {
            const UiLayerDecision d = layerDecision(c, m.claims, true);
            check(d == UiLayerDecision::kRedirect,
                  std::string("decide: a ") + pairClassName(c) + " draw of the pair is redirected in " + m.name + ", not left to another fix");
            check(d != UiLayerDecision::kVerdict, std::string("decide: never 'another fix swallows it' for a ") + pairClassName(c) + " draw in " + m.name);
        }
        for (PairClass c : {PairClass::kWorld, PairClass::kNotPair}) {
            check(layerDecision(c, m.claims, true) == UiLayerDecision::kNotUi,
                  std::string("decide: a ") + pairClassName(c) + " draw of the pair is not UI to the layer in " + m.name);
        }
    }
    // NEGATIVE CONTROL: without the guard the glare fix claims the draw, and the decision is the one Sean's
    // dump showed: "left in the game's frame: hologram ... (another fix swallows or re-issues it)".
    for (PairClass c : {PairClass::kRadar, PairClass::kPad})
        check(layerDecision(c, true, false) == UiLayerDecision::kVerdict,
              "negative control: without the claim guard a radar or pad draw reaches kVerdict when the glare fix claims by shape");
    check(layerDecision(PairClass::kRadar, false, false) == UiLayerDecision::kRedirect,
          "negative control: in stock the old rule took the pair, which is why stock took a real glare train too");
    UiFamilyFacts oldRule;   // what the take did for the pair before: by vertex shader alone, whatever the class
    oldRule.targetKind = 1;
    oldRule.vs = kSharedPairVs;
    oldRule.pairClass = PairClass::kWorld;
    check(uiLayerFamilyFor(oldRule) == UiLayerFamily::kNone && uiHoloGenericHash(kSharedPairVs),
          "negative control: the pair is still on the take's list by hash; only its class keeps a world draw out");
}

// ---- 7: the runtime half -------------------------------------------------------------------------------
std::vector<std::string> takeLines() {
    std::vector<std::string> out;
    out.swap(g_lines);
    return out;
}
bool hasLine(const std::vector<std::string>& lines, const char* needle) {
    for (const auto& l : lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

void runtimeCases() {
    sharedPairShutdown();
    takeLines();
    int eyeA = 0, eyeB = 0, eyeC = 0;
    uint32_t frame = 1;
    const uint32_t hud = kPairStencilRef;
    // The pair's draw at an ordinal, with the shaders of the pair and the stencil reference given (the HUD's by default).
    auto pairAt = [&](uint32_t fr, const void* rtv, uint32_t ordinal, uint32_t instances, uint32_t stencil = kPairStencilRef) {
        return sharedPairClassify(fr, rtv, ordinal, kSharedPairVs, kSharedPairPs, instances, stencil);
    };
    // A pad frame: console draws, then the pair's two draws an eye; a radar frame; a glare-shaped draw on the scene target.
    for (int eye = 0; eye < 2; ++eye) {
        const void* rtv = eye ? &eyeB : &eyeA;
        sharedPairNoteEyeDraw(frame, rtv, 100 + eye * 400, kPairConsoleVsB);
        sharedPairNoteEyeDraw(frame, rtv, 102 + eye * 400, kPairConsoleVsA);
        check(pairAt(frame, rtv, 300 + eye * 400, 3392) == PairClass::kPad, "runtime: a pad draw");
        check(pairAt(frame, rtv, 301 + eye * 400, 94) == PairClass::kPad, "runtime: the pad's second draw");
    }
    check(sharedPairClassify(frame, &eyeA, 302, kSharedPairVs, 0x1234, 9, hud) == PairClass::kNotPair &&
              sharedPairClassify(frame, &eyeA, 303, 0x1234, kSharedPairPs, 9, hud) == PairClass::kNotPair &&
              sharedPairClassify(frame, &eyeA, 304, kHoloContactD, kSharedPairPs, 9, 0) == PairClass::kNotPair,
          "runtime: another pixel shader, another vertex shader: not the pair, counted nowhere, whatever the reference");
    std::vector<std::string> lines = takeLines();
    check(hasLine(lines, "the first draw of the pad class: 3392 instances, a console draw 198 draws before it on its target and no radar family within 250, stencil reference 4.") &&
              !hasLine(lines, "the first draw of the radar class"),
          "runtime: the first pad draw is named once, with its instances, the mark's distance and the reference");
    ++frame;
    sharedPairNoteEyeDraw(frame, &eyeA, 189, kHoloContactD);
    sharedPairNoteEyeDraw(frame, &eyeA, 190, kHoloIconCore);
    check(pairAt(frame, &eyeA, 192, 39) == PairClass::kRadar, "runtime: a radar draw");
    check(pairAt(frame, &eyeB, 60, 8) == PairClass::kWorld, "runtime: a draw on a target with no marks is world");
    lines = takeLines();
    check(hasLine(lines, "the first draw of the radar class: 39 instances, a radar family drew 2 draws before it") &&
              hasLine(lines, "the first draw of the world class: 8 instances, no radar family within 250 and no console draw within 400 draws before it on its target; the frame's HUD marks are on another target; its stencil reference is 4.") &&
              !hasLine(lines, "the nearest mark on its own target"),
          "runtime: the first radar and the first world draw are named, the world one with the marks-elsewhere note and, having no mark of its own, no near-miss note");
    check(pairAt(frame, &eyeA, 193, 39) == PairClass::kRadar && takeLines().empty(),
          "runtime: a class is named once, not at every draw");

    // the 30 s line
    sharedPairFrameBoundary(1000, true);          // window starts
    sharedPairFrameBoundary(20000, true);
    check(takeLines().empty(), "runtime: no line before 30 s");
    sharedPairFrameBoundary(31000, true);
    lines = takeLines();
    check(lines.size() == 1 && lines[0].rfind("shared pair: radar 2, pad 4, world 1 (30 s; ", 0) == 0,
          "runtime: the line reads 'shared pair: radar N, pad N, world N' with this window's counts (2 radar, 4 pad, 1 world)");
    check(lines.size() == 1 && lines[0].find("radar family 2-3 draws back, console draw 198-199") != std::string::npos &&
              lines[0].find("1 world draws in a frame whose HUD marks were on another target") != std::string::npos,
          "runtime: the line carries the marks' nearest distances and the world draws with marks elsewhere");
    check(lines.size() == 1 &&
              lines[0].find("stencil guard (the reference must be 4): 0 draws the marks called radar (0) or pad (0) went to world, references seen: -.") != std::string::npos,
          "runtime: a window in which every radar and pad call had reference 4 says the guard turned nothing away: 0 draws, references '-'");
    // the window reset, and a window in which the rule ran but no pair draw came: a line of zeros
    sharedPairFrameBoundary(62000, true);
    lines = takeLines();
    check(lines.size() == 1 && lines[0].rfind("shared pair: radar 0, pad 0, world 0 (31 s; ", 0) == 0 && lines[0].find("radar family - draws back, console draw -;") != std::string::npos,
          "runtime: a window the rule ran in with no pair draw prints zeros: 'ran, saw none' -- distinguishable from no line");
    // NEGATIVE CONTROL: a window the rule never ran in prints nothing at all
    sharedPairFrameBoundary(93000, false);
    sharedPairFrameBoundary(124000, false);
    check(takeLines().empty(), "negative control: a window the rule never ran in prints no line, so absence means 'not running'");
    sharedPairFrameBoundary(155000, false);
    sharedPairFrameBoundary(186000, true);        // ran only in the last frame of a window: still a line
    check(takeLines().size() == 1, "runtime: one active frame in a window is enough for its line");

    // A NEAR MISS: a world call with a mark of its own on its own target, past the window. The first one is
    // named with how far back the mark was, and the 30 s line counts them; a world draw with no mark of its own
    // and a radar draw are not near misses. This is where a window that is too short for a busier HUD would show.
    sharedPairShutdown();
    takeLines();
    ++frame;
    sharedPairNoteEyeDraw(frame, &eyeA, 10, kHoloContactB);
    sharedPairNoteEyeDraw(frame, &eyeA, 20, kPairConsoleVsA);
    check(pairAt(frame, &eyeA, 470, 60) == PairClass::kWorld,
          "runtime: a radar family 460 and a console draw 450 draws back are past both windows (250, 400): world");
    lines = takeLines();
    check(hasLine(lines, "the first draw of the world class: 60 instances, no radar family within 250 and no console draw within 400 draws before it on its target; the nearest mark on its own target is 450 draws back, past the window; its stencil reference is 4.") &&
              !hasLine(lines, "HUD marks are on another target"),
          "runtime: the first world draw names its near miss and how far back the nearest mark was");
    check(pairAt(frame, &eyeA, 490, 60) == PairClass::kWorld && pairAt(frame, &eyeB, 330, 60) == PairClass::kWorld && takeLines().empty(),
          "runtime: a second near miss, and a world draw on a target with no marks of its own, are not named again");
    sharedPairNoteEyeDraw(frame, &eyeB, 335, kHoloContactC);
    check(pairAt(frame, &eyeB, 337, 39) == PairClass::kRadar && hasLine(takeLines(), "the first draw of the radar class"),
          "runtime: a radar draw after the world ones");
    sharedPairFrameBoundary(200000, true);
    sharedPairFrameBoundary(230000, true);
    lines = takeLines();
    check(lines.size() == 1 && lines[0].rfind("shared pair: radar 1, pad 0, world 3 (30 s; ", 0) == 0 &&
              lines[0].find("1 world draws in a frame whose HUD marks were on another target") != std::string::npos &&
              lines[0].find("2 world draws with a mark on their own target past the window (450-470 draws back)") != std::string::npos,
          "runtime: the line counts the near misses (2 of the 3 world draws) and how far back their marks were; the draw with no mark of its own and the radar draw are not among them");
    sharedPairFrameBoundary(260000, true);
    lines = takeLines();
    check(lines.size() == 1 && lines[0].find("0 world draws with a mark on their own target past the window (- draws back)") != std::string::npos,
          "runtime: the next window starts from zero near misses");

    // THE STENCIL GUARD at run time. A draw the marks call radar or pad and the reference sends to world is named
    // once (the guard's own note, not the world class's), counted with the references seen, and a reference that
    // could not be read counts as one; a world draw by the marks, and a draw with reference 4, are not among them.
    sharedPairShutdown();
    takeLines();
    ++frame;
    sharedPairNoteEyeDraw(frame, &eyeA, 10, kHoloContactD);
    sharedPairNoteEyeDraw(frame, &eyeB, 20, kPairConsoleVsA);
    check(pairAt(frame, &eyeA, 12, 15, 0) == PairClass::kWorld,
          "runtime: a train-shaped draw two draws after a radar family with reference 0 is world");
    lines = takeLines();
    check(hasLine(lines, "the first draw the stencil guard sent to world: 15 instances, the marks said radar (a radar family 2 draws back on its target) but its stencil reference is 0, and the rule needs 4.") &&
              !hasLine(lines, "the first draw of the world class"),
          "runtime: the guard's first refusal is named, with what the marks said and the reference; the world class's note is not spent on it");
    check(pairAt(frame, &eyeA, 14, 15, 0) == PairClass::kWorld && pairAt(frame, &eyeA, 16, 15, 0) == PairClass::kWorld &&
              pairAt(frame, &eyeB, 320, 3392, 5) == PairClass::kWorld &&
              pairAt(frame, &eyeB, 321, 94, kPairStencilUnknown) == PairClass::kWorld && takeLines().empty(),
          "runtime: further refusals (reference 0, 5 and an unreadable one) are world and not named again");
    check(pairAt(frame, &eyeC, 8, 8, 0) == PairClass::kWorld, "runtime: a draw on a target with no marks and reference 0 is world, not a refusal");
    lines = takeLines();
    check(hasLine(lines, "the first draw of the world class: 8 instances") && hasLine(lines, "its stencil reference is 0.") &&
              !hasLine(lines, "stencil guard sent"),
          "runtime: a plain world draw still gets the world class's note, with its reference");
    check(pairAt(frame, &eyeA, 18, 39) == PairClass::kRadar && hasLine(takeLines(), "the first draw of the radar class"),
          "runtime: a radar draw with reference 4 in the same frame stands");
    sharedPairFrameBoundary(300000, true);
    sharedPairFrameBoundary(330000, true);
    lines = takeLines();
    check(lines.size() == 1 && lines[0].rfind("shared pair: radar 1, pad 0, world 6 (30 s; ", 0) == 0 &&
              lines[0].find("stencil guard (the reference must be 4): 5 draws the marks called radar (3) or pad (2) went to world, references seen: 0 x 3, 5 x 1, unreadable x 1.") != std::string::npos &&
              lines[0].find("0 world draws with a mark on their own target past the window (- draws back)") != std::string::npos,
          "runtime: the line counts the refusals (3 by a radar family, 2 by a console draw) with the references seen and 'unreadable'; refusals are no near misses");
    // more references than the table holds: the first six are listed, the rest are counted together
    ++frame;
    sharedPairNoteEyeDraw(frame, &eyeA, 10, kHoloContactD);
    for (uint32_t ref : {0u, 1u, 2u, 3u, 5u, 6u, 7u, 9u})
        check(pairAt(frame, &eyeA, 12 + ref, 15, ref) == PairClass::kWorld, "runtime: a refused draw, reference " + std::to_string(ref));
    sharedPairFrameBoundary(360000, true);
    lines = takeLines();
    check(lines.size() == 1 && lines[0].find("8 draws the marks called radar (8) or pad (0) went to world, references seen: 0 x 1, 1 x 1, 2 x 1, 3 x 1, 5 x 1, 6 x 1, other x 2.") != std::string::npos,
          "runtime: the references table holds six values and counts the rest as 'other'");
    // the next window starts from an empty table; a first refusal with an unreadable reference says so
    sharedPairShutdown();
    takeLines();
    ++frame;
    sharedPairNoteEyeDraw(frame, &eyeA, 10, kHoloContactD);
    check(pairAt(frame, &eyeA, 12, 15, kPairStencilUnknown) == PairClass::kWorld, "runtime: an unreadable reference is world");
    lines = takeLines();
    check(hasLine(lines, "the first draw the stencil guard sent to world: 15 instances, the marks said radar (a radar family 2 draws back on its target) but its stencil reference could not be read, and the rule needs 4."),
          "runtime: an unreadable reference is named as such in the guard's first note, and (after a shutdown) the note is once more spent");
    sharedPairFrameBoundary(400000, true);
    sharedPairFrameBoundary(430000, true);
    lines = takeLines();
    check(lines.size() == 1 && lines[0].find("1 draws the marks called radar (1) or pad (0) went to world, references seen: unreadable x 1.") != std::string::npos,
          "runtime: the line counts an unreadable reference among the refusals");
    sharedPairShutdown();
    takeLines();
}

// ---- 8: the wiring, by scan --------------------------------------------------------------------------------
std::string readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    check(bool(in), std::string(path) + " opens (this rig runs from the repository root)");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    out.reserve(text.size());
    for (char c : text) if (c != '\r') out += c;
    return out;
}
std::string withoutLineComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "//") == 0) {
            while (i < text.size() && text[i] != '\n') ++i;
        } else {
            out += text[i++];
        }
    }
    return out;
}
size_t countOf(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
    return n;
}
// The text from `from` to the first line that is just "}" after it: one function's body.
std::string bodyFrom(const std::string& text, const std::string& from) {
    const size_t at = text.find(from);
    if (at == std::string::npos) return {};
    const size_t end = text.find("\n}\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// The first thing wrong with the wiring in vscreen.cpp and ui_layer_math.h, or an empty string.
std::string wiringProblem(const std::string& rawVscreen, const std::string& rawMath) {
    const std::string v = withoutLineComments(rawVscreen);
    const std::string m = withoutLineComments(rawMath);
    const std::string counter = "++s->eyeDrawsThisFrame;";
    const size_t at = v.find(counter);
    if (at == std::string::npos || countOf(v, counter) != 1) return "the eye-draw counter is not where the scan looks";
    const std::string feed = v.substr(at, 1800);
    if (feed.find("if (s->pairTrackerOn)") == std::string::npos) return "the tracker's feed is not gated on pairTrackerOn";
    if (feed.find("sharedPairNoteEyeDraw(s->frameNo") == std::string::npos) return "the marks are not fed from every eye draw";
    if (feed.find("if (pairVs == kSharedPairVs)") == std::string::npos || feed.find("s->pairClassThisDraw = sharedPairClassify(") == std::string::npos)
        return "a draw of the pair is not classified where the counter is";
    // The stencil guard's input: read from the draw's own state for the pair's two shaders together, and handed to the rule.
    if (feed.find("isSharedPair(pairVs, pairPs) ? pairReadStencilRef(self) : kPairStencilUnknown") == std::string::npos)
        return "the stencil reference is not read from the draw for the pair's own draws";
    if (feed.find("pairPs, instances, pairStencil);") == std::string::npos) return "the stencil reference is not handed to the rule";
    const std::string readStencil = bodyFrom(v, "uint32_t pairReadStencilRef(ID3D11DeviceContext* ctx) {");
    if (readStencil.find("guardedBudget(g_pairStencilBudget") == std::string::npos) return "the stencil read is not under a fault budget";
    if (readStencil.find("OMGetDepthStencilState(&dss, &ref)") == std::string::npos)
        return "the stencil reference is not read from OMGetDepthStencilState the way the census reads its st= column";
    if (readStencil.find(": kPairStencilUnknown;") == std::string::npos) return "an unreadable stencil reference does not read as unknown";
    const size_t reset = v.find("s->pairClassThisDraw = PairClass::kNotPair;");
    if (reset == std::string::npos || reset > at) return "the per-draw class is not cleared before the counter";
    const size_t guard = v.find("pairGlareMayClaim(s->pairClassThisDraw) &&");
    if (guard == std::string::npos) return "the glare claim is not guarded by the class";
    const std::string afterGuard = v.substr(guard, 160);
    if (afterGuard.find("sunglareTrainShape(kind, count, instances)") == std::string::npos) return "the class guard does not sit on the glare claim";
    if (guard < at) return "the glare claim's guard comes before the classification";
    const std::string family = bodyFrom(v, "UiLayerFamily uiLayerFamilyOf(");
    if (family.find("f.vs == kSharedPairVs") == std::string::npos || family.find("f.pairClass = s->pairClassThisDraw;") == std::string::npos)
        return "uiLayerFamilyOf does not hand the layer the pair's class";
    const std::string gate = bodyFrom(v, "bool drawGateSubscribed(State* s) {");
    if (gate.find("sharedPairWantsDraws()") == std::string::npos) return "the draw gate does not include the tracker's subscription";
    if (countOf(v, "s->pairTrackerOn = sharedPairWantsDraws();") != 2) return "the tracker's subscription is not sampled at both gate refreshes";
    if (v.find("sharedPairFrameBoundary(GetTickCount64(), g_state->pairTrackerOn)") == std::string::npos) return "the frame boundary does not tick the shared pair";
    if (v.find("sharedPairShutdown();") == std::string::npos) return "the shared pair is not shut down with the others";
    if (m.find("uiHoloGenericHash(f.vs) && (f.vs != kSharedPairVs || pairLayerAdmits(f.pairClass))") == std::string::npos)
        return "the layer's family rule does not take the pair only as radar or pad";
    return {};
}

void wiringCases() {
    const std::string vscreen = readFile("src/d3d11/vscreen.cpp");
    const std::string math = readFile("src/d3d11/ui_layer_math.h");
    const std::string problem = wiringProblem(vscreen, math);
    check(problem.empty(), problem.empty() ? "wiring" : "wiring: " + problem);

    struct WiringMutant {
        const char* name;
        std::string vscreen, math;
        const char* caught;
    };
    auto replaced = [](std::string text, const std::string& from, const std::string& to) {
        const size_t at = text.find(from);
        check(at != std::string::npos, "wiring mutant: the text to change is there: " + from);
        text.replace(at, from.size(), to);
        return text;
    };
    std::vector<WiringMutant> mutants;
    mutants.push_back({"the marks are never fed", replaced(vscreen, "sharedPairNoteEyeDraw(s->frameNo", "sharedPairNoteEyeDrawX(s->frameNo"), math, "fed from every eye draw"});
    mutants.push_back({"the pair is never classified", replaced(vscreen, "s->pairClassThisDraw = sharedPairClassify(", "s->pairClassThisDraw = sharedPairClassifyX("), math, "not classified where the counter is"});
    mutants.push_back({"the tracker runs whatever is subscribed", replaced(vscreen, "if (s->pairTrackerOn) {", "if (true) {"), math, "not gated on pairTrackerOn"});
    mutants.push_back({"the glare claim is not guarded", replaced(vscreen, "pairGlareMayClaim(s->pairClassThisDraw) &&", ""), math, "not guarded by the class"});
    mutants.push_back({"the class is not cleared each draw", replaced(vscreen, "s->pairClassThisDraw = PairClass::kNotPair;", ""), math, "not cleared before the counter"});
    mutants.push_back({"the layer is never told the class", replaced(vscreen, "f.pairClass = s->pairClassThisDraw;", ""), math, "does not hand the layer"});
    mutants.push_back({"the draw gate forgets the subscription", replaced(vscreen, "        sharedPairWantsDraws() ||\n", ""), math, "draw gate"});
    mutants.push_back({"one gate refresh forgets the tracker", replaced(vscreen, "    s->pairTrackerOn = sharedPairWantsDraws();\n", ""), math, "both gate refreshes"});
    mutants.push_back({"the boundary never ticks", replaced(vscreen, "sharedPairFrameBoundary(GetTickCount64(), g_state->pairTrackerOn)", "0"), math, "does not tick the shared pair"});
    mutants.push_back({"the layer's rule takes the pair as world too", vscreen, replaced(math, "pairLayerAdmits(f.pairClass)", "true"), "only as radar or pad"});
    mutants.push_back({"the guard is fed a constant, not the draw's reference", replaced(vscreen, "isSharedPair(pairVs, pairPs) ? pairReadStencilRef(self) : kPairStencilUnknown", "kPairStencilRef"), math, "not read from the draw"});
    mutants.push_back({"the reference is read and dropped", replaced(vscreen, "pairPs, instances, pairStencil);", "pairPs, instances, kPairStencilRef);"), math, "not handed to the rule"});
    mutants.push_back({"the reference is not read from the bound state", replaced(vscreen, "ctx->OMGetDepthStencilState(&dss, &ref);", "ref = kPairStencilRef;"), math, "OMGetDepthStencilState the way the census reads"});
    mutants.push_back({"the read is not under the pair's own fault budget", replaced(vscreen, "guardedBudget(g_pairStencilBudget,", "guardedBudget(g_billboardCaptureBudget,"), math, "not under a fault budget"});
    mutants.push_back({"an unreadable reference reads as 4", replaced(vscreen, "return got ? static_cast<uint32_t>(ref) : kPairStencilUnknown;", "return static_cast<uint32_t>(ref);"), math, "does not read as unknown"});
    for (const WiringMutant& mu : mutants) {
        const std::string why = wiringProblem(mu.vscreen, mu.math);
        check(mu.vscreen != vscreen || mu.math != math, std::string("wiring mutant is a real edit: ") + mu.name);
        check(!why.empty() && why.find(mu.caught) != std::string::npos,
              std::string("wiring mutant caught by its own rule: ") + mu.name + " (said: " + why + ")");
    }
    std::printf("shared_pair_test: the wiring held by a scan of vscreen.cpp and ui_layer_math.h; %zu mutants of it, each caught by its own rule\n", mutants.size());
}

void run() {
    tableCases();
    ruleCases();
    trackerCases();
    fixtureCases();
    gateCases();
    runtimeCases();
    wiringCases();
    if (!g_failures.empty()) {
        for (const std::string& why : g_failures) std::fprintf(stderr, "FAIL: %s\n", why.c_str());
        throw std::runtime_error(std::to_string(g_failures.size()) + " of " + std::to_string(g_checks) + " shared pair checks failed (--keep-going)");
    }
    std::printf("PASS: %u shared pair checks\n", g_checks);
}

} // namespace
} // namespace edvr

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && !std::strcmp(argv[1], "--dry-run")) {
        std::puts("dry-run: no fixtures, no sources, no files");
        return 0;
    }
    if (argc == 3 && !std::strcmp(argv[1], "--self-test") && !std::strcmp(argv[2], "--keep-going")) {
        g_keepGoing = true;
    } else if (argc != 2 || std::strcmp(argv[1], "--self-test")) {
        std::fputs("usage: --self-test [--keep-going] | --dry-run\n", stderr);
        return 2;
    }
    try {
        edvr::run();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}

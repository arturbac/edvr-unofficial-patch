// The shared pair's rule (src/d3d11/shared_pair.h) and every place that acts on it.
//
// VS 94D5C556DFD6D705 / PS 912477AEF6958379 draws the radar's contact markers, the landing-pad display's rings
// and the sun's glare train. The rule tells them apart by what was drawn before them on their target; the glare
// fix never claims a radar or pad draw, and the UI layer takes the pair only as radar or pad.
//
// What is held here:
//   1. the mark table, against the take list it is cut from;
//   2. the rule's arithmetic, at its edges;
//   3. the per-frame tracker: targets, order, frame reset, capacity, evidence;
//   4. SEQUENCES RECORDED FROM THE DRAW CENSUS (tools\shared_pair_test\fixtures, reduced by
//      tools\pair_class_scan.py): the production tracker, run over each frame's draws in order, must give each
//      draw of the pair the class its capture was judged to be (the eye dump named the display: the pad in
//      the hangar, the pad on the approach, the radar in space) -- and tools\pair_class_scan.py, a second
//      implementation of the rule, holds the same fixtures in its own --self-test;
//   5. NEGATIVE CONTROLS: an independent reference implementation of the rule, mutated one way at a time
//      (no radar flag, no console flag, marks on any target, marks after the draw, no window, an unbounded
//      window, the console first); each mutant must disagree with the recordings somewhere, so the recordings
//      are known to be sensitive to every part of the rule;
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
void check(bool ok, const std::string& why) {
    ++g_checks;
    if (!ok) throw std::runtime_error(why);
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
    check(pairClassFor(100, 0, 0) == PairClass::kWorld, "rule: no marks is world");
    check(pairClassFor(100, 98, 0) == PairClass::kRadar, "rule: a radar family two draws back is radar");
    check(pairClassFor(350, 100, 0) == PairClass::kRadar, "rule: 250 draws back is inside the window");
    check(pairClassFor(351, 100, 0) == PairClass::kWorld, "rule: 251 draws back is outside it");
    check(pairClassFor(300, 0, 100) == PairClass::kPad, "rule: a console draw and no radar family is pad");
    check(pairClassFor(351, 0, 100) == PairClass::kWorld, "rule: a console draw 251 back is outside the window too");
    check(pairClassFor(100, 90, 50) == PairClass::kRadar, "rule: both marks: radar wins");
    check(pairClassFor(600, 100, 500) == PairClass::kPad, "rule: a radar family out of the window does not shadow a console draw in it");
    check(pairClassFor(100, 200, 0) == PairClass::kWorld && pairClassFor(100, 0, 200) == PairClass::kWorld,
          "rule: a mark after the draw does not count");
    check(pairClassFor(100, 100, 0) == PairClass::kWorld, "rule: a mark at the draw's own ordinal is not before it");
    check(kPairWindow == 250, "rule: the window is 250 draws");
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
    PairTracker t;
    PairEvidence ev;
    const uint32_t f = 7;
    t.note(f, &g_a, 10, kHoloContactD);
    check(t.classify(f, &g_a, 11) == PairClass::kRadar, "tracker: a draw after a mark and before the next reads that mark (the tracker is a stream: draws are classified as they arrive)");
    t.note(f, &g_a, 12, kHoloIconCore);
    t.note(f, &g_b, 20, kPairConsoleVsA);
    check(t.classify(f, &g_a, 14, &ev) == PairClass::kRadar && ev.radarBack == 2 && ev.consoleBack == 0 && ev.marksElsewhere,
          "tracker: the last radar mark on the draw's target, its distance, and marks elsewhere in the frame");
    check(t.classify(f, &g_b, 30, &ev) == PairClass::kPad && ev.consoleBack == 10 && ev.radarBack == 0,
          "tracker: another target's own marks: the console draw's distance");
    check(t.classify(f, &g_c, 30, &ev) == PairClass::kWorld && ev.marksElsewhere && ev.radarBack == 0 && ev.consoleBack == 0,
          "tracker: a target with no marks is world, and the evidence says the frame has marks elsewhere");
    check(t.classify(f, &g_a, 12) == PairClass::kWorld, "tracker: a draw at a mark's ordinal is not after it (the mark at 12 is the last noted)");
    // frame change: every mark is cleared, even when no new mark has arrived
    check(t.classify(f + 1, &g_a, 14) == PairClass::kWorld, "tracker: a new frame clears the marks");
    t.note(f + 1, &g_a, 5, kHoloContactA);
    check(t.classify(f + 1, &g_a, 6) == PairClass::kRadar && t.classify(f + 1, &g_b, 40) == PairClass::kWorld,
          "tracker: the new frame's marks, and only its own");
    check(t.classify(f, &g_a, 14) == PairClass::kWorld, "tracker: an old frame's draw does not read the new frame's marks");
    // the pair's own vertex shader and non-marks set nothing
    PairTracker u;
    u.note(1, &g_a, 3, kSharedPairVs);
    u.note(1, &g_a, 4, kHoloCoronaFamily);
    u.note(1, &g_a, 5, 0x1111);
    check(u.classify(1, &g_a, 9, &ev) == PairClass::kWorld && !ev.marksElsewhere,
          "tracker: the pair's own shader, the corona and any other draw are no marks");
    // capacity: four targets are tracked, a fifth reads as no marks
    PairTracker c;
    c.note(1, &g_a, 1, kHoloIconCore);
    c.note(1, &g_b, 2, kHoloIconCore);
    c.note(1, &g_c, 3, kHoloIconCore);
    c.note(1, &g_d, 4, kHoloIconCore);
    c.note(1, &g_e, 5, kHoloIconCore);
    check(c.classify(1, &g_d, 9) == PairClass::kRadar && c.classify(1, &g_e, 9) == PairClass::kWorld,
          "tracker: four targets are tracked; a fifth is not, and fails toward world");
    c.reset();
    check(c.classify(1, &g_a, 9) == PairClass::kWorld, "tracker: reset clears everything");
    // the same mark twice on one target keeps the LAST (nearest) one
    PairTracker n;
    n.note(1, &g_a, 10, kHoloContactB);
    n.note(1, &g_a, 400, kHoloContactB);
    check(n.classify(1, &g_a, 500, &ev) == PairClass::kRadar && ev.radarBack == 100,
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
            char a[64] = {}, b[64] = {}, c[64] = {};
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
            } else if (std::sscanf(line.c_str(), "%c %u %u %63s %63s", &kind, &ordinal, &second, b, c) == 5 && kind == 'p') {
                check(!fx.frames.empty(), fx.name + ": a pair draw before any frame");
                Entry e;
                e.kind = 'p';
                e.ordinal = ordinal;
                e.instances = second;
                e.target = b;
                e.expected = c;
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
        else out.push_back(t.classify(fr.id, target, e.ordinal, last));
    }
    return out;
}

// The reference: the rule written again, plainly, with switches to break one part of it at a time.
enum class Mutant { kNone, kNoRadar, kNoConsole, kAnyTarget, kMarksAfter, kNoWindow, kUnboundedWindow, kConsoleFirst };
const char* mutantName(Mutant m) {
    switch (m) {
        case Mutant::kNoRadar: return "no radar flag";
        case Mutant::kNoConsole: return "no console flag";
        case Mutant::kAnyTarget: return "marks on any target";
        case Mutant::kMarksAfter: return "marks after the draw count";
        case Mutant::kNoWindow: return "a window of zero";
        case Mutant::kUnboundedWindow: return "an unbounded window";
        case Mutant::kConsoleFirst: return "the console tested before the radar";
        default: return "none";
    }
}
std::vector<PairClass> referenceClasses(const Frame& fr, Mutant m) {
    std::vector<PairClass> out;
    const uint32_t window = m == Mutant::kNoWindow ? 0u : m == Mutant::kUnboundedWindow ? 0xFFFFFFFFu : 250u;
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
            if (!before || !sameTarget || back > window) continue;
            const PairMark mark = pairMarkOf(d.vs);
            if (mark == PairMark::kRadar && m != Mutant::kNoRadar) radar = true;
            if (mark == PairMark::kConsole && m != Mutant::kNoConsole) console = true;
        }
        out.push_back(m == Mutant::kConsoleFirst ? (console ? PairClass::kPad : radar ? PairClass::kRadar : PairClass::kWorld)
                                                : (radar ? PairClass::kRadar : console ? PairClass::kPad : PairClass::kWorld));
    }
    return out;
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
                     Mutant::kUnboundedWindow, Mutant::kConsoleFirst}) {
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
    int eyeA = 0, eyeB = 0;
    uint32_t frame = 1;
    // A pad frame: console draws, then the pair's two draws an eye; a radar frame; a glare-shaped draw on the scene target.
    for (int eye = 0; eye < 2; ++eye) {
        const void* rtv = eye ? &eyeB : &eyeA;
        sharedPairNoteEyeDraw(frame, rtv, 100 + eye * 400, kPairConsoleVsB);
        sharedPairNoteEyeDraw(frame, rtv, 102 + eye * 400, kPairConsoleVsA);
        check(sharedPairClassify(frame, rtv, 300 + eye * 400, kSharedPairVs, kSharedPairPs, 3392) == PairClass::kPad, "runtime: a pad draw");
        check(sharedPairClassify(frame, rtv, 301 + eye * 400, kSharedPairVs, kSharedPairPs, 94) == PairClass::kPad, "runtime: the pad's second draw");
    }
    check(sharedPairClassify(frame, &eyeA, 302, kSharedPairVs, 0x1234, 9) == PairClass::kNotPair &&
              sharedPairClassify(frame, &eyeA, 303, 0x1234, kSharedPairPs, 9) == PairClass::kNotPair &&
              sharedPairClassify(frame, &eyeA, 304, kHoloContactD, kSharedPairPs, 9) == PairClass::kNotPair,
          "runtime: another pixel shader, another vertex shader: not the pair, counted nowhere");
    std::vector<std::string> lines = takeLines();
    check(hasLine(lines, "the first draw of the pad class: 3392 instances, a console draw 198 draws before it on its target and no radar family within 250") &&
              !hasLine(lines, "the first draw of the radar class"),
          "runtime: the first pad draw is named once, with its instances and the mark's distance");
    ++frame;
    sharedPairNoteEyeDraw(frame, &eyeA, 189, kHoloContactD);
    sharedPairNoteEyeDraw(frame, &eyeA, 190, kHoloIconCore);
    check(sharedPairClassify(frame, &eyeA, 192, kSharedPairVs, kSharedPairPs, 39) == PairClass::kRadar, "runtime: a radar draw");
    check(sharedPairClassify(frame, &eyeB, 60, kSharedPairVs, kSharedPairPs, 8) == PairClass::kWorld, "runtime: a draw on a target with no marks is world");
    lines = takeLines();
    check(hasLine(lines, "the first draw of the radar class: 39 instances, a radar family drew 2 draws before it") &&
              hasLine(lines, "the first draw of the world class: 8 instances, no radar family or console draw within 250 draws before it on its target; the frame's HUD marks are on another target"),
          "runtime: the first radar and the first world draw are named, the world one with the marks-elsewhere note");
    check(sharedPairClassify(frame, &eyeA, 193, kSharedPairVs, kSharedPairPs, 39) == PairClass::kRadar && takeLines().empty(),
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
    const std::string feed = v.substr(at, 1400);
    if (feed.find("if (s->pairTrackerOn)") == std::string::npos) return "the tracker's feed is not gated on pairTrackerOn";
    if (feed.find("sharedPairNoteEyeDraw(s->frameNo") == std::string::npos) return "the marks are not fed from every eye draw";
    if (feed.find("if (pairVs == kSharedPairVs)") == std::string::npos || feed.find("s->pairClassThisDraw = sharedPairClassify(") == std::string::npos)
        return "a draw of the pair is not classified where the counter is";
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
    if (argc != 2 || std::strcmp(argv[1], "--self-test")) {
        std::fputs("usage: --self-test | --dry-run\n", stderr);
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

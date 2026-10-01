// The rig for the interface composites the layer leaves in the scene, and for the second cockpit holo-panel
// shader pair (docs/ui-layer-2026-09-23.md, "2026-10-01: Disable GUI effects"). No D3D.
//
//   R1       the family rule (src\d3d11\ui_layer_math.h): both holo-panel vertex shaders -- the stock one's and the one the game
//            switches in with Elite's Disable GUI effects on -- name the cockpit holo panels on the lit HDR target, and on the
//            post-tonemap target over a learned surface; a vertex shader one bit away names nothing; the hashes are the
//            disassembled ones (src\d3d11\holo_material.h).
//   R2..R7   the census (src\d3d11\ui_scene_composites.h): the window's table, the zero line, the line with pairs, the cap on its
//            length, the detector-off wording, a window with no frames.
//   R8       the setting off changes nothing: the production family rule against a FROZEN copy of the rule as it stood before the
//            second shader was named, over every combination of target, vertex shader, pixel shader and SRV facts -- the same family
//            and the same reason for everything but the new shader, which answers exactly as the stock one does.
//   tools\ui_composite_census_test\mutants.py compiles this rig against copies of the headers with ONE rule flipped and requires
//   the rig to fail on the case that belongs to the rule (the label of its first FAIL starts with the mutation's label prefix);
//   that is why every check of R1..R8 carries a label "R<n><letter>: ...".
//   P1..P6   the pins, by source scan from the repo root: where the census is wired -- the flag ui_depth publishes and where it is
//            cleared and set, the scope that consumes it, the count after BOTH takes (and that it only reads), the line once a
//            window with its zeros, the frames the count could run in -- that depth is left out for the new pair on purpose, the
//            eye-run diagnostic's watch list, and that the reader's fixture is the formatter's own output (--emit-fixture
//            regenerates it). Each wiring pin carries a control: the same predicate over copies of the source with one edit must
//            fail (--self-test <repo root>; skipped when no root is given, and never run by the mutation tool).
//
// Usage: --self-test [<repo root>] [--only R1,R5,...]   |   --dry-run (no checks run)   |   --emit-fixture
#include "ui_layer_math.h"
#include "ui_scene_composites.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace edvr;

// ---- the harness ---------------------------------------------------------------------------------------------------------
unsigned g_checks = 0;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) throw std::runtime_error(label);
}

// The pair the disassembly named (docs\ui-layer-2026-09-23.md, 2026-10-01) and the stock pair it replaces.
constexpr uint64_t kGuiFxVs = 0x1989E6D3B405FDE0ull, kGuiFxPs = 0xEAB8A1C95A13FFBEull;
constexpr uint64_t kStockVs = 0x81216C77F90DEDD6ull, kStockPs = 0xA2965EC2931A39C8ull;

// ---- R1: the family rule --------------------------------------------------------------------------------------------------------
UiFamilyFacts hdr(uint64_t vs) {
    UiFamilyFacts f;
    f.targetKind = 1;   // the lit HDR eye target
    f.vs = vs;
    return f;
}

void caseR1() {
    UiFamilyWhy why = UiFamilyWhy::kOther;
    check(uiLayerFamilyFor(hdr(kStockVs), &why) == UiLayerFamily::kHolo && why == UiFamilyWhy::kDirect,
          "R1a: the stock holo-panel vertex shader names the cockpit holo panels on the lit HDR target, a direct shader");
    why = UiFamilyWhy::kOther;
    check(uiLayerFamilyFor(hdr(kGuiFxVs), &why) == UiLayerFamily::kHolo && why == UiFamilyWhy::kDirect,
          "R1b: the vertex shader of Disable GUI effects names the cockpit holo panels on the lit HDR target, a direct shader, exactly as the stock one does");
    UiFamilyFacts ldr;
    ldr.targetKind = 2;           // the post-tonemap eye target
    ldr.learnedSurface = true;
    for (uint64_t vs : {kStockVs, kGuiFxVs}) {
        ldr.vs = vs;
        why = UiFamilyWhy::kOther;
        check(uiLayerFamilyFor(ldr, &why) == UiLayerFamily::kHolo && why == UiFamilyWhy::kLearnedSurface,
              "R1c: on the post-tonemap target, over a learned surface, both holo-panel vertex shaders are the holo panels");
    }
    // A vertex shader one bit from either names nothing on the HDR target (the rule is an exact compare), and so does zero.
    bool nothing = true;
    for (uint64_t vs : {kGuiFxVs ^ 1ull, kStockVs ^ 1ull, kGuiFxVs ^ 0x8000000000000000ull, kStockVs ^ 0x100ull, kGuiFxPs, kStockPs, 0ull}) {
        why = UiFamilyWhy::kOther;
        nothing = nothing && uiLayerFamilyFor(hdr(vs), &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kNotPostTonemap;
    }
    check(nothing, "R1d: a vertex shader one bit away from either, a pixel shader's hash, and zero name nothing on the HDR target");
    // Not an eye target: no family, whatever the shaders say.
    for (uint64_t vs : {kStockVs, kGuiFxVs}) {
        UiFamilyFacts f = hdr(vs);
        f.targetKind = 0;
        why = UiFamilyWhy::kOther;
        check(uiLayerFamilyFor(f, &why) == UiLayerFamily::kNone && why == UiFamilyWhy::kNotEyeTarget,
              "R1e: a draw that is not into an eye target is no family, whatever its shaders");
    }
    check(kHoloGuiFxOffVs == kGuiFxVs && kHoloGuiFxOffPs == kGuiFxPs && kUiVsHoloGuiFxOff == kGuiFxVs && kUiVsHolo == kStockVs,
          "R1f: the hashes are the disassembled ones: vs 1989E6D3B405FDE0 / ps EAB8A1C95A13FFBE with the setting on, vs 81216C77F90DEDD6 without");
    check(uiVsIsHoloPanel(kStockVs) && uiVsIsHoloPanel(kGuiFxVs) && !uiVsIsHoloPanel(kUiVsFlightHud) && !uiVsIsHoloPanel(kUiVsSprite) &&
              !uiVsIsHoloPanel(kUiVsPanel) && !uiVsIsHoloPanel(0) && !uiVsIsHoloPanel(kGuiFxPs),
          "R1g: uiVsIsHoloPanel is true for exactly the two holo-panel vertex shaders");
    check(std::strcmp(uiLayerFamilyName(UiLayerFamily::kHolo), "cockpit holo panels") == 0,
          "R1h: the family's census name is unchanged: the 30 s lines and the reader name it \"cockpit holo panels\"");
    UiFamilyWhy wh = UiFamilyWhy::kOther;
    check(uiLayerFamilyFor(hdr(kUiVsFlightHud), &wh) == UiLayerFamily::kFlightHud && uiLayerFamilyFor(hdr(kUiVsSprite)) == UiLayerFamily::kSprite &&
              uiLayerFamilyFor(hdr(kHoloIconCore)) == UiLayerFamily::kHoloGeneric,
          "R1i: the other named HDR families are still named (the flight HUD, the target sprite, a hologram of the take)");
}

// ---- R2: the table --------------------------------------------------------------------------------------------------------------
void caseR2() {
    UiSceneCompositeWindow w;
    check(w.seen == 0 && w.left == 0 && w.pastTable == 0 && w.framesLive == 0 && w.used == 0, "R2a: a fresh window counts nothing");
    w.noteTaken();
    w.noteTaken();
    check(w.seen == 2 && w.left == 0 && w.used == 0, "R2b: a taken composite is seen and not left, and names no pair");
    w.noteLeft(kGuiFxVs, kGuiFxPs, 0);
    check(w.seen == 3 && w.left == 1 && w.used == 1 && w.pairs[0].vs == kGuiFxVs && w.pairs[0].ps == kGuiFxPs && w.pairs[0].family == 0 &&
              w.pairs[0].draws == 1,
          "R2c: a left composite is seen and left, and names its pair: vertex shader, pixel shader, family, one draw");
    w.noteLeft(kGuiFxVs, kGuiFxPs, 0);
    check(w.used == 1 && w.pairs[0].draws == 2 && w.left == 2, "R2d: the same pair again is one entry with two draws");
    w.noteLeft(kGuiFxVs, kStockPs, 0);
    w.noteLeft(kStockVs, kGuiFxPs, 0);
    w.noteLeft(kGuiFxVs, kGuiFxPs, static_cast<int>(UiLayerFamily::kHolo));
    check(w.used == 4 && w.pairs[0].draws == 2 && w.pairs[1].draws == 1 && w.pairs[2].draws == 1 && w.pairs[3].draws == 1,
          "R2e: another pixel shader, another vertex shader, or another family is another entry");
    // The table is full at kUiSceneCompositePairs; the rest are counted, unnamed, and nothing is lost.
    UiSceneCompositeWindow full;
    for (uint64_t i = 0; i < kUiSceneCompositePairs; ++i) full.noteLeft(0x1000 + i, 0x2000 + i, 0);
    check(full.used == kUiSceneCompositePairs && full.pastTable == 0, "R2f: the table holds its pairs");
    full.noteLeft(0x9999, 0x8888, 0);
    full.noteLeft(0x9999, 0x8888, 0);
    full.noteLeft(0x1000, 0x2000, 0);  // a pair the table already names still aggregates
    uint64_t named = 0;
    for (uint32_t i = 0; i < full.used; ++i) named += full.pairs[i].draws;
    check(full.used == kUiSceneCompositePairs && full.pastTable == 2 && full.pairs[0].draws == 2 && named + full.pastTable == full.left &&
              full.left == kUiSceneCompositePairs + 3,
          "R2g: a pair past the table is counted in pastTable, a named one still aggregates, and the pairs and the overflow add up to every left draw");
    w.framesLive = 77;
    w.reset();
    check(w.seen == 0 && w.left == 0 && w.pastTable == 0 && w.framesLive == 0 && w.used == 0 && w.pairs[0].draws == 0 && w.pairs[0].vs == 0,
          "R2h: a window's reset zeroes every counter and the table");
}

// ---- R3: the zero line ----------------------------------------------------------------------------------------------------------
void caseR3() {
    UiSceneCompositeWindow w;
    w.framesLive = 2700;
    for (int i = 0; i < 5400; ++i) w.noteTaken();
    const std::string none = uiSceneCompositeText(w, 2700, true);
    check(none == "ui quality: composites left in the scene: 0 of 5400 composite draws (0.00 a frame) in 2700 frames (2700 live) -- none: every "
                  "interface composite drawn into an eye went into the layer.",
          "R3a: nothing left is a line of its own, with the zero said: 0 of N composite draws, none, every composite went into the layer");
    UiSceneCompositeWindow idle;
    idle.framesLive = 2700;
    const std::string quiet = uiSceneCompositeText(idle, 2700, true);
    check(quiet == "ui quality: composites left in the scene: 0 of 0 composite draws (0.00 a frame) in 2700 frames (2700 live) -- no draw into an eye "
                   "sampled an interface surface in this window.",
          "R3b: no composite drawn at all is told apart from none left: 0 of 0, and the sentence says no draw sampled an interface surface");
    check(none.find('\n') == std::string::npos && quiet.find('\n') == std::string::npos, "R3c: each is one line");
}

// ---- R4: the line with pairs --------------------------------------------------------------------------------------------------
void caseR4() {
    UiSceneCompositeWindow w;
    w.framesLive = 2500;   // not the window's frames: the two are told apart in the line
    for (int i = 0; i < 5120; ++i) w.noteTaken();
    for (int i = 0; i < 56320; ++i) w.noteLeft(kGuiFxVs, kGuiFxPs, 0);
    const std::string line = uiSceneCompositeText(w, 2560, true);
    check(line == "ui quality: composites left in the scene: 56320 of 61440 composite draws (22.00 a frame) in 2560 frames (2500 live) -- vs "
                  "1989E6D3B405FDE0 ps EAB8A1C95A13FFBE (no family) 22.00 a frame.",
          "R4a: composites left name their pair: both hashes in sixteen upper-case hex digits, the family or \"no family\", the draws a frame -- and the "
          "total, the seen count, the frames and the frames the layer was live lead");
    UiSceneCompositeWindow m;
    m.framesLive = 100;
    for (int i = 0; i < 30; ++i) m.noteLeft(kStockVs, kStockPs, static_cast<int>(UiLayerFamily::kHolo));
    for (int i = 0; i < 50; ++i) m.noteLeft(kGuiFxVs, kGuiFxPs, 0);
    const std::string two = uiSceneCompositeText(m, 100, true);
    check(two == "ui quality: composites left in the scene: 80 of 80 composite draws (0.80 a frame) in 100 frames (100 live) -- vs "
                 "81216C77F90DEDD6 ps A2965EC2931A39C8 (cockpit holo panels, not taken) 0.30 a frame; vs 1989E6D3B405FDE0 ps EAB8A1C95A13FFBE (no family) 0.50 a frame.",
          "R4b: a named family is said with its census name and \"not taken\", pairs come in the order first left, separated by semicolons");
    check(line.find('\n') == std::string::npos && two.find('\n') == std::string::npos, "R4c: one line");
    // The rate is draws a FRAME, never draws a composite: 4 of 400 draws over 8 frames is 0.50, not 0.01.
    UiSceneCompositeWindow r;
    for (int i = 0; i < 396; ++i) r.noteTaken();
    for (int i = 0; i < 4; ++i) r.noteLeft(0xAB, 0xCD, 0);
    const std::string rate = uiSceneCompositeText(r, 8, true);
    check(rate.find("(0.50 a frame) in 8 frames") != std::string::npos && rate.find("00000000000000AB ps 00000000000000CD (no family) 0.50 a frame") != std::string::npos,
          "R4d: the rate is per frame of the window, and a hash is padded to sixteen digits");
    // The overflow, named.
    UiSceneCompositeWindow o;
    for (uint64_t i = 0; i < kUiSceneCompositePairs; ++i) o.noteLeft(0x1000 + i, 0x2000 + i, 0);
    for (int i = 0; i < 40; ++i) o.noteLeft(0x7777, 0x6666, 0);
    const std::string past = uiSceneCompositeText(o, 10, true);
    check(past.find("; 40 draws of pairs past the table's 8 (4.00 a frame).") != std::string::npos && past.find("0000000000007777") == std::string::npos,
          "R4e: draws of pairs past the table are said as a count and a rate, and not named");
}

// ---- R5: the length ---------------------------------------------------------------------------------------------------------------
void caseR5() {
    // The Log's line holds 1200 characters; the table is full and every pair carries the longest family name.
    UiSceneCompositeWindow w;
    w.framesLive = 99999;
    for (uint64_t i = 0; i < kUiSceneCompositePairs; ++i)
        for (int k = 0; k < 123456; ++k) w.noteLeft(0xFFFFFFFFFFFFFFF0ull + i, 0xFFFFFFFFFFFFFF00ull + i, static_cast<int>(UiLayerFamily::kGuiDirect));
    for (int k = 0; k < 123456; ++k) w.noteLeft(1, 2, 0);
    const std::string text = uiSceneCompositeText(w, 1, true);
    check(text.size() < 1100, "R5a: the longest line a full table makes is under 1100 characters (the Log's line holds 1200)");
    // The buffer form never overruns and always terminates.
    char guarded[64 + 8];
    std::memset(guarded, 0x5A, sizeof(guarded));
    const size_t n = uiSceneCompositeFormat(guarded, 64, w, 1, true);
    check(n == 63 && guarded[63] == 0 && std::memcmp(guarded, text.data(), 63) == 0, "R5b: a small buffer gets the line's head, terminated, no more than it holds");
    bool intact = true;
    for (size_t i = 64; i < sizeof(guarded); ++i) intact = intact && guarded[i] == 0x5A;
    check(intact, "R5c: nothing is written past the buffer's end");
    char one[1] = {'x'};
    check(uiSceneCompositeFormat(one, 1, w, 1, true) == 0 && one[0] == 0 && uiSceneCompositeFormat(nullptr, 0, w, 1, true) == 0,
          "R5d: a buffer of one is terminated and a null buffer is refused");
    char big[1100];
    const size_t m = uiSceneCompositeFormat(big, sizeof(big), w, 1, true);
    check(m == text.size() && std::string(big) == text, "R5e: a buffer that holds the line gets all of it");
}

// ---- R6: the detector off -------------------------------------------------------------------------------------------------------
void caseR6() {
    UiSceneCompositeWindow w;   // nothing counted: the pass that finds composites was not running
    const std::string off = uiSceneCompositeText(w, 2700, false);
    check(off == "ui quality: composites left in the scene: NOT COUNTED (the interface depth pass is not running: fix.temporal_aa is off or it stood "
                 "down, so no draw is recognised as a composite) in 2700 frames.",
          "R6a: with the interface depth pass off the line says NOT COUNTED and why -- never a zero that means nothing");
    UiSceneCompositeWindow some;
    some.noteLeft(1, 2, 0);
    check(uiSceneCompositeText(some, 2700, false) == off, "R6b: ...whatever the window holds");
    check(uiSceneCompositeText(some, 2700, true) != off, "R6c: ...and with the pass running the same window is a count");
}

// ---- R7: a window with no frames ------------------------------------------------------------------------------------------------
void caseR7() {
    UiSceneCompositeWindow w;
    w.noteLeft(kGuiFxVs, kGuiFxPs, 0);
    w.noteTaken();
    const std::string line = uiSceneCompositeText(w, 0, true);
    check(line.find("(0.00 a frame) in 0 frames (0 live)") != std::string::npos && line.find("(no family) 0.00 a frame") != std::string::npos &&
              line.find("nan") == std::string::npos && line.find("inf") == std::string::npos,
          "R7a: a window with no frames says 0.00 a frame, not a division by zero");
}

// ---- R8: with the setting off the rule is the rule that was ---------------------------------------------------------------------
// A frozen copy of uiLayerFamilyFor as it stood on main (24d4c6c4) before the second holo-panel vertex shader was named. The grid below holds the
// production rule to it for EVERY input but the new shader's: the stock pair -- Disable GUI effects off, nearly every cockpit -- and every other
// draw get the same answer and the same reason as before, not merely "still taken". The new shader answers exactly as the stock one does, and
// the frozen copy says what it answered before (nothing: the field's defect).
UiLayerFamily frozenFamilyFor(const UiFamilyFacts& f, UiFamilyWhy* why) {
    UiFamilyWhy w = UiFamilyWhy::kOther;
    UiLayerFamily out = UiLayerFamily::kNone;
    if (f.targetKind == 0) {
        w = UiFamilyWhy::kNotEyeTarget;
    } else if (f.targetKind == 1) {
        w = UiFamilyWhy::kNotPostTonemap;
        out = f.vs == kUiVsHolo        ? UiLayerFamily::kHolo
              : f.vs == kUiVsFlightHud ? UiLayerFamily::kFlightHud
              : f.vs == kUiVsSprite    ? UiLayerFamily::kSprite
              : (uiHoloGenericHash(f.vs) ||
                 (f.vs == kHoloTargetSphere &&
                  (f.ps == 0xEA02FAC2BD6C643Cull || f.ps == 0xE95634B0F61D218Full)))
                  ? UiLayerFamily::kHoloGeneric : UiLayerFamily::kNone;
        if (out != UiLayerFamily::kNone) w = UiFamilyWhy::kDirect;
    } else if (f.excluded) {
        w = UiFamilyWhy::kExcluded;
    } else if (f.panelSized) {
        w = UiFamilyWhy::kScreen;
        out = UiLayerFamily::kScreen;
    } else if (f.learnedSurface) {
        w = UiFamilyWhy::kLearnedSurface;
        out = f.vs == kUiVsPanel    ? UiLayerFamily::kPanel
              : f.vs == kUiVsLoader ? UiLayerFamily::kLoader
              : f.vs == kUiVsHolo   ? UiLayerFamily::kHolo
              : f.vs == kUiVsSprite ? UiLayerFamily::kSprite
                                    : UiLayerFamily::kSurface;
    } else if (f.vs == kUiVsPanel && uiKnownPs(kUiPanelPs, sizeof(kUiPanelPs) / sizeof(kUiPanelPs[0]), f.ps)) {
        w = UiFamilyWhy::kShaderPair;
        out = UiLayerFamily::kPanel;
    } else if (f.vs == kUiVsLoader &&
               uiKnownPs(kUiLoaderPs, sizeof(kUiLoaderPs) / sizeof(kUiLoaderPs[0]), f.ps)) {
        w = UiFamilyWhy::kShaderPair;
        out = UiLayerFamily::kLoader;
    } else if (f.vs == kUiVsGuiVector || f.vs == kUiVsGuiText || f.vs == kUiVsGuiIcons) {
        w = UiFamilyWhy::kDirect;
        out = UiLayerFamily::kGuiDirect;
    } else if (f.vs == kUiVsFlightHud) {
        w = UiFamilyWhy::kDirect;
        out = UiLayerFamily::kFlightHud;
    } else if (f.vs == kUiVsPanel || f.vs == kUiVsLoader) {
        w = UiFamilyWhy::kNoSurface;
    }
    if (why) *why = w;
    return out;
}

void caseR8() {
    const uint64_t vsList[] = {kStockVs, kGuiFxVs, kUiVsFlightHud, kUiVsSprite, kUiVsPanel, kUiVsLoader, kUiVsGuiVector, kUiVsGuiText, kUiVsGuiIcons,
                               kHoloIconCore, kHoloIconStalkA, kHoloIconStalkB, kHoloContactA, kHoloContactB, kHoloContactC, kHoloContactD,
                               kHoloContactE, kHoloTargetSphere, kHoloCoronaFamily, kHoloCanopy, kHoloWorldMarkerReticle, kGuiFxVs ^ 1ull,
                               kStockVs ^ 1ull, 0x1234ull, 0ull};
    const uint64_t psList[] = {0ull, kUiPanelPs[0], kUiPanelPs[1], kUiPanelPs[2], kUiPanelPs[3], kUiLoaderPs[0], kUiLoaderPs[1], 0xEA02FAC2BD6C643Cull,
                               0xE95634B0F61D218Full, 0x2D037A047171BF3Bull, kStockPs, kGuiFxPs, 0xDEADBEEFCAFEF00Dull};
    unsigned compared = 0, differing = 0, effectsOffAsStock = 0, effectsOffBefore = 0;
    for (int kind = 0; kind <= 2; ++kind)
        for (uint64_t vs : vsList)
            for (uint64_t ps : psList)
                for (int bits = 0; bits < 8; ++bits) {
                    UiFamilyFacts f;
                    f.targetKind = kind;
                    f.vs = vs;
                    f.ps = ps;
                    f.excluded = (bits & 1) != 0;
                    f.panelSized = (bits & 2) != 0;
                    f.learnedSurface = (bits & 4) != 0;
                    UiFamilyFacts g = f;
                    g.vs = vs == kGuiFxVs ? kStockVs : vs;   // the new shader answers as the stock one would
                    UiFamilyWhy wNew = UiFamilyWhy::kOther, wOld = UiFamilyWhy::kOther;
                    const UiLayerFamily a = uiLayerFamilyFor(f, &wNew), b = frozenFamilyFor(g, &wOld);
                    ++compared;
                    if (a != b || wNew != wOld) ++differing;
                    if (vs == kGuiFxVs) {
                        ++effectsOffAsStock;
                        UiFamilyWhy wBefore = UiFamilyWhy::kOther;
                        // what the rule said about the new shader before it was named: no holo panel (on the HDR target, no family at all)
                        if (frozenFamilyFor(f, &wBefore) != UiLayerFamily::kHolo) ++effectsOffBefore;
                    }
                }
    check(compared == 3 * 25 * 13 * 8 && differing == 0,
          "R8a: for every input but the new shader's -- 3 targets x 25 vertex shaders x 13 pixel shaders x 8 states of the SRV facts -- the rule's family and reason "
          "are exactly the frozen rule's (the stock pair and every other draw are unchanged); the new shader answers exactly as the stock one");
    check(effectsOffAsStock == 3 * 13 * 8 && effectsOffBefore == effectsOffAsStock,
          "R8b: the frozen rule never named the new shader a holo panel, on any target and with any facts: the field's defect, kept in the rig as its control");
}

// ---- the reader's fixture ---------------------------------------------------------------------------------------------------------
// tools\ui_composites_fixture.log is the synthetic flight the reader's self-test (tools\edvr_log.py --ui-composites) parses: every
// "composites left in the scene" line in it is the formatter's own output for the numbers below (the rig compares the file to this text,
// byte for byte, so a formatter that changes fails HERE and the fixture is regenerated: ui_composite_census_test --emit-fixture >
// tools\ui_composites_fixture.log); the other lines are literal. It is a CATALOGUE of the shapes the line takes, one window of 30 s each,
// in an order the reader's self-test picks from by clock time -- not one flight: a log is one build. The windows:
//   16:00:30  a menu, all taken            16:01:00  nothing drawn into an eye       16:01:30  a cockpit, nothing left (the fixed build)
//   16:02:00  the defect: the Disable-GUI-effects panels left, no family names them (22.00 a frame), and again at 16:02:30
//   16:03:00  a named family left (the layer was not armed for a few frames)         16:03:30  more pairs than the table holds
//   16:04:00  the interface depth pass off, NOT COUNTED                               16:04:30  the layer live for half the window
std::string fixtureText() {
    std::string out;
    auto add = [&](const char* ts, const std::string& text) {
        out += "[";
        out += ts;
        out += "] ";
        out += text;
        out += "\n";
    };
    add("16:00:00.000", "version v0.0.0-fixture (build 00000000) -- this DLL was linked 2026-10-01 00:00:00 UTC");
    UiSceneCompositeWindow w;
    // A menu: composites drawn into the post-tonemap eye, all taken.
    w.framesLive = 2700;
    for (int i = 0; i < 5400; ++i) w.noteTaken();
    add("16:00:30.000", "ui quality: layer: 30 s, 2700 frames, 4032x3898 per eye + composite output 4032x3898; 2.00 draws a frame redirected (menu panel 2.00), 0.00 multiplies.");
    add("16:00:30.001", uiSceneCompositeText(w, 2700, true));
    // Nothing drawn into an eye that samples an interface surface (a loading screen's black frames).
    w = UiSceneCompositeWindow{};
    w.framesLive = 2700;
    add("16:01:00.001", uiSceneCompositeText(w, 2700, true));
    // The cockpit with the pair taken (the build with the fix; or the setting off): 24 composite draws a frame, all in the layer.
    w = UiSceneCompositeWindow{};
    w.framesLive = 2560;
    for (int i = 0; i < 61440; ++i) w.noteTaken();
    add("16:01:30.000", "ui quality: layer: 30 s, 2560 frames, 4032x3898 per eye + composite output 4032x3898; 26.55 draws a frame redirected (cockpit holo panels 22.00, flight HUD 4.81, target sprite 2.00), 0.00 multiplies.");
    add("16:01:30.001", uiSceneCompositeText(w, 2560, true));
    // The defect (the build before the fix, the setting on): the panels left in the scene, no family names them. Twice, a window apart.
    w = UiSceneCompositeWindow{};
    w.framesLive = 2560;
    for (int i = 0; i < 5120; ++i) w.noteTaken();
    for (int i = 0; i < 56320; ++i) w.noteLeft(kGuiFxVs, kGuiFxPs, 0);
    add("16:02:00.001", uiSceneCompositeText(w, 2560, true));
    add("16:02:30.001", uiSceneCompositeText(w, 2560, true));
    // A named family left: the layer was not armed for the first frames of the window.
    w = UiSceneCompositeWindow{};
    w.framesLive = 2560;
    for (int i = 0; i < 61410; ++i) w.noteTaken();
    for (int i = 0; i < 30; ++i) w.noteLeft(kStockVs, kStockPs, static_cast<int>(UiLayerFamily::kHolo));
    add("16:03:00.001", uiSceneCompositeText(w, 2560, true));
    // More pairs than the table holds.
    w = UiSceneCompositeWindow{};
    w.framesLive = 2560;
    for (uint64_t i = 0; i < kUiSceneCompositePairs; ++i)
        for (int k = 0; k < 20; ++k) w.noteLeft(0x5000 + i, 0x6000 + i, 0);
    for (int k = 0; k < 4000; ++k) w.noteLeft(0x7777, 0x6666, 0);
    add("16:03:30.001", uiSceneCompositeText(w, 2560, true));
    // The interface depth pass off.
    w = UiSceneCompositeWindow{};
    w.framesLive = 2560;
    add("16:04:00.001", uiSceneCompositeText(w, 2560, false));
    // The layer live for half the window.
    w = UiSceneCompositeWindow{};
    w.framesLive = 1280;
    for (int i = 0; i < 30720; ++i) w.noteTaken();
    add("16:04:30.001", uiSceneCompositeText(w, 2560, true));
    return out;
}

// ---- the pins, by source scan ---------------------------------------------------------------------------------------------
std::string g_root;
std::string readFile(const char* rel) {
    std::ifstream in(g_root + "\\" + rel, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}
// The code with its comments and whitespace removed, so a check reads statements, not layout (string and character literals are kept whole).
std::string squeeze(const std::string& s) {
    std::string t;
    enum { kCode, kString, kChar, kLine, kBlock } state = kCode;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i], next = i + 1 < s.size() ? s[i + 1] : '\0';
        switch (state) {
            case kCode:
                if (c == '/' && next == '/') {
                    state = kLine;
                    ++i;
                } else if (c == '/' && next == '*') {
                    state = kBlock;
                    ++i;
                } else if (c == '"') {
                    state = kString;
                    t += c;
                } else if (c == '\'') {
                    state = kChar;
                    t += c;
                } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                    t += c;
                }
                break;
            case kString:
                t += c;
                if (c == '\\' && next) {
                    t += next;
                    ++i;
                } else if (c == '"') {
                    state = kCode;
                }
                break;
            case kChar:
                t += c;
                if (c == '\\' && next) {
                    t += next;
                    ++i;
                } else if (c == '\'') {
                    state = kCode;
                }
                break;
            case kLine:
                if (c == '\n') state = kCode;
                break;
            case kBlock:
                if (c == '*' && next == '/') {
                    state = kCode;
                    ++i;
                }
                break;
        }
    }
    return t;
}
// The squeezed text of the function whose definition starts at `head` (first occurrence), up to its closing brace at column 0.
std::string functionBody(const std::string& src, const char* head) {
    const size_t at = src.find(head);
    if (at == std::string::npos) return {};
    size_t end = src.find("\n}\n", at);
    if (end == std::string::npos) end = src.size();
    return squeeze(src.substr(at, end - at + 2));
}
bool has(const std::string& s, const char* piece) { return s.find(squeeze(piece)) != std::string::npos; }
size_t countOf(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) ++n;
    return n;
}
bool inOrder(const std::string& s, std::initializer_list<const char*> pieces) {
    size_t prev = 0;
    for (const char* p : pieces) {
        const size_t at = s.find(squeeze(p), prev);
        if (at == std::string::npos) return false;
        prev = at;
    }
    return true;
}
// `text` with the first occurrence of `from` replaced by `to` (both squeezed first); text unchanged when `from` is absent.
std::string edited(const std::string& text, const char* from, const char* to) {
    const std::string f = squeeze(from);
    const size_t at = text.find(f);
    if (at == std::string::npos) return text;
    std::string out = text;
    out.replace(at, f.size(), squeeze(to));
    return out;
}

void pins() {
    if (g_root.empty()) {
        std::puts("  pins: no repo root given (--self-test <root>); the source pins were not run");
        return;
    }
    const std::string math = readFile("src\\d3d11\\ui_layer_math.h");
    const std::string depth = readFile("src\\d3d11\\ui_depth.cpp");
    const std::string vsc = readFile("src\\d3d11\\vscreen.cpp");
    const std::string layer = readFile("src\\d3d11\\ui_layer.cpp");
    check(!math.empty() && !depth.empty() && !vsc.empty() && !layer.empty(), "P0: the four sources are readable from the repo root");

    // P1: the family rule asks uiVsIsHoloPanel in BOTH branches and the stock hash is compared nowhere else, so neither branch can name only the
    // stock panels.
    const std::string rule = functionBody(math, "inline UiLayerFamily uiLayerFamilyFor(");
    const auto rulePlaces = [](const std::string& body, const std::string& whole) {
        return countOf(body, "uiVsIsHoloPanel(f.vs)?UiLayerFamily::kHolo") == 2 && countOf(whole, "f.vs==kUiVsHolo") == 0 &&
               has(whole, "constexpr bool uiVsIsHoloPanel(uint64_t vs) { return vs == kUiVsHolo || vs == kUiVsHoloGuiFxOff; }");
    };
    const std::string mathAll = squeeze(math);
    check(!rule.empty() && rulePlaces(rule, mathAll),
          "P1: uiLayerFamilyFor names the holo panels through uiVsIsHoloPanel in both its branches, and nothing in ui_layer_math.h compares the stock holo hash by itself");
    {
        const std::string one = "uiVsIsHoloPanel(f.vs)?UiLayerFamily::kHolo";
        std::string noHdr = rule, noLdr = rule, stockOnly = mathAll;
        const size_t a = noHdr.find(squeeze(one));
        if (a != std::string::npos) noHdr.replace(a, squeeze(one).size(), "f.vs==kUiVsHolo?UiLayerFamily::kHolo");
        const size_t b = noLdr.rfind(squeeze(one));
        if (b != std::string::npos) noLdr.replace(b, squeeze(one).size(), "f.vs==kUiVsHolo?UiLayerFamily::kHolo");
        stockOnly = edited(mathAll, "constexpr bool uiVsIsHoloPanel(uint64_t vs) { return vs == kUiVsHolo || vs == kUiVsHoloGuiFxOff; }",
                           "constexpr bool uiVsIsHoloPanel(uint64_t vs) { return vs == kUiVsHolo; }");
        check(!rulePlaces(noHdr, noHdr) && !rulePlaces(noLdr, noLdr) && !rulePlaces(rule, stockOnly),
              "P1 control: with the HDR branch, the post-tonemap branch or the predicate naming only the stock panels, the pin fails");
    }

    // P2: ui_depth publishes the composite flag from the loop that already finds the surface: false first, before any early return, and set
    // from the same `composite` the pass classifies by. And depth is LEFT OUT for the new pair on purpose: no stand-in names it and the
    // hologram family list does not, because the stand-in's input signature (TEXCOORD4/7/8) is not the new vertex shader's output.
    const std::string onEye = functionBody(depth, "bool uiDepthOnEyeDraw(ID3D11DeviceContext* ctx, const HoloDraw& draw) {");
    const auto depthPlaces = [](const std::string& body) {
        return inOrder(body, {"detail::g_uiDepthDrawComposite = false;", "if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown) return false;",
                              "const bool composite = surfaceSlot >= 0;", "detail::g_uiDepthDrawComposite = composite;", "const uint64_t h = boundVsHash(ctx);"}) &&
               countOf(body, "detail::g_uiDepthDrawComposite=composite;") == 1 && countOf(body, "detail::g_uiDepthDrawComposite=false;") == 1;
    };
    check(depthPlaces(onEye),
          "P2a: uiDepthOnEyeDraw clears the composite flag first (before its early returns) and sets it once, from the composite it classifies by");
    {
        std::string late = onEye, missing = onEye, twice = onEye;
        late = edited(late, "detail::g_uiDepthDrawComposite = false;", "");
        late = edited(late, "if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown) return false;",
                      "if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown) return false; detail::g_uiDepthDrawComposite = false;");
        missing = edited(missing, "detail::g_uiDepthDrawComposite = composite;", "");
        twice = edited(twice, "detail::g_uiDepthDrawComposite = composite;", "detail::g_uiDepthDrawComposite = composite; detail::g_uiDepthDrawComposite = composite;");
        check(!depthPlaces(late) && !depthPlaces(missing) && !depthPlaces(twice),
              "P2a control: cleared after the early returns, never set, or set twice, the pin fails");
    }
    // ...and the new pair appears in ui_depth.cpp ONLY where the one-shot family note says why there is none: the stand-in table and the hologram-depth
    // family list do not name it, and neither does anything else.
    const std::string depthAll = squeeze(depth);
    const std::string standIns = [&] {
        const size_t a = depth.find("DepthShader g_depthShaders[");
        const size_t b = a == std::string::npos ? a : depth.find("};", a);
        return a == std::string::npos || b == std::string::npos ? std::string() : squeeze(depth.substr(a, b - a + 2));
    }();
    const std::string familyList = functionBody(depth, "uint32_t holoBuildFamilyList(");
    const auto depthLeftOut = [](const std::string& all, const std::string& table, const std::string& list) {
        return !table.empty() && !list.empty() && countOf(all, "kHoloGuiFxOffVs") == 1 && countOf(all, "kHoloGuiFxOffPs") == 0 &&
               has(all, "noteFamily(h, ph, h == kHoloGuiFxOffVs ?") && countOf(all, "0x1989E6D3B405FDE0") == 0 && countOf(all, "0xEAB8A1C95A13FFBE") == 0 &&
               table.find("kHoloGuiFxOff") == std::string::npos && list.find("kHoloGuiFxOff") == std::string::npos;
    };
    check(depthLeftOut(depthAll, standIns, familyList),
          "P2b: ui_depth.cpp names the Disable-GUI-effects vertex shader once, in its family note, and neither shader in the stand-in table or the hologram-depth family list: "
          "no depth for it. The stand-in (kHoloDepthHlsl) reads TEXCOORD4, TEXCOORD7 and TEXCOORD8 and the new vertex shader writes TEXCOORD0 and TEXCOORD6 only "
          "(docs/ui-layer-2026-09-23.md, 2026-10-01); a stand-in for it is new HLSL, not a table entry");
    {
        const std::string entry = "{{kHoloGuiFxOffPs,0,0,0},{kHoloGuiFxOffVs,0,0,0},2,kUiDepthHoloBytecode,sizeof(kUiDepthHoloBytecode),\"x\",nullptr,false},";
        std::string inTable = standIns, inList = familyList, ps = depthAll, twice = depthAll;
        inTable.insert(inTable.size() - 2, entry);
        inList.insert(inList.size() - 1, "out[count++]=kHoloGuiFxOffVs;");   // before the function's closing brace
        ps += "kHoloGuiFxOffPs";
        twice += "kHoloGuiFxOffVs";
        check(!depthLeftOut(depthAll, inTable, familyList) && !depthLeftOut(depthAll, standIns, inList) && !depthLeftOut(ps, standIns, familyList) &&
                  !depthLeftOut(twice, standIns, familyList),
              "P2b control: with the pair in the stand-in table, in the hologram-depth family list, its pixel shader named, or its vertex shader named a second time, the pin fails");
    }

    // P3: vscreen.cpp. The flag is cleared with the other per-draw flags, set right after the pass's own call (inside the pass's gate), taken and
    // cleared by the scope, and the census is settled once, AFTER both takes -- the family's and the after-UI retry's -- and before the loader
    // panel's and the curve's substitutions.
    const std::string fwd = functionBody(vsc, "void forwardWithVerdict(ID3D11DeviceContext* self, DrawVerdict v,");
    const std::string beginOverride = functionBody(vsc, "DrawVerdict beginPanelOverride(ID3D11DeviceContext* self, char kind, UINT count,");
    // The count only READS: between its opening test and the substitutions that follow it, nothing is assigned to the draw's own state (the take, the
    // world re-issue), nothing is issued and nothing returns -- so with the setting off, or on, it can change no draw and no picture.
    const auto countOnlyReads = [](const std::string& body) {
        const size_t a = body.find(squeeze("if (compositeCounted) {"));
        const size_t b = a == std::string::npos ? a : body.find(squeeze("if (v == DrawVerdict::kLoaderPanel) {"), a);
        if (a == std::string::npos || b == std::string::npos) return false;
        const std::string block = body.substr(a, b - a);
        return block.find("uiLayer=") == std::string::npos && block.find("worldReissue") == std::string::npos && block.find("draw(") == std::string::npos &&
               block.find("return") == std::string::npos && block.find("uiLayerBegin") == std::string::npos && block.find("g_state") == std::string::npos;
    };
    const auto vsPlaces = [&countOnlyReads](const std::string& begin, const std::string& body) {
        return countOnlyReads(body) &&
               inOrder(begin, {"t_uiDepthThisDraw = false;", "t_compositeThisDraw = false;", "if (!drawGateWanted()) {"}) &&
               has(begin, "if (uiDepthWantsDraws()) { t_uiDepthThisDraw = uiDepthOnEyeDraw(") && has(begin, "t_compositeThisDraw = uiDepthDrawSampledSurface(); }") &&
               countOf(begin, "t_compositeThisDraw=uiDepthDrawSampledSurface();") == 1 &&
               has(body, "composite(t_compositeThisDraw)") && has(body, "t_compositeThisDraw = false;") &&
               inOrder(body, {"uiLayer = uiLayerDecide(self, static_cast<int>(uiFamily)", "uiLayer = uiLayerNoteOther(", "if (compositeCounted) {",
                              "uiLayerNoteCompositeTaken();", "uiLayerNoteCompositeLeft(bindingShaderHash(BindSlot::Vs), bindingShaderHash(BindSlot::Ps),",
                              "if (v == DrawVerdict::kLoaderPanel) {"}) &&
               has(body, "compositeCounted = uiDepthScope.composite;") && has(body, "compositeFamily = uiFamily;") &&
               countOf(body, "uiLayerNoteCompositeTaken();") == 1 && countOf(body, "uiLayerNoteCompositeLeft(") == 1;
    };
    check(!fwd.empty() && !beginOverride.empty() && vsPlaces(beginOverride, fwd),
          "P3: vscreen.cpp clears the composite flag with the other per-draw flags, sets it right after uiDepthOnEyeDraw, consumes it in the scope, and counts the composite "
          "once, after the family's take and the after-UI retry and before the substitutions");
    {
        const std::string taken = "uiLayerNoteCompositeTaken();";
        std::string early = fwd, noLeft = fwd, extraTaken = fwd, notCleared = beginOverride, notSet = beginOverride, notConsumed = fwd, writes = fwd, returns = fwd;
        writes = edited(writes, "if (compositeCounted) {", "if (compositeCounted) { uiLayer = false;");
        returns = edited(returns, "uiLayerNoteCompositeTaken();", "uiLayerNoteCompositeTaken(); return;");
        // the count moved in front of the after-UI retry
        early = edited(early, "if (compositeCounted) {", "if (false) {");
        early = edited(early, "uiLayer = uiLayerNoteOther(", "if (compositeCounted) { uiLayerNoteCompositeTaken(); } uiLayer = uiLayerNoteOther(");
        noLeft = edited(noLeft, "uiLayerNoteCompositeLeft(", "uiLayerNoteOtherThing(");
        extraTaken = edited(extraTaken, taken.c_str(), "uiLayerNoteCompositeTaken(); uiLayerNoteCompositeTaken();");
        notCleared = edited(notCleared, "t_compositeThisDraw = false;", "");
        notSet = edited(notSet, "t_compositeThisDraw = uiDepthDrawSampledSurface();", "");
        notConsumed = edited(notConsumed, "composite(t_compositeThisDraw)", "composite(false)");
        check(!vsPlaces(beginOverride, early) && !vsPlaces(beginOverride, noLeft) && !vsPlaces(beginOverride, extraTaken) && !vsPlaces(notCleared, fwd) &&
                  !vsPlaces(notSet, fwd) && !vsPlaces(beginOverride, notConsumed) && !vsPlaces(beginOverride, writes) && !vsPlaces(beginOverride, returns),
              "P3 control: counted before the after-UI retry, with no left-composite call, with a second taken call, with the flag never cleared, never set, "
              "not consumed by the scope, with the count assigning the take or returning, the pin fails");
    }

    // P4: ui_layer.cpp. The line is printed once a window, between "left in the game's frame" and the family census, with the pass's own state; the
    // window struct carries the census (so the window's reset is the census's); the two notes are one-liners into it; the frames the count could run
    // in are counted at the boundary, once, under live.
    const std::string logTot = functionBody(layer, "void logTotals(double seconds) {");
    const std::string layerAll = squeeze(layer);
    const auto layerPlaces = [](const std::string& body, const std::string& whole) {
        return countOf(whole, "uiSceneCompositeFormat(") == 1 &&
               inOrder(body, {"\"ui quality: left in the game's frame: %s.\"", "uiSceneCompositeFormat(composites, sizeof(composites), g_win.scene, g_win.frames, uiDepthWantsDraws());",
                              "\"ui quality: families: %s.\""}) &&
               has(whole, "UiSceneCompositeWindow scene;") && has(whole, "void uiLayerNoteCompositeTaken() { g_win.scene.noteTaken(); }") &&
               has(whole, "void uiLayerNoteCompositeLeft(uint64_t vs, uint64_t ps, int family) { g_win.scene.noteLeft(vs, ps, family); }") &&
               countOf(whole, "++g_win.scene.framesLive;") == 1 && has(whole, "if (detail::g_uiLayerLive) ++g_win.scene.framesLive;") &&
               inOrder(whole, {"++g_win.frames;", "if (detail::g_uiLayerLive) ++g_win.scene.framesLive;", "g_frameTakenDraws = 0;"}) &&
               countOf(whole, "g_win=Window{};") == 1;
    };
    check(!logTot.empty() && layerPlaces(logTot, layerAll),
          "P4: the layer prints the composite line once a window, in logTotals between \"left in the game's frame\" and the families line, with the pass's own state; the census is "
          "a member of the window struct, the notes are one-liners into it, and the frames it could run in are counted once at the boundary under live");
    {
        std::string twice = layerAll, wrongState = logTot, noLive = layerAll, notMember = layerAll, outOfOrder = logTot;
        twice += squeeze("uiSceneCompositeFormat(composites, sizeof(composites), g_win.scene, g_win.frames, true);");
        wrongState = edited(wrongState, "uiDepthWantsDraws());", "true);");
        noLive = edited(noLive, "if (detail::g_uiLayerLive) ++g_win.scene.framesLive;", "++g_win.scene.framesLive;");
        notMember = edited(notMember, "UiSceneCompositeWindow scene;", "");
        outOfOrder = edited(outOfOrder, "\"ui quality: families: %s.\"", "\"ui quality: families moved: %s.\"");
        check(!layerPlaces(logTot, twice) && !layerPlaces(wrongState, layerAll) && !layerPlaces(logTot, noLive) && !layerPlaces(logTot, notMember) && !layerPlaces(outOfOrder, layerAll),
              "P4 control: printed twice, with the detector's state faked, with the live frames counted unconditionally, with the census out of the window struct, or with the line "
              "not before the families line, the pin fails");
    }

    // P5: the reader's fixture is the formatter's own output (a formatter that changes fails here; --emit-fixture regenerates it).
    std::string fixture = readFile("tools\\ui_composites_fixture.log");
    fixture.erase(std::remove(fixture.begin(), fixture.end(), '\r'), fixture.end());
    check(!fixture.empty() && fixture == fixtureText(),
          "P5: tools\\ui_composites_fixture.log is exactly what the formatter writes for its numbers (regenerate: ui_composite_census_test --emit-fixture > tools\\ui_composites_fixture.log)");

    // P6: the eye-run diagnostic records the new pair too: its watch list and its surface capture name the Disable-GUI-effects vertex shader beside the stock one
    // (a run on such a rig otherwise misses the panels and the shader bytes). Behaviour is held by tools\eye_draw_snapshot_test; this keeps the two names together.
    const std::string snap = squeeze(readFile("src\\d3d11\\eye_draw_snapshot.h"));
    check(has(snap, "case kHolo: case kHoloGuiFxOff: case kHud:") && has(snap, "if (vs==kHolo || vs==kHoloGuiFxOff || vs==kSprite") &&
              has(snap, "(vs==kHolo || vs==kHoloGuiFxOff)?holoSurfaceSlot(ps)"),
          "P6: the eye-run snapshot watches and captures the Disable-GUI-effects holo vertex shader beside the stock one");
}

// ---- the runner ------------------------------------------------------------------------------------------------------------
struct Case {
    const char* id;
    void (*run)();
};
const Case kCases[] = {{"R1", caseR1}, {"R2", caseR2}, {"R3", caseR3}, {"R4", caseR4},
                       {"R5", caseR5}, {"R6", caseR6}, {"R7", caseR7}, {"R8", caseR8}};

bool selected(const std::string& only, const char* id) {
    if (only.empty()) return true;
    return ("," + only + ",").find(std::string(",") + id + ",") != std::string::npos;
}

int run(const std::string& only, bool withPins) {
    for (const char* id = only.c_str(); *id;) {
        const char* comma = std::strchr(id, ',');
        const std::string one = comma ? std::string(id, comma) : std::string(id);
        bool known = false;
        for (const Case& c : kCases) known = known || one == c.id;
        if (!known) {
            std::fprintf(stderr, "FAIL: --only names a case that does not exist: %s\n", one.c_str());
            return 1;
        }
        id = comma ? comma + 1 : id + one.size();
    }
    unsigned ran = 0;
    try {
        for (const Case& c : kCases) {
            if (!selected(only, c.id)) continue;
            c.run();
            ++ran;
        }
        if (withPins) pins();
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    std::printf("PASS: %u composite census checks (%u cases%s)\n", g_checks, ran, withPins && !g_root.empty() ? ", pins" : "");
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    std::string only;
    bool self = false, dry = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--emit-fixture")) {
            const std::string text = fixtureText();
#ifdef _WIN32
            _setmode(_fileno(stdout), _O_BINARY);   // LF, not the console's CRLF: the file is checked in as written
#endif
            std::fwrite(text.data(), 1, text.size(), stdout);
            return 0;
        }
        if (!std::strcmp(argv[i], "--self-test")) self = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (argv[i][0] != '-') g_root = argv[i];
        else {
            std::fputs("usage: ui_composite_census_test --self-test [<repo root>] [--only R1,R5,...] | --dry-run | --emit-fixture\n", stderr);
            return 2;
        }
    }
    if (dry) {
        std::puts("ui composite census test: dry run (no checks run)");
        return 0;
    }
    if (!self) {
        std::fputs("usage: ui_composite_census_test --self-test [<repo root>] [--only R1,R5,...] | --dry-run | --emit-fixture\n", stderr);
        return 2;
    }
    // The pins read the sources and are skipped under --only (the mutation tool runs one rule at a time, without a root).
    return run(only, only.empty());
}

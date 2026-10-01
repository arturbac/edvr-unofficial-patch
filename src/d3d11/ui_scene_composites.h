// The interface composites the layer left in the scene -- pure and header-only: no device, no
// Config, no Log. docs/ui-layer-2026-09-23.md, "2026-10-01: Disable GUI effects". The DLL
// (ui_layer.cpp counts, vscreen.cpp reports each draw) and the build-gate rig
// (tools/ui_composite_census_test) include this one file.
//
// WHY. The layer names an interface draw by its vertex shader. A draw into the lit HDR eye that
// samples an interface surface and has a vertex shader no family names gets no decision and no
// refusal line: it stays in the scene, is upscaled with it, and the log says nothing. User 5's
// cockpit panels were that for a month -- Elite's Disable GUI effects switches the holo panels to
// another shader pair -- and the only trace was ui depth's totals line counting 22 draws a frame
// "left alone" under no name. This is the line that names them, every 30 s, in the layer's own
// census.
//
// WHAT. Every owner draw into an eye target that samples a learned interface surface (ui_depth's
// test, the one that makes a draw a composite) is counted once: taken into the layer, or left in
// the game's frame -- and a left one is named by its vertex shader, its pixel shader and the family
// the rule gave it (none when it named nothing). Every window the layer prints ONE line, with
// zeros: a line that says "0 of 5108" is the count having run and found nothing; no line at all
// is the code never having run (the layer off, or a build before this one).
//
// A composite no family names is the case this exists for; a named one that was not taken (the
// layer not armed for the frame, the world-screen gate holding the 2D screen) is also left in the
// scene and shows here with its family, beside the reasons the decided table already gives.
//
// LIMITS. A draw is a composite where ui depth's classifier says so, and that loop runs for an eye
// draw with a depth target bound (every cockpit composite and every recorded menu composite binds
// one): a composite drawn into an eye with none is not seen. A draw the layer took and then refused
// at issue still counts as taken here; the gates line counts those on its own.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "ui_layer_math.h"

namespace edvr {

// Pairs a window names. A frame of cockpit has at most a handful of composite pairs; more than this
// many DIFFERENT pairs left in the scene is itself the finding, and the rest are counted, unnamed.
constexpr size_t kUiSceneCompositePairs = 8;

struct UiSceneCompositePair {
    uint64_t vs = 0, ps = 0, draws = 0;
    int family = 0;  // UiLayerFamily as an int; 0 = no family named it
};

struct UiSceneCompositeWindow {
    uint64_t seen = 0;        // composite draws into an eye target, taken or left
    uint64_t left = 0;        // ...the ones left in the game's frame
    uint64_t pastTable = 0;   // of left: draws of pairs the table had no room for
    uint64_t framesLive = 0;  // frames of the window the layer was live, so the count could run
    uint32_t used = 0;
    UiSceneCompositePair pairs[kUiSceneCompositePairs];

    // A composite the layer took (or whose take is the layer's business): counted, not named.
    void noteTaken() { ++seen; }

    // A composite left in the game's frame, by (vs, ps, family).
    void noteLeft(uint64_t vs, uint64_t ps, int family) {
        ++seen;
        ++left;
        for (uint32_t i = 0; i < used; ++i) {
            UiSceneCompositePair& p = pairs[i];
            if (p.vs == vs && p.ps == ps && p.family == family) {
                ++p.draws;
                return;
            }
        }
        if (used < kUiSceneCompositePairs) {
            UiSceneCompositePair& p = pairs[used++];
            p.vs = vs;
            p.ps = ps;
            p.family = family;
            p.draws = 1;
            return;
        }
        ++pastTable;
    }

    void reset() { *this = UiSceneCompositeWindow{}; }
};

// The line, whole, with its "ui quality: " prefix (the Log adds the time). frames: every frame the
// boundary counted in the window. detectorOn: the interface depth pass was running at the print --
// without it nothing is learned, so no draw is a composite and the zero would mean nothing.
inline std::string uiSceneCompositeText(const UiSceneCompositeWindow& w, uint64_t frames, bool detectorOn) {
    char buf[320];
    const double perFrame = frames ? 1.0 / static_cast<double>(frames) : 0.0;
    std::string out = "ui quality: composites left in the scene: ";
    if (!detectorOn) {
        std::snprintf(buf, sizeof(buf),
                      "NOT COUNTED (the interface depth pass is not running: fix.temporal_aa is off or it stood "
                      "down, so no draw is recognised as a composite) in %llu frames.",
                      static_cast<unsigned long long>(frames));
        return out + buf;
    }
    std::snprintf(buf, sizeof(buf), "%llu of %llu composite draws (%.2f a frame) in %llu frames (%llu live) -- ",
                  static_cast<unsigned long long>(w.left), static_cast<unsigned long long>(w.seen),
                  static_cast<double>(w.left) * perFrame, static_cast<unsigned long long>(frames),
                  static_cast<unsigned long long>(w.framesLive));
    out += buf;
    if (!w.seen) {
        out += "no draw into an eye sampled an interface surface in this window.";
        return out;
    }
    if (!w.left) {
        out += "none: every interface composite drawn into an eye went into the layer.";
        return out;
    }
    for (uint32_t i = 0; i < w.used; ++i) {
        const UiSceneCompositePair& p = w.pairs[i];
        char label[64];
        if (p.family == 0) {
            std::snprintf(label, sizeof(label), "no family");
        } else {
            std::snprintf(label, sizeof(label), "%s, not taken", uiLayerFamilyName(static_cast<UiLayerFamily>(p.family)));
        }
        std::snprintf(buf, sizeof(buf), "%svs %016llX ps %016llX (%s) %.2f a frame", i ? "; " : "",
                      static_cast<unsigned long long>(p.vs), static_cast<unsigned long long>(p.ps), label,
                      static_cast<double>(p.draws) * perFrame);
        out += buf;
    }
    if (w.pastTable) {
        std::snprintf(buf, sizeof(buf), "%s%llu draws of pairs past the table's %u (%.2f a frame)", w.used ? "; " : "",
                      static_cast<unsigned long long>(w.pastTable), static_cast<unsigned>(kUiSceneCompositePairs),
                      static_cast<double>(w.pastTable) * perFrame);
        out += buf;
    }
    out += ".";
    return out;
}

// The same into a caller's buffer, truncated (never overrun) and always terminated; the length
// written. The Log's line holds 1200 characters; the longest line this makes is under 900.
inline size_t uiSceneCompositeFormat(char* out, size_t cap, const UiSceneCompositeWindow& w, uint64_t frames,
                                     bool detectorOn) {
    if (!out || !cap) return 0;
    const std::string text = uiSceneCompositeText(w, frames, detectorOn);
    const size_t n = text.size() < cap - 1 ? text.size() : cap - 1;
    std::memcpy(out, text.data(), n);
    out[n] = 0;
    return n;
}

}  // namespace edvr

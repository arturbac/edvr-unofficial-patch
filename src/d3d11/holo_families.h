// The hologram families' vertex-shader hashes, shared by the two modules
// that must agree on what a hologram is: ui_depth.cpp's generic
// hologram/icon depth pass (advanced.temporal_aa_hologram_depth -- the
// contribution and element-depth re-issues, whose family list these seed)
// and the crisp-HUD half of fix.ui_quality (ui_layer_math.h's
// uiLayerFamilyFor, which names all eleven the ONE family kHoloGeneric on
// the lit HDR target, Phase 3 of docs/cockpit-hud-layer-design-2026-09-27.md).
//
// They lived file-local in ui_depth.cpp until the take needed the SAME
// list -- a second spelling of the eleven would have let the take and the
// depth pass drift apart about what a hologram is. Moved, not copied, in
// draw_state_describe.h's pattern: ui_depth.cpp includes this header, and
// the per-family commentary below is its own, moved with the constants.
#pragma once

#include <cstdint>

namespace edvr {

// The generic hologram/icon depth pass's other built-in families (ui_depth.cpp,
// "GENERIC HOLOGRAM/ICON DEPTH COVERAGE"): the radar's star icon core, its
// two stalks, and the sun's corona family, which also paints the icon's
// glow. Unlike kHoloPanel these carry no per-family coverage shader at
// all -- eye dump eye_135907, frame 17847: the radar star icon over sky
// carried the sky's motion at 0% AA depth coverage.
constexpr uint64_t kHoloIconCore     = 0xF8D8A92E96419901ull;
constexpr uint64_t kHoloCoronaFamily = 0xD1281DF454A153ADull;
constexpr uint64_t kHoloIconStalkA   = 0xDF3503CD07F9B10Cull;
constexpr uint64_t kHoloIconStalkB   = 0x5453D19B6D362364ull;
// The target hologram's sphere (two premultiplied quads, ps EA02FAC2BD6C643C
// and E95634B0F61D218F) and the radar's five contact-marker families --
// flight 20260924_155636, eye dump eye_155832: unlisted, the target sphere
// carried the sky's motion at (-3.9,-6.1) px/frame rolling, and the contact
// bars (pool\draws_155832.bin, frame 8548, right after the two stalks in each
// eye's cockpit section) read 0% contribution.
constexpr uint64_t kHoloTargetSphere = 0x5559BD94B6852E83ull;
constexpr uint64_t kHoloContactA     = 0xA2C2D5510BF1926Dull;
constexpr uint64_t kHoloContactB     = 0x9B34C331902DC1EDull;
constexpr uint64_t kHoloContactC     = 0x9611A454527F7FEBull;
constexpr uint64_t kHoloContactD     = 0xB932058F26B76691ull;
constexpr uint64_t kHoloContactE     = 0x94D5C556DFD6D705ull;
// The target reticle's three 3D triangles (72 non-indexed vertices, three
// prisms -- pool\draws_163515.bin, frame 27528, right after the canopy):
// a WORLD MARKER, not a cockpit family. It tracks the targeted ship, which
// can be kilometres out, so the depth pass never radius-clips it like the
// families above -- eye dump eye_163515: sky MV (+0.59,-0.89) against the
// bracketed ship's (+0.14,+0.27) at 1.65 km, going indistinct with speed.
constexpr uint64_t kHoloWorldMarkerReticle = 0x71DD8B8B09060A81ull;

// The built-in cockpit list: the ten short-range panel/icon/hologram
// families above.
constexpr uint64_t kHoloFamiliesBuiltIn[10] = {kHoloIconCore, kHoloCoronaFamily,
                                               kHoloIconStalkA, kHoloIconStalkB,
                                               kHoloTargetSphere, kHoloContactA, kHoloContactB,
                                               kHoloContactC, kHoloContactD, kHoloContactE};
// WORLD MARKERS: a second, separate built-in list for draws that must be
// covered wherever they are, not just inside the cockpit radius (the
// families above are all short-range panel/icon geometry; a world marker
// tracks something that can be kilometres out). Fixed, never extended by
// advanced.temporal_aa_hologram_families -- see holoWorldMarkerList in
// ui_depth.cpp.
constexpr uint64_t kHoloWorldMarkers[1] = {kHoloWorldMarkerReticle};
constexpr uint32_t kHoloWorldMarkerCount = static_cast<uint32_t>(sizeof(kHoloWorldMarkers) / sizeof(kHoloWorldMarkers[0]));

// The canopy sits in front of the whole sky; covering it would smear the
// stars behind it. The depth pass refuses it even when named in
// advanced.temporal_aa_hologram_families (holoBuildFamilyList in
// ui_depth.cpp), and the crisp take refuses it the same way: it is not one
// of the eleven uiHoloGenericHash matches, so a canopy draw names no family
// and stays stock.
constexpr uint64_t kHoloCanopy = 0x8C091FFD08644E02ull;

// The crisp take's whole hologram set: the ten cockpit families plus the
// world marker -- the hologram pass's eleven. (kHoloPanel is NOT one of
// them: the take has named it kHolo since Phase 1.) The take has no
// cockpit-radius concept and needs none: the world marker is covered
// wherever it draws, exactly as the depth pass covers it.
inline bool uiHoloGenericHash(uint64_t vs) {
    for (uint64_t h : kHoloFamiliesBuiltIn)
        if (h == vs) return true;
    for (uint64_t h : kHoloWorldMarkers)
        if (h == vs) return true;
    return false;
}

}  // namespace edvr

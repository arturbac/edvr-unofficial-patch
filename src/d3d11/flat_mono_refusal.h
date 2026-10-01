#pragma once
// The resolver prep kernel's pixel classes and the refusal census (design doc section 82, stage 2 experiment build). Pure: no D3D, no
// I/O, so the route's rig and the resolver's rig read the same numbers the shader writes.
//
// WHAT THE CLASS IS. Every render pixel the prep treats gets one byte, written to a private texture only on a frame that needs
// it (a census sample, or the route's refusal view): the low seven bits say what the pixel IS, bit 7 says the prep REFUSED its
// history (rejection 1: the finish shows the raw input, which under a jittered world is a different sample every frame, so a
// fine pattern on a refused pixel shimmers). Classes 1..6 are the eye path's source kinds (screen_motion's sourceEngine and the
// temporal pass's motion_source view), with the same numbers and the same colours, so the two views read alike; 7 and up are what
// only the resolver's prep can say. The HLSL repeats the numbers as `static const uint kClass...` (flat_mono_shader_source.h) and
// tools\flat_mono_resolve_test holds the two to each other.
#include <cstdint>

namespace edvr {

constexpr uint32_t kFlatMonoClassNone = 0;            // no engine slot: not a keyed pool surface, the camera term
constexpr uint32_t kFlatMonoClassJoined = 1;          // a rig record Elite's records followed from last frame: its exact motion
constexpr uint32_t kFlatMonoClassMasked = 2;          // a rig record with no usable history this frame (first seen, after a gap): REFUSED
constexpr uint32_t kFlatMonoClassNotRig = 3;          // a pool surface that is not a rig record: the camera term
constexpr uint32_t kFlatMonoClassStale = 4;           // the slot's depth is not the pixel's, a later draw changed it: REFUSED, or the camera term with the steady-detail key on
constexpr uint32_t kFlatMonoClassCorrupt = 5;         // a slot code or a record number that did not survive intact: REFUSED
constexpr uint32_t kFlatMonoClassStaleStamp = 6;      // a joined marker from an older frame: the camera term
constexpr uint32_t kFlatMonoClassSentinel = 7;        // a slot with no depth under it (sky), or the out-of-range marker: REFUSED
constexpr uint32_t kFlatMonoClassUnreprojectable = 8; // a moved record whose reprojection failed: REFUSED
constexpr uint32_t kFlatMonoClassCamera = 9;          // the camera term could not be formed (singular, behind the camera): REFUSED
constexpr uint32_t kFlatMonoClassRange = 10;          // the reprojection leaves the screen or is not finite: REFUSED
constexpr uint32_t kFlatMonoClassDepth = 11;          // the pixel's own depth is not finite or outside [0,1]: REFUSED
constexpr uint32_t kFlatMonoClassWeapon = 12;         // an attached first-person pixel with a valid map: the map's motion
constexpr uint32_t kFlatMonoClassWeaponRefused = 13;  // an attached first-person pixel the map cannot place: REFUSED
constexpr uint32_t kFlatMonoClassReset = 14;          // a reset frame, every pixel refused: never sampled, never painted
constexpr uint32_t kFlatMonoClassRefusedBit = 0x80u;  // the prep refused this pixel's history
constexpr uint32_t kFlatMonoClassMask = 0x7Fu;

// The census counts REFUSED pixels by class (slot = class, 0..14) and, in slot 15, the stale pixels that were not refused because
// the steady-detail key sent them to the camera term ("forgiven"): with the key on the stale share stays measurable. Accepted
// pixels are not counted (the contended counters they would need cost more than the answer is worth); the total examined is the
// sampled frames' render size, known on the host.
constexpr uint32_t kFlatMonoRefusalSlots = 16;
constexpr uint32_t kFlatMonoRefusalForgiven = 15;
// One sample every this many resolves that ask for the census; the 5 s line names it.
constexpr uint32_t kFlatMonoRefusalEvery = 4;
constexpr uint32_t kFlatMonoRefusalStripes = 16;     // the counter buffer's stripes (spreads the atomics); the host sums them

inline const char* flatMonoClassName(uint32_t cls) {
    switch (cls) {
        case kFlatMonoClassNone: return "none";
        case kFlatMonoClassJoined: return "joined";
        case kFlatMonoClassMasked: return "masked";
        case kFlatMonoClassNotRig: return "not-rig";
        case kFlatMonoClassStale: return "stale";
        case kFlatMonoClassCorrupt: return "corrupt";
        case kFlatMonoClassStaleStamp: return "stale-stamp";
        case kFlatMonoClassSentinel: return "sentinel";
        case kFlatMonoClassUnreprojectable: return "unreprojectable";
        case kFlatMonoClassCamera: return "camera";
        case kFlatMonoClassRange: return "range";
        case kFlatMonoClassDepth: return "depth";
        case kFlatMonoClassWeapon: return "weapon";
        case kFlatMonoClassWeaponRefused: return "weapon-refused";
        case kFlatMonoClassReset: return "reset";
    }
    return "?";
}

// What flatMonoResolveTakeRefusalCensus() hands back: the samples read back since the last take.
struct FlatMonoRefusalCensus {
    uint64_t asked = 0;        // resolves that asked for the census (the key on, not a reset frame)
    uint64_t sampled = 0;      // samples dispatched
    uint64_t dropped = 0;      // samples skipped because every readback slot was still pending
    uint64_t frames = 0;       // samples whose counts were read back
    uint64_t pixels = 0;       // pixels those samples examined (render width x height each)
    uint64_t counts[kFlatMonoRefusalSlots] = {};   // refused pixels by class; [kFlatMonoRefusalForgiven] stale pixels forgiven
    uint32_t width = 0, height = 0;                // the render size of the last sample read back
    uint32_t every = kFlatMonoRefusalEvery;
    uint64_t refused() const {
        uint64_t n = 0;
        for (uint32_t i = 0; i < kFlatMonoRefusalForgiven; ++i) n += counts[i];
        return n;
    }
};

// The refusal view's palette (the eye path's, temporal_shader_source.h motion_source, painted by the HDR finish before the game's
// tone pass: it scales each colour by the pixel's own level, so the hue survives the tone but the absolute colour does not).
//   green  1 joined      red  2 masked      blue 3 not a rig record      yellow 4 stale slot
//   magenta 5 corrupt    orange 6 stale stamp   cyan 12 weapon           white  any other refusal (7..11, 13)
//   dimmed to a quarter  no engine slot (0)
// The names below are for the log line.
inline const char* flatMonoViewLegend() {
    return "green exact record, red masked record, blue pool surface (camera term), yellow stale slot, magenta corrupt slot, "
           "orange stale stamp, cyan first-person, white any other refusal, dimmed no engine slot";
}

}  // namespace edvr

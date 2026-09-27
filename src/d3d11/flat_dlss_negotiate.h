// Flat backend negotiation (gate 2 step 4): given the vendor's mode ranges
// for one display output D and the game's render size R, name the effective
// treatment -- the serving mode and the evaluation size E. Served at D
// stands; an input under the floor evaluates at the largest output the
// input reaches (the VR door's rule, dlss_floor.h) with the game's own copy
// upsampling the rest, never a silent TAA substitution. Pure policy, shared
// by flat_runtime.cpp and the rig; the NGX query stays driver-side.
#pragma once
#include "dlss_floor.h"

namespace edvr {

struct FlatDlssNegotiation {
    bool known = false;         // the vendor answered for D at all
    bool served = false;        // some mode serves R at the returned E
    DlssMode mode = DlssMode::Quality;  // the mode the vendor names for it
    bool fromRange = false;     // the mode came from a queried range, not a ratio guess
    uint32_t evalWidth = 0, evalHeight = 0;   // E the resolve should evaluate at
    bool cut = false;           // E shrank below D because R was under the floor
};

inline FlatDlssNegotiation flatDlssNegotiate(const DlssModeRange modes[kDlssModeCount],
                                             uint32_t rW, uint32_t rH, uint32_t dW, uint32_t dH) {
    FlatDlssNegotiation out{};
    out.evalWidth = dW; out.evalHeight = dH;
    if (!dW || !dH || !rW || !rH) return out;
    // Any answered mode at D means the vendor spoke; nothing answered means
    // !known and the resolve's own availability path decides.
    bool anyOk = false;
    for (int k = 0; k < kDlssModeCount; ++k) anyOk |= modes[k].ok;
    if (!anyOk) return out;
    out.known = true;
    if (dlssRangesServe(modes, rW, rH)) {
        out.served = true;
        DlssModeRange range{};
        dlssChooseMode(modes, rW, rH, dW, &out.mode, &out.fromRange, &range);
        return out;
    }
    // Under the floor: the largest output the input reaches (dlssFloorOutput),
    // the game's copy upsampling the rest -- exactly the VR door's rule.
    uint32_t cw = dW, ch = dH; int mode = -1;
    if (dlssFloorOutput(modes, dW, dH, rW, rH, &cw, &ch, &mode) && (cw != dW || ch != dH)) {
        out.served = true; out.cut = true;
        out.evalWidth = cw; out.evalHeight = ch;
        out.mode = static_cast<DlssMode>(mode); out.fromRange = true;
    }
    return out;
}
} // namespace edvr

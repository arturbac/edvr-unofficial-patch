#pragma once

#include <cstdio>
#include "../../src/d3d11/flat_negotiated_eval.h"

// rc-since-rc2 review F5: the negotiated-E gate's truth table. The runtime
// calls this only after the vendor negotiation has answered for the current
// contract; these checks pin the signature gating itself.
inline int flatNegotiatedEvalTests() {
    using edvr::FlatMonoResolveMode;
    using edvr::flatNegotiatedEval;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: negotiated eval %s\n", name); ++failures; }
    };
    uint32_t w = 99, h = 99;
    // A matching contract receives the negotiated cut E.
    flatNegotiatedEval(2496, 1404, FlatMonoResolveMode::Dlss, 1920, 1080, 3840, 2160,
                       FlatMonoResolveMode::Dlss, 1920, 1080, 3840, 2160, w, h);
    expect(w == 2496 && h == 1404, "matching contract receives the negotiated E");
    // A zeroed negotiation (unserved, native, non-DLSS) overrides nothing.
    w = h = 99;
    flatNegotiatedEval(0, 0, FlatMonoResolveMode::Dlss, 1920, 1080, 3840, 2160,
                       FlatMonoResolveMode::Dlss, 1920, 1080, 3840, 2160, w, h);
    expect(w == 0 && h == 0, "no negotiation answer means route default");
    // Every signature field gates: mode, render size, output size.
    w = h = 99;
    flatNegotiatedEval(2496, 1404, FlatMonoResolveMode::Dlss, 1920, 1080, 3840, 2160,
                       FlatMonoResolveMode::Fsr, 1920, 1080, 3840, 2160, w, h);
    expect(w == 0 && h == 0, "a different backend does not inherit the override");
    w = h = 99;
    flatNegotiatedEval(2496, 1404, FlatMonoResolveMode::Dlss, 1920, 1080, 3840, 2160,
                       FlatMonoResolveMode::Dlss, 2496, 1404, 3840, 2160, w, h);
    expect(w == 0 && h == 0, "a changed render size does not inherit the override");
    w = h = 99;
    flatNegotiatedEval(2496, 1404, FlatMonoResolveMode::Dlss, 1920, 1080, 3840, 2160,
                       FlatMonoResolveMode::Dlss, 1920, 1080, 2560, 1440, w, h);
    expect(w == 0 && h == 0, "a changed output size does not inherit the override");
    return failures;
}

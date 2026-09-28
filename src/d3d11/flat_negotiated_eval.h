#pragma once
#include <cstdint>
#include "flat_mono_resolve.h"

namespace edvr {
// The negotiated evaluation-size override for one frame or resolve plan,
// gated to the exact contract it was negotiated for (mode/R/D); anything else
// resolves on the route's default E. Call AFTER the vendor negotiation has
// answered for this contract (rc-since-rc2 review F5): reading the state
// before then hands the previous contract's answer -- or none -- to the
// first frame of a new contract, resolving an under-floor input at the
// route's default E (backend refusal and a spatial fallback), preflighting
// at the default size, and reallocating resources on the next frame.
inline void flatNegotiatedEval(uint32_t negotiatedEvalW, uint32_t negotiatedEvalH,
                               FlatMonoResolveMode negotiatedMode,
                               uint32_t negotiatedRenderW, uint32_t negotiatedRenderH,
                               uint32_t negotiatedOutputW, uint32_t negotiatedOutputH,
                               FlatMonoResolveMode mode, uint32_t rW, uint32_t rH,
                               uint32_t dW, uint32_t dH,
                               uint32_t& evalW, uint32_t& evalH) {
    const bool match = negotiatedEvalW && negotiatedEvalH &&
        mode == negotiatedMode && rW == negotiatedRenderW && rH == negotiatedRenderH &&
        dW == negotiatedOutputW && dH == negotiatedOutputH;
    evalW = match ? negotiatedEvalW : 0;
    evalH = match ? negotiatedEvalH : 0;
}
} // namespace edvr

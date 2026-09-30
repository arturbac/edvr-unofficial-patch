#pragma once
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>
#include "flat_compute_readback.h"
#include "flat_projection_scope.h"
#include "flat_substitution.h"
#include <optional>
namespace edvr {
struct FlatMonoFrame;   // flat_mono_frame.h: the selector's result, passed by reference to the HDR route's treatment
extern std::atomic<bool> g_flatRuntimeLive;
inline bool flatRuntimeActive() { return g_flatRuntimeLive.load(std::memory_order_relaxed) && !g_flatComputeInternal; }
// Last qualified flat scene extent, published for the menu on any thread.
bool flatRuntimeNativeScale();
// The refusal state the F8 panel's settings warning follows (flat_standdown.h): true while the
// work is stood down for a chain-shape refusal that found an output copy (never for
// no-known-output-copy: a startup or loading frame has no final copy at all, and nothing in
// Elite's settings to turn off). It ends with the resume. reasonName is the selector's own name
// for the stand-down's current reason, standingDown says the work is paused (always, with the
// warning). Any thread; false with a treated, transiently refused or merely slow-to-start
// session, and with the mode off. The caller checks that a temporal mode is selected.
bool flatRuntimeStructuralRefusal(const char** reasonName, bool* standingDown);
// Whether the HDR route (experimental.temporal_aa_before_post = auto, flat_hdr_route.h) is what treats frames now:
// published with the refusal state, so it is meaningful only while flatRuntimeStructuralRefusal is true. The F8
// warning drops the Bloom and Depth of field advice while it holds.
bool flatRuntimeHdrRouteActive();
// Whether the route's key is auto and its last selection found the game rendering below the output on both axes
// (Elite's supersampling under 1.0), so the route leaves the frames to the copy route: then true, with the measured
// render and output sizes (the route's own, taken at its trigger, not Elite's settings file). False with the key off,
// with a selection at R >= D and after a resize. Any thread. The F8 warning adds its supersampling line from it, and
// only while frames are refused and the route is not treating them (flatWarningFlags).
bool flatRuntimeHdrRouteBelowOutput(uint32_t* renderWidth, uint32_t* renderHeight, uint32_t* outputWidth,
                                    uint32_t* outputHeight);
// The upstream camera injector's read points into the phase machine: the
// current phase in render pixels and the validated resolve plan's render
// extent (w/h); applied is the machine's own applied count this frame.
void flatRuntimePhaseState(float* x, float* y, uint32_t* w, uint32_t* h, uint32_t* applied);
// The camera injector applied the phase at the source: fold it into the
// phase machine exactly as a scope application would.
void flatRuntimeNoteCameraApplied();
// Whether a legacy projection plan exists for the current frame (the
// ownership policy's legacyEligible input).
bool flatRuntimeLegacyPlanExists();
void flatRuntimePresent(IDXGISwapChain*, uint64_t frame, HRESULT, UINT flags);
void flatRuntimeBeforePresent();
void flatRuntimeResize();
void flatRuntimeConstantBuffers(UINT start, UINT count, ID3D11Buffer* const*);
void flatRuntimeUavs(UINT start, UINT count, ID3D11UnorderedAccessView* const*);
struct FlatRuntimeDispatchScope {
    std::optional<FlatProjectionBindingScope> projection;
    explicit FlatRuntimeDispatchScope(ID3D11DeviceContext*);
};
void flatRuntimeViewport(UINT, const D3D11_VIEWPORT*);
void flatRuntimeMap(ID3D11Resource*, D3D11_MAP, void*);
void flatRuntimeUnmap(ID3D11Resource*);
void flatRuntimeUpdate(ID3D11Resource*, const void*, const D3D11_BOX*);
void flatRuntimeWritten(ID3D11Resource*);
void flatRuntimeUnknown();
void flatRuntimeArmProjectionAudit();
void flatRuntimeCreateBuffer(ID3D11Buffer*, const void* initialData);
void flatRuntimeClearBindings();
// A hooked call of the kind flat_substitution.h names has come, or the frame is ending: put the game's state back where
// engine motion's substitution is still bound (a lazy run of substituted producer draws), or forget it (the context
// lost its state). Every hook of that kind calls this before its real call; a load and a compare when nothing of EDVR's
// is bound, which is nearly always.
void flatRuntimeSubstitution(ID3D11DeviceContext* ctx, FlatSubstEvent event);
struct FlatRuntimeDrawScope {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11ShaderResourceView* original = nullptr;
    bool gameHadTarget6 = false;   // the game's own slot 6 was occupied under a substituted draw
    bool producer = false, replaced = false;
    bool drawCaptureStarted = false;
    std::optional<FlatProjectionBindingScope> projection;
    FlatRuntimeDrawScope(ID3D11DeviceContext*, uint32_t instances,
                         char kind='?', uint32_t count=0, uint32_t start=0,
                         int32_t base=0, uint32_t startInstance=0);
    ~FlatRuntimeDrawScope();
    bool recover(const char* reason);
    // The HDR route's treatment at its trigger draw (flat_hdr_route.h, design section 81): the resolve of the game's HDR
    // scene target H, written back into H, before the game's pass that reads it. `srvSlot` is the pixel-shader slot that
    // binds H for this draw. Declines quietly (the copy route then serves the frame) and never leaves H half-written.
    void treatHdr(const FlatMonoFrame& selected, uint32_t srvSlot);
};
} // namespace edvr

// advanced.hud_census -- the Phase 0 "census gates" instrument of the
// crisp-HUD design (docs/cockpit-hud-layer-design-2026-09-27.md). ONE
// measurement flight at HMD Quality 0.75 with DLSS, fix.ui_quality OFF:
// before anything about where the cockpit HUD is composited may change,
// the doc's BELIEVED facts about the holo panels (vs 81216C77F90DEDD6),
// the flight HUD (vs B7790CBFC6554097) and the target sprite (vs
// E508648660A352B2) each get a measured gate. Every line is prefixed
// "hud layer census:" and names its gate:
//
//   G-A  the families, per frame: target resource and format, blend and
//        depth/stencil state, SRV sizes, draws per frame, and the draw
//        ordinal against the tonemap draw's (the door is after both by
//        construction). State lines are first-seen-per-session per state
//        fingerprint (a last-one compare flooded the log cap in flight 2);
//        draws per frame and ordinals on the 30-second window lines.
//   G-B  the tonemap: the admitted variant (vs/ps logged for EVERY shape-
//        matching draw, deduplicated, so an EDHM swap names itself), its
//        SRV identities (exposure, LUT, HDR), PS b2's size, and whether
//        the HDR SRV's resource is one the family draws targeted.
//   G-C  the family's VS cb0, read whole (up to 16 rows), each 4-row
//        quad voted against the scene's jittered projection for that eye,
//        rebuilt from native_temporal's own frusta/shift/planes -- jitter
//        carried, carried but unjittered, or no quad is the scene
//        projection (rows 4..7 alone were a composed model-view transform
//        in flight 1; a no-quad draw dumps every row for offline
//        factorisation). Verdict on change; share per 30 s.
//   G-D  the share of each family's pixels its GEQUAL depth test rejects:
//        an occlusion-query pair per sampled family draw, the game's own
//        depth state (writes masked off) against depth-off, both re-issued
//        with NO colour target. Rejected share per family per eye, 30 s.
//   G-E  1 Hz: the exposure scalar read at the tonemap draw's OWN VS t0,
//        beside HUD-region HDR luma (a centre crop of its PS t1 source,
//        R11G11B10 decoded), so a bright and a dark scene compare.
//
// WHAT IT COSTS. Unarmed: one bool load per eye draw, beside the draw
// census's own. Armed: the family tests are three hash compares and a
// shape prefilter per eye draw; the state reads, the cb0 staging copy,
// the occlusion pair and the exposure/luma copies run for family and
// tonemap draws only (tens a frame), the readbacks resolve a frame later
// with DO_NOT_WAIT maps and a bounded force-block, and nothing logs per
// draw -- aggregate window lines, first-seen lines, and on-change lines.
//
// WHY IT CHANGES NO RENDERING. Every game draw is forwarded exactly as it
// arrived; the module binds nothing and writes nothing of its own. The
// G-D pair re-issues the game's OWN draw (vscreen's pureDrawReissue, zero
// hook recursion) with the render targets UNBOUND -- colour cannot change
// -- and a depth-stencil state whose write masks are zeroed (test kept,
// writes masked) or whose tests are off, so the game's depth and stencil
// buffers cannot change either; the full OM state is saved and restored
// around the pair, and the game's own draw then runs untouched. Queries
// are never waited on; a pair that the GPU never answers is dropped.
// Everything else is reads: binding-shadow lookups, Get*/GetDesc state
// reads, and staging copies of the tonemap draw's inputs.
//
// Recognition does NOT go through fix.ui_quality's uiLayerDecide, which
// runs only with the layer live and refuses refused draws any state
// reads: the census hooks the eye-draw branch independently, so it
// measures the stock game (the design flight has fix.ui_quality off).
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

#include "draw_census.h"  // DrawArgs

namespace edvr {

class Config;

namespace detail {
extern bool g_hudLayerCensusOn;
}  // namespace detail
// Read on the draw path beside drawCensusArmed(): the unarmed price of the
// whole module. Inline for the same reason as the draw census's -- the
// build has no /GL to fold a cross-TU getter for one bool load.
inline bool hudLayerCensusArmed() { return detail::g_hudLayerCensusOn; }

// advanced.hud_census = off | on (default off), from both of vscreen's
// config sweeps, beside objectProbeConfigure.
void hudLayerCensusConfigure(Config& cfg);

// One draw that reached an eye-sized target, from vscreen's eye-draw
// branch (owner context only), called while its bindings are certainly
// the ones it will run with. Returns true when this is a family draw the
// G-D budget selected for an occlusion pair: the caller must then run its
// colourless re-issue pair (vscreen's own helper around pureDrawReissue)
// before the game's draw proceeds.
bool hudLayerCensusEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances,
                           uint32_t eyeDrawIndex, const DrawArgs& args);

// What the colourless re-issue pair touches, held by the caller across
// the three steps: the OM bindings and depth-stencil state to restore,
// and the query-pair slot the module is using. Default-constructed by the
// caller; the module owns everything else.
struct HudCensusGdSave {
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> dss;
    uint32_t stencilRef = 0;
    int slot = -1;       // the module's query-pair slot, -1 while none held
    bool aOpen = false;  // query A begun, not yet End'd
    bool bOpen = false;  // query B begun, not yet End'd
};

// From vscreen's query-bracket hooks while the census is armed (owner
// context only): a game query began or End'd. The G-D pair declines while
// a SAMPLE-COUNTING game query is open on the context (occlusion,
// stream-out or pipeline statistics) -- a re-issued family draw inside
// such a bracket would feed its counter, and changing what the game
// measures changes what it draws a frame later. Timestamps, their
// disjoint and events count no samples and are ignored (gpu_span's
// frame-wide TIMESTAMP_DISJOINT would otherwise decline every pair, as
// it did in flight 1). The census's own ring queries are recognised and
// ignored.
void hudLayerCensusNoteGameQuery(bool begin, void* async);

// The three steps of the G-D pair, driven by vscreen's helper around its
// pureDrawReissue:
//   Begin:  save OM state; bind no colour target, keep the game's DSV;
//           set the write-masked clone of the game's depth state; Begin
//           query A. False (state untouched) when the pair cannot run --
//           predication active, the query ring full, a derived-state
//           failure (which disables G-D for the session, said once).
//   Swap:   End A; set the depth-and-stencil-off clone; Begin query B.
//   End:    End B; restore the saved OM state exactly. Always runs, even
//           on a mid-pair fault: the game's own draw comes next and must
//           find its bindings as they were.
bool hudLayerCensusGdBegin(ID3D11DeviceContext* ctx, HudCensusGdSave& save);
void hudLayerCensusGdSwapToDepthOff(ID3D11DeviceContext* ctx, HudCensusGdSave& save);
void hudLayerCensusGdEnd(ID3D11DeviceContext* ctx, HudCensusGdSave& save);

// Once per frame, from vScreenFrameBoundary beside uiLayerFrameBoundary:
// the query polls, the deferred readbacks (G-C rows, G-E exposure/luma),
// the frame's tallies, and the 30-second window report. Prints whenever
// armed -- never gated on fix.ui_quality, whose logTotals gate is the
// anti-pattern this instrument exists to avoid (a census that must be
// taken with the layer OFF cannot require it ON).
void hudLayerCensusFrameBoundary(ID3D11DeviceContext* ctx);

// Release every query, staging texture and derived state; a session
// summary line if anything was measured. From shutdownVScreenFixes,
// beside uiLayerShutdown.
void hudLayerCensusShutdown();

}  // namespace edvr

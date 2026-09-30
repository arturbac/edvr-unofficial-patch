// The VR on-foot world route, the runtime (vr_world_route.h; the pure half is vr_world_route_math.h; design doc section 82).
//
// One call per game draw while the route is wanted (vrWorldRouteDraw) finds the flat HDR route's trigger, the tone, from
// the draw hooks' binding shadow; at the trigger it resolves the world's HDR scene image with flatMonoResolve, into H
// itself, on its own upscaler slot; the frame boundary accounts the frame and steps the ownership machine. What follows from
// an owned route (the eye shift off, the layer re-issuing each eye's screen draw, the door layer-only) lives in
// native_temporal.cpp, ui_layer.cpp and native_sharpen.cpp and reads this module through vr_world_route.h.
//
// COST. A draw with no colour target (16.8k shadow draws a frame on foot) dies at one shadow load. A coloured draw reads two
// view pointers from the shadow and looks them up in a per-frame table: a view's resource, size and format are resolved once
// a frame (four COM calls under the shadow's own guard and budget), never per draw. No allocation, no lock and no D3D call
// per draw. FlatContractObservation is ~360 bytes: one static instance, eleven fields written.
//
// THE JITTER SEAM. This build resolves an UNJITTERED world (phase 0, rows phase 0): the flight judges cost and plumbing, not
// image quality. flatPhase() is where the camera injector's phase plugs in later; nothing else here changes.
#include "vr_world_route.h"
#include "vr_world_route_math.h"
#include "binding_shadow.h"
#include "engine_velocity.h"
#include "dlaa.h"
#include "flat_mono_resolve.h"
#include "gpu_census.h"
#include "panel_curve.h"    // panelCurveWants: the curved screen's substitution, which the layer cannot re-issue
#include "ui_layer.h"
#include "vr_camera_census.h"
#include "vr_world_mips.h"
#include "weapon_motion.h"
#include "vscreen.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/temporal_mode.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <windows.h>
#include <cstring>
#include <string>

namespace edvr {

static_assert(kVrWorldFeatureSlot < kUpscalerSlots && kVrWorldFeatureSlot >= kUpscalerEyeSlots,
              "the world's upscaler slot is one the backends have, and not one of the eyes' (dlaa.h)");

bool g_vrWorldWants = false;
bool g_vrWorldWatchWrites = false;
thread_local bool g_vrWorldInternal = false;

namespace {
using Microsoft::WRL::ComPtr;

// ---- the state of the route -------------------------------------------------------------------------------------
VrWorldMachine g_machine;
VrWorldKey g_key = VrWorldKey::Off;
VrWorldWindow g_win;
bool g_census = false;
const char* g_notLiveNoted = nullptr;        // the reason the route last said it stays off (one line per reason)
uint64_t g_frameNo = 0;                      // present frames seen by the route, consecutive; the resolver's gap test reads it
uint64_t g_windowStartMs = 0;
uint64_t g_ownedFramesEpisode = 0;           // frames this ownership episode (for the release line)
bool g_sceneResetEvent = false;              // the transition detector reset the eyes' history: the machine lets go at the boundary
bool g_sceneResetPending = false;            // ... and the resolver's history resets at the next treat
bool g_tookNoted = false;                    // the first layer take of this ownership episode was logged
bool g_firstTriggerLogged = false, g_latchLogged = false, g_lateWriteLogged = false;
uint32_t g_declineLines = 0;
std::atomic<bool> g_ownsNext{false};
uint64_t g_takenSequence[2] = {0, 0};
uint64_t g_countedLayerOnly[2] = {0, 0};     // the sequence each eye's layer-only door was last counted for (it is asked twice an eye)
bool g_startedOwned = false;                 // the frame now running started owned (the eye shift was off)
bool g_resourcesLive = false;                // the resolver (and the mipped screen) made resources: release them when the key goes off
bool g_gatePrev = false, g_gateSeen = false; // the on-foot gate at the last boundary, for the window's flip count

// ---- the per-frame detector state ------------------------------------------------------------------------------------
struct ViewEntry {
    const void* view = nullptr;      // the shadow's view pointer this entry answers for
    VrWorldView v{};
};
constexpr unsigned kViewCache = 96;
struct Frame {
    FlatHdrFrame hdr;
    ViewEntry views[kViewCache];
    unsigned viewCount = 0;
    uint32_t viewOverflow = 0;
    uint32_t seq = 0;                        // coloured draws seen (the detector's q)
    uint32_t draws = 0;                      // every draw seen (the census's ordinal)
    const void* candDepth[kFlatHdrCandidates] = {};
    // The last coloured draw's target pair and whether it showed the route nothing to watch (vrWorldRouteDraw): the next draw
    // into the same pair skips at two compares. 16k draws into one shadow atlas are one lookup and 16k compares.
    const void* runRtv = nullptr;
    const void* runDsv = nullptr;
    bool runSkippable = false;
    bool depthMixed = false;
    bool treated = false;
    bool triggered = false;
};
Frame g_f;
FlatContractObservation g_k;                 // one instance; eleven fields written per coloured draw

// What the resolver kept from the last treated frame, for the next one's history decisions.
struct Previous {
    bool valid = false;
    uint64_t frame = 0;
    float rows[6][4] = {};
    const void* depth = nullptr;
    const void* color = nullptr;
    uint32_t w = 0, h = 0;
};
Previous g_prev;
uint64_t g_lastTreatMs = 0;

// The depth texture's shader view, made once per texture (the resolver wants an SRV over the world's depth).
struct DepthViews {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> depth;
    ComPtr<ID3D11ShaderResourceView> stencil;   // the stencil plane (R32G8X24 only): the weapon fold-in's second input; null otherwise
};
DepthViews g_depthViews;

FlatMonoResolveMode resolveModeFromConfig(bool* known) {
    const std::string m = Config::get().getString("fix.temporal_aa", "off");
    if (known) *known = temporalModeEnabled(m);
    if (_stricmp(m.c_str(), "fsr") == 0) return FlatMonoResolveMode::Fsr;
    if (_stricmp(m.c_str(), "dlss") == 0) return FlatMonoResolveMode::Dlss;
    if (_stricmp(m.c_str(), "dlaa") == 0) return FlatMonoResolveMode::Dlaa;
    return FlatMonoResolveMode::Taa;
}

const ViewEntry* lookup(Frame& f, void* view) {
    for (unsigned i = 0; i < f.viewCount; ++i)
        if (f.views[i].view == view) return &f.views[i];
    if (f.viewCount == kViewCache) { ++f.viewOverflow; return nullptr; }
    ViewEntry& e = f.views[f.viewCount++];
    e.view = view;
    ResourceInfo info{};
    e.v = VrWorldView{};
    if (bindingResolveProbe(view, &info)) {
        e.v.known = true; e.v.texture2d = info.isTexture2D; e.v.resource = info.resource;
        e.v.width = info.a; e.v.height = info.b; e.v.format = info.fmt;
    }
    return &e;
}

void declineLine(const char* why) {
    g_win.hdr.lastVerdict = why;
    ++g_win.hdr.declined;
    if (g_declineLines < 12) {
        ++g_declineLines;
        Log::get().note("vr world route: declined at frame=%llu seq=%u: %s (the eye route serves this frame)",
                        static_cast<unsigned long long>(g_frameNo), g_f.hdr.trigger.sequence, why);
    }
}

bool makeDepthViews(ID3D11Device* device, ID3D11Texture2D* depth) {
    if (g_depthViews.texture.Get() == depth && g_depthViews.depth) return true;
    g_depthViews = DepthViews{};
    if (!depth) return false;
    D3D11_TEXTURE2D_DESC d{};
    depth->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || d.ArraySize != 1 || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return false;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    switch (d.Format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS: fmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        case DXGI_FORMAT_R32_TYPELESS: fmt = DXGI_FORMAT_R32_FLOAT; break;
        case DXGI_FORMAT_R24G8_TYPELESS: fmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case DXGI_FORMAT_R16_TYPELESS: fmt = DXGI_FORMAT_R16_UNORM; break;
        default: return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = fmt;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(depth, &sd, srv.GetAddressOf()))) return false;
    g_depthViews.texture = depth;
    g_depthViews.depth = srv;
    if (d.Format == DXGI_FORMAT_R32G8X24_TYPELESS) {
        // The same texture's stencil plane, for the first-person fold-in. Optional: without it the route runs without the
        // weapon inputs (the flat path's weapon handling).
        sd.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        ComPtr<ID3D11ShaderResourceView> stencil;
        if (SUCCEEDED(device->CreateShaderResourceView(depth, &sd, stencil.GetAddressOf()))) g_depthViews.stencil = stencil;
    }
    return true;
}

// THE WEAPON SEAM (docs section 82, item 3: Sean, 2026-09-30, builds the fold-in). The first-person motion map (weapon_motion.cpp:
// the weapon's own animated vertices, rebuilt into a source-sized map each frame a weapon draws) and the stencil plane of the
// world's depth (bit 0x10, written by the game's first-person draws) are two optional inputs of the resolver's prep: attached
// pixels take the map's motion, or reject their history, and never the world's camera term at the weapon's depth and field of
// view. BOTH OR NEITHER. Neither (no weapon drew this frame, fix.weapon_stability off, the depth without a stencil plane, a
// map the resolver refuses) is the flat path's weapon handling: today's arithmetic, bit for bit. What the fold-in needs at this
// seam, for whoever changes it: a map of FlatMonoResolveFrame::firstPersonMotion's contract and the depth's stencil view.
struct WorldFirstPerson {
    ID3D11ShaderResourceView* motion = nullptr;
    ID3D11ShaderResourceView* stencil = nullptr;
};
WorldFirstPerson worldFirstPerson() {
    WorldFirstPerson fp;
    ID3D11ShaderResourceView* map = weaponMotionView();   // null unless a weapon draw made the map THIS frame (and it is not ambiguous)
    if (!map || !g_depthViews.stencil) return fp;
    fp.motion = map;
    fp.stencil = g_depthViews.stencil.Get();
    return fp;
}

// THE JITTER SEAM. The phase the world's camera is rendered with (x, y), and the phase the camera ROWS themselves carry
// (rowsX, rowsY: nonzero only when the game derived b1[270..275] from a jittered frustum, which is what the camera injector
// does; the resolver removes it from the camera, the previous camera and the engine's scene snapshots before any
// reprojection, FlatMonoResolveFrame::rowsJitter*), in render pixels, positive right and down. Both are zero in this
// build: the world is unjittered, the injector is not installed, and the flight judges cost and plumbing, not image
// quality. When the injector plugs in, this function answers from its phase machine (flatCameraRowsPhase) and nothing else
// in the route changes; the eye exclusion it needs is what the VR camera census (vr_camera_census.h) decides.
struct WorldPhase { float x = 0.0f, y = 0.0f, rowsX = 0.0f, rowsY = 0.0f; };
WorldPhase worldPhase() { return {}; }

void treatWorld(ID3D11DeviceContext* ctx) {
    Frame& fr = g_f;
    const FlatHdrTrigger& t = fr.hdr.trigger;
    if (g_machine.state == VrWorldState::Latched) { declineLine("latched-off"); return; }
    if (g_key != VrWorldKey::Auto) return;   // the detector runs for the census alone; nothing to treat

    bool engineKnown = false;
    const FlatMonoResolveMode mode = resolveModeFromConfig(&engineKnown);
    uint32_t screenW = 0, screenH = 0;
    vScreenPanelSize(&screenW, &screenH);

    // Which depth did H's draws use? The candidate that matched the consumer.
    const FlatHdrCandidate* cand = flatHdrFindCandidate(fr.hdr, t.hdr);
    const void* depthRes = nullptr;
    if (cand) depthRes = fr.candDepth[cand - fr.hdr.candidates];

    ID3D11Texture2D* depthTex = static_cast<ID3D11Texture2D*>(const_cast<void*>(depthRes));
    EngineVelocityViews ev{};
    VrWorldSelectFacts facts;
    facts.triggered = fr.hdr.triggered;
    facts.ambiguous = t.ambiguous;
    facts.engineKnown = engineKnown;
    facts.extentOk = t.hdrWidth == screenW && t.hdrHeight == screenH && screenW && screenH;
    facts.depthKnown = depthRes != nullptr && !fr.depthMixed;
    facts.depthNamed = facts.depthKnown && engineVelocitySourceIsNamed(depthTex);
    float rows[6][4] = {};
    bool rowsOk = false;
    if (facts.depthNamed) {
        facts.engineViews = engineVelocitySourceViews(depthTex, &ev);
        float shaped[6][4];
        rowsOk = engineVelocitySourceCameraRows(rows) &&
                 flat_mono_detail::cameraShape(reinterpret_cast<const unsigned char*>(rows), shaped);
    }
    facts.cameraRows = rowsOk;
    const VrWorldSelect sel = vrWorldSelect(facts);
    g_win.hdr.noteSelection(vrWorldSelectName(sel));
    struct Release {
        EngineVelocityViews& v;
        ~Release() { if (v.slots) v.slots->Release(); if (v.pool) v.pool->Release(); if (v.sceneNow) v.sceneNow->Release();
                     if (v.scenePrev) v.scenePrev->Release(); if (v.gameMark) v.gameMark->Release(); }
    } release{ev};
    if (sel != VrWorldSelect::Selected) { declineLine(vrWorldSelectName(sel)); return; }

    ComPtr<ID3D11Device> device;
    ctx->GetDevice(device.GetAddressOf());
    if (!device) { declineLine("no-device"); return; }
    if (!flatHdrRouteEvaluatesAtRender(mode, t.hdrWidth, t.hdrHeight, screenW, screenH)) {
        declineLine("route-does-not-evaluate-at-render-size");
        return;
    }
    // Verify the actual binding once: the shadow only nominated this draw.
    if (!t.srvKnown || t.srvSlot > 3) { declineLine("trigger-without-a-shader-resource-slot"); return; }
    ComPtr<ID3D11ShaderResourceView> hdrView;
    ctx->PSGetShaderResources(t.srvSlot, 1, hdrView.GetAddressOf());
    ComPtr<ID3D11Resource> hdrResource;
    if (hdrView) hdrView->GetResource(hdrResource.GetAddressOf());
    if (!hdrView || hdrResource.Get() != t.hdr || !makeDepthViews(device.Get(), depthTex)) {
        declineLine("actual-hdr-binding-or-depth-view-refused");
        return;
    }

    FlatMonoResolveFrame f{};
    f.color = hdrView.Get();
    f.depth = g_depthViews.depth.Get();
    f.hdr = true;
    f.renderWidth = t.hdrWidth; f.renderHeight = t.hdrHeight;
    f.outputWidth = screenW; f.outputHeight = screenH;
    f.frame = g_frameNo;
    f.mode = mode;
    f.slot = kVrWorldFeatureSlot;
    f.staticScene = false;
    std::memcpy(f.camera, rows, sizeof(f.camera));
    const bool resetMissing = !g_prev.valid;
    const bool resetGap = g_prev.valid && g_prev.frame + 1 != g_frameNo;
    const bool resetDepth = g_prev.valid && g_prev.depth != depthRes;
    const bool resetColor = g_prev.valid && g_prev.color != t.hdr;
    const bool resetExtent = g_prev.valid && (g_prev.w != t.hdrWidth || g_prev.h != t.hdrHeight);
    const bool resetScene = g_sceneResetPending;
    f.reset = resetMissing || resetGap || resetDepth || resetColor || resetExtent || resetScene;
    const WorldPhase phase = worldPhase();
    f.jitterX = phase.x; f.jitterY = phase.y;
    f.previousJitterX = phase.x; f.previousJitterY = phase.y;   // the seam: this frame's phase is the last frame's too (both zero)
    f.rowsJitterX = phase.rowsX; f.rowsJitterY = phase.rowsY;
    f.previousRowsJitterX = phase.rowsX; f.previousRowsJitterY = phase.rowsY;
    std::memcpy(f.previousCamera, f.reset ? rows : g_prev.rows, sizeof(f.previousCamera));
    f.engine = ev;
    const uint64_t nowMs = GetTickCount64();
    f.deltaMs = g_lastTreatMs ? static_cast<float>(nowMs - g_lastTreatMs) : 11.111f;
    const WorldFirstPerson fp = worldFirstPerson();
    f.firstPersonMotion = fp.motion;
    f.firstPersonStencil = fp.stencil;

    ComPtr<ID3D11ShaderResourceView> none;
    const char* why = nullptr;
    bool ok = false;
    try {
        GpuCensusScope census(ctx, GpuCensusSection::FrameWorldResolve);
        VrWorldInternalScope internal;
        ok = flatMonoResolve(device.Get(), ctx, f, none.GetAddressOf(), &why);
    } catch (...) {
        ok = false;
        why = "exception-in-resolve";
    }
    if (!ok) {
        // The backend or the route's own guard refused before H was written (the resolver writes H last): H is still the
        // game's own, and the world is unjittered, so there is nothing to recover. The eye route serves the frame.
        g_prev.valid = false;
        declineLine(why ? why : "resolve-refused");
        return;
    }
    fr.treated = true;
    g_resourcesLive = true;
    g_sceneResetPending = false;
    g_lastTreatMs = nowMs;
    ++g_win.hdr.treated;
    g_win.hdr.lastVerdict = f.reset ? "treated-reset" : "treated";
    g_prev.valid = true;
    g_prev.frame = g_frameNo;
    std::memcpy(g_prev.rows, rows, sizeof(g_prev.rows));
    g_prev.depth = depthRes;
    g_prev.color = t.hdr;
    g_prev.w = t.hdrWidth; g_prev.h = t.hdrHeight;
}

void onTrigger(ID3D11DeviceContext* ctx) {
    Frame& fr = g_f;
    fr.triggered = true;
    g_vrWorldWatchWrites = true;
    if (!g_firstTriggerLogged) {
        g_firstTriggerLogged = true;
        char line[512];
        vrWorldFormatFirstTrigger(line, sizeof(line), g_frameNo, fr.hdr);
        Log::get().note("%s", line);
    }
    treatWorld(ctx);
}

}  // namespace

// ---- the published state (final: other modules link against these) --------------------------------------------------------
bool vrWorldRouteEnabled() { return g_key == VrWorldKey::Auto; }
VrWorldState vrWorldRouteState() { return g_machine.state; }
bool vrWorldRouteOwnsNextFrame() { return g_ownsNext.load(std::memory_order_acquire); }
bool vrWorldRouteTreatedThisFrame() { return g_f.treated; }
bool vrWorldRouteLayerMayTake() { return vrWorldLayerMayTake(g_machine.state, g_f.treated); }
void vrWorldRouteNoteEyeTaken(uint32_t eye, uint64_t sequence) {
    if (eye < 2) g_takenSequence[eye] = sequence;
    ++g_win.takes;
    if (!g_tookNoted) {
        g_tookNoted = true;
        Log::get().note("vr world route: the layer took the screen for eye %u at frame=%llu (sequence %llu): the eye upscaler, "
                        "its motion prep and UI resolve stand aside for this eye while the route holds the world",
                        eye, static_cast<unsigned long long>(g_frameNo), static_cast<unsigned long long>(sequence));
    }
}
bool vrWorldRouteDoorLayerOnly(uint32_t eye, uint64_t sequence) {
    if (eye >= 2 || !vrWorldDoorLayerOnly(g_takenSequence[eye], sequence)) return false;
    // Asked twice an eye and sequence (the temporal door and then the sharpen pass): counted once.
    if (g_countedLayerOnly[eye] != sequence) { g_countedLayerOnly[eye] = sequence; ++g_win.layerOnly; }
    return true;
}
void vrWorldRouteNoteSceneReset() {
    if (g_key != VrWorldKey::Auto) return;
    g_sceneResetEvent = true;
    g_sceneResetPending = true;
    ++g_win.resets;
}
bool vrWorldRouteDrawProgress(uint32_t* drawOrdinal, bool* toneSeen, uint64_t* frame) {
    if (!g_vrWorldWants) return false;
    if (drawOrdinal) *drawOrdinal = g_f.draws;
    if (toneSeen) *toneSeen = g_f.triggered;
    if (frame) *frame = g_frameNo;
    return true;
}

// SKELETON (stage 2): the route does not jitter yet.
bool vrWorldRouteWorldPhase(float* x, float* y) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    return false;
}

// ---- writes after the trigger (the late-write latch's inputs) ---------------------------------------------------------------
void vrWorldRouteNoteWrite(const void* resource) {
    if (!g_f.hdr.triggered) return;
    flatHdrObserveExplicitWrite(g_f.hdr, resource);
}
void vrWorldRouteNoteDispatch() {
    if (!g_f.hdr.triggered) return;
    for (uint32_t i = 0; i < 4; ++i) {
        void* uav = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::CsUav0) + i));
        if (!uav) continue;
        const ViewEntry* e = lookup(g_f, uav);
        if (e && e->v.known && e->v.resource) flatHdrObserveDispatchWrite(g_f.hdr, e->v.resource);
    }
}
void vrWorldRouteNoteRtvClear(void* rtv) {
    if (!g_f.hdr.triggered || !rtv) return;
    const ViewEntry* e = lookup(g_f, rtv);
    if (e && e->v.known && e->v.resource) flatHdrObserveExplicitWrite(g_f.hdr, e->v.resource);
}

// ---- the per-draw hook ---------------------------------------------------------------------------------------------------------
void vrWorldRouteDraw(ID3D11DeviceContext* ctx) {
    Frame& f = g_f;
    ++f.draws;
    void* rtv = bindingGet(BindSlot::Rtv0);
    if (!rtv) return;                                   // depth-only: no colour target, so no candidate, consumer or H write
    const uint32_t q = ++f.seq;
    void* dsv = bindingGet(BindSlot::Dsv0);
    // A run of draws into one target pair that the last of them showed to be nothing the route watches for (not a candidate,
    // not a consumer, not a write into H) is the same answer again: the shadow atlas takes thousands in a row.
    if (f.runSkippable && rtv == f.runRtv && dsv == f.runDsv) return;
    f.runRtv = rtv; f.runDsv = dsv; f.runSkippable = false;
    const ViewEntry* c = lookup(f, rtv);
    const ViewEntry* d = dsv ? lookup(f, dsv) : nullptr;
    FlatContractObservation& k = g_k;
    if (!c || !vrWorldFillObservation(k, &c->v, d ? &d->v : nullptr, rtv, dsv, bindingShaderHash(BindSlot::Vs),
                                      bindingShaderHash(BindSlot::Ps))) {
        f.runSkippable = true;                          // a colour target the shadow cannot read is ignored, run or not
        return;
    }
    if (!f.hdr.outputWidth) {   // before the first boundary armed the detector
        uint32_t outW = 0, outH = 0;
        vScreenPanelSize(&outW, &outH);
        flatHdrBeginFrame(f.hdr, outW, outH);
    }

    const void* srv[4] = {};
    bool srvKnown = false;
    const bool consumer = !f.hdr.triggered && flatHdrCouldConsume(f.hdr, k);
    if (consumer) {
        for (uint32_t i = 0; i < 4; ++i) {
            void* v = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + i));
            if (!v) continue;
            const ViewEntry* e = lookup(f, v);
            srv[i] = e && e->v.known ? e->v.resource : nullptr;
        }
        srvKnown = true;
    }
    const bool candidate = !f.hdr.triggered && flatHdrCandidateDraw(k, f.hdr.outputWidth, f.hdr.outputHeight);
    const bool trigger = flatHdrObserveDraw(f.hdr, k, q, srv, srvKnown);
    if (candidate) {
        // One depth target for all of H's draws: the route resolves against it.
        if (const FlatHdrCandidate* hc = flatHdrFindCandidate(f.hdr, k.color)) {
            const void*& slot = f.candDepth[hc - f.hdr.candidates];
            if (!slot) slot = k.depth;
            else if (slot != k.depth) f.depthMixed = true;
        }
    }
    f.runSkippable = !trigger && !candidate && !consumer && !(f.hdr.triggered && flatHdrFindCandidate(f.hdr, k.color));
    if (trigger) onTrigger(ctx);
}

// ---- the frame boundary ------------------------------------------------------------------------------------------------------------
void vrWorldRouteFrameBoundary() {
    // The key, live: read here, so the frame that starts now is the first to see a change. Off unless "auto".
    const VrWorldKey key = vrWorldKeyFromText(Config::get().getString("experimental.temporal_aa_on_foot_world", "off").c_str());
    g_key = key;
    g_census = vrCameraCensusWanted();
    if (key == VrWorldKey::Off && g_machine.state == VrWorldState::Off && !g_census) {
        // Nothing to account and nothing to arm: the key-off path is this early return.
        g_vrWorldWants = false; g_vrWorldWatchWrites = false;
        g_ownsNext.store(false, std::memory_order_release);
        return;
    }
    ++g_frameNo;

    // The frame that ended. The layer re-issues the game's OWN screen draw, and a curved screen is not that draw (the geometry
    // substitution swallows it), so with fix.panel_curvature on the layer would refuse every frame (curved-screen) and an owned
    // route would pay for the world resolve while the eye route still served the eyes. The route does not own such a frame: the
    // same predicate that decides the substitution (panel_curve.h) holds it off, with the reason in its one line. (Follow-up: a
    // curve-aware re-issue through panelCurveSubstitute.)
    const bool curved = panelCurveWants();
    const bool layerLive = uiLayerLiveForWorldRoute() && !curved;
    const bool gate = uiLayerWorldScreenHeld();
    if (key == VrWorldKey::Auto) {
        if (g_win.hdr.frames == 0 && g_windowStartMs == 0) g_windowStartMs = GetTickCount64();
        if (g_vrWorldWants) {
            g_win.hdr.noteFrame(g_f.hdr);
            ++g_win.gateFrames;
        }
        if (g_gateSeen && gate != g_gatePrev) ++g_win.gateFlips;
        g_gatePrev = gate; g_gateSeen = true;
        if (g_startedOwned) { ++g_win.ownedFrames; ++g_ownedFramesEpisode; }
        VrWorldFrameEnd e;
        e.keyOn = true;
        e.layerLive = layerLive;
        e.gate = gate;
        e.treated = g_f.treated;
        e.lateWrites = g_f.treated && flatHdrLateWrites(g_f.hdr) != 0;
        e.sceneReset = g_sceneResetEvent;              // the machine lets go; the resolver resets at its next treat
        g_sceneResetEvent = false;
        if (e.lateWrites) {   // (the window's own late-write counters are noted by noteFrame above)
            if (!g_lateWriteLogged) {
                g_lateWriteLogged = true;
                Log::get().note("vr world route: frame=%llu wrote the scene HDR after the trigger: %u draw(s), %u dispatch(es), "
                                "%u explicit write(s); the first is seq %u VS=%016llX PS=%016llX; a treated frame with such "
                                "writes counts toward the latch (%u turns the route off)",
                                static_cast<unsigned long long>(g_frameNo), g_f.hdr.lateDraws, g_f.hdr.lateDispatches,
                                g_f.hdr.lateExplicit, g_f.hdr.lateSequence,
                                static_cast<unsigned long long>(g_f.hdr.lateVs), static_cast<unsigned long long>(g_f.hdr.latePs),
                                kFlatHdrLatchFrames);
            }
        }
        const VrWorldStep step = g_machine.frameEnd(e);
        if (step.entered) {
            ++g_win.enters;
            g_tookNoted = false;
            g_ownedFramesEpisode = 0;
            char line[384];
            vrWorldFormatEntered(line, sizeof(line), g_frameNo, kVrWorldWarmFrames);
            Log::get().note("%s", line);
        }
        if (step.released != VrWorldRelease::None) {
            ++g_win.releases;
            g_win.lastRelease = vrWorldReleaseName(step.released);
            char line[384];
            vrWorldFormatReleased(line, sizeof(line), g_frameNo, step.released, g_ownedFramesEpisode);
            Log::get().note("%s", line);
            g_ownedFramesEpisode = 0;
        }
        if (g_machine.state == VrWorldState::Latched && !g_latchLogged) {
            g_latchLogged = true;
            Log::get().note("vr world route: turned off at frame=%llu after %u treated frame(s) wrote the scene HDR after the "
                            "resolve; the eye route serves the eyes until experimental.temporal_aa_on_foot_world is set off and "
                            "auto again",
                            static_cast<unsigned long long>(g_frameNo), kFlatHdrLatchFrames);
        }
        if (g_machine.state != VrWorldState::Latched) g_latchLogged = false;
        // The key is auto but the layer is not live: one line per reason, saying the route stays off and why.
        if (!layerLive) {
            const char* why = curved ? "fix.panel_curvature bends the on-foot screen and the layer cannot re-issue a curved screen yet"
                                     : uiLayerNotLiveReason();
            if (!why) why = "the UI layer is not live";
            if (g_notLiveNoted != why && (!g_notLiveNoted || std::strcmp(g_notLiveNoted, why) != 0)) {
                g_notLiveNoted = why;
                Log::get().note("vr world route: experimental.temporal_aa_on_foot_world is auto but the route stays off, and "
                                "on-foot VR keeps today's two-eye route: %s (the route hands the eyes the resolved screen through "
                                "the UI layer, so keep fix.ui_quality on and fix.panel_curvature at 0)", why);
            }
        } else {
            g_notLiveNoted = nullptr;
        }
        // The 5 s window: zeros included while the key is auto.
        const uint64_t now = GetTickCount64();
        if (g_windowStartMs && now - g_windowStartMs >= 5000) {
            char line[1024];
            vrWorldFormatWindow(line, sizeof(line), key, g_machine.state, layerLive, gate, g_win);
            Log::get().note("%s", line);
            g_win.reset();
            g_windowStartMs = now;
        }
    } else {
        // The key went off (or never was on): the machine lets go at once and the route's state is cleaned up.
        VrWorldFrameEnd e;
        const VrWorldStep step = g_machine.frameEnd(e);
        if (step.released != VrWorldRelease::None) {
            char line[384];
            vrWorldFormatReleased(line, sizeof(line), g_frameNo, step.released, g_ownedFramesEpisode);
            Log::get().note("%s", line);
        }
        g_ownedFramesEpisode = 0; g_tookNoted = false; g_win.reset(); g_windowStartMs = 0;
        g_sceneResetPending = g_sceneResetEvent = false; g_prev.valid = false; g_declineLines = 0;
        if (g_resourcesLive) {
            // The key went off after the route had made its resolver resources (about 300 MB at 5040x2835) and the mipped
            // screen: let them go once, on the render thread, inside the route's own scope.
            g_resourcesLive = false;
            g_depthViews = DepthViews{};
            VrWorldInternalScope internal;
            flatMonoResolveReset();
            vrWorldMipsReset();
        }
    }

    // The state the next frame starts in.
    g_ownsNext.store(g_machine.owned(), std::memory_order_release);
    g_startedOwned = g_machine.owned();
    g_vrWorldWants = g_census || (key == VrWorldKey::Auto && g_machine.wantsDraws() && gate && layerLive);
    g_vrWorldWatchWrites = false;

    // Arm the detector for the frame that starts now.
    uint32_t outW = 0, outH = 0;
    vScreenPanelSize(&outW, &outH);
    flatHdrBeginFrame(g_f.hdr, outW, outH);
    g_f.viewCount = 0; g_f.viewOverflow = 0; g_f.seq = 0; g_f.draws = 0;
    g_f.runRtv = g_f.runDsv = nullptr; g_f.runSkippable = false;
    std::memset(g_f.candDepth, 0, sizeof(g_f.candDepth));
    g_f.depthMixed = false; g_f.treated = false; g_f.triggered = false;
    g_takenSequence[0] = g_takenSequence[1] = 0;
    g_countedLayerOnly[0] = g_countedLayerOnly[1] = 0;
}

}  // namespace edvr

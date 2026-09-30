// The VR camera census (vr_camera_census.h; the decisions and every line's text are vr_camera_census_core.h).
//
// THREADS. Everything runs on the game's render thread -- the thread that calls Present and so calls
// vrCameraCensusFrameBoundary, which makes it the injector's owner thread -- except the off-thread observer, which is
// lock-free and touches only the VrCensusOffThread table. The refresh detour's two halves (observePre before the game's
// body, observePost after it) run on the owner thread, inside the game's own call: NO I/O, no allocation and no lock
// there, ever. They record into fixed tables; the boundary, once a frame, prints.
//
// KEY OFF. g_wanted is false and g_state is null: the detour is not installed, nothing is allocated, nothing is logged.
// Every entry point returns before touching anything else (tools\vr_camera_census_test's source scan reads these
// functions' first statements).
#include "vr_camera_census.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstring>
#include <new>

#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "flat_camera_inject.h"
#include "flat_camera_phase.h"
#include "flat_compute_readback.h"
#include "journal_watch.h"
#include "vr_camera_census_core.h"
#include "vr_world_route.h"

namespace edvr {
namespace {

using Microsoft::WRL::ComPtr;

// What the refresh's pre half knows while the game's body runs; the post half completes it.
struct Pending {
    bool valid = false;
    VrCensusCall* record = nullptr;   // null when the frame's buffer is full or no sequence is being recorded
    uintptr_t camera = 0, view = 0, ctx = 0;
    uint32_t ordinal = 0, callerRva = 0, draw = 0;
    bool drawKnown = false;
    VrCensusTone tone = VrCensusTone::None;
};

struct State {
    bool active = false;
    VrCensusFoot foot = VrCensusFoot::Off;   // what Elite's journal said at the last boundary
    uint64_t frame = 0;               // the frame in progress, 1-based; 0 before the first boundary
    uint64_t lastWindowMs = 0;
    uint32_t tick = 0;                // 5 s windows since the census started
    uint32_t sequencesLogged = 0;
    bool announced = false;
    bool suppressedNoted[static_cast<size_t>(VrCensusLines::kCount)] = {};
    VrCensusFrame current;
    Pending pending;
    VrCensusCameraTable cameras;
    VrCensusOffThread offThread;
    uint64_t offThreadReported = 0;   // the off-thread table's total at the last 5 s line
    VrCensusWindow window;
    VrCensusBudget budget;
    VrCensusEyeBudget eye;
    ComPtr<ID3D11Buffer> staging;     // the 64-byte readback of an eye's rows, made at the first eye draw
};
State* g_state = nullptr;             // allocated at the first boundary with the key on, never freed
bool g_wanted = false;                // render thread; vrCameraCensusWanted()

// ---- guarded reads of the game's memory (no C++ object lives in a function that has a __try) ------------------------
bool readCamera(uintptr_t camera, VrCensusSnap* out) noexcept {
    __try {
        std::memcpy(out->bytes, reinterpret_cast<const void*>(camera + kVrCensusSnapFrom), kVrCensusSnapBytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool readU32(uintptr_t at, uint32_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void say(State* s, VrCensusLines cls, const char* line) {
    if (s->budget.take(cls)) { Log::get().note("%s", line); return; }
    // The class is capped: the line is counted, not printed, and the first time says so.
    const size_t i = static_cast<size_t>(cls);
    if (!s->suppressedNoted[i] && s->budget.take(VrCensusLines::Info)) {
        s->suppressedNoted[i] = true;
        Log::get().note("vr camera census: line budget reached for %s lines (cap %u): further lines of that kind are "
                        "counted, not printed", vrCensusLinesName(cls), VrCensusBudget::kCap[i]);
    }
}

// ---- the refresh detour's observer (flat_camera_inject.h) ----------------------------------------------------------
// Pre: the call's place in the frame and its record. Returns true: the return is redirected so the post half runs.
bool observePre(const FlatCameraObserveCall& call) noexcept {
    State* s = g_state;
    if (!s || !s->active) return false;
    uint32_t draw = 0;
    bool toneSeen = false;
    uint64_t routeFrame = 0;
    const bool have = vrWorldRouteDrawProgress(&draw, &toneSeen, &routeFrame);
    const VrCensusTone tone = !have ? VrCensusTone::None : toneSeen ? VrCensusTone::After : VrCensusTone::Before;
    if (have) {
        s->window.progressSeen = true;
        s->current.progress = true;
        if (toneSeen) s->current.toneSeen = true;
    }
    const uint32_t callerRva = static_cast<uint32_t>(call.callerRva);
    s->window.noteCall(call.kindReadable, call.kind, callerRva, call.camera, tone, call.window != 0);
    // A sequence is recorded only while one is still wanted and the journal does not rule the frame out (the last one is
    // printed at the boundary that finds its frame sampled); otherwise a call is counted and nothing else.
    const bool recording = vrCensusMayRecord(s->foot) && vrCensusPrintsSequence(true, s->sequencesLogged);
    VrCensusCall* record = nullptr;
    if (recording) record = s->current.add(); else ++s->current.calls;
    Pending& p = s->pending;
    p.valid = true;
    p.record = record;
    p.camera = call.camera;
    p.view = call.p2;
    p.ctx = call.ctx;
    p.ordinal = s->current.calls;
    p.callerRva = callerRva;
    p.draw = draw;
    p.drawKnown = have;
    p.tone = tone;
    if (record) {
        record->camera = call.camera;
        record->view = call.p2;
        record->kind = call.kind;
        record->kindReadable = call.kindReadable;
        record->callerRva = callerRva;
        record->draw = draw;
        record->drawKnown = have;
        record->tone = tone;
        uint32_t flags = 0;
        record->preFlags = readU32(call.camera + kVrCensusFlags, &flags) ? flags : 0;
    }
    return true;
}

// Post: what the body derived. One guarded copy of the camera, the rows the composer wrote for it, its tangents.
void observePost(uintptr_t camera, uintptr_t /*ctx*/) noexcept {
    State* s = g_state;
    if (!s || !s->active) return;
    const Pending p = s->pending;
    s->pending.valid = false;
    if (!p.valid || p.camera != camera) return;
    ++s->window.posts;
    VrCensusSnap snap;
    if (!readCamera(camera, &snap)) return;
    VrCensusSig sig;
    vrCensusSigFromSnap(snap, &sig);
    float tangents[4] = {};
    const bool tangentsValid = vrCensusTangents(sig, tangents);
    if (p.record) {   // the rows are only for a call that is being recorded: none once the sequences are out
        p.record->postSeen = true;
        p.record->postFlags = sig.flags;
        p.record->rowsValid = vrCensusComposeRows(snap, p.record->rows);
    }
    VrCensusCameraTable::Call first;
    first.frame = s->frame;
    first.callerRva = p.callerRva;
    first.ordinal = p.ordinal;
    first.draw = p.draw;
    first.drawKnown = p.drawKnown;
    first.tone = p.tone;
    first.view = p.view;
    first.ctx = p.ctx;
    s->cameras.note(camera, sig, tangents, tangentsValid, first);
}

// Any thread but the owner's: counted into the lock-free table, nothing more.
void observeOffThread(uintptr_t camera, uint64_t callerRva, uint32_t kind, bool kindReadable, uint32_t thread) noexcept {
    State* s = g_state;
    if (!s) return;
    s->offThread.note(thread, camera, kind, kindReadable, callerRva);
}

const FlatCameraObserver g_observer = {&observePre, &observePost, &observeOffThread};

// What Elite's own journal says about the commander, asked once a frame at the boundary (journal_watch.h: atomic peeks).
VrCensusFoot currentFoot() {
    return vrCensusFootFrom(journalWatchActive(), journalOnFootKnown(), journalOnFoot());
}

// ---- the key ---------------------------------------------------------------------------------------------------------
bool readWanted() {
    if (!runtimeVrProfile()) return false;   // a flat profile reads the key off already (Config refuses it); asked twice
    const std::string text = Config::get().getString("advanced.vr_camera_census", "off");
    return vrCameraCensusWantedFor(true, vrCameraCensusKeyFromText(text.c_str()));
}

// ---- the boundary's work ------------------------------------------------------------------------------------------
void activate(State* s) {
    s->active = true;
    s->foot = currentFoot();
    s->frame = 0;
    s->current.reset();
    s->pending = Pending{};
    s->window.reset();
    s->lastWindowMs = GetTickCount64();
    flatCameraInjectSetObserver(&g_observer);   // before the hook can exist: its first call already reports
    flatCameraInjectPause(false);               // a hook that was paused by a key-off reopens its gate
    if (!s->announced && s->budget.take(VrCensusLines::Info)) {
        s->announced = true;
        Log::get().note("vr camera census: on (advanced.vr_camera_census); observe-only, nothing is written to any camera; "
                        "owner thread %lu; 5 s line per window for %u windows, then one per %u; cameras first %u, "
                        "call sequences first %u and eye draws first %u on-foot frames (the tone drawn while the journal, "
                        "read=%s, says on foot); line budget %u",
                        static_cast<unsigned long>(GetCurrentThreadId()), kVrCensusEveryWindow, kVrCensusThinTo,
                        static_cast<unsigned>(VrCensusCameraTable::kCapacity), kVrCensusMaxSequences, kVrCensusMaxEyeFrames,
                        journalWatchActive() ? "yes" : "no: the tone alone decides", VrCensusBudget::capTotal());
    }
}

void deactivate() {
    g_wanted = false;
    flatCameraInjectSetObserver(nullptr);
    flatCameraInjectPause(true);                // the relay's gate closes: the game's refresh runs straight through
    State* s = g_state;
    if (!s) return;
    s->active = false;
    s->pending = Pending{};
    s->current.reset();
    s->staging.Reset();
    if (s->budget.take(VrCensusLines::Info))
        Log::get().note("vr camera census: off (advanced.vr_camera_census); the refresh hook stays in place, inert, until the "
                        "game exits");
}

void printCameraLines(State* s) {
    char line[kVrCensusLineBytes + 16];
    for (size_t i = 0; i < s->cameras.used(); ++i) {
        if (s->cameras.takeCameraLine(i)) {
            vrCensusFormatCamera(line, kVrCensusLineBytes + 1, s->cameras.at(i));
            say(s, VrCensusLines::Camera, line);
        }
        if (s->cameras.takeChangeLine(i)) {
            vrCensusFormatChanged(line, kVrCensusLineBytes + 1, s->cameras.at(i));
            say(s, VrCensusLines::Changed, line);
        }
    }
}

void printOffThread(State* s) {
    char line[kVrCensusLineBytes + 16];
    VrCensusOffThread::Entry e;
    while (s->offThread.takeNew(&e)) {
        vrCensusFormatOtherThread(line, kVrCensusLineBytes + 1, e);
        say(s, VrCensusLines::Thread, line);
    }
}

// The frame that ended was an on-foot frame (the tone was seen): its whole call sequence, in order.
void printSequence(State* s) {
    char line[kVrCensusLineBytes + 16];
    const VrCensusFrame& f = s->current;
    ++s->sequencesLogged;
    vrCensusFormatSequence(line, kVrCensusLineBytes + 1, s->frame, s->sequencesLogged, s->foot, f.calls, f.recorded);
    say(s, VrCensusLines::Call, line);
    for (uint32_t i = 0; i < f.recorded; ++i) {
        vrCensusFormatCall(line, kVrCensusLineBytes + 1, s->frame, i + 1, f.call[i]);
        say(s, VrCensusLines::Call, line);
    }
}

// The frame that just ended is accounted and printed; the next one starts empty.
void rollFrame(State* s) {
    if (s->frame) {
        const bool sampled = vrCensusSamplesFrame(s->current.toneSeen, s->current.progress, s->foot);
        ++s->window.frames;
        if (s->current.toneSeen) ++s->window.toneFrames;
        if (sampled) ++s->window.onFootFrames;
        printCameraLines(s);      // a camera's line precedes the sequence that names it
        printOffThread(s);
        // The frame the journal flips at was recorded under the old word (nothing): it has no calls to print and must not
        // spend one of the sequences as an empty header. The next frame is recorded under the new word.
        if (vrCensusPrintsSequence(sampled, s->sequencesLogged) && s->current.recorded > 0) printSequence(s);
    }
    s->current.reset();
    s->pending = Pending{};
    ++s->frame;
}

void tickWindow(State* s) {
    const uint64_t now = GetTickCount64();
    if (now - s->lastWindowMs < 5000) return;
    s->lastWindowMs = now;
    ++s->tick;
    ++s->window.windows;
    if (!vrCensusWindowPrints(s->tick)) return;   // the counters keep adding; the line that prints covers every window
    VrCensusWindowText text;
    text.hook = flatCameraInjectObserveStatus();
    text.foot = s->foot;
    const uint64_t total = s->offThread.total();
    text.offThread = total - s->offThreadReported;
    text.offThreadOverflow = s->offThread.overflow();
    text.camerasTotal = static_cast<uint32_t>(s->cameras.used());
    text.cameraTableOverflow = s->cameras.overflow();
    char line[kVrCensusLineBytes + 16];
    vrCensusFormatWindow(line, kVrCensusLineBytes + 1, s->window, text);
    say(s, VrCensusLines::Window, line);
    s->offThreadReported = total;
    s->window.reset();
}

// ---- the eye draw's readback ---------------------------------------------------------------------------------------
// VS constant buffer slot 1 -> a 64-byte staging copy of rows 270..273 -> Map. One stall per call, at most eight calls a
// session. The census's own D3D calls step past the hooks (FlatComputeInternalScope).
bool readEyeRows(State* s, ID3D11DeviceContext* ctx, float rows[16], uint64_t* b1Out, uint32_t* firstOut, uint32_t* bytesOut,
                 const char** why) {
    ComPtr<ID3D11Buffer> b1;
    UINT first = 0, num = 0;
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))) && ctx1) ctx1->VSGetConstantBuffers1(1, 1, b1.GetAddressOf(), &first, &num);
    else ctx->VSGetConstantBuffers(1, 1, b1.GetAddressOf());
    if (!b1) { *why = "no-b1"; return false; }
    D3D11_BUFFER_DESC bd{};
    b1->GetDesc(&bd);
    *b1Out = reinterpret_cast<uint64_t>(b1.Get());
    *firstOut = first;
    *bytesOut = bd.ByteWidth;
    const uint32_t offset = (first + 270u) * 16u;
    if (bd.ByteWidth < offset + 64u) { *why = "b1-too-small"; return false; }
    if (!s->staging) {
        ComPtr<ID3D11Device> device;
        b1->GetDevice(&device);
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = 64;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!device || FAILED(device->CreateBuffer(&sd, nullptr, &s->staging))) { *why = "staging"; return false; }
    }
    const D3D11_BOX box{offset, 0, 0, offset + 64u, 1, 1};
    ctx->CopySubresourceRegion(s->staging.Get(), 0, 0, 0, 0, b1.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(s->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) { *why = "map"; return false; }
    std::memcpy(rows, mapped.pData, 64);
    ctx->Unmap(s->staging.Get(), 0);
    return true;
}

}  // namespace

// ---- the public entry points ------------------------------------------------------------------------------------
bool vrCameraCensusWanted() { return g_wanted; }

void vrCameraCensusFrameBoundary() {
    const bool wanted = readWanted();
    if (!wanted) {
        if (g_wanted) deactivate();   // the key went off while the census ran: detach, close the gate, say so once
        return;
    }
    State* s = g_state;
    if (!s) {
        s = new (std::nothrow) State;
        if (!s) return;
        g_state = s;                   // published before the observer is registered (the release store below)
    }
    if (!s->active) activate(s);
    g_wanted = true;
    flatCameraInjectDisarm();          // the Present edge: the window closes and THIS thread is the owner
    s->foot = currentFoot();           // the journal's word for the frame that ended, and for the one that starts
    rollFrame(s);
    flatCameraInjectObserveFrame();    // installs the hook once (observe-only), opens the window
    tickWindow(s);
}

void vrCameraCensusEyeDraw(ID3D11DeviceContext* ctx, uint32_t eye) {
    if (!g_wanted) return;
    State* s = g_state;
    if (!s || !s->active || !ctx || eye > 1) return;
    ++s->window.eyeDraws;
    uint32_t draw = 0;
    bool toneSeen = false;
    uint64_t routeFrame = 0;
    const bool have = vrWorldRouteDrawProgress(&draw, &toneSeen, &routeFrame);
    if (have) {
        s->window.progressSeen = true;
        s->current.progress = true;
        if (toneSeen) s->current.toneSeen = true;
    }
    // Only a frame the census samples is read back: the tone drawn before this draw and the journal, when it is read, saying
    // on foot; or, with no draw progress at all, the journal alone saying on foot.
    if (!vrCensusSamplesFrame(toneSeen, have, s->foot)) return;
    ++s->window.eyeOnFoot;
    if (!s->eye.take(s->frame)) return;

    float rows[16] = {};
    uint64_t b1 = 0;
    uint32_t first = 0, bytes = 0;
    const char* why = nullptr;
    bool read = false;
    {
        FlatComputeInternalScope internal;   // the hooks step aside for the census's own copy, Map and Unmap
        read = readEyeRows(s, ctx, rows, &b1, &first, &bytes, &why);
    }
    double measX = 0, measY = 0;
    bool measured = false;
    if (read) {
        float six[6][4] = {};   // rows 270..275 in flatCameraMeasureRowShift's shape; 274 and 275 are unused by it
        std::memcpy(six, rows, sizeof(rows));
        measured = flatCameraMeasureRowShift(six, measX, measY);
    }
    char line[kVrCensusLineBytes + 16];
    vrCensusFormatEye(line, kVrCensusLineBytes + 1, eye, s->frame, s->foot, true, draw, b1, first, bytes,
                      read ? rows : nullptr, measured, measX, measY, read ? nullptr : why);
    say(s, VrCensusLines::Eye, line);
    uint64_t sequence = 0;
    float frustum[4] = {}, shift[2] = {};
    const bool known = nativeTemporalEyeGeometry(eye, &sequence, frustum, shift);
    vrCensusFormatEyeGeometry(line, kVrCensusLineBytes + 1, eye, s->frame, known, sequence, frustum, shift, measured, measX,
                              measY);
    say(s, VrCensusLines::Eye, line);
}

}  // namespace edvr

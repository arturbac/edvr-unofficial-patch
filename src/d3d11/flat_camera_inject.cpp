#include "flat_camera_inject.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "flat_camera_ownership.h"
#include "flat_runtime.h"

namespace edvr {
namespace {

// The upstream camera injector (docs/design-flat-camera-integration.md, C3
// wiring plan addendum). One CodeHook on the game's view-constant refresh
// (FUN_1405921f0): the detour applies the temporal phase to the camera's
// bound pair transiently -- mutate on the entry values, call the original,
// restore immediately (the restore-after-call protocol: no accumulation,
// nothing left behind on disable, mid-frame setters compose naturally).
// Bits 4 and 8 are raised so the refresh's own finalizers re-derive the
// projection and the cached VP from the mutated parameters in the same
// call; bits 2/1 stay untouched (a projection-only jitter leaves view rows
// and ray snapshots legitimately unchanged).
//
// Safety discipline is the producer probe's, learned from its two crashed
// flights and the probe rebuild: prologue-verified single site, gate-first
// 44-byte relay, forward-and-return through the trampoline, and hold-open
// for process lifetime -- disable closes the gate and nothing more (no
// uninstall, no free, ever).

constexpr uintptr_t kRefreshRva = 0x5921f9;
// The function's own first instruction is a conditional jump CodeHook
// cannot move; the hook sits one instruction later, after the param_2
// null check (a null param_2 is no refresh anyway).
constexpr uint8_t kRefreshPrologue[16] = {0x48, 0x8B, 0xC4, 0x41, 0x55, 0x41, 0x56, 0x41,
                                         0x57, 0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00};
constexpr size_t kRelayBytes = 44;
constexpr uint32_t kOriginalLiteral = 36;

// The camera struct's fields (camera-relative, the typed table).
constexpr uint32_t kCamKind = 0x264;
constexpr uint32_t kCamBoundX = 0x28C;
constexpr uint32_t kCamBoundY = 0x290;
constexpr uint32_t kCamFlags = 0x250;
constexpr uint32_t kFlagProj = 4, kFlagVP = 8;

struct InjectState {
    std::atomic<bool> installed{false};
    CodeHook hook;
    uint8_t* relay = nullptr;
    const char* failReason = "not attempted";
    // The ownership machine for the main view-group (auxiliary cameras are
    // named unsupported this increment, per group reporting).
    FlatCameraOwnershipState owner{};
    FlatCameraOwnershipDecision decision{};
    bool decisionValid = false;
    uint64_t frame = 0;
    // Counters, reported on the cadence tick.
    std::atomic<uint64_t> refreshCalls{0};
    std::atomic<uint64_t> injectedCalls{0};
    std::atomic<uint64_t> kindRefusals{0};
    std::atomic<uint64_t> unsupportedCameras{0};
    std::atomic<uint64_t> warmingCalls{0};
    std::atomic<uint64_t> rayCbLogged{0};
    uint64_t lastLogMs = 0;
    uint64_t lastKindRefusalLogMs = 0;
    float lastRay[4] = {};
};
InjectState g_inject;
std::atomic<uintptr_t> g_gate{0};
std::atomic<uintptr_t> g_refreshForward{0};

bool sehCheck(uintptr_t at, const uint8_t* expected, size_t bytes) noexcept {
    __try { return std::memcmp(reinterpret_cast<const void*>(at), expected, bytes) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehReadU32(uintptr_t at, uint32_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehReadF32(uintptr_t at, float* out) noexcept {
    __try {
        *out = *reinterpret_cast<const float*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehReadU64(uintptr_t at, uint64_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const uint64_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehWriteF32(uintptr_t at, float value) noexcept {
    __try {
        *reinterpret_cast<float*>(at) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehWriteU32(uintptr_t at, uint32_t value) noexcept {
    __try {
        *reinterpret_cast<uint32_t*>(at) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint8_t* allocateRelay(uintptr_t target) noexcept {
    const uintptr_t granularity = 64 * 1024, span = 0x7FFF0000ull;
    const uintptr_t begin = (target - span) & ~(granularity - 1);
    for (uintptr_t candidate = target; candidate >= begin; candidate -= granularity) {
        auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096,
                                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (p) return p;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback];
    // original: jmp [trampoline]. The proven 44-byte pose_reader_watch layout.
    const uint8_t body[kRelayBytes] = {
        0x48,0xB8,0,0,0,0,0,0,0,0, 0x48,0x83,0x38,0,
        0x74,0x0E, 0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0,
        0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8); std::memcpy(code + 22, &callbackAddress, 8);
}

bool prepareRelay(void* trampoline, void*) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(g_inject.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_inject.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), g_inject.relay, kRelayBytes)) return false;
    g_refreshForward.store(address, std::memory_order_release);
    return true;
}

// The ray CB observation: after the original refresh returns, read the
// slot record at lVar4+0x78 (lVar4 = *(ctx+0x28) per the refresh's own
// layout) and log the composition's first floats when they materially
// change. This is the follow-up that pins the consumer: the composed
// values cross-check the rig's composeRayCb against the live game, and
// the slot record is the handle for naming the binding shader next.
void observeRayCb(uintptr_t ctx) {
    uint64_t lVar4 = 0;
    if (!sehReadU64(ctx + 0x28, &lVar4) || !lVar4) return;
    uint64_t staging = 0;
    if (!sehReadU64(static_cast<uintptr_t>(lVar4) + 0x78, &staging) || !staging) return;
    float row[4] = {};
    bool ok = true;
    for (uint32_t i = 0; i < 4; ++i) ok &= sehReadF32(static_cast<uintptr_t>(staging) + 4 * i, &row[i]);
    if (!ok) return;
    const float drift = std::fabs(row[0] - g_inject.lastRay[0]) +
                        std::fabs(row[1] - g_inject.lastRay[1]) +
                        std::fabs(row[2] - g_inject.lastRay[2]) +
                        std::fabs(row[3] - g_inject.lastRay[3]);
    if (g_inject.rayCbLogged.load(std::memory_order_relaxed) != 0 && drift < 1e-3f) return;
    for (int i = 0; i < 4; ++i) g_inject.lastRay[i] = row[i];
    const uint64_t n = g_inject.rayCbLogged.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 8) return; // bounded: first anchor plus a few material changes
    float more[12] = {};
    for (uint32_t i = 0; i < 12; ++i)
        if (!sehReadF32(static_cast<uintptr_t>(staging) + 16 + 4 * i, &more[i])) return;
    Log::get().note("flat camera inject: ray CB slot %p anchors (%llux): "
        "[%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f]",
        reinterpret_cast<void*>(staging), (unsigned long long)n,
        row[0], row[1], row[2], row[3], more[0], more[1], more[2], more[3],
        more[4], more[5], more[6], more[7], more[8], more[9], more[10], more[11]);
}

using RefreshFn = void (__fastcall*)(uintptr_t, uintptr_t, uintptr_t);
void __fastcall refreshDetour(uintptr_t ctx, uintptr_t p2, uintptr_t camera) noexcept {
    const auto forward = reinterpret_cast<RefreshFn>(g_refreshForward.load(std::memory_order_acquire));
    if (!forward) return;
    g_inject.refreshCalls.fetch_add(1, std::memory_order_relaxed);

    // Admission for THIS call: the injector owns only a kind-3 camera whose
    // frame ownership is Upstream. Anything else passes through untouched
    // (and unsupported cameras are counted, not silently jittered).
    uint32_t kind = 0;
    const bool readable = camera && sehReadU32(camera + kCamKind, &kind);
    if (!readable || kind != 3) {
        g_inject.kindRefusals.fetch_add(1, std::memory_order_relaxed);
        const uint64_t now = GetTickCount64();
        if (now - g_inject.lastKindRefusalLogMs > 30000) {
            g_inject.lastKindRefusalLogMs = now;
            Log::get().note("flat camera inject: camera %p kind %u is not the proven branch (named unsupported; no mutation)",
                            reinterpret_cast<void*>(camera), readable ? kind : 0xffffffffu);
        }
        forward(ctx, p2, camera);
        return;
    }
    if (!g_inject.decisionValid || g_inject.decision.owner != FlatCameraOwner::Upstream) {
        forward(ctx, p2, camera);
        return;
    }

    // The phase, in RENDER pixels from the validated resolve plan (R5).
    float jx = 0, jy = 0;
    uint32_t rw = 0, rh = 0, applied = 0;
    flatRuntimePhaseState(&jx, &jy, &rw, &rh, &applied);
    if (!rw || !rh || (jx == 0.0f && jy == 0.0f)) {
        g_inject.warmingCalls.fetch_add(1, std::memory_order_relaxed);
        forward(ctx, p2, camera);
        return;
    }

    float entryX = 0, entryY = 0;
    if (!sehReadF32(camera + kCamBoundX, &entryX) || !sehReadF32(camera + kCamBoundY, &entryY)) {
        forward(ctx, p2, camera);
        return;
    }
    // The D3D sign convention W1 proved: content right by jx needs
    // boundX += jx/R_w; content down by jy needs boundY += -jy/R_h.
    const float jitX = entryX + jx / static_cast<float>(rw);
    const float jitY = entryY - jy / static_cast<float>(rh);
    uint32_t flags = 0;
    const bool haveFlags = sehReadU32(camera + kCamFlags, &flags);
    if (!sehWriteF32(camera + kCamBoundX, jitX) ||
        !sehWriteF32(camera + kCamBoundY, jitY) ||
        (haveFlags && !sehWriteU32(camera + kCamFlags, flags | kFlagProj | kFlagVP))) {
        forward(ctx, p2, camera);
        return;
    }
    forward(ctx, p2, camera);
    // Restore-after-call: the entry values ARE the authoritative originals.
    // The derived blocks keep this call's jitter (the committed phase); the
    // sources return pristine for the next derivation.
    sehWriteF32(camera + kCamBoundX, entryX);
    sehWriteF32(camera + kCamBoundY, entryY);
    if (haveFlags) sehWriteU32(camera + kCamFlags, flags);
    g_inject.injectedCalls.fetch_add(1, std::memory_order_relaxed);
    flatRuntimeNoteCameraApplied();
    observeRayCb(ctx);
}

void standDown(const char* why) {
    if (g_gate.exchange(0, std::memory_order_acq_rel) != 0) {
        Log::get().note("flat camera inject: %s; the refresh hook stays in place as an inert "
                        "pass-through for process lifetime", why);
    }
}

} // namespace

bool flatCameraInjectWanted() {
    if (!runtimeFlatProfile()) return false;
    return _stricmp(Config::get().getString("fix.temporal_aa_camera", "off").c_str(), "on") == 0;
}

bool flatCameraInjectUpstreamOwns() {
    return g_inject.decisionValid && g_inject.decision.owner == FlatCameraOwner::Upstream;
}

bool flatCameraInjectBypassRefusal(const char* reason) {
    if (!flatCameraInjectUpstreamOwns() || !reason) return false;
    // The observation-veto class only: an unknown recipe on a certified
    // camera lineage. True violations keep refusing in flat_runtime.
    return std::strcmp(reason, "unknown-scene-projection-recipe") == 0;
}

void flatCameraInjectFrame(uint64_t frame) {
    if (!runtimeFlatProfile()) { standDown("the flat profile is off"); return; }
    if (!flatCameraInjectWanted()) {
        standDown("fix.temporal_aa_camera is off");
        g_inject.decisionValid = false;
        return;
    }
    if (frame != g_inject.frame) {
        g_inject.frame = frame;
        flatCameraOwnerBegin(g_inject.owner);
        FlatCameraGroupInput in;
        in.upstreamCertified = true; // the detour re-verifies kind 3 per call
        in.legacyEligible = flatRuntimeLegacyPlanExists();
        in.legacyObserving = false;
        g_inject.decision = flatCameraOwnerSelect(g_inject.owner, in);
        g_inject.decisionValid = true;
    }
    if (!g_inject.installed.load(std::memory_order_acquire)) {
        if (g_inject.relay) return; // a failed install is final for the session
        const HMODULE game = GetModuleHandleW(L"EliteDangerous64.exe");
        if (!game) {
            if (!g_inject.failReason[0] || std::strcmp(g_inject.failReason, "not attempted") == 0) {
                g_inject.failReason = "EliteDangerous64.exe is not loaded in this process";
                Log::get().note("flat camera inject: wanted but %s; standing down", g_inject.failReason);
            }
            return;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(game);
        if (!sehCheck(base + kRefreshRva, kRefreshPrologue, sizeof(kRefreshPrologue))) {
            g_inject.failReason = "refresh prologue mismatch at this build (not the Ghidra-verified shape)";
            Log::get().note("flat camera inject: %s; standing down", g_inject.failReason);
            g_inject.relay = reinterpret_cast<uint8_t*>(1); // do not retry
            return;
        }
        g_inject.relay = allocateRelay(base + kRefreshRva);
        if (!g_inject.relay) { g_inject.failReason = "relay allocation failed (no free memory within 2 GB)"; return; }
        buildRelay(g_inject.relay, &g_gate, &refreshDetour);
        if (!g_inject.hook.install(reinterpret_cast<void*>(base + kRefreshRva), g_inject.relay, nullptr,
                                   "camera-inject-refresh", &prepareRelay, &g_inject)) {
            VirtualFree(g_inject.relay, 0, MEM_RELEASE); g_inject.relay = reinterpret_cast<uint8_t*>(1);
            g_inject.failReason = "CodeHook refused it (its own line above names why)";
            Log::get().note("flat camera inject: %s; standing down", g_inject.failReason);
            return;
        }
        g_inject.installed.store(true, std::memory_order_release);
        g_gate.store(1, std::memory_order_release);
        Log::get().note("flat camera inject: refresh hook installed at EliteDangerous64.exe+0x%llX; "
                        "kind-3 cameras now get the temporal phase applied transiently at the source",
                        static_cast<unsigned long long>(kRefreshRva));
    }
    const uint64_t now = GetTickCount64();
    if (now - g_inject.lastLogMs >= 5000) {
        g_inject.lastLogMs = now;
        Log::get().note("flat camera inject 5s: refresh-calls=%llu injected=%llu warming=%llu "
                        "kind-refusals=%llu unsupported=%llu owner=%s history=%s",
            (unsigned long long)g_inject.refreshCalls.exchange(0),
            (unsigned long long)g_inject.injectedCalls.exchange(0),
            (unsigned long long)g_inject.warmingCalls.exchange(0),
            (unsigned long long)g_inject.kindRefusals.exchange(0),
            (unsigned long long)g_inject.unsupportedCameras.exchange(0),
            g_inject.decisionValid && g_inject.decision.owner == FlatCameraOwner::Upstream
                ? "upstream" : "legacy/none",
            g_inject.owner.historyValid ? "valid" : "invalid");
    }
}

} // namespace edvr

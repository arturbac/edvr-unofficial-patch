#include "flat_camera_producer_probe.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/module_name.h"
#include "../common/runtime_profile.h"
#include "../common/vtable_hook.h" // isExecutableAddress
#include "pose_reader_watch_core.h"

namespace edvr {
namespace {

// The camera producer probe (docs/design-flat-camera-integration.md C1->C2).
//
// Step 1 hooks the Ghidra-validated single-site upload helper
// (EliteDangerous64.exe+0x51B640, the f3dx Map/memcpy/Unmap path) to learn
// the staging pool block of the 5376-byte scene CB. Step 2 arms one hardware
// write breakpoint on the block's camera-row span (rows 270..271, the
// 0x10E0..0x1100 window) on the uploading thread; each hit names the
// writer's RIP with a bounded stack. Budgets: 8 distinct blocks, 8 re-arms,
// 8 writer hits, 30 seconds armed. A refusal stands down and says why; it
// never guesses.

constexpr uintptr_t kUploadRva = 0x51B640;
constexpr uint8_t kUploadPrologue[16] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                         0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x41};
constexpr uint32_t kSceneBytes = 5376;
constexpr uint32_t kCameraRowOffset = 270u * 16u; // 0x10E0
constexpr size_t kRelayBytes = 44;
constexpr uint32_t kOriginalLiteral = 36;
constexpr uint32_t kMaxBlocks = 8, kMaxRearms = 8, kMaxHits = 8, kArmMs = 30000;

struct ProbeState {
    std::atomic<bool> installed{false};
    CodeHook hook;
    uint8_t* relay = nullptr;
    const char* failReason = "not attempted";
    // The staging blocks seen from scene-sized uploads (dedup by pointer).
    const void* blocks[kMaxBlocks]{};
    uint32_t blockCount = 0;
    // The hardware write watch on the camera-row span.
    std::atomic<uintptr_t> watchAddress{0};
    bool armed = false;
    uint64_t armedAtMs = 0;
    uint32_t rearmCount = 0, hits = 0;
    bool handlerInstalled = false, baseNoted = false;
};
ProbeState g_probe;
std::atomic<uintptr_t> g_gate{0};
// The trampoline to the real upload helper, stored by prepareRelay; the
// relay callback calls it so the scene CB upload is never swallowed.
std::atomic<uintptr_t> g_uploadForward{0};

bool sehCheck(uintptr_t at, const uint8_t* expected, size_t bytes) noexcept {
    __try { return std::memcmp(reinterpret_cast<const void*>(at), expected, bytes) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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
    // original: jmp [trampoline]. RAX/flags are volatile on entry here.
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
    std::memcpy(g_probe.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_probe.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), g_probe.relay, kRelayBytes)) return false;
    g_uploadForward.store(address, std::memory_order_release);
    return true;
}

void disarmWatch(const char* reason) {
    if (!g_probe.armed) return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(GetCurrentThread(), &ctx)) {
        const uint32_t low = static_cast<uint32_t>(ctx.Dr7 & 0xFFFFFFFFull);
        const uint32_t high = static_cast<uint32_t>((ctx.Dr7 >> 32) & 0xFFFFFFFFull);
        ctx.Dr7 = (static_cast<DWORD64>(high) << 32) | prw::disarmSlot0Dr7(low);
        SetThreadContext(GetCurrentThread(), &ctx);
    }
    g_probe.armed = false;
    Log::get().note("flat camera producer: camera-row write watch disarmed (%s; %u hit(s) recorded)",
                    reason, g_probe.hits);
}

void armWatch(const void* block, uint64_t frame) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(block) + kCameraRowOffset;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(GetCurrentThread(), &ctx)) return;
    const uint32_t low = static_cast<uint32_t>(ctx.Dr7 & 0xFFFFFFFFull);
    const uint32_t high = static_cast<uint32_t>((ctx.Dr7 >> 32) & 0xFFFFFFFFull);
    ctx.Dr7 = (static_cast<DWORD64>(high) << 32) | prw::armSlot0Dr7(low, prw::kDr7RwWrite);
    ctx.Dr0 = static_cast<DWORD64>(address);
    if (!SetThreadContext(GetCurrentThread(), &ctx)) return;
    ++g_probe.rearmCount;
    g_probe.armed = true; g_probe.armedAtMs = GetTickCount64();
    g_probe.watchAddress.store(address, std::memory_order_release);
    Log::get().note("flat camera producer: camera-row write watch armed at %p (frame=%llu, re-arm %u/%u)",
                    reinterpret_cast<void*>(address), (unsigned long long)frame,
                    g_probe.rearmCount, kMaxRearms);
}

LONG CALLBACK producerWatchVeh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (!(ep->ContextRecord->Dr6 & 1) || !g_probe.armed) return EXCEPTION_CONTINUE_SEARCH;
    ++g_probe.hits;
    if (g_probe.hits <= kMaxHits) {
        char rip[96]; moduleBrief(reinterpret_cast<void*>(ep->ContextRecord->Rip), rip, sizeof(rip));
        void* frames[8] = {};
        const USHORT n = CaptureStackBackTrace(0, 8, frames, nullptr);
        char line[768]{}; size_t used = std::snprintf(line, sizeof(line), "%s", rip);
        for (USHORT i = 0; i < n && used < sizeof(line) - 96; ++i) {
            if (!isExecutableAddress(frames[i])) continue;
            char brief[96]; moduleBrief(frames[i], brief, sizeof(brief));
            used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " <- %s", brief));
        }
        Log::get().note("flat camera producer: camera-row writer #%u: %s", g_probe.hits, line);
    }
    ep->ContextRecord->EFlags |= 0x10000; // RF: do not re-trigger on this instruction
    if (g_probe.hits >= kMaxHits) disarmWatch("the writer budget is full");
    return EXCEPTION_CONTINUE_EXECUTION;
}

void observeSceneUpload(const void* src, int sizeA, int sizeB) {
    if (static_cast<int64_t>(sizeA) * sizeB != kSceneBytes || !src) return;
    for (uint32_t i = 0; i < g_probe.blockCount; ++i)
        if (g_probe.blocks[i] == src) return;
    if (g_probe.blockCount >= kMaxBlocks) return; // the block budget is full; count only
    g_probe.blocks[g_probe.blockCount++] = src;
    Log::get().note("flat camera producer: scene-sized staging block %p (%u/%u)",
                    src, g_probe.blockCount, kMaxBlocks);
    if (!g_probe.armed && g_probe.rearmCount < kMaxRearms) armWatch(src, 0);
}

using UploadFn = int (__fastcall*)(uintptr_t, uintptr_t, const void*, int, int);
int __fastcall uploadRelay(uintptr_t a, uintptr_t b, const void* src, int sizeA, int sizeB) noexcept {
    const auto forward = reinterpret_cast<UploadFn>(g_uploadForward.load(std::memory_order_acquire));
    if (!forward) return 0;
    observeSceneUpload(src, sizeA, sizeB);
    return forward(a, b, src, sizeA, sizeB);
}

void standDown(const char* why) {
    g_gate.store(0, std::memory_order_release);
    disarmWatch(why);
    if (g_probe.installed.exchange(false)) {
        g_probe.hook.uninstall();
        Log::get().note("flat camera producer: upload hook removed (%s)", why);
    }
    if (g_probe.relay) { VirtualFree(g_probe.relay, 0, MEM_RELEASE); g_probe.relay = nullptr; }
}

} // namespace

void flatCameraProducerProbeFrame(uint64_t frame) {
    if (!runtimeFlatProfile()) { standDown("the flat profile is off"); return; }
    const bool wanted = _stricmp(Config::get().getString("advanced.flat_camera_producer_probe", "off").c_str(), "off") != 0;
    if (!wanted) { standDown("advanced.flat_camera_producer_probe is off"); return; }
    if (g_probe.armed && GetTickCount64() - g_probe.armedAtMs > kArmMs) disarmWatch("30 s armed without a full budget");
    if (g_probe.installed.load(std::memory_order_acquire)) return;
    if (g_probe.relay) return; // a failed install is final for the session
    const HMODULE game = GetModuleHandleW(L"EliteDangerous64.exe");
    if (!game) {
        if (!g_probe.baseNoted) {
            g_probe.baseNoted = true;
            g_probe.failReason = "EliteDangerous64.exe is not loaded in this process";
            Log::get().note("flat camera producer: wanted but %s; standing down", g_probe.failReason);
        }
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(game);
    if (!sehCheck(base + kUploadRva, kUploadPrologue, sizeof(kUploadPrologue))) {
        g_probe.failReason = "upload helper prologue mismatch at this build (not the Ghidra-verified shape)";
        Log::get().note("flat camera producer: %s; the probe stands down", g_probe.failReason);
        g_probe.relay = reinterpret_cast<uint8_t*>(1); // do not retry
        return;
    }
    g_probe.relay = allocateRelay(base + kUploadRva);
    if (!g_probe.relay) { g_probe.failReason = "relay allocation failed (no free memory within 2 GB)"; return; }
    buildRelay(g_probe.relay, &g_gate, &uploadRelay);
    if (!g_probe.handlerInstalled) {
        AddVectoredExceptionHandler(1, &producerWatchVeh);
        g_probe.handlerInstalled = true;
    }
    if (!g_probe.hook.install(reinterpret_cast<void*>(base + kUploadRva), g_probe.relay, nullptr,
                              "camera-producer-upload", &prepareRelay, &g_probe)) {
        VirtualFree(g_probe.relay, 0, MEM_RELEASE); g_probe.relay = reinterpret_cast<uint8_t*>(1);
        g_probe.failReason = "CodeHook refused it (its own line above names why)";
        Log::get().note("flat camera producer: %s; the probe stands down", g_probe.failReason);
        return;
    }
    g_probe.installed.store(true, std::memory_order_release);
    g_gate.store(1, std::memory_order_release);
    Log::get().note("flat camera producer: upload hook installed at EliteDangerous64.exe+0x%llX; "
                    "scene-sized uploads now name their staging block, first one arms the camera-row write watch",
                    static_cast<unsigned long long>(kUploadRva));
}

} // namespace edvr

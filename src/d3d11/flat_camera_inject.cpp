#include "flat_camera_inject.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <intrin.h>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "flat_camera_ownership.h"
#include "flat_runtime.h"

extern "C" DWORD _tls_index; // CRT-provided once a __declspec(thread) exists

namespace edvr {
namespace {

// The upstream camera injector (docs/design-flat-camera-integration.md, C3
// wiring plan addendum). One CodeHook on the game's view-constant refresh
// (FUN_1405921f0): the detour applies the temporal phase to the camera's
// bound pair transiently -- mutate on the entry values, run the original,
// restore immediately (the restore-after-call protocol: no accumulation,
// nothing left behind on disable, mid-frame setters compose naturally).
// Bits 4 and 8 are raised so the refresh's own finalizers re-derive the
// projection and the cached VP from the mutated parameters in the same
// call; bits 2/1 stay untouched (a projection-only jitter leaves view rows
// and ray snapshots legitimately unchanged).
//
// THE MECHANISM, rewritten after five .rdata crashes. The hook site is
// mid-function: the relay JUMPS to the detour with rsp = S, the game's
// live stack -- no return address is pushed, and [S]/[S+8]/[S+0x10] hold
// the game's saved r14, saved r13 and the function's own return address.
// A compiled C detour is incompatible with that twice over: its prologue
// spills through [rsp+8/10h/18h] onto exactly those slots, and a
// call-forward parks the game body 0xE0 bytes below its own frame, so the
// body's epilogue (add rsp,0xE0; pop r15/r14/r13; ret) reads the detour's
// frame -- in the 19:14 dump r14 held the call's own return address
// (d3d11+0x6FCA5) and the body's ret jumped to a spilled log-string
// pointer, which is the RIP-in-.rdata signature of all five crashes.
//
// So the detour is generated code, built at install:
//   stubA (the relay's callback): push all fifteen GPRs (landing
//     16-aligned), call refreshPre, pop every register -- r15 included,
//     because the trampoline's stolen "push r15" must save the GAME's
//     r15 -- and jmp to the trampoline with rsp = S exactly. The body
//     then runs byte-identical to unhooked.
//   refreshPre: the pre-forward half (admission, the phase, the dirty
//     bits) plus the redirection that gives the post-half control: the
//     body's ret pops whatever sits at [R0], so stubB's address goes
//     there and the real return address is kept in TLS. A refusal is
//     simply "don't redirect": stubA joins the trampoline with the stack
//     exactly as found and the body returns straight to its caller.
//   stubB: the body's ret lands here with rsp = R0+8 and the game's
//     return state in every register. It preserves rax/xmm0 and the
//     scratch set, calls refreshPost (restore-after-call, counters, the
//     ray CB observation), then jumps to the real return address from
//     TLS. r11 alone carries the TLS walk: it has no ABI role across a
//     return.
// Single-level per thread: the refresh is not recursive (the C2 lineage
// and the decompile agree), so one TLS slot per thread suffices. A body
// that unwound instead of returning would leave the armed flag set; the
// next call clears it, and nothing else ever reads it.
//
// Safety discipline is the producer probe's, learned from its two crashed
// flights and the probe rebuild: prologue-verified single site, gate-first
// relay that preserves EVERY register (the 20:09/03:50 crashes: this
// pipeline family inherits a frame base in r11 and the unhooked refresh
// preserves it, so the old r11-gate relay broke the callers' invariant),
// forward through the trampoline, and hold-open for process lifetime --
// disable closes the gate and nothing more (no uninstall, no free, ever).

constexpr uintptr_t kRefreshRva = 0x592200;
// Two site constraints, learned from two refused installs: the function's
// second instruction is a conditional jump CodeHook will not move, and the
// patch site must be eight-byte aligned for the atomic store. +0x592200 is
// the first site satisfying both (push r15; sub rsp,0xE0; register moves
// after that replay inside the trampoline). The detour's arguments arrive
// in rcx/rdx/r8 untouched either way, and rax (the frame anchor the
// prologue set with mov rax,rsp) must reach the body intact.
constexpr uint8_t kRefreshPrologue[16] = {0x41, 0x57, 0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00,
                                         0x00, 0x4D, 0x8B, 0xF8, 0x4C, 0x8B, 0xEA, 0x4C};
constexpr size_t kRelayBytes = 46;
constexpr uint32_t kCallbackLiteral = 24;
constexpr uint32_t kOriginalLiteral = 38;
// The generated stubs share the relay's page; every cross-reference is
// absolute, so placement only needs to stay inside the allocation.
constexpr uint32_t kStubAOffset = 48;   // 8-aligned, right after the relay
constexpr uint32_t kStubBOffset = 160;  // after stubA (111 bytes), 8-aligned

// The camera struct's fields (camera-relative, the typed table).
constexpr uint32_t kCamKind = 0x264;
constexpr uint32_t kCamBoundX = 0x28C;
constexpr uint32_t kCamBoundY = 0x290;
constexpr uint32_t kCamFlags = 0x250;
constexpr uint32_t kFlagProj = 4, kFlagVP = 8;

// Per-thread forward state. While the body runs with its return address
// redirected to stubB, everything the post-half needs travels here --
// nothing may live on the stack, because the stack belongs to the game.
struct RefreshTls {
    uint64_t realRet = 0; // MUST STAY FIRST: stubB's disp32 targets this field
    uintptr_t ctx = 0;
    uintptr_t camera = 0;
    uint64_t callNo = 0;
    float entryX = 0, entryY = 0;
    uint32_t flags = 0;
    uint32_t haveFlags = 0;
    uint32_t armed = 0;
    uint32_t pad = 0;
};
__declspec(thread) RefreshTls g_refreshTls;
static_assert(offsetof(RefreshTls, realRet) == 0, "stubB reads realRet at the struct base");

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
    // fix.temporal_aa_camera_trace, refreshed per frame. Per-call log
    // writes are gated behind it: the 2026-09-28 20:09 crash (a null-read
    // downstream in the camera pipeline, sentinel-caught) came with the
    // detour doing NOTHING but pass-through plus three note() writes on
    // the game thread inside the view-constant refresh, so call-path I/O
    // is the lead suspect and stays off unless a flight explicitly wants
    // the breadcrumbs. The same information rides out on these fields and
    // the 5s tick instead.
    bool trace = false;
    // Counters, reported on the cadence tick.
    std::atomic<uint64_t> refreshCalls{0};
    std::atomic<uint64_t> injectedCalls{0};
    std::atomic<uint64_t> kindRefusals{0};
    std::atomic<uint64_t> unsupportedCameras{0};
    std::atomic<uint64_t> warmingCalls{0};
    std::atomic<uint64_t> rayCbLogged{0};
    // Last-call triage for the tick (no call-path I/O).
    std::atomic<uint64_t> lastCallNo{0};
    std::atomic<uintptr_t> lastCtx{0};
    std::atomic<uintptr_t> lastP2{0};
    std::atomic<uintptr_t> lastCamera{0};
    std::atomic<uint32_t> lastKind{0};
    std::atomic<uint64_t> raySeq{0};
    uint64_t lastRaySeqReported = 0;
    uintptr_t lastRaySlot = 0;
    uint64_t lastLogMs = 0;
    uint64_t lastKindRefusalLogMs = 0;
    float lastRay[4] = {};
};
InjectState g_inject;
std::atomic<uintptr_t> g_gate{0};
std::atomic<uintptr_t> g_refreshForward{0};
// The incoming r11 of the most recent refresh call, captured by stubA's
// moffs store BEFORE anything can clobber it. The 20:09/03:50 crashes put
// this pipeline family's r11 convention under suspicion (an inherited
// frame base the unhooked refresh preserves); the tick prints it -- a
// stack pointer here is the convention proven in the log.
std::atomic<uintptr_t> g_lastIncomingR11{0};
uintptr_t g_stubB = 0;
uint32_t g_stubATrampOfs = 0;

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

bool sehWriteU64(uintptr_t at, uint64_t value) noexcept {
    __try {
        *reinterpret_cast<uint64_t*>(at) = value;
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
    // push rax; mov rax,&gate; cmp qword ptr[rax],0; pop rax; jz original;
    // jmp [callback]; original: jmp [trampoline].
    //
    // NO register is left clobbered. The first form carried the gate in r11:
    // rax was the known constraint (this target's frame anchor is mov rax,rsp
    // BEFORE the patch site, so rax must survive), but the 20:09/03:50
    // crashes showed r11 is equally load-bearing here -- this pipeline family
    // passes a frame base down in r11 (the refresh's caller FUN_140594dc8
    // spills through [r11+0x10] at entry with no local setup), and the
    // unhooked refresh never touches r11, so the invariant held until the
    // relay overwrote it. push/pop rax preserves the anchor AND r11; the cmp
    // clobbers flags, which is safe because the trampoline's stolen
    // sub rsp,0xE0 re-sets them before the body can read them.
    const uint8_t body[kRelayBytes] = {
        0x50, 0x48,0xB8,0,0,0,0,0,0,0,0, 0x48,0x83,0x38,0,
        0x58, 0x74,0x0E, 0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0,
        0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 3, &gateAddress, 8);
    std::memcpy(code + kCallbackLiteral, &callbackAddress, 8);
}

// A tiny emitter for the two stubs. Fixed byte sequences with immediates
// appended through u32/u64; every offset below is derived from the byte
// counts in the comments, and buildStubA returns the literal offset it
// actually used so prepareRelay patches the same place.
struct CodeCursor {
    uint8_t* p;
    void b(std::initializer_list<uint8_t> vs) { for (const uint8_t v : vs) *p++ = v; }
    void u32(uint32_t v) { std::memcpy(p, &v, 4); p += 4; }
    void u64(uint64_t v) { std::memcpy(p, &v, 8); p += 8; }
};

// stubA -- the relay's callback. Entered by jmp with rsp = S (the game's
// stack at the hook site; S = 8 mod 16, the ABI's post-call alignment
// minus the two pushes the game made before the patch). Saves every GPR
// (fifteen pushes keep the alignment), sets up refreshPre's arguments
// from the save slots, then restores everything and joins the trampoline
// with rsp = S: the game body must see a byte-identical stack. Returns
// the offset of the trampoline literal for prepareRelay.
uint32_t buildStubA(uint8_t* at, const void* preFn, void* r11Store) noexcept {
    CodeCursor c{at};
    c.b({0x50, 0x53, 0x51, 0x52, 0x56, 0x57, 0x55});           // push rax rbx rcx rdx rsi rdi rbp
    c.b({0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53,           // push r8 r9 r10 r11
         0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57});          // push r12 r13 r14 r15
    // rsp = S-0x78, 16-aligned. Slots: r15@[+0] .. rax@[+0x70].
    c.b({0x48,0x8B,0x44,0x24,0x20});                            // mov rax,[rsp+0x20]  = saved r11 (incoming!)
    c.b({0x48,0xA3}); c.u64(reinterpret_cast<uint64_t>(r11Store)); // mov [&g_lastIncomingR11],rax
    c.b({0x48,0x8D,0x8C,0x24}); c.u32(0x88);                    // lea rcx,[rsp+0x88]  = R0 (the game's retaddr slot)
    c.b({0x48,0x8B,0x54,0x24,0x60});                            // mov rdx,[rsp+0x60]  = saved rcx (ctx)
    c.b({0x4C,0x8B,0x44,0x24,0x58});                            // mov r8, [rsp+0x58]  = saved rdx (p2)
    c.b({0x4C,0x8B,0x4C,0x24,0x38});                            // mov r9, [rsp+0x38]  = saved r8  (camera)
    c.b({0x49,0xBB}); c.u64(reinterpret_cast<uint64_t>(preFn)); // mov r11,&refreshPre
    c.b({0x41,0xFF,0xD3});                                      // call r11
    c.b({0x41,0x5F, 0x41,0x5E, 0x41,0x5D, 0x41,0x5C,           // pop r15 r14 r13 r12
         0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58});          // pop r11 r10 r9 r8
    c.b({0x5D, 0x5F, 0x5E, 0x5A, 0x59, 0x5B, 0x58});            // pop rbp rdi rsi rdx rcx rbx rax
    c.b({0xFF,0x25}); c.u32(0);                                 // jmp [rip+0]
    const uint32_t literalOfs = static_cast<uint32_t>(c.p - at);
    c.u64(0);                                                   // trampoline literal, patched by prepareRelay
    return literalOfs;
}

// stubB -- the body's redirected return. Entered by the body's ret with
// rsp = R0+8 (16-aligned) and the game's return state in every register.
// Preserves rax/xmm0 and the scratch registers around refreshPost, then
// jumps to the real return address from TLS. r11 has no ABI role across a
// return, so it alone carries the TLS walk; _tls_index is process-
// constant after CRT init and is baked in as an immediate.
void buildStubB(uint8_t* at, const void* postFn, uint32_t tlsIndex, uint32_t tlsRealRetOfs) noexcept {
    CodeCursor c{at};
    c.b({0x50, 0x51, 0x52});                                    // push rax rcx rdx
    c.b({0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53});          // push r8 r9 r10 r11
    c.b({0x48,0x83,0xEC,0x18});                                 // sub rsp,0x18 (16-aligned from here)
    c.b({0x0F,0x29,0x04,0x24});                                 // movaps [rsp],xmm0
    c.b({0x49,0xBB}); c.u64(reinterpret_cast<uint64_t>(postFn));// mov r11,&refreshPost
    c.b({0x41,0xFF,0xD3});                                      // call r11
    c.b({0x0F,0x28,0x04,0x24});                                 // movaps xmm0,[rsp]
    c.b({0x48,0x83,0xC4,0x18});                                 // add rsp,0x18
    c.b({0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58});          // pop r11 r10 r9 r8
    c.b({0x5A, 0x59, 0x58});                                    // pop rdx rcx rax
    c.b({0x65,0x4C,0x8B,0x1C,0x25}); c.u32(0x58);               // mov r11,gs:[0x58] (TLS array)
    c.b({0x4F,0x8B,0x9B}); c.u32(tlsIndex * 8);                 // mov r11,[r11+tlsIndex*8] (disp32: the index is per-process, seen > 15)
    c.b({0x4F,0x8B,0x9B}); c.u32(tlsRealRetOfs);                // mov r11,[r11+tlsRealRetOfs]
    c.b({0x41,0xFF,0xE3});                                      // jmp r11
}

bool prepareRelay(void* trampoline, void*) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(g_inject.relay + kOriginalLiteral, &address, 8);
    std::memcpy(g_inject.relay + kStubAOffset + g_stubATrampOfs, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_inject.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), g_inject.relay, 4096)) return false;
    g_refreshForward.store(address, std::memory_order_release);
    return true;
}

// The ray CB observation: after the original refresh returns, read the
// slot record at lVar4+0x78 (lVar4 = *(ctx+0x28) per the refresh's own
// layout) and record the composition's first floats when they materially
// change. This is the follow-up that pins the consumer: the composed
// values cross-check the rig's composeRayCb against the live game, and
// the slot record is the handle for naming the binding shader next. The
// reads stay on the call path (plain memory, no I/O); the note is emitted
// from the 5s tick, or immediately when the trace key is on.
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
    g_inject.lastRaySlot = static_cast<uintptr_t>(staging);
    const uint64_t n = g_inject.rayCbLogged.fetch_add(1, std::memory_order_relaxed) + 1;
    g_inject.raySeq.store(n, std::memory_order_relaxed);
    if (n > 8) return; // bounded: first anchor plus a few material changes
    if (!g_inject.trace) return; // the tick reports the anchor instead
    float more[12] = {};
    for (uint32_t i = 0; i < 12; ++i)
        if (!sehReadF32(static_cast<uintptr_t>(staging) + 16 + 4 * i, &more[i])) return;
    Log::get().note("flat camera inject: ray CB slot %p anchors (%llux): "
        "[%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f]",
        reinterpret_cast<void*>(staging), (unsigned long long)n,
        row[0], row[1], row[2], row[3], more[0], more[1], more[2], more[3],
        more[4], more[5], more[6], more[7], more[8], more[9], more[10], more[11]);
}

// The pre-forward half, called by stubA with R0 (the game's return-
// address slot) and the live argument registers. Everything the old
// detour did before forward() -- admission, the phase, the dirty bits --
// plus the return redirection. Any refusal leaves the camera and [R0]
// untouched, which is the whole of "forward unmodified" now: stubA joins
// the trampoline regardless and the body returns straight to its caller.
void refreshPre(uintptr_t r0, uintptr_t ctx, uintptr_t p2, uintptr_t camera) noexcept {
    g_refreshTls.armed = 0; // a previous body that unwound never disarmed
    const uint64_t callNo = g_inject.refreshCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    // Breadcrumbs are gated behind the trace key (see InjectState::trace);
    // the triage fields ride out on the 5s tick either way.
    const bool trace = g_inject.trace && callNo <= 8;
    g_inject.lastCallNo.store(callNo, std::memory_order_relaxed);
    g_inject.lastCtx.store(ctx, std::memory_order_relaxed);
    g_inject.lastP2.store(p2, std::memory_order_relaxed);
    g_inject.lastCamera.store(camera, std::memory_order_relaxed);
    if (trace) {
        uint64_t literal = 0;
        if (sehReadU64(reinterpret_cast<uintptr_t>(g_inject.relay) + kCallbackLiteral, &literal)) {
            Log::get().note("flat camera inject: refresh call #%llu entered (ctx=%p p2=%p camera=%p stubA=%p trampoline=%p)",
                            (unsigned long long)callNo, reinterpret_cast<void*>(ctx),
                            reinterpret_cast<void*>(p2), reinterpret_cast<void*>(camera),
                            reinterpret_cast<void*>(literal),
                            reinterpret_cast<void*>(g_refreshForward.load(std::memory_order_relaxed)));
        } else {
            Log::get().note("flat camera inject: refresh call #%llu entered (ctx=%p p2=%p camera=%p; stubA literal unreadable)",
                            (unsigned long long)callNo, reinterpret_cast<void*>(ctx),
                            reinterpret_cast<void*>(p2), reinterpret_cast<void*>(camera));
        }
    }

    // Admission for THIS call: the injector owns only a kind-3 camera whose
    // frame ownership is Upstream. Anything else passes through untouched
    // (and unsupported cameras are counted, not silently jittered).
    uint32_t kind = 0;
    const bool readable = camera && sehReadU32(camera + kCamKind, &kind);
    g_inject.lastKind.store(readable ? kind : 0xffffffffu, std::memory_order_relaxed);
    if (!readable || kind != 3) {
        g_inject.kindRefusals.fetch_add(1, std::memory_order_relaxed);
        if (trace) {
            const uint64_t now = GetTickCount64();
            if (now - g_inject.lastKindRefusalLogMs > 30000) {
                g_inject.lastKindRefusalLogMs = now;
                Log::get().note("flat camera inject: camera %p kind %u is not the proven branch (named unsupported; no mutation)",
                                reinterpret_cast<void*>(camera), readable ? kind : 0xffffffffu);
            }
            Log::get().note("flat camera inject: refresh call #%llu forwards unmodified (kind=%u)", (unsigned long long)callNo, kind);
        }
        return;
    }
    if (!g_inject.decisionValid || g_inject.decision.owner != FlatCameraOwner::Upstream) return;

    // The phase, in RENDER pixels from the validated resolve plan (R5).
    float jx = 0, jy = 0;
    uint32_t rw = 0, rh = 0, applied = 0;
    flatRuntimePhaseState(&jx, &jy, &rw, &rh, &applied);
    if (!rw || !rh || (jx == 0.0f && jy == 0.0f)) {
        g_inject.warmingCalls.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    float entryX = 0, entryY = 0;
    if (!sehReadF32(camera + kCamBoundX, &entryX) || !sehReadF32(camera + kCamBoundY, &entryY)) return;
    // The D3D sign convention W1 proved: content right by jx needs
    // boundX += jx/R_w; content down by jy needs boundY += -jy/R_h.
    const float jitX = entryX + jx / static_cast<float>(rw);
    const float jitY = entryY - jy / static_cast<float>(rh);
    uint32_t flags = 0;
    const bool haveFlags = sehReadU32(camera + kCamFlags, &flags);
    const bool wroteX = sehWriteF32(camera + kCamBoundX, jitX);
    const bool wroteY = wroteX && sehWriteF32(camera + kCamBoundY, jitY);
    const bool wroteAll = wroteY && (!haveFlags || sehWriteU32(camera + kCamFlags, flags | kFlagProj | kFlagVP));
    if (!wroteAll) { // roll back whatever landed; the body runs pristine
        if (wroteX) sehWriteF32(camera + kCamBoundX, entryX);
        if (wroteY) sehWriteF32(camera + kCamBoundY, entryY);
        return;
    }
    // Redirect the body's return to stubB. This must be the LAST step: if
    // it fails the mutation is undone and the body returns to its caller.
    uint64_t realRet = 0;
    if (!sehReadU64(r0, &realRet) || !sehWriteU64(r0, static_cast<uint64_t>(g_stubB))) {
        sehWriteF32(camera + kCamBoundX, entryX);
        sehWriteF32(camera + kCamBoundY, entryY);
        if (haveFlags) sehWriteU32(camera + kCamFlags, flags);
        return;
    }
    g_refreshTls.realRet = realRet;
    g_refreshTls.ctx = ctx;
    g_refreshTls.camera = camera;
    g_refreshTls.callNo = callNo;
    g_refreshTls.entryX = entryX;
    g_refreshTls.entryY = entryY;
    g_refreshTls.flags = flags;
    g_refreshTls.haveFlags = haveFlags ? 1u : 0u;
    g_refreshTls.armed = 1u;
    if (trace) {
        Log::get().note("flat camera inject: refresh call #%llu injecting phase=(%.5f, %.5f) at %ux%u over bound=(%.5f, %.5f); return %p redirected to stubB",
                        (unsigned long long)callNo, jx, jy, rw, rh, entryX, entryY,
                        reinterpret_cast<void*>(realRet));
    }
}

// The post-forward half, called by stubB after the body's ret. The entry
// values ARE the authoritative originals (restore-after-call, unchanged):
// the derived blocks keep this call's jitter (the committed phase); the
// sources return pristine for the next derivation.
void refreshPost() noexcept {
    if (!g_refreshTls.armed) return; // defensive: stubB only fires after a redirect
    g_refreshTls.armed = 0;
    const uintptr_t camera = g_refreshTls.camera;
    sehWriteF32(camera + kCamBoundX, g_refreshTls.entryX);
    sehWriteF32(camera + kCamBoundY, g_refreshTls.entryY);
    if (g_refreshTls.haveFlags) sehWriteU32(camera + kCamFlags, g_refreshTls.flags);
    const uint64_t callNo = g_refreshTls.callNo;
    g_inject.injectedCalls.fetch_add(1, std::memory_order_relaxed);
    flatRuntimeNoteCameraApplied();
    if (g_inject.trace && callNo <= 8) {
        Log::get().note("flat camera inject: refresh call #%llu restored entry values; observing ray CB",
                        (unsigned long long)callNo);
    }
    observeRayCb(g_refreshTls.ctx);
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

// Own function: the string temporary needs unwinding, which flatCamera-
// InjectFrame's readback __try forbids (C2712).
bool flatCameraInjectTraceWanted() {
    return _stricmp(Config::get().getString("fix.temporal_aa_camera_trace", "off").c_str(), "on") == 0;
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
        g_inject.trace = flatCameraInjectTraceWanted();
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
        // TLS placement for stubB's walk: _tls_index is process-constant
        // after CRT init, and the struct's offset from this thread's TLS
        // base is the same on every thread.
        const auto tlsArray = reinterpret_cast<const uintptr_t*>(__readgsqword(0x58));
        const uintptr_t tlsBase = tlsArray ? tlsArray[_tls_index] : 0;
        const uintptr_t tlsStruct = reinterpret_cast<uintptr_t>(&g_refreshTls);
        if (!tlsBase || tlsStruct < tlsBase || tlsStruct - tlsBase > 0xFFFFFFFFull ||
            _tls_index == 0) {
            static char tlsDetail[160];
            std::snprintf(tlsDetail, sizeof(tlsDetail),
                "the thread-local the return stub needs is outside its reach "
                "(tlsBase=%p tlsStruct=%p tlsIndex=%u)",
                reinterpret_cast<void*>(tlsBase), reinterpret_cast<void*>(tlsStruct), _tls_index);
            g_inject.failReason = tlsDetail;
            Log::get().note("flat camera inject: %s; standing down", g_inject.failReason);
            VirtualFree(g_inject.relay, 0, MEM_RELEASE);
            g_inject.relay = reinterpret_cast<uint8_t*>(1);
            return;
        }
        g_stubB = reinterpret_cast<uintptr_t>(g_inject.relay) + kStubBOffset;
        buildRelay(g_inject.relay, &g_gate, g_inject.relay + kStubAOffset);
        g_stubATrampOfs = buildStubA(g_inject.relay + kStubAOffset, &refreshPre,
                                     &g_lastIncomingR11);
        buildStubB(g_inject.relay + kStubBOffset, &refreshPost, _tls_index,
                   static_cast<uint32_t>(tlsStruct - tlsBase));
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
        // Readback verification: the relay stub, its callback literal, the
        // trampoline's and stubA's first bytes, so a later crash can be
        // compared against what was actually built.
        uint8_t relayBytes[46] = {};
        uint64_t callbackLiteral = 0, fwd = g_refreshForward.load(std::memory_order_relaxed);
        uint8_t trampBytes[16] = {};
        uint8_t stubABytes[16] = {};
        __try {
            std::memcpy(relayBytes, g_inject.relay, sizeof(relayBytes));
            std::memcpy(&callbackLiteral, g_inject.relay + kCallbackLiteral, 8);
            std::memcpy(trampBytes, reinterpret_cast<const void*>(fwd), sizeof(trampBytes));
            std::memcpy(stubABytes, g_inject.relay + kStubAOffset, sizeof(stubABytes));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        char hex1[192]{}, hex2[96]{}, hex3[96]{};
        for (int i = 0; i < 46; ++i) std::snprintf(hex1 + i * 3, sizeof(hex1) - i * 3, "%02X ", relayBytes[i]);
        for (int i = 0; i < 16; ++i) std::snprintf(hex2 + i * 3, sizeof(hex2) - i * 3, "%02X ", trampBytes[i]);
        for (int i = 0; i < 16; ++i) std::snprintf(hex3 + i * 3, sizeof(hex3) - i * 3, "%02X ", stubABytes[i]);
        Log::get().note("flat camera inject: relay[0..45]=%s| stubA=%p trampoline=%p tramp[0..15]=%s stubA[0..15]=%s",
                        hex1, reinterpret_cast<void*>(callbackLiteral), reinterpret_cast<void*>(fwd), hex2, hex3);
    }
    const uint64_t now = GetTickCount64();
    if (now - g_inject.lastLogMs >= 5000) {
        g_inject.lastLogMs = now;
        Log::get().note("flat camera inject 5s: refresh-calls=%llu injected=%llu warming=%llu "
                        "kind-refusals=%llu unsupported=%llu owner=%s history=%s trace=%s",
            (unsigned long long)g_inject.refreshCalls.exchange(0),
            (unsigned long long)g_inject.injectedCalls.exchange(0),
            (unsigned long long)g_inject.warmingCalls.exchange(0),
            (unsigned long long)g_inject.kindRefusals.exchange(0),
            (unsigned long long)g_inject.unsupportedCameras.exchange(0),
            g_inject.decisionValid && g_inject.decision.owner == FlatCameraOwner::Upstream
                ? "upstream" : "legacy/none",
            g_inject.owner.historyValid ? "valid" : "invalid",
            g_inject.trace ? "on" : "off");
        // The call triage that used to ride the per-call notes, reported
        // here instead: no I/O on the game's refresh path (the 20:09
        // crash hypothesis).
        const uint64_t lastNo = g_inject.lastCallNo.load(std::memory_order_relaxed);
        if (lastNo) {
            Log::get().note("flat camera inject lastcall: #%llu ctx=%p p2=%p camera=%p kind=%u r11-in=%p",
                (unsigned long long)lastNo,
                reinterpret_cast<void*>(g_inject.lastCtx.load(std::memory_order_relaxed)),
                reinterpret_cast<void*>(g_inject.lastP2.load(std::memory_order_relaxed)),
                reinterpret_cast<void*>(g_inject.lastCamera.load(std::memory_order_relaxed)),
                g_inject.lastKind.load(std::memory_order_relaxed),
                reinterpret_cast<void*>(g_lastIncomingR11.load(std::memory_order_relaxed)));
        }
        const uint64_t raySeq = g_inject.raySeq.load(std::memory_order_relaxed);
        if (raySeq != g_inject.lastRaySeqReported) {
            g_inject.lastRaySeqReported = raySeq;
            Log::get().note("flat camera inject ray CB: slot %p anchors (%llux): [%.5f %.5f %.5f %.5f]",
                reinterpret_cast<void*>(g_inject.lastRaySlot), (unsigned long long)raySeq,
                g_inject.lastRay[0], g_inject.lastRay[1], g_inject.lastRay[2], g_inject.lastRay[3]);
        }
    }
}

} // namespace edvr

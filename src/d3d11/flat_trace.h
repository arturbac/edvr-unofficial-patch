// Frame-contract trace: serialize the reducer's input stream (FlatRuntimeDraw
// events) per frame, so a diagnostic replay runs the SAME reducer
// (flatRuntimeObserve) over identical inputs and compares the produced
// FlatFrameContract's hash. Gate 1 of the staged program in
// docs/design-flat-temporal-aa-2026-09-23.md: consolidation without changing
// output. Pointers in the trace are opaque identity tokens, replayed
// verbatim; only per-draw-alias pointers (camera bytes, projection shadow
// bytes) are reduced to flags.
#pragma once
#include "flat_frame_contract.h"

namespace edvr {

// Event kinds. Draws carry a full FlatRuntimeDraw; the prefix-mutating paths
// that run BETWEEN draws (resource writes, dispatch UAV guards, uncertain
// markers) record as their own kinds so a replay applies every mutation in
// sequence order. Anything less diverges on the first dispatch-heavy frame.
constexpr uint32_t kFlatTraceEventDraw = 0;
constexpr uint32_t kFlatTraceEventWriteResource = 1;    // key.color = resource
constexpr uint32_t kFlatTraceEventDispatchWritten = 2;  // key.color = UAV resource
constexpr uint32_t kFlatTraceEventMarkUncertain = 3;
// Camera captures share prefix.sequence with draws (capture() in
// flat_runtime.cpp); each one advances the counter, so it must interleave
// in the trace or every later draw's q is low by the capture count.
constexpr uint32_t kFlatTraceEventCameraCapture = 4;

struct FlatTraceEvent {
    FlatContractObservation key{};   // camera and projection[].bytes are null
    unsigned char camera[kFlatCameraBytes]{};
    uint32_t instances = 1;
    uint32_t flags = 0;
    uint32_t kind = kFlatTraceEventDraw;
};
constexpr uint32_t kFlatTraceHasCamera = 1u << 0;
constexpr uint32_t kFlatTraceSupported = 1u << 1;
constexpr uint32_t kFlatTraceHdrCopyVerified = 1u << 2;
constexpr uint32_t kFlatTraceMenuCopyVerified = 1u << 3;
constexpr uint32_t kFlatTraceImageSourceVerified = 1u << 4;
// The draw ctor observed foreign (non-owner) work this frame; the replay sets
// prefix.uncertain before the reducer sees the draw, as the ctor does.
constexpr uint32_t kFlatTraceForeignWork = 1u << 5;

inline FlatTraceEvent flatTraceEventFromDraw(const FlatRuntimeDraw& d, bool foreignWork) {
    FlatTraceEvent e{};
    e.key = d.key;
    e.flags |= d.key.camera ? kFlatTraceHasCamera : 0;
    e.key.camera = nullptr;
    for (uint32_t slot = 0; slot < kFlatProjectionSlots; ++slot)
        e.key.projection[slot].bytes = nullptr;
    std::memcpy(e.camera, d.camera, sizeof(e.camera));
    e.instances = d.instances;
    e.flags |= d.supported ? kFlatTraceSupported : 0;
    e.flags |= d.hdrCopyVerified ? kFlatTraceHdrCopyVerified : 0;
    e.flags |= d.menuHdrCopyVerified ? kFlatTraceMenuCopyVerified : 0;
    e.flags |= d.imageSourceCameraIndependentVerified ? kFlatTraceImageSourceVerified : 0;
    e.flags |= foreignWork ? kFlatTraceForeignWork : 0;
    e.kind = kFlatTraceEventDraw;
    return e;
}
// The non-draw prefix mutations carry only their resource token (or nothing).
inline FlatTraceEvent flatTraceEventMarker(uint32_t kind, const void* resource) {
    FlatTraceEvent e{};
    e.kind = kind;
    e.key.color = resource;
    return e;
}
inline FlatRuntimeDraw flatTraceEventToDraw(const FlatTraceEvent& e) {
    FlatRuntimeDraw d{};
    d.key = e.key;
    std::memcpy(d.camera, e.camera, sizeof(d.camera));
    d.key.camera = (e.flags & kFlatTraceHasCamera) ? d.camera : nullptr;
    d.supported = (e.flags & kFlatTraceSupported) != 0;
    d.hdrCopyVerified = (e.flags & kFlatTraceHdrCopyVerified) != 0;
    d.menuHdrCopyVerified = (e.flags & kFlatTraceMenuCopyVerified) != 0;
    d.imageSourceCameraIndependentVerified = (e.flags & kFlatTraceImageSourceVerified) != 0;
    d.instances = e.instances;
    return d;
}

struct FlatTraceHeader {
    char magic[8] = {'E','D','V','R','F','T','R','3'};
    uint32_t frameCount = 0;
    uint32_t reserved = 0;
};
struct FlatTraceFrameHeader {
    uint64_t frame = 0;
    const void* output = nullptr;   // identity token within the trace
    uint32_t width = 0, height = 0, format = 0;
    uint32_t eventCount = 0;        // events following this header
    uint32_t truncated = 0;         // the frame's draws exceeded the slot
    uint32_t produced = 0;          // a copy draw produced a contract
    uint64_t contractHash = 0;      // flatFrameContractHash of the produced contract
};

// The runtime's bounded ring: the last few complete frames, frame-atomic so a
// dump never holds half a frame. Fixed storage, no allocation on the draw
// path; a frame with more draws than the slot marks itself truncated and is
// skipped by the dump.
constexpr uint32_t kFlatTraceFrames = 4;
constexpr uint32_t kFlatTraceEventsPerFrame = 4096;
struct FlatTraceRing {
    FlatTraceEvent events[kFlatTraceFrames][kFlatTraceEventsPerFrame];
    FlatTraceFrameHeader headers[kFlatTraceFrames]{};
    uint32_t slot = 0;
    bool slotUsed[kFlatTraceFrames]{};
};

inline void flatTraceBeginFrame(FlatTraceRing& r, uint64_t frame, const void* output,
                                uint32_t width, uint32_t height, uint32_t format) {
    r.slot = (r.slot + 1) % kFlatTraceFrames;
    auto& h = r.headers[r.slot];
    h = FlatTraceFrameHeader{};
    h.frame = frame; h.output = output; h.width = width; h.height = height; h.format = format;
    r.slotUsed[r.slot] = true;
}
inline void flatTraceRecord(FlatTraceRing& r, const FlatRuntimeDraw& d, bool foreignWork) {
    if (!r.slotUsed[r.slot]) return;
    auto& h = r.headers[r.slot];
    if (h.eventCount >= kFlatTraceEventsPerFrame) { h.truncated = 1; return; }
    r.events[r.slot][h.eventCount++] = flatTraceEventFromDraw(d, foreignWork);
}
inline void flatTraceMark(FlatTraceRing& r, uint32_t kind, const void* resource) {
    if (!r.slotUsed[r.slot]) return;
    auto& h = r.headers[r.slot];
    if (h.eventCount >= kFlatTraceEventsPerFrame) { h.truncated = 1; return; }
    r.events[r.slot][h.eventCount++] = flatTraceEventMarker(kind, resource);
}
inline void flatTraceSeal(FlatTraceRing& r, bool produced, uint64_t contractHash) {
    if (!r.slotUsed[r.slot]) return;
    r.headers[r.slot].produced = produced ? 1u : 0u;
    r.headers[r.slot].contractHash = contractHash;
}

// Serialize complete frames oldest-first, skipping the current slot: its
// frame is mid-flight, unsealed and partial. Returns total bytes written.
template <class Write>
inline uint32_t flatTraceDump(const FlatTraceRing& r, Write&& write) {
    FlatTraceHeader header{};
    for (uint32_t i = 0; i < kFlatTraceFrames; ++i) {
        const uint32_t slot = (r.slot + 1 + i) % kFlatTraceFrames;
        const auto& h = r.headers[slot];
        if (slot != r.slot && r.slotUsed[slot] && !h.truncated && h.eventCount)
            ++header.frameCount;
    }
    uint32_t bytes = write(&header, sizeof(header));
    for (uint32_t i = 0; i < kFlatTraceFrames; ++i) {
        const uint32_t slot = (r.slot + 1 + i) % kFlatTraceFrames;
        if (slot == r.slot || !r.slotUsed[slot]) continue;
        const auto& h = r.headers[slot];
        if (h.truncated || !h.eventCount) continue;
        bytes += write(&h, sizeof(h));
        bytes += write(r.events[slot], h.eventCount * sizeof(FlatTraceEvent));
    }
    return bytes;
}

// Parse one trace document, invoking onFrame(header) then onEvent(event)
// per event in order. Returns false on any malformed input.
template <class OnFrame, class OnEvent>
inline bool flatTraceParse(const unsigned char* data, size_t size,
                           OnFrame&& onFrame, OnEvent&& onEvent) {
    if (!data || size < sizeof(FlatTraceHeader)) return false;
    FlatTraceHeader header{};
    std::memcpy(&header, data, sizeof(header));
    if (std::memcmp(header.magic, "EDVRFTR3", 8) != 0) return false;
    size_t at = sizeof(FlatTraceHeader);
    for (uint32_t f = 0; f < header.frameCount; ++f) {
        if (size - at < sizeof(FlatTraceFrameHeader)) return false;
        FlatTraceFrameHeader fh{};
        std::memcpy(&fh, data + at, sizeof(fh));
        at += sizeof(fh);
        if (!fh.eventCount || fh.eventCount > kFlatTraceEventsPerFrame) return false;
        if (size - at < fh.eventCount * sizeof(FlatTraceEvent)) return false;
        onFrame(fh);
        for (uint32_t i = 0; i < fh.eventCount; ++i) {
            FlatTraceEvent e{};
            std::memcpy(&e, data + at, sizeof(e));
            at += sizeof(e);
            onEvent(e);
        }
    }
    return at == size;
}
} // namespace edvr

// The frame contract: the flat temporal reducer's immutable per-frame
// artifact (docs/design-flat-temporal-aa-2026-09-23.md's staged program,
// gate 1, and docs/review-flat-temporal-aa-2026-09-26.md). One streaming
// reducer -- flatRuntimeObserve over FlatRuntimeDraw events -- produces it
// at the frame's output-copy draw; the online driver consumes its selection,
// the diagnostic replay consumes and hashes the whole artifact. Neither the
// artifact nor the hash changes any decision: gate 1 is consolidation only.
#pragma once
#include "flat_runtime_model.h"

namespace edvr {

// The online assembly's bound (flat_runtime_model.h's copy branch builds at
// most 36 fixture records: HDR aggregates, camera, sources, tone, copy).
constexpr uint32_t kFlatFrameContractRecordCapacity = 36;

struct FlatFrameContract {
    uint64_t frame = 0, epoch = 0;
    const void* output = nullptr;
    uint32_t outputWidth = 0, outputHeight = 0, outputFormat = 0;
    FlatContractRecord records[kFlatFrameContractRecordCapacity]{};
    uint32_t recordCount = 0;
    FlatMonoFrame selection{};      // the reducer's decision for this frame
    FlatRuntimeWitness conflict{};  // the selector's conflict witness, if any
    bool produced = false;          // an output copy ran this frame
};

// Identity over exactly the fields that decide treatment: the assembled
// records' keys/ranges and the selection's outputs. Pointer/token identities
// are compared as values -- a trace replays within its own frame's namespace.
inline uint64_t flatFrameContractHash(const FlatFrameContract& c) {
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&hash](const void* data, size_t bytes) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * 1099511628211ull;
    };
    mix(&c.frame, sizeof(c.frame)); mix(&c.epoch, sizeof(c.epoch));
    mix(&c.output, sizeof(c.output));
    mix(&c.outputWidth, sizeof(c.outputWidth)); mix(&c.outputHeight, sizeof(c.outputHeight));
    mix(&c.outputFormat, sizeof(c.outputFormat));
    mix(&c.recordCount, sizeof(c.recordCount));
    for (uint32_t i = 0; i < c.recordCount; ++i) {
        const auto& r = c.records[i];
        // Field-wise, never raw struct bytes: padding is not deterministic.
        // The observation's per-draw-only pointers (camera, projection bytes)
        // carry no decision state and are skipped.
        const auto& k = r.key;
        mix(&k.color, sizeof(k.color)); mix(&k.depth, sizeof(k.depth));
        mix(&k.rtv, sizeof(k.rtv)); mix(&k.dsv, sizeof(k.dsv)); mix(&k.b1, sizeof(k.b1));
        mix(k.srvView, sizeof(k.srvView)); mix(k.srvResource, sizeof(k.srvResource));
        mix(&k.cameraHash, sizeof(k.cameraHash));
        mix(&k.vs, sizeof(k.vs)); mix(&k.ps, sizeof(k.ps));
        mix(&k.writeEpoch, sizeof(k.writeEpoch)); mix(&k.writeSeq, sizeof(k.writeSeq));
        mix(&k.width, sizeof(k.width)); mix(&k.height, sizeof(k.height));
        mix(&k.format, sizeof(k.format));
        mix(&k.depthWidth, sizeof(k.depthWidth)); mix(&k.depthHeight, sizeof(k.depthHeight));
        mix(&k.depthFormat, sizeof(k.depthFormat));
        mix(k.viewport, sizeof(k.viewport)); mix(&k.viewportCount, sizeof(k.viewportCount));
        mix(&k.sequence, sizeof(k.sequence)); mix(&k.count, sizeof(k.count));
        mix(&k.instances, sizeof(k.instances)); mix(&k.kind, sizeof(k.kind));
        for (uint32_t slot = 0; slot < kFlatProjectionSlots; ++slot) {
            const auto& pr = k.projection[slot];
            mix(&pr.resource, sizeof(pr.resource)); mix(&pr.width, sizeof(pr.width));
            mix(&pr.copied, sizeof(pr.copied)); mix(&pr.hash, sizeof(pr.hash));
            mix(&pr.writeSeq, sizeof(pr.writeSeq)); mix(&pr.writeEpoch, sizeof(pr.writeEpoch));
            mix(&pr.status, sizeof(pr.status));
        }
        mix(r.camera, sizeof(r.camera));
        mix(&r.draws, sizeof(r.draws)); mix(&r.first, sizeof(r.first)); mix(&r.last, sizeof(r.last));
        mix(&r.firstWriteEpoch, sizeof(r.firstWriteEpoch));
        mix(&r.lastWriteEpoch, sizeof(r.lastWriteEpoch));
        mix(&r.firstWriteSeq, sizeof(r.firstWriteSeq)); mix(&r.lastWriteSeq, sizeof(r.lastWriteSeq));
    }
    const auto& sel = c.selection;
    mix(&sel.reason, sizeof(sel.reason));
    mix(&sel.color, sizeof(sel.color)); mix(&sel.hdr, sizeof(sel.hdr));
    mix(&sel.depth, sizeof(sel.depth)); mix(&sel.dsv, sizeof(sel.dsv));
    mix(&sel.sceneConstants, sizeof(sel.sceneConstants));
    mix(&sel.renderWidth, sizeof(sel.renderWidth)); mix(&sel.renderHeight, sizeof(sel.renderHeight));
    mix(&sel.depthFormat, sizeof(sel.depthFormat));
    mix(&sel.cameraHash, sizeof(sel.cameraHash)); mix(&sel.nearPlane, sizeof(sel.nearPlane));
    mix(&sel.sourceFirst, sizeof(sel.sourceFirst)); mix(&sel.sourceLast, sizeof(sel.sourceLast));
    mix(&sel.hdrFirst, sizeof(sel.hdrFirst)); mix(&sel.hdrLast, sizeof(sel.hdrLast));
    mix(&sel.toneSequence, sizeof(sel.toneSequence)); mix(&sel.copySequence, sizeof(sel.copySequence));
    mix(&sel.firstLaterOutput, sizeof(sel.firstLaterOutput));
    mix(&sel.supportedDraws, sizeof(sel.supportedDraws));
    mix(&sel.unsupportedDraws, sizeof(sel.unsupportedDraws));
    mix(&c.conflict.cause, sizeof(c.conflict.cause));
    return hash;
}

// The reducer's contract-producing entry point, called at the frame's output
// copy (the only draw whose branch assembles fixture records). Returns the
// same FlatMonoFrame the two-argument flatRuntimeObserve would return for
// the same prefix and draw -- consolidation, not a decision change.
inline FlatMonoFrame flatRuntimeObserveContract(FlatRuntimePrefix& p, const FlatRuntimeDraw& d,
                                                FlatFrameContract& contract) {
    contract = FlatFrameContract{};
    contract.frame = contract.epoch = p.frame;
    contract.output = p.output;
    contract.outputWidth = p.width; contract.outputHeight = p.height;
    contract.outputFormat = p.format;
    FlatRuntimeContractSink sink{contract.records, kFlatFrameContractRecordCapacity, 0};
    const FlatMonoFrame selection = flatRuntimeObserve(p, d, &sink);
    contract.produced = sink.count != 0;
    contract.recordCount = sink.count;
    contract.selection = selection;
    contract.conflict = p.selectedConflict;
    return selection;
}
} // namespace edvr

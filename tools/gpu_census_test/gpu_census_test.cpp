// Issue #38's per-feature GPU cost census (gpu_census.h/.cpp): the
// estimator math, the calibration math (a section's own empty-pair
// overhead, subtracted from its timed mean), the rotation and its K cap, a
// real GpuTimer round trip on WARP including the calibration pair, and the
// 30 s line's format -- including "-" for a section that never occurred
// and for a window with no null samples.
//
// Whitebox, tools\ui_depth_test's and hologram_depth_test's own
// convention: the production .cpp is included directly so this rig can
// reach its rotation state and its window formatter, neither of which
// gpu_census.h exposes on purpose (nothing outside gpu_census.cpp needs
// them). gpu_timing.cpp/gpu_frame_timing.cpp/gpu_span_d3d11.cpp are real,
// linked sources (build.bat), so the WARP round trip is a real disjoint
// timestamp pair, not a fake.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/common/system_d3d11.h"
#include "../../src/common/log.h"

using Microsoft::WRL::ComPtr;

namespace edvr {
// Captures the formatted lines instead of writing them anywhere, so the
// format checks below can inspect the actual text -- hologram_depth_test's
// own convention, not tools\ui_depth_test's no-op stub. g_lastLog is the
// census's MAIN line ("EDVR GPU census: ..."), which the older checks read;
// g_lines is every line, in order, for the lines that follow it (the frame
// gap's, the altered draws').
std::string g_lastLog;
std::vector<std::string> g_lines;
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_lines.push_back(buf);
    if (std::strncmp(buf, "EDVR GPU census:", 16) == 0) g_lastLog = buf;
    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
}
} // namespace edvr

#include "../../src/d3d11/gpu_census.cpp"

namespace {
unsigned g_checks = 0;
void check(bool ok, const char* why) { ++g_checks; if (!ok) throw std::runtime_error(why); }
void hr(HRESULT h) { check(h == S_OK, "D3D operation failed"); }

struct Runtime {
    decltype(&D3D11CreateDevice) create = edvr::systemD3D11CreateDevice();
    Runtime() { check(create != nullptr, "System32 D3D11CreateDevice"); }
};
struct Device {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    explicit Device(Runtime& rt) {
        D3D_FEATURE_LEVEL level{};
        hr(rt.create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                      D3D11_SDK_VERSION, &dev, &level, &ctx));
    }
};

ComPtr<ID3D11Texture2D> makeTarget(ID3D11Device* dev, ComPtr<ID3D11RenderTargetView>& rtvOut) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 8;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex;
    hr(dev->CreateTexture2D(&td, nullptr, &tex));
    hr(dev->CreateRenderTargetView(tex.Get(), nullptr, &rtvOut));
    return tex;
}
} // namespace

namespace edvr {
namespace {

// ---- 1: the estimator math, as a pure function -------------------------
void estimatorMathCases() {
    SectionState st;
    {
        const auto snap = snapshotOf(st, 100);
        check(!snap.occurred, "estimator: zero occurrences reads as not-occurred ('-')");
    }
    {
        st.occurrences = 3;   // no sample completed yet -- still "occurred", 0 ms/frame
        const auto snap = snapshotOf(st, 100);
        check(snap.occurred, "estimator: an occurrence with no completed sample still counts as occurred");
        check(snap.msPerFrame == 0.0, "estimator: no completed sample means 0 ms/frame, never a guess");
        check(std::fabs(snap.perFrame - 0.03) < 1e-9, "estimator: occurrences/frame = occurrences / frames");
    }
    {
        st = SectionState{};
        st.occurrences = 10;
        st.sampler.totals.ms = 5.0;
        st.sampler.totals.samples = 5;   // 5 completed samples averaging 1 ms each
        const auto snap = snapshotOf(st, 50);   // 10 occurrences / 50 frames = 0.2/frame
        check(std::fabs(snap.perFrame - 0.2) < 1e-9, "estimator: occurrences/frame with real samples");
        check(std::fabs(snap.msPerFrame - 0.2) < 1e-9, "estimator: (ms/occurrence) * (occurrences/frame)");
    }
    {
        // A running GpuIntervals total never resets itself -- the window's
        // own contribution is a delta off a baseline, not the raw total.
        st = SectionState{};
        st.occurrences = 4;
        st.baseMs = 20.0; st.baseSamples = 10;
        st.sampler.totals.ms = 23.0; st.sampler.totals.samples = 12;   // +3 ms over +2 samples this window
        const auto snap = snapshotOf(st, 20);   // 4/20 = 0.2/frame
        check(std::fabs(snap.msPerFrame - (1.5 * 0.2)) < 1e-9,
              "estimator: reads a delta off the running totals, not the totals themselves");
    }
}

// ---- 1b: the calibration math, as a pure function -----------------------
void calibrationMathCases() {
    check(std::fabs(correctedMsPerCall(0.010, 0.008) - 0.002) < 1e-9,
          "calibration: corrected ms/call = timed mean - null mean");
    check(correctedMsPerCall(0.010, 0.020) == 0.0,
          "calibration: a null mean above the timed mean clamps to 0, not negative");
    check(correctedMsPerCall(0.010, 0.010) == 0.0,
          "calibration: a null mean equal to the timed mean also clamps to 0");
    check(correctedMsPerCall(0.010, 0.0) == 0.010,
          "calibration: no null samples yet (mean 0) leaves the timed mean uncorrected");
}

// ---- 2/3: rotation (one active section, the K cap) and a real WARP round trip
void rotationAndRealTimerCase(Device& d) {
    check(gpuTimingBind(d.dev.Get(), d.ctx.Get()), "bind canonical WARP timer owner");
    check(occurrenceCapFor(GpuCensusSection::DoorSharpen) == 4, "K cap: a door section caps at 4 (both eyes, two fovea crops each)");
    check(occurrenceCapFor(GpuCensusSection::FrameUiLayerReissues) == 8, "K cap: a per-draw section caps at 8");
    check(isDoorSection(GpuCensusSection::DoorFssHeal), "section split: the last door section is still door");
    check(!isDoorSection(GpuCensusSection::FrameHologramPasses), "section split: the first in-frame section is not door");

    ComPtr<ID3D11RenderTargetView> rtv;
    auto tex = makeTarget(d.dev.Get(), rtv);
    const float colour[4] = {0.2f, 0.4f, 0.6f, 1.0f};

    for (auto& s : g_section) s = SectionState{};
    g_activeSection = static_cast<int>(GpuCensusSection::DoorSharpen);
    g_activeCalls = g_activeTimed = 0;
    g_activeStride = 1;
    g_activeOffset = 0;
    g_activeNullDone = false;
    auto& st = g_section[static_cast<size_t>(GpuCensusSection::DoorSharpen)];

    // Exactly one section is active: a call for any other section declines
    // -- cheaply, one branch and an increment -- and never touches a timer.
    check(!gpuCensusBegin(d.ctx.Get(), GpuCensusSection::DoorMenu),
          "rotation: a non-active section's begin returns false");
    check(g_section[static_cast<size_t>(GpuCensusSection::DoorMenu)].occurrences == 1,
          "rotation: a non-active section still counts its occurrence");
    gpuCensusEnd(d.ctx.Get(), GpuCensusSection::DoorMenu);   // must be a harmless no-op

    // Four real occurrences this "frame": a genuine Begin/Clear/End pair
    // each time, within the door cap of 4.
    for (int i = 0; i < 4; ++i) {
        check(gpuCensusBegin(d.ctx.Get(), GpuCensusSection::DoorSharpen),
              "rotation: the active section's begin succeeds within its cap");
        d.ctx->ClearRenderTargetView(rtv.Get(), colour);
        gpuCensusEnd(d.ctx.Get(), GpuCensusSection::DoorSharpen);
    }
    check(g_activeTimed == 4, "rotation: four occurrences consumed the door cap");
    check(st.occurrences == 4, "rotation: every occurrence counted");
    // Calibration: the turn's first timed call (i == 0 above) also armed the
    // null pair -- once, not once per occurrence.
    check(g_activeNullDone, "calibration: the turn's first timed call armed the null pair");

    // A fifth, same frame: the K cap declines it before the sampler is
    // ever touched -- no lease spent, no change to the timer's own totals.
    const unsigned samplerSkippedBefore = st.sampler.totals.skipped;
    check(!gpuCensusBegin(d.ctx.Get(), GpuCensusSection::DoorSharpen),
          "rotation: a fifth occurrence this frame is declined (K=4 for a door section)");
    check(g_activeTimed == 4, "rotation: the cap does not advance past K");
    check(st.sampler.totals.skipped == samplerSkippedBefore,
          "rotation: a K-cap decline never touches the timer's own capacity accounting");
    check(st.skippedThisWindow == 0, "rotation: a K-cap decline is by design, never counted as a failed span");
    gpuCensusEnd(d.ctx.Get(), GpuCensusSection::DoorSharpen);   // still a harmless no-op

    // The stride: a per-draw section that ran 40 times a frame this window
    // (400 calls over 10 frames) is timed every 5th call (40 / K=8), from an
    // offset that rotates with its turns (7 turns taken -> offset 2).
    auto& holo = g_section[static_cast<size_t>(GpuCensusSection::FrameHologramPasses)];
    holo.occurrences = 400;
    holo.turns = 7;
    g_windowFrames = 9;                       // gpuCensusFrame counts this frame first: 10
    g_windowStartMs = GetTickCount64();       // no window closes during the case
    g_activeSection = static_cast<int>(GpuCensusSection::FrameHologramPasses) - 1;
    gpuCensusFrame(d.ctx.Get());
    check(g_activeSection == static_cast<int>(GpuCensusSection::FrameHologramPasses),
          "stride: the rotation lands on the next section");
    check(g_activeStride == 5, "stride: calls per frame / K");
    check(g_activeOffset == 2, "stride: the offset rotates with the section's turns");
    check(holo.turns == 8, "stride: the turn is counted");
    // Which calls the stride selects, read from g_activeTimed rather than
    // Begin's result: outside a native frame span every interval here takes
    // a whole disjoint-clock record (8 in all), so whether each selected
    // call also got a timer depends on the rig's own record use, not on
    // the selection under test.
    std::string timedAt;
    for (unsigned call = 0; call < 45; ++call) {
        const unsigned before = g_activeTimed;
        gpuCensusBegin(d.ctx.Get(), GpuCensusSection::FrameHologramPasses);
        if (g_activeTimed > before) timedAt += std::to_string(call) + ",";
        gpuCensusEnd(d.ctx.Get(), GpuCensusSection::FrameHologramPasses);
    }
    check(timedAt == "2,7,12,17,22,27,32,37,", "stride: timed calls are spread across the frame, capped at K=8");
    check(g_activeTimed == 8, "stride: the per-draw cap holds");

    // Only this fixture flushes (native_timing_gpu_test's own comment,
    // verbatim): production readback is nonblocking and DONOTFLUSH, and
    // gpu_census.cpp's own poll still never waits or flushes. Without a
    // Present anywhere in this process, nothing else would ever hand the
    // clear and its timestamps to WARP to execute, and gpu_interval.h's
    // four-poll gate is a COUNT, not a clock, so a tight loop would spin
    // it without ever giving WARP real wall-clock time either way.
    // Both the real sampler (four standalone records, i == 0..3 above) and
    // the null sampler (one more, from i == 0's calibration pair) are
    // outside a native frame span here, so each took a whole disjoint-clock
    // record: five of the eight, comfortably within budget without draining
    // anything first (gpu_disjoint_clock.h).
    d.ctx->Flush();
    const uint64_t deadline = GetTickCount64() + 1500;
    while ((st.sampler.totals.samples == 0 || st.nullSampler.totals.samples == 0) && GetTickCount64() < deadline) {
        gpuCensusFrame(d.ctx.Get());
        if (st.sampler.totals.samples == 0 || st.nullSampler.totals.samples == 0) Sleep(1);
    }
    check(st.sampler.totals.samples > 0,
          "WARP round trip: a real Begin/Clear/End pair produced a completed sample within the poll margin");
    check(st.sampler.totals.invalid == 0, "WARP round trip: no invalid/disjoint result on a clean WARP run");
    check(st.nullSampler.totals.samples == 1,
          "calibration: exactly one null pair completed -- the turn's first timed call only, not all four");
    check(st.nullSampler.totals.invalid == 0, "calibration: no invalid/disjoint result on the null pair either");

    // ---- Elite's altered draws (gpu_census.h): the scope counts and times only a classed draw ----
    check(occurrenceCapFor(GpuCensusSection::AlteredPoolFamily) == 8 && !isDoorSection(GpuCensusSection::AlteredFixFirst) &&
              occurrenceCapFor(alteredFixSectionOf(AlteredFix::Unnamed)) == 8,
          "altered: the altered-draw sections, the fix ones too, are per-draw, K = 8, like the in-frame sections");
    for (auto& s : g_section) s = SectionState{};
    g_windowStartMs = GetTickCount64();
    g_activeSection = static_cast<int>(GpuCensusSection::AlteredPoolFamily);
    g_activeCalls = g_activeTimed = 0;
    g_activeStride = 1;
    g_activeOffset = 0;
    g_activeNullDone = false;
    const auto occurrencesEverywhere = [] {
        uint64_t total = 0;
        for (const auto& s : g_section) total += s.occurrences;
        return total;
    };
    { GpuCensusAlteredScope untouched(d.ctx.Get(), AlteredDrawClass::None); }
    check(occurrencesEverywhere() == 0 && g_activeTimed == 0,
          "altered: a draw EDVR left as the game issued it counts nothing and times nothing");
    auto& pool = g_section[static_cast<size_t>(GpuCensusSection::AlteredPoolFamily)];
    for (int i = 0; i < 3; ++i) {
        GpuCensusAlteredScope timed(d.ctx.Get(), AlteredDrawClass::PoolFamily);
        d.ctx->ClearRenderTargetView(rtv.Get(), colour);   // stands in for the game's draw
    }
    check(pool.occurrences == 3 && g_activeTimed == 3, "altered: three pool-family draws on their section's turn are counted and selected for timing");
    { GpuCensusAlteredScope other(d.ctx.Get(), AlteredDrawClass::TerrainOriginal); }
    { GpuCensusAlteredScope other(d.ctx.Get(), AlteredDrawClass::UiLayer); }
    { GpuCensusAlteredScope other(d.ctx.Get(), AlteredDraw(AlteredDrawClass::Verdict, AlteredFix::GlareSteady)); }
    check(g_section[static_cast<size_t>(GpuCensusSection::AlteredTerrain)].occurrences == 1 &&
              g_section[static_cast<size_t>(GpuCensusSection::AlteredUiLayer)].occurrences == 1 &&
              g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::GlareSteady))].occurrences == 1 &&
              pool.occurrences == 3 && occurrencesEverywhere() == 6 && g_activeTimed == 3,
          "altered: each class, and each fix, counts in its own section only, and a section that is not on its turn is counted, not timed");
    // The rotation reaches every TURN once a cycle, the altered-draw ones included; the fix sections after the
    // first share its turn and are never landed on.
    g_windowFrames = 0;
    g_activeSection = 0;
    bool visited[kSections] = {};
    for (size_t i = 0; i < kSections; ++i) {
        gpuCensusFrame(d.ctx.Get());
        visited[static_cast<size_t>(g_activeSection)] = true;
    }
    bool ownersVisited = true, membersSkipped = true;
    for (size_t i = 0; i < kSections; ++i) {
        if (turnOwnerOf(static_cast<GpuCensusSection>(i)) == static_cast<GpuCensusSection>(i)) ownersVisited = ownersVisited && visited[i];
        else membersSkipped = membersSkipped && !visited[i];
    }
    check(ownersVisited && visited[static_cast<size_t>(GpuCensusSection::AlteredTerrain)] &&
              visited[static_cast<size_t>(GpuCensusSection::AlteredFixFirst)],
          "altered: the frame rotation visits every turn, the altered-draw ones and the fix sections' shared one too");
    check(membersSkipped, "altered: the fix sections after the first are not turns of their own: the rotation never lands on them");

    gpuCensusShutdown();
    check(gpuTimingShutdown(d.ctx.Get()), "explicit owner shutdown");
}

// ---- 4: the line's format, including "-" for an absent section --------
void logFormatCase() {
    for (auto& s : g_section) s = SectionState{};
    g_activeSection = 0;
    g_activeCalls = g_activeTimed = 0;
    g_windowFrames = 200;
    const uint64_t start = GetTickCount64() - 30000;
    g_windowStartMs = start;
    g_p50Cursor = 0;
    g_p50Count = 0;   // no Application-render samples landed this window

    auto& sharpen = g_section[static_cast<size_t>(GpuCensusSection::DoorSharpen)];
    sharpen.occurrences = 20;
    sharpen.sampler.totals.ms = 4.0;
    sharpen.sampler.totals.samples = 2;   // 2 ms/occurrence * (20/200 = 0.1/frame) = 0.2 ms/frame

    g_lastLog.clear();
    logAndResetWindow(start + 30000);

    check(g_lastLog.rfind("EDVR GPU census:", 0) == 0, "log line: starts with the fixed preamble");
    check(g_lastLog.find("30 s, 200 frames") != std::string::npos, "log line: the window length and frame count");
    check(g_lastLog.find("sharpen 0.200 (0.10/frame)") != std::string::npos,
          "log line: a timed section reports ms/frame with its occurrences/frame");
    check(g_lastLog.find("menu -") != std::string::npos, "log line: an absent door section prints '-'");
    check(g_lastLog.find("planet -") != std::string::npos, "log line: an absent in-frame section prints '-'");
    check(g_lastLog.find("application render p50 -;") != std::string::npos,
          "log line: no Application-render samples this window also prints '-'");
    check(g_lastLog.find("timer floor -;") != std::string::npos,
          "log line: no null samples this window also prints '-' for the timer floor");
    check(g_lastLog.find("failed 0.") != std::string::npos, "log line: failed spans are reported by name");
    check(g_lastLog.find("door 0.200") != std::string::npos,
          "log line: door total folds in the sharpen leaf (one of the five top-level door passes)");

    check(sharpen.occurrences == 0, "log line: the window resets occurrences after logging");
    check(g_windowFrames == 0, "log line: the window resets its frame count after logging");
    check(g_p50Count == 0, "log line: the window resets the p50 sample buffer after logging");

    // With Application-render samples: their median, and the game's share
    // as that median less EDVR's total (0.2 ms from the sharpen leaf above).
    for (auto& s : g_section) s = SectionState{};
    g_windowFrames = 200;
    g_windowStartMs = start;
    sharpen.occurrences = 20;
    sharpen.sampler.totals.ms = 4.0;
    sharpen.sampler.totals.samples = 2;
    g_p50Samples[0] = 12.0; g_p50Samples[1] = 10.0; g_p50Samples[2] = 11.0;
    g_p50Count = 3;
    g_lastLog.clear();
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("application render p50 11.000 ms/frame (game ~10.800)") != std::string::npos,
          "log line: application render reports its median and the game's share (median less EDVR)");
    check(g_lastLog.find("(census over the frame total)") == std::string::npos,
          "log line: the over-total note is absent when EDVR's corrected total does not exceed R");

    // The timer floor and the over-total note: a section with both real and
    // null samples (corrected ms/frame = (4.0/2 - 0.4/2) * (20/200) = 0.18),
    // and R below that corrected total so the game's share goes negative.
    for (auto& s : g_section) s = SectionState{};
    g_windowFrames = 200;
    g_windowStartMs = start;
    sharpen.occurrences = 20;
    sharpen.sampler.totals.ms = 4.0;
    sharpen.sampler.totals.samples = 2;
    sharpen.nullSampler.totals.ms = 0.4;
    sharpen.nullSampler.totals.samples = 2;   // 0.2 ms/pair -> 200 us/pair pooled floor
    g_p50Samples[0] = 0.1;
    g_p50Count = 1;
    g_lastLog.clear();
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("sharpen 0.180 (0.10/frame)") != std::string::npos,
          "log line: a section's ms/frame is corrected by its own null mean before reaching the line");
    check(g_lastLog.find("timer floor 200.0 us/pair") != std::string::npos,
          "log line: the timer floor is the pooled null mean across sections, in us/pair");
    check(g_lastLog.find("(game ~-0.080)") != std::string::npos,
          "log line: the game's share stays raw and negative when EDVR's corrected total exceeds R");
    check(g_lastLog.find("(census over the frame total)") != std::string::npos,
          "log line: EDVR's corrected total over R is flagged, not hidden");
}

// ---- 5: the frame gap: fake spans with known GPU ticks, through the census's own intake ------------------
// A span is 10 ms wide at 10 MHz (100000 ticks); the gap after it is what each case varies.
constexpr uint64_t kFreq = 10000000;
constexpr uint64_t kSpanTicks = 100000;
GpuSpanResult fakeSpan(uint64_t sequence, uint64_t first, uint64_t last, uint64_t freq = kFreq) {
    GpuSpanResult r;
    r.sequence = sequence;
    r.reason = GpuSpanReason::Valid;
    r.source = GpuSpanSource::ApplicationRender;
    r.outerMs = freq ? static_cast<double>(last - first) * 1000.0 / static_cast<double>(freq) : 0.0;
    r.firstTick = first;
    r.lastTick = last;
    r.frequency = freq;
    return r;
}
// count frames, sequences firstSeq.., gapTicks(i) after frame i.
template <class Gap>
void feedFrames(unsigned count, uint64_t firstSeq, Gap gapTicks) {
    uint64_t t = 5000000;
    for (unsigned i = 0; i < count; ++i) {
        noteApplicationCompletion(fakeSpan(firstSeq + i, t, t + kSpanTicks));
        t += kSpanTicks + gapTicks(i);
    }
}
void freshWindow(uint64_t start) {
    for (auto& s : g_section) s = SectionState{};
    g_windowFrames = 200;
    g_windowStartMs = start;
    g_p50Count = 0;
    g_gap = GpuFrameGap{};
    g_lines.clear();
    g_lastLog.clear();
}
const std::string* lineWith(const char* prefix) {
    for (const auto& l : g_lines)
        if (l.rfind(prefix, 0) == 0) return &l;
    return nullptr;
}
void gapCases() {
    const uint64_t start = GetTickCount64() - 30000;

    // Saturated: the next frame's first command follows the last one within 0.05 ms, twice by 2 ms.
    freshWindow(start);
    feedFrames(100, 1, [](unsigned i) -> uint64_t { return (i == 10 || i == 50) ? 20000 : 500; });
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("frame gap p50 0.05 / p95 0.05 ms over 99 pairs;") != std::string::npos,
          "gap: a saturated GPU (0.05 ms between frames) reads p50 0.05 / p95 0.05 over 99 pairs, on the census line");
    const std::string* detail = lineWith("EDVR GPU census, frame gap:");
    check(detail && detail->find("p50 0.05 ms, p95 0.05 ms, max 2.00 ms over 99 pairs of 100 valid frames") != std::string::npos,
          "gap: the detail line adds the max (the two 2 ms gaps) and the pair and frame counts");
    check(g_lastLog.find("application render p50 10.000 ms/frame") != std::string::npos,
          "gap: the same completions still feed the application-render median");
    check(g_gap.finishWindow().frames == 0, "gap: the window starts over after the line");

    // Starved: the GPU waits 5 ms for the CPU between frames.
    freshWindow(start);
    feedFrames(100, 1000, [](unsigned) -> uint64_t { return 50000; });
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("frame gap p50 5.00 / p95 5.00 ms over 99 pairs;") != std::string::npos,
          "gap: a starved GPU (5 ms between frames) reads 5.00");

    // Compositor-sized: about 1 ms. The number is printed as measured, and the line says that it is not proof of idleness.
    freshWindow(start);
    feedFrames(60, 1, [](unsigned) -> uint64_t { return 11000; });
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(g_lastLog.find("frame gap p50 1.10 / p95 1.10 ms over 59 pairs;") != std::string::npos &&
              detail && detail->find("a gap of about 1 ms is not proof of idleness") != std::string::npos &&
              detail->find("SteamVR's compositor (another process on the same GPU)") != std::string::npos,
          "gap: a compositor-sized gap (1.10 ms) is reported, and the line says the compositor shares the GPU");
    check(detail && detail->find("upper bound on GPU idle, not idle") != std::string::npos,
          "gap: the wording calls the figure an upper bound on idle, not idle");

    // A missing pair: frame 3 never completes, so 2->3 and 3->4 make no pair. Two pairs, four frames.
    freshWindow(start);
    {
        uint64_t t = 5000000;
        for (uint64_t seq : {1ull, 2ull, 4ull, 5ull}) {
            noteApplicationCompletion(fakeSpan(seq, t, t + kSpanTicks));
            t += kSpanTicks + 1000;
        }
    }
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(g_lastLog.find("over 2 pairs;") != std::string::npos &&
              detail && detail->find("over 2 pairs of 4 valid frames") != std::string::npos,
          "gap: a frame that never completes leaves its two pairs unmade, said as fewer pairs than frames");

    // The same frames, the missing one arriving LAST (results complete out of order): each pair is made once.
    freshWindow(start);
    {
        uint64_t t[6];
        t[0] = 5000000;
        for (unsigned i = 1; i < 6; ++i) t[i] = t[i - 1] + kSpanTicks + 1000;
        for (unsigned i : {0u, 1u, 3u, 4u, 2u}) noteApplicationCompletion(fakeSpan(1 + i, t[i], t[i] + kSpanTicks));
    }
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(detail && detail->find("over 4 pairs of 5 valid frames") != std::string::npos,
          "gap: the late frame completes both its pairs, once each, whatever order the results settle in");
    // A completion repeated is not a second frame.
    freshWindow(start);
    noteApplicationCompletion(fakeSpan(7, 5000000, 5100000));
    noteApplicationCompletion(fakeSpan(7, 5000000, 5100000));
    noteApplicationCompletion(fakeSpan(8, 5101000, 5201000));
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(detail && detail->find("over 1 pairs of 2 valid frames") != std::string::npos,
          "gap: a duplicated completion is one frame, not two");

    // Nothing paired: absent on the census line, never 0.00.
    freshWindow(start);
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(g_lastLog.find("frame gap - (no pairs);") != std::string::npos && g_lastLog.find("frame gap p50") == std::string::npos &&
              detail && detail->find("no pairs this window (0 valid frames") != std::string::npos,
          "gap: a window with no spans reads '-', not 0.00, on both lines");
    // One frame alone: valid, but no neighbour.
    freshWindow(start);
    noteApplicationCompletion(fakeSpan(9, 5000000, 5100000));
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("frame gap - (no pairs);") != std::string::npos, "gap: one frame alone makes no pair");

    // Clock rules: another frequency, overlapping spans, a stall, an invalid span, the other source.
    freshWindow(start);
    noteApplicationCompletion(fakeSpan(1, 5000000, 5100000, kFreq));
    noteApplicationCompletion(fakeSpan(2, 5101000, 5201000, kFreq * 2));   // the clock changed
    noteApplicationCompletion(fakeSpan(3, 5100000, 5200000, kFreq * 2));   // starts before frame 2 ended: overlap
    noteApplicationCompletion(fakeSpan(4, 5200000 + 30000000, 5300000 + 30000000, kFreq * 2));   // 3 s gap: a stall
    GpuSpanResult bad = fakeSpan(5, 0, 0);
    bad.reason = GpuSpanReason::Disjoint;
    noteApplicationCompletion(bad);   // not valid: not fed at all
    GpuSpanResult legacy = fakeSpan(6, 1, 2);
    legacy.source = GpuSpanSource::RenderToSubmit;
    noteApplicationCompletion(legacy);   // the other span: not fed
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(g_lastLog.find("frame gap - (no pairs);") != std::string::npos && detail &&
              detail->find("no pairs this window (4 valid frames") != std::string::npos &&
              detail->find("3 pairs rejected as not comparable (overlapping, another clock, or over 1 s)") != std::string::npos,
          "gap: another clock, an overlap and a stall are rejected, an invalid span and the legacy span never fed");
    // A span with no ticks (a result from before the fields existed, or a poisoned one) is unusable, not a zero gap.
    freshWindow(start);
    noteApplicationCompletion(fakeSpan(1, 0, 0, 0));
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(detail && detail->find("1 spans unusable") != std::string::npos && g_lastLog.find("frame gap - (no pairs);") != std::string::npos,
          "gap: a valid span carrying no ticks or frequency is counted unusable, never paired");
}

// ---- 6: Elite's altered draws: which class a draw is, and the line that reports them ---------------------------
void alteredClassCases() {
    using C = AlteredDrawClass;
    //                      owner, verdictNone, poolSubstituted, terrainOriginal, uiLayered
    check(classifyAlteredDraw(true, true, false, false, false) == C::None, "class: a plain draw EDVR did not touch is not altered");
    check(classifyAlteredDraw(true, true, true, false, false) == C::PoolFamily,
          "class: a verdict-free draw with EDVR's slot target and shaders bound is a pool-family draw");
    check(classifyAlteredDraw(true, false, true, false, false) == C::Verdict,
          "class: the pool flag is stale under a verdict (engineVelocityBeforeDraw did not run): the verdict's wrapper, not a pool-family draw");
    check(classifyAlteredDraw(true, true, false, true, false) == C::TerrainOriginal, "class: a terrain original with the motion target bound");
    check(classifyAlteredDraw(true, true, false, false, true) == C::UiLayer, "class: a draw redirected into the UI layer");
    check(classifyAlteredDraw(true, false, false, false, true) == C::UiLayer,
          "class: a verdict draw the UI layer redirected is the redirect (its target moved)");
    check(classifyAlteredDraw(true, false, false, false, false) == C::Verdict, "class: any other verdict's wrapper");
    check(classifyAlteredDraw(true, true, true, true, true) == C::PoolFamily &&
              classifyAlteredDraw(true, true, false, true, true) == C::TerrainOriginal,
          "class: one class per draw, in the order pool family, terrain, UI layer, verdict");
    check(classifyAlteredDraw(false, true, true, true, true) == C::None && classifyAlteredDraw(false, false, false, false, false) == C::None,
          "class: a foreign (non-owner) context is never counted");
    check(alteredSectionOf(C::PoolFamily) == GpuCensusSection::AlteredPoolFamily &&
              alteredSectionOf(C::TerrainOriginal) == GpuCensusSection::AlteredTerrain &&
              alteredSectionOf(C::UiLayer) == GpuCensusSection::AlteredUiLayer &&
              alteredSectionOf(AlteredDraw(C::Verdict, AlteredFix::Remlok)) == alteredFixSectionOf(AlteredFix::Remlok),
          "class: each class maps to its own section, and a Verdict draw to its fix's");
}

void alteredLineCases() {
    const uint64_t start = GetTickCount64() - 30000;
    // Pool-family draws: 2000 in 200 frames (10 a frame), four timed at 0.2 ms each: 2.000 ms/frame.
    // UI-layer draws: 40 (0.20 a frame), two timed at 0.1 ms: 0.020 ms/frame. Terrain and verdict: never ran.
    freshWindow(start);
    auto& pool = g_section[static_cast<size_t>(GpuCensusSection::AlteredPoolFamily)];
    pool.occurrences = 2000;
    pool.sampler.totals.ms = 0.8;
    pool.sampler.totals.samples = 4;
    auto& ui = g_section[static_cast<size_t>(GpuCensusSection::AlteredUiLayer)];
    ui.occurrences = 40;
    ui.sampler.totals.ms = 0.2;
    ui.sampler.totals.samples = 2;
    logAndResetWindow(start + 30000);
    const std::string* line = lineWith("EDVR GPU census, Elite's own draws that EDVR alters");
    check(line != nullptr, "altered line: a line of its own follows the main census line");
    if (line) {
        check(line->find("pool-family draws (EDVR's slot target and shaders) 2.000 (10.00/frame)") != std::string::npos,
              "altered line: pool-family draws: ms/frame with their count a frame");
        check(line->find("UI draws (redirected to EDVR's layer) 0.020 (0.20/frame)") != std::string::npos,
              "altered line: UI-layer draws report too");
        check(line->find("terrain prepasses (EDVR's motion target and shader) -") != std::string::npos &&
                  line->find("other fix-wrapped draws -") != std::string::npos,
              "altered line: a class that never ran prints '-', never 0.000");
        check(line->find("together 2.020 ms/frame") != std::string::npos, "altered line: the classes are disjoint draws, so together is their sum");
        check(line->find("includes the game's own work in it, not only what EDVR adds") != std::string::npos &&
                  line->find("the game's draw timed whole") != std::string::npos,
              "altered line: it says the figures include the game's own work in those draws");
    }
    check(g_lastLog.find("EDVR ~0.000 ms/frame") != std::string::npos && g_lastLog.find("pool-family") == std::string::npos,
          "altered line: the altered draws are not in EDVR's total and not on the main line");
    check(g_section[static_cast<size_t>(GpuCensusSection::AlteredPoolFamily)].occurrences == 0,
          "altered line: the window resets the altered sections' occurrences");

    // Nothing ran: all four '-', together 0.000 (a sum, not a measurement), and the wording still says what '-' means.
    freshWindow(start);
    logAndResetWindow(start + 30000);
    line = lineWith("EDVR GPU census, Elite's own draws that EDVR alters");
    check(line && line->find("pool-family draws (EDVR's slot target and shaders) -, terrain prepasses (EDVR's motion target and shader) -, "
                             "UI draws (redirected to EDVR's layer) -, other fix-wrapped draws -;") != std::string::npos &&
              line->find("\"-\" means no such draw ran this window") != std::string::npos,
          "altered line: a window with no altered draw prints '-' for all four and says what that means");
    // The timer floor and the spans count include the altered sections' own timers.
    freshWindow(start);
    auto& terrain = g_section[static_cast<size_t>(GpuCensusSection::AlteredTerrain)];
    terrain.occurrences = 20;
    terrain.sampler.totals.ms = 0.4;
    terrain.sampler.totals.samples = 2;
    terrain.nullSampler.totals.ms = 0.1;
    terrain.nullSampler.totals.samples = 2;
    logAndResetWindow(start + 30000);
    line = lineWith("EDVR GPU census, Elite's own draws that EDVR alters");
    check(line && line->find("terrain prepasses (EDVR's motion target and shader) 0.015 (0.10/frame)") != std::string::npos &&
              g_lastLog.find("timer floor 50.0 us/pair") != std::string::npos && g_lastLog.find("spans timed 2,") != std::string::npos,
          "altered line: a class is corrected by its own null pair ((0.4/2 - 0.1/2) x 0.1 = 0.015), and its spans count in the census's totals");
}

// ---- 7: the draws another fix wraps, named fix by fix ---------------------------------------------------------
constexpr const char* kFixLinePrefix = "EDVR GPU census, the other fix-wrapped draws above by the fix that wraps each";
void alteredFixCases() {
    constexpr int kFixes = kAlteredFixCount;
    // The names: one for each fix, fixed strings, all different, in the enum's order.
    bool distinct = true, filled = true;
    for (int i = 0; i < kFixes; ++i) {
        filled = filled && kAlteredFixNames[i] && kAlteredFixNames[i][0];
        for (int j = i + 1; j < kFixes; ++j) distinct = distinct && std::strcmp(kAlteredFixNames[i], kAlteredFixNames[j]) != 0;
    }
    check(filled && distinct, "fix names: one non-empty name for each fix, no two alike");
    check(std::strcmp(kAlteredFixNames[static_cast<int>(AlteredFix::Panel)], "panel distance") == 0 &&
              std::strcmp(kAlteredFixNames[static_cast<int>(AlteredFix::NightVision)], "night vision") == 0 &&
              std::strcmp(kAlteredFixNames[static_cast<int>(AlteredFix::GlareSteady)], "sun glare steady") == 0 &&
              std::strcmp(kAlteredFixNames[static_cast<int>(AlteredFix::Particle)], "particles") == 0 &&
              std::strcmp(kAlteredFixNames[static_cast<int>(AlteredFix::Unnamed)], "unnamed fix") == 0,
          "fix names: each name is at its fix's place in the enum's order");

    // Sections: one each, in order, the last of the census.
    bool sectionsOk = true;
    for (int i = 0; i < kFixes; ++i) {
        const GpuCensusSection sec = alteredSectionOf(AlteredDraw(AlteredDrawClass::Verdict, static_cast<AlteredFix>(i)));
        sectionsOk = sectionsOk && static_cast<int>(sec) == static_cast<int>(GpuCensusSection::AlteredFixFirst) + i &&
                     sec == alteredFixSectionOf(static_cast<AlteredFix>(i)) && sec < GpuCensusSection::Count;
    }
    check(sectionsOk && static_cast<int>(GpuCensusSection::Count) == static_cast<int>(GpuCensusSection::AlteredFixFirst) + kFixes,
          "fix sections: each fix has its own section, in order, and they are the last of the census");
    check(alteredSectionOf(AlteredDraw()) == GpuCensusSection::Count &&
              alteredSectionOf(AlteredDraw(AlteredDrawClass::None, AlteredFix::Remlok)) == GpuCensusSection::Count,
          "fix sections: a draw of no class has no section, whatever fix it carries");
    check(alteredSectionOf(AlteredDraw(AlteredDrawClass::Verdict)) == alteredFixSectionOf(AlteredFix::Unnamed),
          "fix sections: a Verdict draw that names no fix is the unnamed row: visible, not lost");
    check(alteredSectionOf(AlteredDraw(AlteredDrawClass::PoolFamily, AlteredFix::Remlok)) == GpuCensusSection::AlteredPoolFamily,
          "fix sections: a fix carried by a class that is not Verdict changes nothing");

    // Turns: the fix sections share the first one's, every other section keeps its own.
    bool owners = true;
    for (size_t i = 0; i < kSections; ++i) {
        const auto sec = static_cast<GpuCensusSection>(i);
        owners = owners && turnOwnerOf(sec) == (i >= kAlteredFixFirst ? GpuCensusSection::AlteredFixFirst : sec);
    }
    check(owners, "turns: the fix sections share the first one's turn and every other section is its own owner");
    int at = 0;
    unsigned cycle = 0;
    bool onlyOwners = true;
    do {
        at = nextTurnOwner(at);
        ++cycle;
        onlyOwners = onlyOwners && turnOwnerOf(static_cast<GpuCensusSection>(at)) == static_cast<GpuCensusSection>(at);
    } while (at != 0 && cycle < 200);
    check(onlyOwners && cycle == static_cast<unsigned>(kSections) - kAlteredFixCount + 1,
          "turns: the rotation lands only on turn owners, and a cycle is one turn for each section but the later fix ones (21, as before the split)");
    for (auto& s : g_section) s = SectionState{};
    g_section[kAlteredFixFirst + 2].occurrences = 7;
    g_section[kAlteredFixFirst + 11].occurrences = 5;
    g_section[static_cast<size_t>(GpuCensusSection::AlteredUiLayer)].occurrences = 100;
    check(turnOccurrences(GpuCensusSection::AlteredFixFirst) == 12 && turnOccurrences(GpuCensusSection::AlteredUiLayer) == 100,
          "turns: the shared turn's calls are every fix's together, another section's are its own");

    // The shared turn in the rotation and the Begin: one call counter, one K, one stride, one empty pair,
    // every call counted for its own fix. (A null context: no timer is asked for; the selection is what is under test.)
    for (auto& s : g_section) s = SectionState{};
    g_windowStartMs = GetTickCount64();   // no window closes here
    auto& owner = g_section[kAlteredFixFirst];
    auto& glare = g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::GlareSteady))];
    auto& particle = g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::Particle))];
    glare.occurrences = 300;   // two fixes: 300 + 100 calls over ten frames, 40 a frame together
    particle.occurrences = 100;
    owner.turns = 3;
    g_windowFrames = 9;        // gpuCensusFrame counts this frame first: 10
    g_activeSection = static_cast<int>(GpuCensusSection::AlteredUiLayer);
    gpuCensusFrame(nullptr);
    check(g_activeSection == static_cast<int>(GpuCensusSection::AlteredFixFirst),
          "shared turn: the rotation lands on it after the last class");
    check(g_activeStride == 5 && g_activeOffset == 3 && owner.turns == 4,
          "shared turn: the stride is every fix's calls together over K (400 over 10 frames, K 8: every 5th), from the shared turn's own count");
    glare.occurrences = particle.occurrences = 0;
    std::string timedAt;
    for (unsigned call = 0; call < 45; ++call) {
        const GpuCensusSection sec = alteredFixSectionOf(call % 2 ? AlteredFix::Particle : AlteredFix::GlareSteady);
        const unsigned before = g_activeTimed;
        gpuCensusBegin(nullptr, sec);
        if (g_activeTimed > before) timedAt += std::to_string(call) + ",";
        gpuCensusEnd(nullptr, sec);
    }
    check(timedAt == "3,8,13,18,23,28,33,38," && g_activeTimed == 8,
          "shared turn: one call counter and one K across the fixes: every 5th call from the offset, capped at 8");
    check(glare.occurrences == 23 && particle.occurrences == 22,
          "shared turn: each call is counted for its own fix (23 even calls, 22 odd)");
    check(owner.nullPairsTaken == 1 && glare.nullPairsTaken == 0 && particle.nullPairsTaken == 0 && g_activeNullDone,
          "shared turn: ONE empty pair for the turn, kept by the first fix's section whichever fix was timed first");
    // A section that is not the turn's owner is counted and never advances the shared call counter.
    g_activeCalls = g_activeTimed = 0;
    const uint64_t uiBefore = g_section[static_cast<size_t>(GpuCensusSection::AlteredUiLayer)].occurrences;
    check(!gpuCensusBegin(nullptr, GpuCensusSection::AlteredUiLayer) && g_activeCalls == 0 &&
              g_section[static_cast<size_t>(GpuCensusSection::AlteredUiLayer)].occurrences == uiBefore + 1,
          "shared turn: a class section's call during the fixes' turn is counted, not timed, and does not move the fixes' call counter");
    g_activeSection = static_cast<int>(GpuCensusSection::AlteredUiLayer);
    g_activeCalls = g_activeTimed = 0;
    const uint64_t particleBefore = particle.occurrences;
    check(!gpuCensusBegin(nullptr, alteredFixSectionOf(AlteredFix::Particle)) && g_activeCalls == 0 && particle.occurrences == particleBefore + 1,
          "shared turn: a fix's call during another section's turn is counted, not timed");

    // The window's lines. Glare: 1200 calls over 200 frames (6.00 a frame), four timed at 0.4 ms; particles: 200 calls
    // (1.00 a frame), one timed at 0.5 ms; the turn's empty pairs (two, 0.1 ms each) are the first fix's.
    const uint64_t start = GetTickCount64() - 30000;
    freshWindow(start);
    auto& owner2 = g_section[kAlteredFixFirst];
    auto& glare2 = g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::GlareSteady))];
    auto& particle2 = g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::Particle))];
    auto& remlok2 = g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::Remlok))];
    glare2.occurrences = 1200;
    glare2.sampler.totals.ms = 1.6;
    glare2.sampler.totals.samples = 4;
    particle2.occurrences = 200;
    particle2.sampler.totals.ms = 0.5;
    particle2.sampler.totals.samples = 1;
    remlok2.occurrences = 40;   // ran, never timed: 0.000, not '-'
    owner2.nullSampler.totals.ms = 0.2;
    owner2.nullSampler.totals.samples = 2;
    logAndResetWindow(start + 30000);
    const std::string* classes = lineWith("EDVR GPU census, Elite's own draws that EDVR alters");
    const std::string* byFix = lineWith(kFixLinePrefix);
    check(classes != nullptr && byFix != nullptr, "fix line: the classes' line and, after it, a line naming each fix");
    if (classes && byFix) {
        check(classes->find("other fix-wrapped draws 2.200 (7.20/frame)") != std::string::npos &&
                  classes->find("together 2.200 ms/frame") != std::string::npos,
              "fix line: the classes' line keeps its item, now the fixes' sum: (0.4 - 0.1) x 6.00 + (0.5 - 0.1) x 1.00 = 2.200, 7.20 a frame");
        check(byFix->find("sun glare steady 1.800 (6.00/frame)") != std::string::npos &&
                  byFix->find("particles 0.400 (1.00/frame)") != std::string::npos,
              "fix line: each fix's ms/frame with its calls a frame, corrected by the turn's empty pair (first fix's)");
        check(byFix->find("RemLok overlay 0.000 (0.20/frame)") != std::string::npos,
              "fix line: a fix that ran and was never timed reads 0.000 with its calls, like a class does");
        check(byFix->find("night vision -") != std::string::npos && byFix->find("panel distance -") != std::string::npos &&
                  byFix->find("unnamed fix -") != std::string::npos,
              "fix line: a fix that never ran prints '-', including the unnamed row");
        check(byFix->find("\"-\" means no draw of that fix ran this window") != std::string::npos,
              "fix line: it says what '-' means");
        check(byFix->find("sun glare clamp") != std::string::npos && byFix->find("FSS reveal") != std::string::npos &&
                  byFix->find("scanner-body resolve") != std::string::npos && byFix->find("menu backdrop") != std::string::npos,
              "fix line: every named fix has its place on the line, ran or not");
    }
    check(g_lastLog.find("timer floor 100.0 us/pair") != std::string::npos && g_lastLog.find("spans timed 5,") != std::string::npos,
          "fix line: the empty pairs kept by the first fix count in the census's timer floor and spans");
    check(g_lastLog.find("EDVR ~0.000 ms/frame") != std::string::npos && g_lastLog.find("sun glare") == std::string::npos,
          "fix line: the fixes' draws are the game's, timed whole: not in EDVR's total and not on the main line");
    check(glare2.occurrences == 0 && owner2.occurrences == 0, "fix line: the window resets the fix sections' occurrences");

    // Nothing ran: every fix '-', and the sum item on the classes' line reads '-' too.
    freshWindow(start);
    logAndResetWindow(start + 30000);
    classes = lineWith("EDVR GPU census, Elite's own draws that EDVR alters");
    byFix = lineWith(kFixLinePrefix);
    std::string allAbsent;
    for (int i = 0; i < kFixes; ++i) allAbsent += std::string(i ? ", " : "") + kAlteredFixNames[i] + " -";
    check(byFix && byFix->find(allAbsent + ";") != std::string::npos,
          "fix line: a window with no wrapped draw prints all nineteen fixes with '-' in the enum's order");
    check(classes && classes->find("other fix-wrapped draws -;") != std::string::npos,
          "fix line: and the classes' line's sum item is '-' too");

    // A Verdict draw that named no fix is the unnamed row.
    freshWindow(start);
    g_section[static_cast<size_t>(alteredSectionOf(AlteredDraw(AlteredDrawClass::Verdict)))].occurrences = 20;
    logAndResetWindow(start + 30000);
    byFix = lineWith(kFixLinePrefix);
    check(byFix && byFix->find("unnamed fix 0.000 (0.10/frame)") != std::string::npos,
          "fix line: a wrapped draw whose fix has no name shows as the unnamed row, never in another fix's");
}

// Every census line at its worst stays under what the log keeps (about 1166 characters of message).
void lineLengths() {
    const uint64_t start = GetTickCount64() - 30000;
    freshWindow(start);
    for (auto& s : g_section) {
        s.occurrences = 999999;            // 9999.99 a frame over the 100 frames below
        s.sampler.totals.ms = 1.0;         // 1 ms a call: 9999.990 ms/frame, four digits before the point
        s.sampler.totals.samples = 1;
    }
    g_windowFrames = 100;
    for (unsigned i = 0; i < 4000; ++i) g_p50Samples[i] = 99999.0;
    g_p50Count = 4000;
    feedFrames(400, 1, [](unsigned i) -> uint64_t { return 1000000ull + i; });
    logAndResetWindow(start + 30000);
    size_t longest = 0;
    for (const auto& l : g_lines) longest = std::max(longest, l.size());
    std::printf("gpu_census_test: the longest census line at its worst is %zu characters (the log keeps about 1166)\n", longest);
    check(longest < 1150, "lines: every census line at its worst fits the log's line, not cut");
    const std::string* byFix = lineWith(kFixLinePrefix);
    const std::string* classes = lineWith("EDVR GPU census, Elite's own draws that EDVR alters");
    check(byFix && classes, "lines: both altered-draw lines were written at the worst case");
    if (byFix && classes) {
        std::printf("gpu_census_test: at their worst the classes' line is %zu characters and the fixes' line %zu\n", classes->size(), byFix->size());
        check(byFix->size() < 1100 && byFix->size() > 700, "lines: the fixes' line names all nineteen at their widest and still fits");
    }
}

void run() {
    estimatorMathCases();
    calibrationMathCases();
    logFormatCase();
    gapCases();
    alteredClassCases();
    alteredLineCases();
    alteredFixCases();
    lineLengths();
    Runtime runtime;
    Device device(runtime);
    rotationAndRealTimerCase(device);
    std::printf("PASS: %u GPU census checks\n", g_checks);
}

} // namespace
} // namespace edvr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && !wcscmp(argv[1], L"--dry-run")) {
        std::puts("dry-run: no module, device, queries or files");
        return 0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--child")) {
        try { edvr::run(); return 0; }
        catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s\n", error.what());
            return 1;
        }
    }
    if (argc != 2 || wcscmp(argv[1], L"--self-test")) {
        std::fputs("usage: --self-test | --dry-run\n", stderr);
        return 2;
    }
    // Relaunched as a child (gpu_timing_test's own convention): a WARP
    // device and the shared timing registry are process-global state, and
    // an unclean exit here must never be read as this harness's own.
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return 2;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessW(executable, &command[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return 2;
    const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    else {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
        std::fputs("FAIL: owned test child timed out\n", stderr);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code != 0) std::fprintf(stderr, "FAIL: owned D3D11 child exited 0x%08lX\n", code);
    return code == 0 ? 0 : 1;
}

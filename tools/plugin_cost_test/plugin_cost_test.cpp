#include "../../src/common/plugin_cost.h"
#include "plugin_manifest.inc"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace pc = edvr::plugin_cost;

bool check(bool condition, const char* label) {
    if (!condition) std::printf("FAIL: %s\n", label);
    return condition;
}

struct FakeClock final {
    static inline uint64_t ticks = 0;
    static inline unsigned reads = 0;
    static uint64_t now() noexcept {
        ++reads;
        ticks += 10;
        return ticks;
    }
};

struct FakeSink final {
    static inline unsigned siteNotes = 0;
    static inline unsigned tickNotes = 0;
    static inline uint64_t tickTotal = 0;
    static void site(pc::Owner, uint16_t, pc::SiteEvent) noexcept { ++siteNotes; }
    static void ticks(pc::Owner, uint16_t, uint64_t value) noexcept {
        ++tickNotes;
        tickTotal += value;
    }
};

template <class Policy>
void policyScope(uint64_t& handlers) {
    Policy::template note<pc::Owner::CockpitVisuals, 7>(pc::SiteEvent::Reached);
    {
        typename Policy::template Scope<pc::Owner::CockpitVisuals, 7> scope;
        (void)scope;
        ++handlers;
    }
}

bool policyChecks() {
    bool ok = true;
    static_assert(static_cast<uint8_t>(pc::Owner::TemporalAa) == edvr::plugins::kPluginTemporalAa);
    static_assert(static_cast<uint8_t>(pc::Owner::CockpitVisuals) == edvr::plugins::kPluginCockpitVisuals);
    static_assert(static_cast<uint8_t>(pc::Owner::Exposure) == edvr::plugins::kPluginExposure);
    static_assert(static_cast<uint8_t>(pc::Owner::Scanners) == edvr::plugins::kPluginScanners);
    static_assert(static_cast<uint8_t>(pc::Owner::Intro) == edvr::plugins::kPluginIntro);
    static_assert(static_cast<uint8_t>(pc::Owner::OnFootPanel) == edvr::plugins::kPluginOnFootPanel);
    static_assert(static_cast<uint8_t>(pc::Owner::Comfort) == edvr::plugins::kPluginComfort);
    static_assert(static_cast<uint8_t>(pc::Owner::Performance) == edvr::plugins::kPluginPerformance);
    static_assert(static_cast<uint8_t>(pc::Owner::Diagnostics) == edvr::plugins::kPluginDiagnostics);
    static_assert(static_cast<uint8_t>(pc::Owner::Core) == 9);

    uint64_t handlers = 0;
    pc::NoCpu::template note<pc::Owner::CockpitVisuals, 7>(pc::SiteEvent::Reached);
    policyScope<pc::NoCpu>(handlers);
    ok &= check(handlers == 1 && FakeClock::reads == 0 && FakeSink::siteNotes == 0 &&
                FakeSink::tickNotes == 0,
                "NoCpu policy compiles to no clock, sink, or counter callbacks");

    using Sampled = pc::SampledCpu<FakeClock, FakeSink>;
    Sampled::template note<pc::Owner::CockpitVisuals, 8>(pc::SiteEvent::Reached);
    policyScope<Sampled>(handlers);
    ok &= check(FakeClock::reads == 2 && FakeSink::siteNotes == 2 &&
                FakeSink::tickNotes == 1 && FakeSink::tickTotal == 10,
                "sampled scope clocks only an invoked handler and reports exact ticks");
    return ok;
}

bool nightVisionAnnotationChecks() {
    std::ifstream input("src/plugins/cockpit_visuals/night_vision.cpp", std::ios::binary);
    if (!input) return check(false, "Night Vision source is available for API coverage verification");
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::istringstream lines(source);
    std::set<std::string> usedSites;
    std::set<std::string> declaredSites;
    std::set<unsigned> declaredValues;
    std::size_t calls = 0;
    bool ok = true;
    std::string line;
    bool inSiteEnum = false;
    const std::size_t getTypeCall = source.find("ctx->GetType()");
    const std::size_t immediateReturn = source.find("if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;");
    const std::size_t sampleRead = source.find("state.costSample=edvrPluginCostApiSampleFrame()!=0;");
    const std::size_t getTypeNote = source.find("noteNvD3dCall<NvD3dCallSite::GetType>(plugin_cost::ApiClass::ReadQuery);");
    const std::size_t getTypeEnd = immediateReturn == std::string::npos
        ? std::string::npos : source.find(';', immediateReturn);
    ok &= check(getTypeCall != std::string::npos && immediateReturn <= getTypeCall &&
                getTypeEnd != std::string::npos && getTypeCall < getTypeEnd &&
                getTypeEnd < sampleRead && sampleRead < getTypeNote &&
                source.find("ctx->GetType()", getTypeCall + 1) == std::string::npos,
                "GetType executes once; deferred rejection precedes the single API-sample read and conditional note");
    while (std::getline(lines, line)) {
        if (line.find("enum class NvD3dCallSite") != std::string::npos) inSiteEnum = true;
        if (inSiteEnum && line.find("};") != std::string::npos) inSiteEnum = false;
        else if (inSiteEnum) {
            const std::size_t equal = line.find('=');
            if (equal != std::string::npos) {
                const std::size_t nameStart = line.find_first_not_of(" \t", line.find('{') == std::string::npos ? 0 : line.find('{') + 1);
                const std::size_t nameEnd = line.find_first_of(" \t", nameStart);
                const std::size_t valueStart = line.find_first_not_of(" \t", equal + 1);
                if (nameStart != std::string::npos && nameEnd != std::string::npos && valueStart != std::string::npos) {
                    const std::string name = line.substr(nameStart, nameEnd - nameStart);
                    const unsigned value = static_cast<unsigned>(std::strtoul(line.c_str() + valueStart, nullptr, 10));
                    declaredSites.insert(name);
                    declaredValues.insert(value);
                }
            }
        }
        const std::size_t comment = line.find("//");
        const std::size_t codeEnd = comment == std::string::npos ? line.size() : comment;
        std::size_t call = line.find("ctx->");
        std::size_t previousCall = std::string::npos;
        while (call != std::string::npos && call < codeEnd) {
            ++calls;
            const std::size_t note = line.rfind("noteNvD3dCall<NvD3dCallSite::", call);
            const std::size_t methodStart = call + std::strlen("ctx->");
            const std::size_t methodEnd = line.find('(', methodStart);
            const std::string method = methodEnd == std::string::npos
                ? std::string{} : line.substr(methodStart, methodEnd - methodStart);
            if (method == "GetType") {
                ok &= check(usedSites.insert("GetType").second,
                            "GetType source site has its stable deferred-safe note identity");
                previousCall = call;
                call = line.find("ctx->", call + std::strlen("ctx->"));
                continue;
            }
            const bool noteAfterPriorCall = note != std::string::npos &&
                (previousCall == std::string::npos || note > previousCall);
            const std::size_t siteEnd = noteAfterPriorCall ? line.find('>', note) : std::string::npos;
            const std::size_t noteEnd = siteEnd == std::string::npos ? std::string::npos : line.find(';', siteEnd);
            const bool adjacent = noteEnd != std::string::npos &&
                line.substr(noteEnd + 1, call - noteEnd - 1).find_first_not_of(" \t") == std::string::npos;
            if (!noteAfterPriorCall || !adjacent) {
                ok &= check(false, "every direct context API call has one immediately preceding note");
            } else {
                const std::string site = line.substr(note + std::strlen("noteNvD3dCall<NvD3dCallSite::"),
                                                     siteEnd - note - std::strlen("noteNvD3dCall<NvD3dCallSite::"));
                ok &= check(usedSites.insert(site).second,
                            "each direct context API source site has a unique coverage identity");
                const std::size_t apiClassPos = line.find("ApiClass::", siteEnd);
                const std::size_t apiClassEnd = apiClassPos == std::string::npos
                    ? std::string::npos : line.find(')', apiClassPos);
                std::string expectedClass;
                if (method == "Dispatch") expectedClass = "Work";
                else if (method == "UpdateSubresource") expectedClass = "Transfer";
                else if (method.rfind("Get", 0) == 0 || method.rfind("OMGet", 0) == 0 ||
                         method.rfind("CSGet", 0) == 0 || method.rfind("PSGet", 0) == 0 ||
                         method.rfind("RSGet", 0) == 0) expectedClass = "ReadQuery";
                else if (method.rfind("OMSet", 0) == 0 || method.rfind("CSSet", 0) == 0 ||
                         method.rfind("PSSet", 0) == 0) expectedClass = "State";
                const std::string actualClass = apiClassEnd == std::string::npos
                    ? std::string{} : line.substr(apiClassPos + std::strlen("ApiClass::"),
                                                  apiClassEnd - apiClassPos - std::strlen("ApiClass::"));
                ok &= check(!expectedClass.empty() && actualClass == expectedClass,
                            "direct context API note uses its matching operation category");
            }
            previousCall = call;
            call = line.find("ctx->", call + std::strlen("ctx->"));
        }
    }
    for (const std::string& site : usedSites)
        ok &= check(declaredSites.count(site) != 0, "annotated API site has a declared stable identity");
    ok &= check(calls == 39 && usedSites.size() == calls && declaredSites.size() == 40 &&
                declaredValues.size() == declaredSites.size(),
                "first Night Vision API slice covers 39 direct calls with unique stable site IDs");
    return ok;
}

std::string functionBody(const std::string& source, const std::string& signature) {
    const std::size_t start = source.find(signature);
    if (start == std::string::npos) return {};
    const std::size_t open = source.find('{', start + signature.size());
    if (open == std::string::npos) return {};
    unsigned depth = 0;
    for (std::size_t i = open; i < source.size(); ++i) {
        if (source[i] == '{') ++depth;
        else if (source[i] == '}' && --depth == 0) return source.substr(open, i - open + 1);
    }
    return {};
}

bool productionCpuRouteChecks() {
    std::ifstream input("src/d3d11/vscreen.cpp", std::ios::binary);
    if (!input) return check(false, "vScreen source is available for CPU policy route verification");
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    bool ok = true;
    constexpr const char* directDraws[] = {
        "void STDMETHODCALLTYPE hookedDraw(",
        "void STDMETHODCALLTYPE hookedDrawIndexed(",
        "void STDMETHODCALLTYPE hookedDrawInstanced(",
        "void STDMETHODCALLTYPE hookedDrawIndexedInstanced(",
    };
    for (const char* signature : directDraws) {
        const std::string body = functionBody(source, signature);
        const std::size_t flat = body.find("runtimeFlatProfile()");
        const std::size_t internal = body.find("g_vrWorldInternal");
        const std::size_t clock = body.find("DrawClock clock;");
        const std::size_t ladder = body.find("withDrawLadderTrace(");
        const std::size_t ownerGate = body.find("clock.cpuOn && g_state && self == g_state->ownerCtx");
        ok &= check(!body.empty() && flat < internal && internal < clock && clock < ladder &&
                    ladder < ownerGate && ownerGate != std::string::npos,
                    "flat/internal bypasses precede profiling and direct draws sample owner context only");
        if (!body.empty() && internal < clock) {
            const std::string internalRoute = body.substr(internal, clock - internal);
            ok &= check(internalRoute.find("return;") != std::string::npos,
                        "internal world reentry returns before direct-draw CPU sampling");
        }
    }

    constexpr const char* bypasses[] = {
        "void STDMETHODCALLTYPE hookedDrawAuto(",
        "void STDMETHODCALLTYPE hookedDrawIndexedInstancedIndirect(",
        "void STDMETHODCALLTYPE hookedDrawInstancedIndirect(",
    };
    for (const char* signature : bypasses) {
        const std::string body = functionBody(source, signature);
        ok &= check(!body.empty() && body.find("DrawClock") == std::string::npos &&
                    body.find("withDrawLadderTrace(") == std::string::npos &&
                    body.find("SampledCpu") == std::string::npos,
                    "Auto and indirect bypass routes contain no classifier CPU clock path");
    }

    const std::string chooser = functionBody(source, "LadderDecision withDrawLadderTrace(");
    const std::size_t capturing = chooser.find("if (capturing)");
    const std::size_t captureNoCpu = chooser.find("plugin_cost::NoCpu noCpu;");
    const std::size_t captureWork = chooser.find("work(trace, noCpu)");
    const std::size_t selected = chooser.find("if (cpuSample && !suppressCpu)");
    const std::size_t sampled = chooser.find("plugin_cost::SampledCpu<> sampledCpu;");
    const std::size_t unsampled = chooser.find("plugin_cost::NoCpu noCpu;", sampled);
    ok &= check(!chooser.empty() && capturing < captureNoCpu && captureNoCpu < captureWork &&
                captureWork < selected && selected < sampled && sampled < unsampled &&
                chooser.find("return work(noTrace, noCpu);") != std::string::npos,
                "trace capture selects NoCpu before the independent sampled-or-unsampled classifier path");
    if (captureWork != std::string::npos && selected > captureWork) {
        const std::string captureBranch = chooser.substr(capturing, selected - capturing);
        ok &= check(captureBranch.find("SampledCpu") == std::string::npos &&
                    captureBranch.find("work(trace, noCpu)") != std::string::npos,
                    "trace capture path has no sampled CPU policy or QPC handler scope");
    }
    return ok;
}

bool collectorHotPathChecks() {
    std::ifstream input("src/d3d11/plugin_cost.cpp", std::ios::binary);
    if (!input) return check(false, "collector source is available for fixed-memory note-path verification");
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    bool ok = true;
    constexpr const char* noteFunctions[] = {
        "extern \"C\" void edvrPluginCostNoteSite(",
        "extern \"C\" void edvrPluginCostNoteCpuTicks(",
        "extern \"C\" void edvrPluginCostNoteD3dCall(",
    };
    for (const char* signature : noteFunctions) {
        const std::string body = functionBody(source, signature);
        ok &= check(!body.empty() && body.find("Log::") == std::string::npos &&
                    body.find("new ") == std::string::npos &&
                    body.find("std::vector") == std::string::npos &&
                    body.find("mutex") == std::string::npos &&
                    body.find("qpcNow") == std::string::npos,
                    "per-site cost notes remain fixed-memory with no logger, allocator, lock, or clock");
    }
    return ok;
}

bool collectorLifecycleChecks() {
    std::ifstream perfInput("src/d3d11/perf_monitor.cpp", std::ios::binary);
    std::ifstream screenInput("src/d3d11/vscreen.cpp", std::ios::binary);
    if (!perfInput || !screenInput)
        return check(false, "production lifecycle sources are available for collector wiring checks");
    const std::string perf((std::istreambuf_iterator<char>(perfInput)), std::istreambuf_iterator<char>());
    const std::string screen((std::istreambuf_iterator<char>(screenInput)), std::istreambuf_iterator<char>());
    bool ok = true;

    const std::string frame = functionBody(perf, "void perfMonitorFrame(");
    const std::size_t drain = frame.find("edvrPluginCostFrameBoundary(");
    const std::size_t publish = frame.find("detail::g_pluginCostApiSampleFrame = nextSampleFrame;");
    const std::size_t reset = frame.find("s.drawWholeTicks = s.drawRealTicks = 0;");
    ok &= check(!frame.empty() && drain < publish && publish < reset &&
                frame.find("const bool closedCpuSampleFrame = detail::g_perfMonitorSampleDraws;") < drain &&
                frame.find("const bool closedApiSampleFrame = detail::g_pluginCostApiSampleFrame;") < drain,
                "frame boundary drains closed CPU/API flags before publishing next API sample and resetting draw totals");

    const std::string install = functionBody(screen, "void installVScreenFixes(");
    const std::size_t commit = install.find("if (!s.hook.commit())");
    const std::size_t failedReturn = install.find("return;", commit);
    const std::size_t configure = install.find("perfMonitorPluginCostConfigure(");
    ok &= check(!install.empty() && commit < failedReturn && failedReturn < configure,
                "collector configuration occurs only after successful vtable commit");

    const std::string shutdown = functionBody(screen, "void shutdownVScreenFixes(");
    const std::size_t uninstall = shutdown.find("g_state->hook.uninstall();");
    const std::size_t stop = shutdown.find("perfMonitorPluginCostShutdown();", uninstall);
    ok &= check(!shutdown.empty() && uninstall < stop,
                "collector shutdown follows hook uninstall after callbacks are quiescent");
    return ok;
}

bool collectorChecks() {
    constexpr uint64_t kFrequency = 64000000;
    constexpr uint8_t kProfile = 1;
    bool ok = true;
    EdvrPluginCostWindowV1 window{};

    edvrPluginCostShutdown();
    edvrPluginCostConfigure(kProfile, kFrequency);
    // Configure can occur mid-frame. The first boundary is discarded, while
    // its explicit next flag arms API notes for the following complete frame.
    ok &= check(!edvrPluginCostFrameBoundary(900, 1, 1, 1, 0, &window),
                "configure discards the first potentially partial frame");

    bool completed = false;
    uint32_t finalFrame = 0;
    for (uint32_t step = 0; step < 1801 && !completed; ++step) {
        const uint32_t frame = 901 + step;
        const bool cpu = frame == 901 || frame == 902 || frame == 903 || frame == 904;
        if (frame == 901) {
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 5,
                                   static_cast<uint8_t>(pc::SiteEvent::Reached));
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 5,
                                   static_cast<uint8_t>(pc::SiteEvent::Invoked));
            edvrPluginCostNoteCpuTicks(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 5, 100);
        } else if (frame == 902) {
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 6,
                                   static_cast<uint8_t>(pc::SiteEvent::Reached));
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 6,
                                   static_cast<uint8_t>(pc::SiteEvent::NotEligible));
        } else if (frame == 903) {
            // A suppressed trace frame must not contaminate the CPU estimate.
            edvrPluginCostMarkTraceSuppressed();
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 9,
                                   static_cast<uint8_t>(pc::SiteEvent::Invoked));
            edvrPluginCostNoteCpuTicks(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 9, 999);
        } else if (frame == 904) {
            // A real timed scope with a measured zero is distinct from no row.
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 10,
                                   static_cast<uint8_t>(pc::SiteEvent::Reached));
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 10,
                                   static_cast<uint8_t>(pc::SiteEvent::Invoked));
            edvrPluginCostNoteCpuTicks(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 10, 0);
        }

        // API sampling is controlled only by the preceding boundary's next
        // flag. The two calls on frame 901 and one on the trace frame are raw
        // counts; none is scaled by the CPU stride.
        if (frame == 901) {
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 20,
                                      static_cast<uint8_t>(pc::ApiClass::ReadQuery));
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 20,
                                      static_cast<uint8_t>(pc::ApiClass::ReadQuery));
        } else if (frame == 903) {
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 21,
                                      static_cast<uint8_t>(pc::ApiClass::Work));
        } else if (frame == 904) {
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 22,
                                      static_cast<uint8_t>(pc::ApiClass::Transfer));
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 23,
                                      static_cast<uint8_t>(pc::ApiClass::State));
            edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 24,
                                      static_cast<uint8_t>(pc::ApiClass::Instrumentation));
        }

        completed = edvrPluginCostFrameBoundary(frame, cpu ? 1 : 0, 1, 1, 0, &window);
        if (completed) finalFrame = frame;
    }
    ok &= check(completed, "collector closes a fixed 1800-frame report window");
    ok &= check(window.version == 1 && window.profileBit == kProfile &&
                window.firstFrame == 901 && window.lastFrame == finalFrame,
                "report identifies profile and exact closed-frame window");
    ok &= check(window.completedCpuSampleFrames == 3 &&
                window.cpuTraceSuppressedFrames == 1,
                "CPU denominator uses closed unsuppressed sample frames only");
    ok &= check(window.completedApiSampleFrames == 1800,
                "API denominator counts explicit closed sample flags, not CPU stride");

    const auto& cockpit = window.owners[static_cast<uint8_t>(pc::Owner::CockpitVisuals)];
    ok &= check(cockpit.cpuObserved == 1 && cockpit.cpuTimedScopes == 2 &&
                cockpit.cpuReached == 3 && cockpit.cpuInvoked == 2 &&
                cockpit.cpuNotEligible == 1,
                "CPU report separates reached, eligible invocations, and declines");
    ok &= check(cockpit.cpuSiteMask[0] == ((uint64_t{1} << 5) | (uint64_t{1} << 6) |
                                          (uint64_t{1} << 10)) &&
                cockpit.cpuSiteMask[1] == 0,
                "trace-suppressed CPU observations do not enter site coverage");
    const double expectedMeanMs = (100.0 * 64.0 * 1000.0 / double(kFrequency)) / 3.0;
    ok &= check(std::abs(cockpit.cpuMeanMs - expectedMeanMs) < 1e-12 &&
                cockpit.cpuStdDevMs > 0.0,
                "CPU sample statistics scale ticks by 64 and include zero frames");
    ok &= check(cockpit.apiObserved == 1 && cockpit.apiCalls[static_cast<uint8_t>(pc::ApiClass::ReadQuery)] == 2 &&
                cockpit.apiCalls[static_cast<uint8_t>(pc::ApiClass::Work)] == 1 &&
                cockpit.apiCalls[static_cast<uint8_t>(pc::ApiClass::Transfer)] == 1 &&
                cockpit.apiCalls[static_cast<uint8_t>(pc::ApiClass::State)] == 1 &&
                cockpit.apiCalls[static_cast<uint8_t>(pc::ApiClass::Instrumentation)] == 1 &&
                cockpit.apiSiteMask[0] == ((uint64_t{1} << 20) | (uint64_t{1} << 21) |
                                           (uint64_t{1} << 22) | (uint64_t{1} << 23) |
                                           (uint64_t{1} << 24)),
                "API calls are counted once without CPU sampling scale");

    // A separate configured window with no owner activity must not invent a
    // zero row. A reached-only owner is observed even when measured ticks are 0.
    edvrPluginCostConfigure(kProfile, kFrequency);
    ok &= check(!edvrPluginCostFrameBoundary(finalFrame + 1, 0, 0, 0, 0, &window),
                "reconfigure resets partial state and skips the first boundary");
    completed = false;
    for (uint32_t step = 0; step < 1801 && !completed; ++step) {
        const uint32_t frame = finalFrame + 2 + step;
        if (frame == finalFrame + 2) {
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::Core), 3,
                                   static_cast<uint8_t>(pc::SiteEvent::Reached));
            edvrPluginCostNoteSite(static_cast<uint8_t>(pc::Owner::Core), 3,
                                   static_cast<uint8_t>(pc::SiteEvent::Invoked));
            edvrPluginCostNoteCpuTicks(static_cast<uint8_t>(pc::Owner::Core), 3, 0);
        }
        completed = edvrPluginCostFrameBoundary(frame, frame == finalFrame + 2 ? 1 : 0,
                                                 0, 0, 0, &window);
    }
    ok &= check(completed && window.owners[static_cast<uint8_t>(pc::Owner::Core)].cpuObserved == 1 &&
                window.owners[static_cast<uint8_t>(pc::Owner::Core)].cpuMeanMs == 0.0 &&
                window.owners[static_cast<uint8_t>(pc::Owner::CockpitVisuals)].cpuObserved == 0 &&
                window.owners[static_cast<uint8_t>(pc::Owner::CockpitVisuals)].apiObserved == 0,
                "measured zero is reported while owners with no observations remain absent");

    edvrPluginCostShutdown();
    edvrPluginCostShutdown();
    edvrPluginCostNoteD3dCall(static_cast<uint8_t>(pc::Owner::CockpitVisuals), 1,
                              static_cast<uint8_t>(pc::ApiClass::State));
    return ok;
}

bool run(bool full) {
    bool ok = policyChecks();
    if (full) {
        ok &= collectorChecks();
        ok &= nightVisionAnnotationChecks();
        ok &= productionCpuRouteChecks();
        ok &= collectorHotPathChecks();
        ok &= collectorLifecycleChecks();
        ok &= collectorLifecycleChecks();
    }
    std::puts(ok ? "plugin_cost_test: PASS" : "plugin_cost_test: FAILED");
    return ok;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) return run(true) ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return run(true) ? 0 : 1;
    std::puts("Usage: plugin_cost_test --dry-run | --self-test");
    return 2;
}

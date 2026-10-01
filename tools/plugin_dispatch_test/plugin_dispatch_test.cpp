#include "../../src/d3d11/plugin_dispatch.h"
#include "../../src/d3d11/plugin_registry.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace edvr { uint32_t loggerNoteCalls = 0; }

namespace {
constexpr uint32_t kPlugin = edvr::plugins::kPluginCockpitVisuals;
constexpr uint32_t kNightVisionClaim = 0x1001u;
constexpr uint32_t kEarlierClaim = 0x2001u;
constexpr uint64_t kVs = 0xFCF7BD2896751D96ull;
constexpr uint64_t kPs = 0xF786D34B5E118D5Eull;
constexpr uint64_t kActive = uint64_t{1} << kPlugin;
constexpr auto kManifestNightVisionClaim = edvr::plugins::kManifest[kPlugin].claims[
    edvr::plugins::kClaimCockpitVisualsNightVision];
constexpr edvr::plugins::dispatch::ShaderClaimKey kClaims[] = {
    {kPlugin, kManifestNightVisionClaim.id,
     kManifestNightVisionClaim.shaderPairs[0].vertexShaderHash,
     kManifestNightVisionClaim.shaderPairs[0].pixelShaderHash},
};
uint32_t checks = 0;

struct RegistryState {
    bool wants = false;
    bool legacyWants = false;
    uint32_t configureCalls = 0;
    uint32_t claimCalls = 0;
    uint32_t beginCalls = 0;
    uint32_t endCalls = 0;
    uint32_t shutdownCalls = 0;
};
RegistryState registryState;
const char* const kRegistryClaimIds[] = {"night-vision"};
const char* const kNullRegistryClaimIds[] = {nullptr};
const char* const kDuplicateRegistryClaimIds[] = {"night-vision", "night-vision"};

void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        std::printf("FAIL: %s\n", message);
        std::exit(1);
    }
}

uint32_t registryWants(void* state) {
    return static_cast<RegistryState*>(state)->wants ? 1u : 0u;
}
uint32_t registryStartupHooksWanted(void* config) {
    return config && *static_cast<const bool*>(config) ? 1u : 0u;
}
void registryConfigure(void* config) {
    ++registryState.configureCalls;
    registryState.wants = config && *static_cast<const bool*>(config);
}
uint32_t registryClaim(void* state, const char* claimId, uint8_t kind,
                       uint32_t count, uint32_t instances) {
    auto* s = static_cast<RegistryState*>(state);
    ++s->claimCalls;
    return claimId && std::strcmp(claimId, "night-vision") == 0 &&
                   edvr::plugins::dispatch::matchesShape(
                       kManifestNightVisionClaim.drawShape, kind, count, instances)
               ? kNightVisionClaim : 0;
}
void registryBegin(void* state, const char* claimId, ID3D11DeviceContext*) {
    if (claimId && std::strcmp(claimId, "night-vision") == 0)
        ++static_cast<RegistryState*>(state)->beginCalls;
}
void registryEnd(void* state, const char* claimId, ID3D11DeviceContext*) {
    if (claimId && std::strcmp(claimId, "night-vision") == 0)
        ++static_cast<RegistryState*>(state)->endCalls;
}
void registryShutdown(void* state) {
    ++static_cast<RegistryState*>(state)->shutdownCalls;
}
uint32_t legacyGate(void* state) {
    return static_cast<RegistryState*>(state)->legacyWants ? 1u : 0u;
}

EdvrPluginOps registryOps() {
    return {sizeof(EdvrPluginOps), edvr::plugins::kPluginCockpitVisuals,
            "cockpit-visuals", "test.cockpit-visuals", kRegistryClaimIds, 1,
            &registryState, &registryConfigure, &registryWants,
            &registryStartupHooksWanted, &registryClaim,
            &registryBegin, &registryEnd, &registryShutdown};
}

bool oldShape(char kind, uint32_t count, uint32_t instances) {
    return kind == 'X' && count == 240 && instances == 1;
}

// Frozen pre-registry behavior at the old night-vision rung. An earlier
// verdict preempts this rung because beginPanelOverride returns at its first
// claim; otherwise the old predicate checked enabled/failure/shape/hash.
uint32_t legacyDecision(uint32_t earlierClaim, char kind, uint32_t count,
                        uint32_t instances, uint64_t vs, uint64_t ps,
                        bool pulse, bool realistic, bool failed) {
    if (earlierClaim) return earlierClaim;
    return (pulse || realistic) && !failed && oldShape(kind, count, instances) &&
                   vs == kVs && ps == kPs
               ? kNightVisionClaim
               : 0;
}

uint32_t tableDecision(uint32_t earlierClaim, char kind, uint32_t count,
                       uint32_t instances, uint64_t candidates,
                       bool pulse, bool realistic, bool failed,
                       uint32_t* shapeCallbacks, uint32_t* claimCallbacks) {
    if (earlierClaim) return earlierClaim;
    return edvr::plugins::dispatch::resolveCandidate(
        candidates, kPlugin, [&]() {
            ++*shapeCallbacks;
            return edvr::plugins::dispatch::matchesShape(
                kManifestNightVisionClaim.drawShape, static_cast<uint8_t>(kind), count, instances);
        }, [&]() {
            ++*claimCallbacks;
            return (pulse || realistic) && !failed ? kNightVisionClaim : 0;
        });
}

void replayMatrix() {
    const char kinds[] = {'X', 'D'};
    const uint32_t counts[] = {240, 6};
    const uint32_t instances[] = {1, 2};
    const uint64_t shaders[][2] = {{kVs, kPs}, {kVs, 0}, {0, kPs}};
    for (bool pulse : {false, true})
        for (bool realistic : {false, true})
            for (bool failed : {false, true})
                for (size_t shape = 0; shape < 2; ++shape)
                    for (const auto& pair : shaders)
                        for (uint32_t earlier : {0u, kEarlierClaim}) {
                            size_t bindChecks = 0;
                            uint32_t shapeCallbacks = 0;
                            uint32_t callbacks = 0;
                            const uint64_t active = (pulse || realistic) ? kActive : 0;
                            const auto candidateMask = edvr::plugins::dispatch::candidatePlugins(
                                pair[0], pair[1], active, kClaims,
                                sizeof(kClaims) / sizeof(kClaims[0]), &bindChecks);
                            const auto old = legacyDecision(earlier, kinds[shape],
                                counts[shape], instances[shape], pair[0], pair[1],
                                pulse, realistic, failed);
                            const auto table = tableDecision(earlier, kinds[shape],
                                counts[shape], instances[shape], candidateMask,
                                pulse, realistic, failed, &shapeCallbacks, &callbacks);
                            check(old == table, "candidate-table replay matches frozen legacy verdict");
                            check(bindChecks == (active ? 1u : 0u),
                                  "shader-pair work occurs only for active plugins at bind/config");
                            const bool reachesCandidate = earlier == 0 && active &&
                                edvr::plugins::dispatch::matchesShape(
                                    kManifestNightVisionClaim.drawShape,
                                    static_cast<uint8_t>(kinds[shape]),
                                    counts[shape], instances[shape]) &&
                                pair[0] == kVs && pair[1] == kPs;
                            check(callbacks == (reachesCandidate ? 1u : 0u),
                                  "per-draw claim callback runs only for a candidate shape");
                            check(shapeCallbacks == (earlier == 0 && active &&
                                  pair[0] == kVs && pair[1] == kPs ? 1u : 0u),
                                  "uninterested/earlier-claimed draws skip shape work");
                        }
}

void disabledAndCacheCases() {
    size_t pairChecks = 0;
    uint32_t callbacks = 0;
    const uint64_t disabled = edvr::plugins::dispatch::candidatePlugins(
        kVs, kPs, 0, kClaims, sizeof(kClaims) / sizeof(kClaims[0]), &pairChecks);
    const uint32_t result = edvr::plugins::dispatch::resolveCandidate(
        disabled, kPlugin, [&]() { ++callbacks; return true; },
        [&]() { return kNightVisionClaim; });
    check(pairChecks == 0, "disabled plugin has no shader-pair comparison");
    check(disabled == 0 && result == 0 && callbacks == 0,
          "disabled plugin has no candidate and receives no draw callback");

    pairChecks = 0;
    const auto match = edvr::plugins::dispatch::candidatePlugins(
        kVs, kPs, kActive, kClaims, sizeof(kClaims) / sizeof(kClaims[0]), &pairChecks);
    check(edvr::plugins::dispatch::hasCandidate(match, kPlugin),
          "matching bound shader pair is cached as a candidate");
    check(!edvr::plugins::dispatch::hasCandidate(
              edvr::plugins::dispatch::candidatePlugins(kVs, 0, kActive, kClaims,
                  sizeof(kClaims) / sizeof(kClaims[0])), kPlugin),
          "a pixel-shader bind change invalidates the candidate pair");
    uint32_t shapeCalls = 0;
    uint32_t resolverCalls = 0;
    const uint32_t noShape = edvr::plugins::dispatch::resolveCandidate(
        match, kPlugin, [&]() { ++shapeCalls; return false; },
        [&]() { ++resolverCalls; return kNightVisionClaim; });
    check(noShape == 0 && shapeCalls == 1 && resolverCalls == 0,
          "inline shape guard evaluates the shape but skips the registry resolver when it fails");
    check(kManifestNightVisionClaim.drawShape.kind == static_cast<uint8_t>('X') &&
              kManifestNightVisionClaim.drawShape.count == 240 &&
              kManifestNightVisionClaim.drawShape.instances == 1 &&
              kClaims[0].vsHash == kVs && kClaims[0].psHash == kPs,
          "generated claim metadata matches the frozen legacy shader and draw contract");
}

void registryLifecycle() {
    edvr::pluginRegistryShutdown();
    registryState = {};
    EdvrPluginOps ops = registryOps();
    EdvrPluginOps invalid = ops;
    invalid.structSize = 0;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects malformed callback record");
    invalid = ops;
    invalid.claimIds = kNullRegistryClaimIds;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects null claim IDs");
    invalid = ops;
    invalid.claimIds = kDuplicateRegistryClaimIds;
    invalid.claimCount = 2;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects duplicate claim IDs");
    invalid = ops;
    invalid.drawGateName = "";
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects empty gate subscription names");
    invalid = ops;
    invalid.end = nullptr;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects a claimed draw without paired End");
    invalid = ops;
    invalid.startupHooksWanted = nullptr;
    check(!edvr::pluginRegistryRegister(&invalid), "registry rejects modules without startup hook demand callback");
    const auto savedProfile = edvr::g_runtimeProfile;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    check(!edvr::pluginRegistryProfileSupports(edvr::plugins::kPluginCockpitVisuals) &&
              !edvr::pluginRegistryRegister(&ops),
          "VR-only module has no registration or shader interest in flat profile");
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(1), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(2), kPs);
    bool unsupportedOn = true;
    check(edvr::pluginRegistryShaderCandidates() == 0 &&
              !edvr::pluginRegistryWantsStartupHooks(&unsupportedOn),
          "flat profile has no candidate mask or startup hook demand");
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Invalid;
    check(!edvr::pluginRegistryProfileSupports(edvr::plugins::kPluginCockpitVisuals),
          "invalid install descriptor disables profile-scoped plugin work");
    edvr::g_runtimeProfile = savedProfile;
    check(edvr::pluginRegistryProfileSupports(edvr::plugins::kPluginCockpitVisuals),
          "legacy VR profile supports the pilot module");
    check(edvr::pluginRegistryRegister(&ops), "registry accepts only the pilot manifest module");
    bool startupOn = true;
    check(edvr::pluginRegistryWantsStartupHooks(&startupOn),
          "enabled pilot alone requests the core hook installation");
    startupOn = false;
    check(!edvr::pluginRegistryWantsStartupHooks(&startupOn),
          "disabled pilot does not keep otherwise-unused hooks installed");
    check(!edvr::pluginRegistryRegister(&ops), "registry rejects duplicate plugin registration");
    check(edvr::pluginRegistryRegisterLegacyDrawGate("legacy.test", &legacyGate, &registryState),
          "registry accepts a stable named legacy subscription");
    check(!edvr::pluginRegistryRegisterLegacyDrawGate("legacy.test", &legacyGate, &registryState),
          "registry rejects duplicate subscription names");

    bool off = false;
    const uint32_t noteBaseline = edvr::loggerNoteCalls;
    edvr::pluginRegistryConfigure(&off);
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(1), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(2), kPs);
    check(edvr::loggerNoteCalls == noteBaseline,
          "off shader binds and configure do not take the logger path");
    check(edvr::pluginRegistryShaderCandidates() == 0,
          "off-period canonical shader binds do not create plugin candidates");
    check(!edvr::pluginRegistryWantsDraws(&registryState),
          "named and plugin subscriptions both report off");

    registryState.legacyWants = true;
    check(edvr::pluginRegistryWantsDraws(&registryState),
          "named legacy subscriber opens the gate independently of the plugin");
    registryState.legacyWants = false;
    bool on = true;
    edvr::pluginRegistryConfigure(&on);
    check(edvr::pluginRegistryWantsDraws(&registryState),
          "plugin subscription opens the gate independently of legacy consumers");
    check(edvr::pluginRegistryShaderCandidates() ==
              (uint64_t{1} << edvr::plugins::kPluginCockpitVisuals),
          "config-on seeds a candidate from the already-bound canonical pair");
    const uint64_t candidates = edvr::pluginRegistryShaderCandidates();
    const auto claim = edvr::plugins::dispatch::resolveCandidate(
        candidates, edvr::plugins::kPluginCockpitVisuals, [&] {
            return edvr::plugins::dispatch::matchesShape(
                kManifestNightVisionClaim.drawShape, 'X', 240, 1);
        }, [&] {
            return edvr::pluginRegistryResolveDraw(candidates, 'X', 240, 1);
        });
    check(claim == kNightVisionClaim && registryState.claimCalls == 1,
          "candidate dispatch invokes the declared night-vision claim");
    check(edvr::loggerNoteCalls == noteBaseline,
          "candidate shader bind and draw resolution only queue breadcrumbs");
    edvr::pluginRegistryReportActivity();
    check(edvr::loggerNoteCalls == noteBaseline + 2,
          "frame-boundary activity report drains both one-shot breadcrumbs");
    edvr::pluginRegistryReportActivity();
    check(edvr::loggerNoteCalls == noteBaseline + 2,
          "drained activity breadcrumbs do not log twice");
    (void)edvr::plugins::dispatch::resolveCandidate(
        candidates, edvr::plugins::kPluginCockpitVisuals, [] { return true; },
        [&] { return edvr::pluginRegistryResolveDraw(candidates, 'X', 240, 1); });
    edvr::pluginRegistryReportActivity();
    check(edvr::loggerNoteCalls == noteBaseline + 2,
          "later candidate draws do not re-enter the logger");
    edvr::pluginRegistryBegin(claim, nullptr);
    edvr::pluginRegistryEnd(claim, nullptr);
    check(registryState.beginCalls == 1 && registryState.endCalls == 1,
          "registry routes matched begin and end callbacks to the owning module");

    edvr::bindingForgetAll();
    check(edvr::pluginRegistryShaderCandidates() == 0,
          "canonical ClearState/unbind observer drops the cached shader candidate");
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(3), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(4), kPs);
    check(edvr::pluginRegistryShaderCandidates() ==
              (uint64_t{1} << edvr::plugins::kPluginCockpitVisuals),
          "active canonical shader repair republishes a matching candidate");
    edvr::pluginRegistrySetDrawGateFailOpen();
    off = false;
    edvr::pluginRegistryConfigure(&off);
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(5), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(6), kPs);
    check(edvr::pluginRegistryShaderCandidates() == 0,
          "configure-off removes the observer while shadow tracking continues");
    edvr::pluginRegistryConfigure(&on);
    check(edvr::pluginRegistryShaderCandidates() ==
              (uint64_t{1} << edvr::plugins::kPluginCockpitVisuals),
          "configure-on reseeds after silent off-period shader changes");
    off = false;
    edvr::pluginRegistryConfigure(&off);
    check(edvr::pluginRegistryWantsDraws(&registryState),
          "registration errors can force the aggregate gate open");
    edvr::pluginRegistryShutdown();
    check(registryState.shutdownCalls == 1 &&
              edvr::pluginRegistryShaderCandidates() == 0 &&
              !edvr::pluginRegistryWantsDraws(&registryState),
          "shutdown releases modules and clears dispatch/subscription caches");
    check(edvr::pluginRegistryRegister(&ops), "registry can restart after shutdown");
    const uint32_t shutdownNoteBaseline = edvr::loggerNoteCalls;
    edvr::pluginRegistryConfigure(&on);
    edvr::bindingSetShader(edvr::BindSlot::Vs, reinterpret_cast<void*>(7), kVs);
    edvr::bindingSetShader(edvr::BindSlot::Ps, reinterpret_cast<void*>(8), kPs);
    const uint64_t shutdownCandidates = edvr::pluginRegistryShaderCandidates();
    (void)edvr::plugins::dispatch::resolveCandidate(
        shutdownCandidates, edvr::plugins::kPluginCockpitVisuals,
        [] { return true; }, [&] {
            return edvr::pluginRegistryResolveDraw(shutdownCandidates, 'X', 240, 1);
        });
    check(edvr::loggerNoteCalls == shutdownNoteBaseline,
          "pending draw activity remains deferred until a safe reporting point");
    edvr::pluginRegistryShutdown();
    check(edvr::loggerNoteCalls == shutdownNoteBaseline + 2,
          "shutdown drains pending bind and draw breadcrumbs away from draw dispatch");
}
}

namespace edvr {
Log& Log::get() { static Log log; return log; }
Log::~Log() = default;
void Log::note(const char*, ...) { ++loggerNoteCalls; }
void breadcrumb(const char*) {}
}

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("Usage: plugin_dispatch_test --self-test");
        return 2;
    }
    replayMatrix();
    disabledAndCacheCases();
    registryLifecycle();
    std::printf("PASS: plugin dispatch (%u checks)\n", checks);
    return 0;
}

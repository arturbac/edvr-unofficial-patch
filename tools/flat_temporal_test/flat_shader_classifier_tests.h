#pragma once
// flat_shader_classifier tests: fixture-driven classification checks against
// the real game/EDHM blobs in tools/flat_temporal_test/fixtures. A missing
// fixture skips its check (the blobs are not in the repository), matching
// engine_velocity_test's treatment of absent local dumps. The --classify-dir
// sweep entry point is at the bottom.
#include "../../src/d3d11/flat_shader_classifier.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <io.h>

namespace flat_shader_classifier_tests {

inline bool loadFixture(const char* name, std::vector<uint8_t>& out) {
    char path[320];
    std::snprintf(path, sizeof(path), "tools/flat_temporal_test/fixtures/%s.dxbc", name);
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamoff size = file.tellg();
    if (size <= 0) return false;
    out.resize(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(file);
}

struct PairVerdict {
    bool present = false;
    edvr::FlatShaderPairClassification c{};
};

inline PairVerdict classifyPair(const char* vs, const char* ps) {
    PairVerdict result;
    std::vector<uint8_t> vsBytes, psBytes;
    if (!loadFixture(vs, vsBytes) || !loadFixture(ps, psBytes)) return result;
    result.present = true;
    result.c = edvr::classifyFlatShaderPair(vsBytes.data(), vsBytes.size(), psBytes.data(), psBytes.size());
    return result;
}

inline const char* vsClassName(edvr::FlatVsProjectionClass c) {
    switch (c) {
    case edvr::FlatVsProjectionClass::NoBytecode: return "NoBytecode";
    case edvr::FlatVsProjectionClass::InertNoCB: return "InertNoCB";
    case edvr::FlatVsProjectionClass::ForwardColumns: return "ForwardColumns";
    case edvr::FlatVsProjectionClass::ForwardDp4: return "ForwardDp4";
    default: return "Unclassified";
    }
}
inline const char* psSafetyName(edvr::FlatPsProjectionSafety s) {
    switch (s) {
    case edvr::FlatPsProjectionSafety::NoBytecode: return "NoBytecode";
    case edvr::FlatPsProjectionSafety::Clean: return "Clean";
    default: return "Consumer";
    }
}

inline int tests() {
    using edvr::FlatVsProjectionClass;
    using edvr::FlatPsProjectionSafety;
    using edvr::FlatClassifierReason;
    using edvr::FlatVposConsumerSubcode;
    int failures = 0, skipped = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) { std::printf("FAIL: shader classifier %s\n", what); ++failures; }
    };
    // Extended to also assert the None/non-None invariant on the new reason
    // fields: a stage that classified successfully must carry reason None,
    // and a refused stage (VS Unclassified / PS Consumer) must carry a
    // reason other than None. The specific reason value for named negative
    // fixtures is pinned separately below via expectReasonPair.
    auto expectPair = [&](const char* vs, const char* ps, FlatVsProjectionClass vc, unsigned slot,
                          unsigned row, FlatPsProjectionSafety psafe, const char* what) {
        const auto r = classifyPair(vs, ps);
        if (!r.present) { std::printf("SKIP: shader classifier %s (fixtures absent)\n", what); ++skipped; return; }
        const bool ok = r.c.vs == vc && r.c.ps == psafe &&
                        (vc == FlatVsProjectionClass::ForwardColumns || vc == FlatVsProjectionClass::ForwardDp4
                             ? (r.c.vsSlot == slot && r.c.vsRow == row)
                             : true) &&
                        ((vc == FlatVsProjectionClass::Unclassified) == (r.c.vsReason != FlatClassifierReason::None)) &&
                        ((psafe == FlatPsProjectionSafety::Consumer) == (r.c.psReason != FlatClassifierReason::None));
        if (!ok) {
            std::printf("FAIL: shader classifier %s: got %s(%u,%u)+%s vsReason=%s psReason=%s\n", what,
                        vsClassName(r.c.vs), r.c.vsSlot, r.c.vsRow, psSafetyName(r.c.ps),
                        edvr::flatClassifierReasonName(r.c.vsReason), edvr::flatClassifierReasonName(r.c.psReason));
            ++failures;
        }
    };
    // Pins the exact reason for a fixture pair already checked by expectPair
    // above (so presence/skip bookkeeping is not duplicated here).
    auto expectReasonPair = [&](const char* vs, const char* ps, FlatClassifierReason wantVs,
                                FlatClassifierReason wantPs, const char* what) {
        const auto r = classifyPair(vs, ps);
        if (!r.present) return;
        const bool ok = r.c.vsReason == wantVs && r.c.psReason == wantPs;
        if (!ok) {
            std::printf("FAIL: shader classifier %s: got vsReason=%s psReason=%s\n", what,
                        edvr::flatClassifierReasonName(r.c.vsReason), edvr::flatClassifierReasonName(r.c.psReason));
            ++failures;
        }
    };

    // --- Positive: must admit -------------------------------------------------
    // EDHM-chain pairs (Epic 20260925_173622/193443 menu capture). VS AACF
    // instructions 139-144: mul/add column sum of cb1[270..273] into o4.
    expectPair("vs_AACFDCF2FB9AD809", "ps_CAD1F585EDDC5641",
               FlatVsProjectionClass::ForwardColumns, 1, 270, FlatPsProjectionSafety::Clean, "AACF+CAD1 columns");
    expectPair("vs_EB5234DB6ADB491D", "ps_63B1524A9F805A4C",
               FlatVsProjectionClass::ForwardColumns, 1, 270, FlatPsProjectionSafety::Clean, "EB52+63B1 columns");
    expectPair("vs_361CD4B7FF213A01", "ps_CDDFE2157F5654B8",
               FlatVsProjectionClass::ForwardColumns, 1, 270, FlatPsProjectionSafety::Clean, "361C+CDDF columns");
    // 0357 instructions 12-15: dp4 o3.x/y/z/w, cb2[10..13], r0 (r0 = v1 + w=1).
    expectPair("vs_0357BBB2DEE43C1F", "ps_BE02244365AD810C",
               FlatVsProjectionClass::ForwardDp4, 2, 10, FlatPsProjectionSafety::Clean, "0357+BE02 dp4");
    // BE02's PS div idiom is the allowed reconstruction-from-grid case:
    // instructions 0-3 ftou vPos -> ld t0 (scene depth) -> div by v2.z.
    // Inert packed-UV clip VS (525D: no dcl_constantbuffer at all).
    expectPair("vs_525D47E3D5E2EFF4", "ps_0D617929FED842F0",
               FlatVsProjectionClass::InertNoCB, 0, 0, FlatPsProjectionSafety::Clean, "525D+0D61 inert EDHM");
    expectPair("vs_525D47E3D5E2EFF4", "ps_F0BAE053476F8730",
               FlatVsProjectionClass::InertNoCB, 0, 0, FlatPsProjectionSafety::Clean, "525D+F0BA inert stock");
    // Stock companions of the same VS families.
    expectPair("vs_361CD4B7FF213A01", "ps_FA7411BF7E4C4088",
               FlatVsProjectionClass::ForwardColumns, 1, 270, FlatPsProjectionSafety::Clean, "361C+FA74 columns");
    expectPair("vs_0357BBB2DEE43C1F", "ps_81812EF97FB4A361",
               FlatVsProjectionClass::ForwardDp4, 2, 10, FlatPsProjectionSafety::Clean, "0357+8181 dp4");
    expectPair("vs_0357BBB2DEE43C1F", "ps_222188632125D14B",
               FlatVsProjectionClass::ForwardDp4, 2, 10, FlatPsProjectionSafety::Clean, "0357+2221 dp4");
    expectPair("vs_EB5234DB6ADB491D", "ps_B7D50283329322C3",
               FlatVsProjectionClass::ForwardColumns, 1, 270, FlatPsProjectionSafety::Clean, "EB52+B7D5 columns");
    expectPair("vs_EB5234DB6ADB491D", "ps_DC603C35BBE74B31",
               FlatVsProjectionClass::ForwardColumns, 1, 270, FlatPsProjectionSafety::Clean, "EB52+DC60 columns");
    expectPair("vs_AACFDCF2FB9AD809", "ps_CF534B32F491561A",
               FlatVsProjectionClass::ForwardColumns, 1, 270, FlatPsProjectionSafety::Clean, "AACF+CF53 columns");

    // ps_4E4F (tile lookup): vPos divided by a cb scalar then ftou into a
    // pixel-grid load -- the allowed tile-lookup shape.
    {
        std::vector<uint8_t> ps;
        if (!loadFixture("ps_4E4FF61E8A08FC7E", ps)) {
            std::printf("SKIP: shader classifier ps_4E4F tile lookup (fixtures absent)\n"); ++skipped;
        } else {
            const auto c = edvr::classifyFlatShaderPair(nullptr, 0, ps.data(), ps.size());
            expect(c.ps == FlatPsProjectionSafety::Clean && c.vs == FlatVsProjectionClass::NoBytecode,
                   "ps_4E4F tile lookup is Clean");
            expect(c.psReason == FlatClassifierReason::None, "ps_4E4F tile lookup reason is None");
        }
    }

    // No-bytecode (null) inputs: NoBytecode verdicts, never admission.
    {
        const auto c = edvr::classifyFlatShaderPair(nullptr, 0, nullptr, 0);
        expect(c.vs == FlatVsProjectionClass::NoBytecode && c.ps == FlatPsProjectionSafety::NoBytecode,
               "null inputs are NoBytecode");
        expect(c.vsReason == FlatClassifierReason::NoBytecode && c.psReason == FlatClassifierReason::NoBytecode,
               "null inputs reason is NoBytecode for both stages");
        std::vector<uint8_t> vs;
        if (loadFixture("vs_0357BBB2DEE43C1F", vs)) {
            const auto v = edvr::classifyFlatShaderPair(vs.data(), vs.size(), nullptr, 0);
            expect(v.vs == FlatVsProjectionClass::ForwardDp4 && v.ps == FlatPsProjectionSafety::NoBytecode,
                   "missing PS blob is NoBytecode, VS still classified");
            expect(v.vsReason == FlatClassifierReason::None && v.psReason == FlatClassifierReason::NoBytecode,
                   "missing PS blob: vsReason None (classified), psReason NoBytecode");
        }
    }

    // --- Negative: must NOT admit ---------------------------------------------
    // 7EAC instructions 1-8: vPos -> mul/mad -> div divisor (cb-ray
    // reconstruction); also cb2[r3.z+11] index-relative at instruction 36.
    {
        std::vector<uint8_t> ps;
        if (!loadFixture("ps_7EAC71963E66C5FE", ps)) { std::printf("SKIP: ps_7EAC (fixtures absent)\n"); ++skipped; }
        else {
            const auto c = edvr::classifyFlatShaderPair(nullptr, 0, ps.data(), ps.size());
            expect(c.ps == FlatPsProjectionSafety::Consumer, "ps_7EAC vPos/cb-ray reconstruction is Consumer");
            // The PS-side multi-row cb combine (facts checked before any
            // taint analysis) preempts the vPos/div idiom the fixture was
            // named for -- verified by running, not assumed from the note.
            expect(c.psReason == FlatClassifierReason::MultiRowTemp, "ps_7EAC reason is MultiRowTemp");
        }
    }
    // 8DEF instruction 0: div r0.xy, v1.xyxx, v1.zzzz -- clip-varying depth-UV.
    {
        std::vector<uint8_t> ps;
        if (!loadFixture("ps_8DEF46452FA459F5", ps)) { std::printf("SKIP: ps_8DEF (fixtures absent)\n"); ++skipped; }
        else {
            const auto c = edvr::classifyFlatShaderPair(nullptr, 0, ps.data(), ps.size());
            expect(c.ps == FlatPsProjectionSafety::Consumer, "ps_8DEF clip-varying depth-UV is Consumer");
            expect(c.psReason == FlatClassifierReason::VposConsumer, "ps_8DEF reason is VposConsumer");
            expect(c.psConsumerSubcode == FlatVposConsumerSubcode::DivOrigin,
                   "ps_8DEF subcode is DivOrigin (div of two Input-origin operands)");
        }
    }
    // vs_7E38 (deferred inverse-ray): position is a passthrough mov; the
    // cb2[14..16] dp3 ray sits outside any position pattern.
    expectPair("vs_7E38A6AA1269C901", "ps_81812EF97FB4A361",
               FlatVsProjectionClass::Unclassified, 0, 0, FlatPsProjectionSafety::Clean, "vs_7E38 deferred ray refuses");
    // Position is a passthrough mov of an input: the form never matches
    // either forward idiom in the first place (the dp3 ray sits elsewhere).
    expectReasonPair("vs_7E38A6AA1269C901", "ps_81812EF97FB4A361",
                     FlatClassifierReason::NotForward, FlatClassifierReason::None, "vs_7E38 reason is NotForward");
    // vs_F8FA (sky inverse): instructions 0-3 combine cb2[11..14] via
    // mul/mad outside the position slice (the safety rule).
    {
        std::vector<uint8_t> vs;
        if (!loadFixture("vs_F8FA801F2CB1E27C", vs)) { std::printf("SKIP: vs_F8FA (fixtures absent)\n"); ++skipped; }
        else {
            const auto c = edvr::classifyFlatShaderPair(vs.data(), vs.size(), nullptr, 0);
            expect(c.vs == FlatVsProjectionClass::Unclassified, "vs_F8FA sky inverse refuses");
            // Position itself does not match either forward idiom (it is
            // computed some other way); the cb1[11..14] combine the fixture
            // was named for sits outside the position slice and the
            // violation scan that would flag it never runs. Verified by
            // running: NotForward, not SecondMatrix.
            expect(c.vsReason == FlatClassifierReason::NotForward, "vs_F8FA sky inverse reason is NotForward");
        }
    }
    // vs_1F17 (conditional two matrices): r3 written under if/else with
    // cb1[41..44] and cb1[45..48]; a branch in the position chain refuses.
    {
        std::vector<uint8_t> vs;
        if (!loadFixture("vs_1F17BF54DB6EE407", vs)) { std::printf("SKIP: vs_1F17 (fixtures absent)\n"); ++skipped; }
        else {
            const auto c = edvr::classifyFlatShaderPair(vs.data(), vs.size(), nullptr, 0);
            expect(c.vs == FlatVsProjectionClass::Unclassified, "vs_1F17 conditional matrices refuse");
            expect(c.vsReason == FlatClassifierReason::ControlFlow,
                   "vs_1F17 conditional matrices reason is ControlFlow");
        }
    }
    // vs_BA16 (skinned bone loop): the dp4 source chain reaches ld_structured
    // inside a loop (index-relative addressing) -- not the clean src idiom.
    expectPair("vs_BA16062A2EB66F1F", "ps_33758387B70944A1",
               FlatVsProjectionClass::Unclassified, 0, 0, FlatPsProjectionSafety::Clean, "vs_BA16 skinned loop refuses");
    // The dp4 idiom's position forms match, but the shared source vector's
    // slice reaches ld_structured inside a loop -- not the clean-src idiom.
    expectReasonPair("vs_BA16062A2EB66F1F", "ps_33758387B70944A1",
                     FlatClassifierReason::SliceNotClean, FlatClassifierReason::None, "vs_BA16 reason is SliceNotClean");

    // --- Section-57 corpus: verdicts documented from the classifier ----------
    // (see the sweep table in the change report; the refuse cases below are
    // asserted, the rest is recorded by printing.)
    // vsReason/psReason are the actual measured values (verified by running
    // this rig, not assumed from the note): the facts checked before any
    // taint/form analysis (IndexableTemp, MultiRowTemp) preempt the idiom
    // each fixture was originally named for in two of the three decals.
    struct Documented {
        const char* vs; const char* ps; bool mustRefuse;
        FlatClassifierReason vsReason; FlatClassifierReason psReason;
        const char* note;
    };
    const Documented documented[] = {
        // Decal pairs: their PSs divide the exported clip varying for a
        // depth-texture UV, so the PSs must refuse (the exact table covers
        // these pairs; generic admission would skip the alignment proof).
        {"vs_0A298DE7DF833A46", "ps_6FD4C38BA927C8C7", true, FlatClassifierReason::None,
         FlatClassifierReason::IndexableTemp, "decal A: PS div v7.xy/v7.z refuses"},
        {"vs_D8FCE3CEA16B9B51", "ps_06AA136E4D58CBA2", true, FlatClassifierReason::None,
         FlatClassifierReason::MultiRowTemp, "decal B: PS div v6.xy/v6.z refuses"},
        {"vs_66DE2CADB1F4AE6B", "ps_BBDE4E71FB78528A", true, FlatClassifierReason::None,
         FlatClassifierReason::MultiRowTemp, "decal D: PS-side cb2[23..26] matrix refuses"},
        // Undocumented companions: verdict recorded by the sweep.
        {"vs_94D5C556DFD6D705", "ps_912477AEF6958379", false, FlatClassifierReason::NotForward,
         FlatClassifierReason::None, "glare pair: VS position is an integer and(), not a forward idiom"},
        {"vs_94D5C556DFD6D705", "ps_06332CA168B6DA63", false, FlatClassifierReason::NotForward,
         FlatClassifierReason::None, "glare VS with the other captured PS"},
    };
    for (const auto& d : documented) {
        const auto r = classifyPair(d.vs, d.ps);
        if (!r.present) { std::printf("SKIP: shader classifier %s (fixtures absent)\n", d.note); ++skipped; continue; }
        std::printf("classifier: %s + %s -> %s(%u,%u)+%s clipXyw=%u vsReason=%s psReason=%s; %s\n", d.vs, d.ps,
                    vsClassName(r.c.vs), r.c.vsSlot, r.c.vsRow, psSafetyName(r.c.ps),
                    r.c.vsExportsClipXyw ? 1u : 0u, edvr::flatClassifierReasonName(r.c.vsReason),
                    edvr::flatClassifierReasonName(r.c.psReason), d.note);
        if (d.mustRefuse)
            expect(!(r.c.ps == FlatPsProjectionSafety::Clean &&
                     (r.c.vs == FlatVsProjectionClass::ForwardColumns || r.c.vs == FlatVsProjectionClass::ForwardDp4 ||
                      r.c.vs == FlatVsProjectionClass::InertNoCB)),
                   d.note);
        expect(r.c.vsReason == d.vsReason, d.note);
        expect(r.c.psReason == d.psReason, d.note);
    }

    // --- Corrupt blobs: never admission, no crash ------------------------------
    // Both mutations break parseContainer's own checks (a truncated chunk
    // table/program header, or the whole-container retail checksum) before
    // the walker ever runs, so the reason is Container, not Walk.
    {
        std::vector<uint8_t> vs, ps;
        if (loadFixture("vs_0357BBB2DEE43C1F", vs) && loadFixture("ps_81812EF97FB4A361", ps)) {
            std::vector<uint8_t> half(vs.begin(), vs.begin() + vs.size() / 2);
            const auto c = edvr::classifyFlatShaderPair(half.data(), half.size(), ps.data(), ps.size());
            expect(c.vs == FlatVsProjectionClass::Unclassified, "half-cut VS blob refuses");
            expect(c.vsReason == FlatClassifierReason::Container, "half-cut VS blob reason is Container");
            std::vector<uint8_t> flipped(ps);
            flipped[flipped.size() / 2] ^= 0xFF;
            const auto f = edvr::classifyFlatShaderPair(vs.data(), vs.size(), flipped.data(), flipped.size());
            expect(f.ps == FlatPsProjectionSafety::Consumer, "byte-flipped PS blob refuses");
            expect(f.psReason == FlatClassifierReason::Container, "byte-flipped PS blob reason is Container");
            const auto both = edvr::classifyFlatShaderPair(half.data(), half.size(), flipped.data(), flipped.size());
            expect(both.vs == FlatVsProjectionClass::Unclassified && both.ps == FlatPsProjectionSafety::Consumer,
                   "corrupt pair refuses both sides");
            expect(both.vsReason == FlatClassifierReason::Container && both.psReason == FlatClassifierReason::Container,
                   "corrupt pair reasons are Container on both sides");
        } else { std::printf("SKIP: shader classifier corrupt blobs (fixtures absent)\n"); ++skipped; }
    }

    // --- Synthetic: unknown opcode (resinfo, 61) is pinned, no file IO --------
    // Hand-assembled ps_5_0 program (real DXBC container, real retail
    // checksum via dxbc_container::makeContainer): a texture2d declaration,
    // then `resinfo r0.xyzw, l(0), t0.xyzw` (opcode 61, unmodeled) and
    // `mov o0, r0`. Ported from the scratchpad's synthetic-program generator.
    {
        using edvr::dxbc_container::Chunk;
        using edvr::dxbc_container::makeContainer;
        constexpr uint32_t R_DST = 0x1000F2, R_SRC = 0x100E46, O_DST = 0x1020F2, CB_SRC = 0x208E46;
        constexpr uint32_t IMM1 = 0x4001, T_DCL = 0x107000, T_SRC = 0x107E46;
        auto opTok = [](uint32_t o, uint32_t len, uint32_t extra = 0) { return o | extra | (len << 24); };
        std::vector<uint32_t> prog = {0x00000050u, 0}; // ps_5_0, length placeholder
        const std::vector<uint32_t> decls = {
            opTok(89, 4), CB_SRC, 0, 3,              // dcl_constantbuffer cb0[3]
            opTok(88, 4, 3u << 11), T_DCL, 0, 0x5555, // dcl_resource_texture2d t0
            opTok(101, 3), O_DST, 0,                  // dcl_output o0.xyzw
            opTok(104, 2), 1,                          // dcl_temps 1
        };
        prog.insert(prog.end(), decls.begin(), decls.end());
        const std::vector<uint32_t> body = {
            opTok(61, 7), R_DST, 0, IMM1, 0, T_SRC, 0, // resinfo r0.xyzw, l(0), t0.xyzw
            opTok(54, 5), O_DST, 0, R_SRC, 0,           // mov o0, r0
        };
        prog.insert(prog.end(), body.begin(), body.end());
        prog.push_back(opTok(62, 1)); // ret
        prog[1] = static_cast<uint32_t>(prog.size());
        Chunk shex; shex.tag = 0x58454853u; // SHEX
        shex.bytes.resize(prog.size() * 4);
        std::memcpy(shex.bytes.data(), prog.data(), shex.bytes.size());
        const auto blob = makeContainer({shex});
        const auto c = edvr::classifyFlatShaderPair(nullptr, 0, blob.data(), blob.size());
        expect(c.ps == FlatPsProjectionSafety::Consumer, "synthetic resinfo PS refuses");
        expect(c.psReason == FlatClassifierReason::UnknownOpcode, "synthetic resinfo reason is UnknownOpcode");
        expect(c.psUnknownOpcode == 61, "synthetic resinfo pins the opcode number (61)");
    }

    // --- Walker robustness: every fixture walks exactly ------------------------
    {
        static const char* fixtures[] = {
            "vs_0357BBB2DEE43C1F", "vs_0A298DE7DF833A46", "vs_1F17BF54DB6EE407", "vs_361CD4B7FF213A01",
            "vs_525D47E3D5E2EFF4", "vs_66DE2CADB1F4AE6B", "vs_7E38A6AA1269C901", "vs_94D5C556DFD6D705",
            "vs_AACFDCF2FB9AD809", "vs_BA16062A2EB66F1F", "vs_D8FCE3CEA16B9B51", "vs_EB5234DB6ADB491D",
            "vs_F8FA801F2CB1E27C",
            "ps_06332CA168B6DA63", "ps_06AA136E4D58CBA2", "ps_0D617929FED842F0", "ps_222188632125D14B",
            "ps_33758387B70944A1", "ps_4E4FF61E8A08FC7E", "ps_63B1524A9F805A4C", "ps_6FD4C38BA927C8C7",
            "ps_7EAC71963E66C5FE", "ps_81812EF97FB4A361", "ps_8DEF46452FA459F5", "ps_912477AEF6958379",
            "ps_B7D50283329322C3", "ps_BBDE4E71FB78528A", "ps_BE02244365AD810C", "ps_CAD1F585EDDC5641",
            "ps_CDDFE2157F5654B8", "ps_CF534B32F491561A", "ps_DC603C35BBE74B31", "ps_F0BAE053476F8730",
            "ps_FA7411BF7E4C4088",
        };
        for (const char* name : fixtures) {
            std::vector<uint8_t> bytes;
            if (!loadFixture(name, bytes)) { std::printf("SKIP: walker robustness %s (fixtures absent)\n", name); ++skipped; continue; }
            const bool vs = std::strncmp(name, "vs_", 3) == 0;
            const bool clean = edvr::flat_shader_classifier_detail::walksClean(
                bytes.data(), bytes.size(), vs ? edvr::flat_shader_classifier_detail::kVs50
                                               : edvr::flat_shader_classifier_detail::kPs50);
            expect(clean, "walker consumes exactly the declared program length");
        }
    }
    if (skipped) std::printf("shader classifier: %d checks skipped (fixtures absent)\n", skipped);
    return failures;
}

// ---------------------------------------------------------------------------
// --classify-dir <dir>: classify every vs_*.dxbc / ps_*.dxbc in a directory.
// Prints "hash stage verdict slot row psSafety walks" per blob and a verdict
// count summary. Returns process exit code.
// ---------------------------------------------------------------------------
inline int sweep(const char* dir) {
    struct Entry { std::string name; char stage; };
    std::vector<Entry> entries;
    struct __finddata64_t data;
    char pattern[512];
    std::snprintf(pattern, sizeof(pattern), "%s\\*.dxbc", dir);
    intptr_t handle = _findfirst64(pattern, &data);
    if (handle == -1) {
        std::printf("classify-dir: no .dxbc files in %s\n", dir);
        return 2;
    }
    do {
        const char* name = data.name;
        if (std::strncmp(name, "vs_", 3) == 0 || std::strncmp(name, "ps_", 3) == 0)
            entries.push_back({name, name[0]});
    } while (_findnext64(handle, &data) == 0);
    _findclose(handle);

    unsigned counts[8] = {};
    unsigned walksCleanCount = 0, walksFailed = 0;
    for (const auto& e : entries) {
        char path[600];
        std::snprintf(path, sizeof(path), "%s\\%s", dir, e.name.c_str());
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) { std::printf("%s: unreadable\n", e.name.c_str()); continue; }
        const std::streamoff size = file.tellg();
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        edvr::FlatShaderPairClassification c;
        if (e.stage == 'v')
            c = edvr::classifyFlatShaderPair(bytes.data(), bytes.size(), nullptr, 0);
        else
            c = edvr::classifyFlatShaderPair(nullptr, 0, bytes.data(), bytes.size());
        const bool clean = edvr::flat_shader_classifier_detail::walksClean(
            bytes.data(), bytes.size(), e.stage == 'v' ? edvr::flat_shader_classifier_detail::kVs50
                                                       : edvr::flat_shader_classifier_detail::kPs50);
        walksCleanCount += clean ? 1u : 0u;
        walksFailed += clean ? 0u : 1u;
        const char* verdict = e.stage == 'v' ? vsClassName(c.vs) : psSafetyName(c.ps);
        std::printf("%s %c %s %u %u %s walks=%u\n", e.name.c_str(), e.stage, verdict,
                    c.vsSlot, c.vsRow, psSafetyName(c.ps), clean ? 1u : 0u);
        if (e.stage == 'v') {
            switch (c.vs) {
            case edvr::FlatVsProjectionClass::NoBytecode: ++counts[0]; break;
            case edvr::FlatVsProjectionClass::InertNoCB: ++counts[1]; break;
            case edvr::FlatVsProjectionClass::ForwardColumns: ++counts[2]; break;
            case edvr::FlatVsProjectionClass::ForwardDp4: ++counts[3]; break;
            default: ++counts[4]; break;
            }
        } else {
            switch (c.ps) {
            case edvr::FlatPsProjectionSafety::NoBytecode: ++counts[5]; break;
            case edvr::FlatPsProjectionSafety::Clean: ++counts[6]; break;
            default: ++counts[7]; break;
            }
        }
    }
    std::printf("summary: blobs=%zu vs-NoBytecode=%u vs-InertNoCB=%u vs-ForwardColumns=%u vs-ForwardDp4=%u "
                "vs-Unclassified=%u ps-NoBytecode=%u ps-Clean=%u ps-Consumer=%u walks-clean=%u walks-failed=%u\n",
                entries.size(), counts[0], counts[1], counts[2], counts[3], counts[4],
                counts[5], counts[6], counts[7], walksCleanCount, walksFailed);
    return 0;
}

} // namespace flat_shader_classifier_tests

inline int flatShaderClassifierTests() { return flat_shader_classifier_tests::tests(); }
inline int flatShaderClassifierSweep(const char* dir) { return flat_shader_classifier_tests::sweep(dir); }

#pragma once

// Generic flat-mode projection admission: classify actual SM5 shader bytecode
// at runtime and admit provably-safe (VS,PS) pairs that the exact-hash recipe
// table (flat_projection_recipes.h) does not cover -- e.g. mod-patched pixel
// shaders (EDHM) whose vertex shader is a known forward-projection family.
// Everything unproven stays refused: the caller's capture+failPhase path
// remains the fallback for NoBytecode/Unclassified/Consumer verdicts.
//
// The walker is length-safe: it ALWAYS advances by the instruction-length
// field (dxbc_container::instructionLength), so partial operand decoding can
// never desync the stream. Operand layout follows the WDK
// d3d11TokenizedProgramFormat.hpp and was verified token-by-token against
// D3DDisassemble listings of the captured game blobs:
//   instruction token: opcode = token&0x7ff, length = (token>>24)&127,
//     bit31 = extended instruction token(s) follow (each ext token's bit31
//     means "another follows"); opcode 53 (custom data) carries its length
//     in the next dword.
//   operand token: numComponents = token&3 (0=0-comp, 1=1-comp, 2=4-comp,
//     3=N/unmodelable), selection mode = (token>>2)&3 (0=mask, 1=swizzle,
//     2=select-1), mask = (token>>4)&15, swizzle = packed 2-bit components
//     from bit 4, type = (token>>12)&255 (0=temp, 1=input, 2=output,
//     4=imm32, 6=sampler, 7=resource, 8=cb), index dimension = (token>>20)&3,
//     per-dimension index representation = (token>>(22+3d))&7 (0=imm32,
//     2=relative, 3=imm32+relative), bit31 = extended operand token follows
//     (modifier; a second-order extension is unmodelable).
//   imm32 operands span 1 dword (1-component) or 4 dwords (4-component);
//   cb operands carry buffer-index + row dwords; r/v/o/t/s operands carry
//   one dword. Operands using index-relative addressing are unanalyzable.
//
// The forward-column and forward-dp4 patterns are the exact recipes' two
// admitted layouts. flatJitterForwardColumns adds jitter*row[i][3] to each
// row's xy and flatJitterForwardDp4 adds jitter*row[3][i] to rows 0/1, so
// clip.xy moves by jitter*clip.W for any linear combination of the four
// rows -- classification only has to prove the position is exactly such a
// combination of four consecutive rows of one buffer (plus the chain and
// safety rules below), not to know the matrix contents. An additive
// constant anywhere in the position chain breaks that identity (the shift
// becomes jitter*(clip.W - K.w)), so the chain is verified link by link.
//
// Safety rules (a violation means Unclassified/Consumer, never a guess):
//  - VS: a temp/output combining >=2 distinct rows of the same cb buffer via
//    mul/mad/add/dp4 into one value anywhere outside the matrix row family
//    (the deferred inverse-ray, sky-inverse and viewport-remap idioms stay
//    on their exact recipes); a branch writing the position chain; the dp4
//    source vector must trace to inputs through a bounded whitelist of ops
//    with no control flow and no loads (skinned bone-loop srcs refuse).
//  - PS: SV_Depth output; vPos reaching anything but a texture-coordinate
//    operand or the integer domain; a div whose divisor and dividend are
//    both input varyings (clip-varying depth-UV); a temp combining >=2
//    distinct rows of the same cb buffer (possible PS-side matrix);
//    unanalyzable instructions involving cb, vPos or an output write.
//  A "combine" needs >=2 non-literal coefficients: origin + t*direction ray
//  idioms (one literal-1 coefficient) are not matrices. dp3, movc, selects
//  and min/max never count as combines.

#include "dxbc_container.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace edvr {

enum class FlatVsProjectionClass { NoBytecode, InertNoCB, ForwardColumns, ForwardDp4, Unclassified };
enum class FlatPsProjectionSafety { NoBytecode, Clean, Consumer };

// Machine-readable reason for a refusal (VS Unclassified / PS Consumer /
// either stage NoBytecode), set alongside the verdict so the runtime can log
// "refused because X" once per pair. Observability only: no reason value
// feeds a classification decision, and a successful verdict always carries
// None.
enum class FlatClassifierReason : uint8_t {
    None = 0,        // classified; also the default for a stage never blamed
    NoBytecode,      // classifyFlatShaderPair: this stage's bytes were null/empty
    Container,       // parseContainer (or parseSignature) threw: size/checksum/version/chunks
    Walk,            // walkProgram failed: an instruction desynced or cf nesting is unbalanced
    IndexableTemp,   // dcl_indexable_temp is present
    TempCount,       // declared/used temp register count exceeds kMaxTemps
    UnknownOpcode,   // an instruction's operand layout is unknown (see unknownOpcode)
    OperandRange,    // a temp/output register index exceeds its table bound
    DepthOutput,     // PS: SV_Depth (or ge/le variant) output declared
    ParseError,      // VS: an instruction's operands did not parse cleanly
    NoPosition,      // no/ambiguous SV_Position, OSGN vs dcl disagreement, or never written
    InertNotClean,   // VS has no cb but the position chain is not the clean no-cb idiom
    NotForward,      // the position forms match neither the columns nor the dp4 idiom
    ControlFlow,     // the position output's form is tainted by control flow
    ChainBreak,      // the columns idiom matched but the row-combine chain check failed
    SliceNotClean,   // the dp4 idiom matched but the shared source vector's slice is not clean
    SecondMatrix,    // a second, out-of-family row combine exists elsewhere in the VS
    MultiRowTemp,    // PS: a temp combines >=2 distinct rows of one cb buffer
    MultiRowOutput,  // PS: an output combines >=2 distinct rows of one cb buffer
    Unmodelable,     // PS: an unmodelable/parse-error instruction touches cb, vPos or an output
    VposConsumer,    // PS: a vPos-derived value reaches something other than a texture coordinate
};

// Sub-cause of a VposConsumer refusal, filled in when the exit site can say
// cheaply which rule fired. Best-effort: within one instruction, the first
// rule that trips `consumer` wins, so a second rule tripped by the same
// instruction is not recorded.
enum class FlatVposConsumerSubcode : uint8_t {
    None = 0,
    MovcCondition,     // movc/swapc select condition itself vPos-tainted
    DivOrigin,         // div of two Input-origin operands (depth-UV reconstruction idiom)
    AgedDivRcp,        // rcp/div reached by taint already aged past raw vPos
    TextureNonCoord,   // texture-fetch non-coordinate operand vPos-tainted
    OutputWrite,       // a tainted value reaches an output write
    OtherOp,           // any other instruction a float-tainted value reaches
};

inline const char* flatClassifierReasonName(FlatClassifierReason r) {
    switch (r) {
    case FlatClassifierReason::None: return "none";
    case FlatClassifierReason::NoBytecode: return "no-bytecode";
    case FlatClassifierReason::Container: return "container";
    case FlatClassifierReason::Walk: return "walk";
    case FlatClassifierReason::IndexableTemp: return "indexable-temp";
    case FlatClassifierReason::TempCount: return "temp-count";
    case FlatClassifierReason::UnknownOpcode: return "unknown-opcode";
    case FlatClassifierReason::OperandRange: return "operand-range";
    case FlatClassifierReason::DepthOutput: return "depth-output";
    case FlatClassifierReason::ParseError: return "parse-error";
    case FlatClassifierReason::NoPosition: return "no-position";
    case FlatClassifierReason::InertNotClean: return "inert-not-clean";
    case FlatClassifierReason::NotForward: return "not-forward";
    case FlatClassifierReason::ControlFlow: return "control-flow";
    case FlatClassifierReason::ChainBreak: return "chain-break";
    case FlatClassifierReason::SliceNotClean: return "slice-not-clean";
    case FlatClassifierReason::SecondMatrix: return "second-matrix";
    case FlatClassifierReason::MultiRowTemp: return "multi-row-temp";
    case FlatClassifierReason::MultiRowOutput: return "multi-row-output";
    case FlatClassifierReason::Unmodelable: return "unmodelable";
    case FlatClassifierReason::VposConsumer: return "vpos-consumer";
    }
    return "unknown";
}

inline const char* flatVposConsumerSubcodeName(FlatVposConsumerSubcode s) {
    switch (s) {
    case FlatVposConsumerSubcode::None: return "none";
    case FlatVposConsumerSubcode::MovcCondition: return "movc-condition";
    case FlatVposConsumerSubcode::DivOrigin: return "div-origin";
    case FlatVposConsumerSubcode::AgedDivRcp: return "aged-div-rcp";
    case FlatVposConsumerSubcode::TextureNonCoord: return "texture-non-coord";
    case FlatVposConsumerSubcode::OutputWrite: return "output-write";
    case FlatVposConsumerSubcode::OtherOp: return "other-op";
    }
    return "unknown";
}

struct FlatShaderPairClassification {
    FlatVsProjectionClass vs = FlatVsProjectionClass::NoBytecode;
    unsigned vsSlot = 0; unsigned vsRow = 0;      // valid when classified
    bool vsExportsClipXyw = false;
    FlatPsProjectionSafety ps = FlatPsProjectionSafety::NoBytecode;
    FlatClassifierReason vsReason = FlatClassifierReason::None;
    FlatClassifierReason psReason = FlatClassifierReason::None;
    uint16_t vsUnknownOpcode = 0;   // valid when vsReason == UnknownOpcode
    uint16_t psUnknownOpcode = 0;   // valid when psReason == UnknownOpcode
    FlatVposConsumerSubcode psConsumerSubcode = FlatVposConsumerSubcode::None; // valid when psReason == VposConsumer
};

namespace flat_shader_classifier_detail {

using dxbc_container::instructionLength;
using dxbc_container::parseContainer;
using dxbc_container::parseSignature;

constexpr uint32_t kVs50 = 0x00010050u, kPs50 = 0x00000050u;
constexpr uint32_t kTagIsgn = 0x4e475349u, kTagOsgn = 0x4e47534fu;
constexpr uint32_t kTagShex = 0x58454853u, kTagShdr = 0x52444853u;

// Opcodes (d3d11TokenizedProgramFormat.hpp; SM5 numbering as emitted by the
// game's shaders, cross-checked against D3DDisassemble listings).
constexpr uint32_t kOpAdd = 0, kOpAnd = 1, kOpBreakc = 3, kOpDiscard = 13, kOpDiv = 14;
constexpr uint32_t kOpDp2 = 15, kOpDp3 = 16, kOpDp4 = 17, kOpElse = 18, kOpEndif = 21;
constexpr uint32_t kOpEndloop = 22, kOpEndswitch = 23, kOpEq = 24, kOpExp = 25, kOpFrc = 26;
constexpr uint32_t kOpFtoi = 27, kOpFtou = 28, kOpGe = 29, kOpIf = 31, kOpIeq = 32;
constexpr uint32_t kOpIge = 33, kOpIlt = 34, kOpImad = 35, kOpImax = 36, kOpImin = 37;
constexpr uint32_t kOpImul = 38, kOpIne = 39, kOpIneg = 40, kOpIshl = 41, kOpIshr = 42;
constexpr uint32_t kOpItof = 43, kOpLd = 45, kOpLog = 47, kOpLoop = 48, kOpLt = 49;
constexpr uint32_t kOpMad = 50, kOpMin = 51, kOpMax = 52, kOpCustomData = 53, kOpMov = 54;
constexpr uint32_t kOpMovc = 55, kOpMul = 56, kOpNe = 57, kOpNot = 59, kOpOr = 60;
constexpr uint32_t kOpResinfo = 61, kOpRet = 62, kOpRetc = 63, kOpRoundNe = 64;
constexpr uint32_t kOpRoundNi = 65, kOpRoundPi = 66, kOpRoundZ = 67, kOpRsq = 68;
constexpr uint32_t kOpSample = 69, kOpSampleL = 72, kOpSampleD = 73, kOpSampleB = 74;
constexpr uint32_t kOpSqrt = 75, kOpSwitch = 76, kOpSincos = 77, kOpUlt = 79, kOpUge = 80;
constexpr uint32_t kOpUmul = 81, kOpUmad = 82, kOpUmax = 83, kOpUmin = 84, kOpUshr = 85;
constexpr uint32_t kOpUtof = 86, kOpXor = 87;
constexpr uint32_t kOpDclConstantBuffer = 89, kOpDclInputPsSiv = 100, kOpDclOutputSgv = 102;
constexpr uint32_t kOpDclOutputSiv = 103, kOpDclTemps = 104, kOpDclIndexableTemp = 105;
constexpr uint32_t kOpGather4 = 109, kOpDerivRtxCoarse = 122, kOpDerivRtyCoarse = 124;
constexpr uint32_t kOpRcp = 129, kOpF16tof32 = 131, kOpUbfe = 138, kOpIbfe = 139;
constexpr uint32_t kOpBfi = 140, kOpSwapc = 142, kOpLdRaw = 165, kOpLdStructured = 167;

constexpr uint32_t kOperandTemp = 0, kOperandInput = 1, kOperandOutput = 2;
constexpr uint32_t kOperandImm32 = 4, kOperandSampler = 6, kOperandResource = 7;
constexpr uint32_t kOperandCb = 8;
// D3D_NAME (d3dcommon.h): POSITION = 1, DEPTH = 65, DEPTH_GREATER_EQUAL = 67,
// DEPTH_LESS_EQUAL = 68.
constexpr uint32_t kSystemValuePosition = 1, kSystemValueDepth = 65;
constexpr uint32_t kMaxTemps = 4096, kMaxOutputs = 32, kMaxCbRow = 4095;

// Operand count per executable opcode; -1 = unknown (unanalyzable).
inline int operandCount(uint32_t op) {
    switch (op) {
    case 2: case 7: case 9: case 10: case kOpElse: case 19: case 20: case kOpEndif:
    case kOpEndloop: case kOpEndswitch: case kOpLoop: case 58: case kOpRet:
        return 0;
    case kOpBreakc: case 8: case kOpDiscard: case kOpIf: case 44: case kOpRetc: case kOpSwitch:
        return 1;
    case kOpExp: case kOpFrc: case kOpFtoi: case kOpFtou: case kOpIneg: case kOpItof:
    case kOpLog: case kOpNot: case kOpRoundNe: case kOpRoundNi: case kOpRoundPi: case kOpRoundZ:
    case kOpRsq: case kOpSqrt: case kOpUtof: case kOpDerivRtxCoarse: case kOpDerivRtyCoarse:
    case 123: case 125: case kOpRcp: case 130: case kOpF16tof32: case 134: case 135:
    case 136: case 137: case 141: case kOpMov:
        return 2;
    case kOpAdd: case kOpAnd: case kOpDiv: case kOpDp2: case kOpDp3: case kOpDp4: case kOpEq:
    case kOpGe: case 30: case kOpIeq: case kOpIge: case kOpIlt: case kOpImax: case kOpImin:
    case kOpIne: case kOpIshl: case kOpIshr: case kOpLt: case kOpMin: case kOpMax:
    case kOpMul: case kOpNe: case kOpOr: case kOpSincos: case kOpUlt: case kOpUge: case kOpUmax:
    case kOpUmin: case kOpUshr: case kOpXor:
        return 3;
    case kOpImad: case kOpMad: case kOpMovc: case kOpUmad: case kOpUbfe:
    case kOpIbfe: case kOpSwapc: case kOpImul: case kOpUmul: case 78: case 132: case 133:
        return 4;
    case kOpBfi:
        return 5;
    case kOpLd: case kOpLdRaw: case 163: case 164: case 166:
        return 3;
    case 46:
        return 4;
    case kOpSample: case kOpGather4:
        return 4;
    case 70: case 71: case kOpSampleL: case kOpSampleB:
        return 5;
    case kOpSampleD:
        return 6;
    case kOpLdStructured:
        return 4;
    default:
        return -1; // resinfo and anything else unmodelable is caught below
    }
}

struct Operand {
    uint32_t type = 0;
    uint32_t numcomp = 2;       // raw field: 0=0-comp, 1=1-comp, 2=4-comp
    uint32_t mode = 0;          // 0=mask, 1=swizzle, 2=select-1
    uint32_t reg = 0, reg2 = 0; // index dwords (register number / cb buffer+row)
    uint32_t mask = 0xF;
    uint32_t swizzle[4] = {0, 1, 2, 3};
    uint32_t sel1 = 0;
    bool relative = false;      // index-relative addressing present
    bool opaque = false;        // cb read of dynamically-indexed rows: no row term
    bool ok = false;
};

// Parse one operand; `at` advances past it. Any encoding this cannot model
// (N-component, imm64, second-order extended operand, imm64 index forms,
// stream overrun) fails the parse and taints the instruction unanalyzable.
inline bool parseOperand(const std::vector<uint32_t>& t, size_t& at, Operand& op) {
    if (at >= t.size()) return false;
    const uint32_t tok = t[at++];
    op.type = (tok >> 12) & 255u;
    op.numcomp = tok & 3u;
    if (op.numcomp == 3u) return false;
    op.mode = (tok >> 2) & 3u;
    if (op.numcomp == 2u) {
        if (op.mode == 0) {
            op.mask = (tok >> 4) & 15u;
        } else if (op.mode == 1) {
            for (int i = 0; i < 4; ++i) op.swizzle[i] = (tok >> (4 + 2 * i)) & 3u;
        } else if (op.mode == 2) {
            op.sel1 = (tok >> 4) & 3u;
            op.mask = 1u << op.sel1;
            for (int i = 0; i < 4; ++i) op.swizzle[i] = op.sel1;
        } else {
            return false; // select-2: not emitted by the game's compilers
        }
    }
    if (tok & 0x80000000u) {           // extended operand token (modifier)
        if (at >= t.size()) return false;
        if (t[at] & 0x80000000u) return false; // second-order: unmodelable
        ++at;
    }
    const uint32_t dim = (tok >> 20) & 3u;
    if (dim > 2) return false;
    for (uint32_t d = 0; d < dim; ++d) {
        const uint32_t repr = (tok >> (22 + 3 * d)) & 7u;
        if (repr == 0) {
            if (at >= t.size()) return false;
            if (d == 0) op.reg = t[at]; else if (d == 1) op.reg2 = t[at];
            ++at;
        } else if (repr == 2 || repr == 3) {
            op.relative = true;
            if (repr == 3) { if (at >= t.size()) return false; ++at; }
            Operand nested;
            if (!parseOperand(t, at, nested)) return false;
        } else {
            return false; // imm64 index forms: unmodelable
        }
    }
    if (op.type == kOperandImm32) {
        at += op.numcomp == 1 ? 1 : 4;
        if (at > t.size()) return false;
    } else if (op.type == 5) {
        return false; // imm64: unmodelable
    }
    op.ok = true;
    return true;
}

// The component of a source operand that feeds dest component `c`.
inline uint32_t sourceComponent(const Operand& op, uint32_t c) {
    return op.numcomp == 2 ? op.swizzle[c] : 0;
}

// A cb operand read as a vector (>=2 distinct components) references the
// whole matrix row; a scalar read is an opaque value, not a row term. A
// dynamically-indexed read (cb#[reg+n]) is opaque: the row is unknown.
inline bool isCbRowRead(const Operand& op) {
    if (op.type != kOperandCb || op.numcomp != 2 || op.opaque) return false;
    if (op.mode == 2) return false;               // select-1: scalar
    if (op.mode == 0) return op.mask != 1 && op.mask != 2 && op.mask != 4 && op.mask != 8;
    uint32_t distinct = 0;
    for (int i = 0; i < 4; ++i) {
        bool seen = false;
        for (int j = 0; j < i; ++j) seen |= op.swizzle[j] == op.swizzle[i];
        if (!seen) ++distinct;
    }
    return distinct >= 2;
}

// An operand read as a vector (>=2 distinct components), as opposed to a
// scalar select: only vector x vector products are row x row products.
inline bool isVectorOperand(const Operand& op) {
    if (op.numcomp != 2) return false;
    if (op.mode == 2) return false;               // select-1: scalar
    if (op.mode == 0) return op.mask != 1 && op.mask != 2 && op.mask != 4 && op.mask != 8;
    uint32_t distinct = 0;
    for (int i = 0; i < 4; ++i) {
        bool seen = false;
        for (int j = 0; j < i; ++j) seen |= op.swizzle[j] == op.swizzle[i];
        if (!seen) ++distinct;
    }
    return distinct >= 2;
}

inline bool isIdentityVectorRead(const Operand& op) {
    return op.numcomp == 2 && op.mask == 0xF && op.swizzle[0] == 0 && op.swizzle[1] == 1 &&
           op.swizzle[2] == 2 && op.swizzle[3] == 3;
}

struct Instr {
    uint32_t opcode = 0;
    uint32_t length = 0;
    size_t at = 0;
    uint32_t cfDepth = 0;
    bool unmodelable = false;   // index-relative, imm64, N-comp, unknown opcode
    bool parseError = false;
    uint8_t opCount = 0;
    Operand ops[6];
};

struct ProgramFacts {
    uint32_t tempCount = 0;
    uint32_t maxTempReg = 0;        // highest temp register referenced + 1
    bool hasConstantBuffer = false;
    bool hasIndexableTemp = false;
    bool operandOutOfRange = false;
    int32_t vposRegister = -1;      // PS: dcl_input_ps_siv position register
    bool depthOutput = false;       // PS: SV_Depth (or ge/le variants) output
    int32_t posOutputRegister = -1; // VS: dcl_output_siv position register
    bool sawUnknownOpcode = false;
    uint16_t firstUnknownOpcode = 0; // opcode number of the first unknown instruction
};

inline bool isDeclaration(uint32_t op) {
    return (op >= 88 && op <= 106) || op == 143 || (op >= 147 && op <= 163) || op == kOpCustomData;
}

// Form/taint banks cover the declared temps plus any the shader references
// past dcl_temps (the runtime tolerates that; see the walker comment).
inline uint32_t bankTempCount(const ProgramFacts& facts) {
    return facts.tempCount > facts.maxTempReg ? facts.tempCount : facts.maxTempReg;
}

// Walk the whole program. Every instruction is recorded; the walk must end
// exactly at the declared dword count or the shader is structurally unsound.
inline bool walkProgram(const std::vector<uint32_t>& t, std::vector<Instr>& instrs, ProgramFacts& facts) {
    size_t at = 2;
    uint32_t cfDepth = 0;
    while (at < t.size()) {
        Instr in;
        in.at = at;
        const uint32_t tok = t[at];
        in.opcode = tok & 0x7ffu;
        uint32_t length = 0;
        try { length = instructionLength(t, at); } catch (...) { return false; }
        in.length = length;
        size_t opAt = at + 1;
        if (tok & 0x80000000u) {
            while (true) {
                if (opAt >= at + length) return false;
                const uint32_t ext = t[opAt++];
                if (!(ext & 0x80000000u)) break;
            }
        }
        in.cfDepth = cfDepth;
        const uint32_t op = in.opcode;
        if (isDeclaration(op)) {
            if (op == kOpDclConstantBuffer) facts.hasConstantBuffer = true;
            else if (op == kOpDclIndexableTemp) facts.hasIndexableTemp = true;
            else if (op == kOpDclTemps) {
                if (length != 2) return false;
                facts.tempCount = t[opAt];
            } else if (op == kOpDclInputPsSiv && length >= 4) {
                if (t[opAt + 2] == kSystemValuePosition) facts.vposRegister = static_cast<int32_t>(t[opAt + 1]);
            } else if ((op == kOpDclOutputSgv || op == kOpDclOutputSiv) && length >= 4) {
                const uint32_t sv = t[opAt + 2];
                if (sv == kSystemValueDepth || sv == 67 || sv == 68) facts.depthOutput = true;
                if (op == kOpDclOutputSiv && sv == kSystemValuePosition)
                    facts.posOutputRegister = static_cast<int32_t>(t[opAt + 1]);
            }
        } else {
            const int count = operandCount(op);
            if (count < 0) {
                in.unmodelable = true;
                if (!facts.sawUnknownOpcode) facts.firstUnknownOpcode = static_cast<uint16_t>(op);
                facts.sawUnknownOpcode = true;
            } else {
                in.opCount = static_cast<uint8_t>(count);
                bool ok = true;
                for (int i = 0; i < count; ++i) {
                    if (!parseOperand(t, opAt, in.ops[i])) { ok = false; break; }
                    // Index-relative reads of non-cb registers are unmodelable;
                    // a dynamically-indexed cb read is an opaque row of unknown
                    // index (a data lookup, e.g. the glare tables), not proof
                    // of a matrix.
                    if (in.ops[i].relative) {
                        if (in.ops[i].type == kOperandCb) in.ops[i].opaque = true;
                        else in.unmodelable = true;
                    }
                }
                if (!ok || opAt != at + length) in.parseError = true;
                if (ok) {
                    for (uint8_t i = 0; i < in.opCount; ++i) {
                        const Operand& operand = in.ops[i];
                        if (operand.type == kOperandTemp) {
                            // The runtime tolerates temps past dcl_temps (an
                            // EDHM patch artifact); track usage instead of
                            // refusing, but keep a sanity bound.
                            if (operand.reg >= 8192) facts.operandOutOfRange = true;
                            if (operand.reg + 1 > facts.maxTempReg) facts.maxTempReg = operand.reg + 1;
                        }
                        if (operand.type == kOperandOutput && operand.reg >= kMaxOutputs)
                            facts.operandOutOfRange = true;
                    }
                }
            }
            if (op == kOpIf || op == kOpLoop || op == kOpSwitch) ++cfDepth;
            else if (op == kOpEndif || op == kOpEndloop || op == kOpEndswitch) {
                if (cfDepth) --cfDepth; else return false;
            }
        }
        instrs.push_back(in);
        at += length;
    }
    // The runtime tolerates temps past dcl_temps (an EDHM patch artifact);
    // the banks cover actual usage, bounded by the walker's sanity check.
    if (facts.maxTempReg > facts.tempCount) facts.tempCount = facts.maxTempReg;
    return at == t.size();
}

// ---------------------------------------------------------------------------
// Linear forms over cb rows: each temp/output component carries the set of
// cb rows it is a linear combination of, with the scalar coefficient's
// identity (for the forward-pattern match) and an arithmetic-combine flag
// (for the safety rules).
// ---------------------------------------------------------------------------

struct Term {
    uint32_t slot = 0, row = 0;
    uint8_t coefKind = 0;   // 0=One, 1=Scalar(reg,comp), 2=Vector(reg), 3=Opaque
    uint8_t coefType = 0;   // 0=temp, 1=input (Scalar/Vector kinds)
    uint16_t coefReg = 0;
    uint8_t coefComp = 0;
    bool identity = false;  // Vector kind: source was an identity 4-comp read
};

struct Form {
    Term terms[6];
    uint8_t termCount = 0;
    bool overflowed = false;
    bool arithMultiRow = false; // >=2 distinct rows of one slot merged arithmetically
    bool cfTainted = false;     // written under control flow (or inherits it)
};

inline void formAddTerm(Form& dst, const Term& term) {
    for (uint8_t i = 0; i < dst.termCount; ++i) {
        const Term& have = dst.terms[i];
        if (have.slot == term.slot && have.row == term.row && have.coefKind == term.coefKind &&
            have.coefReg == term.coefReg && have.coefComp == term.coefComp)
            return;
    }
    if (dst.termCount < 6) dst.terms[dst.termCount++] = term;
    else dst.overflowed = true;
}

inline void formUnion(Form& dst, const Form& src) {
    for (uint8_t i = 0; i < src.termCount; ++i) formAddTerm(dst, src.terms[i]);
    dst.arithMultiRow |= src.arithMultiRow;
    dst.cfTainted |= src.cfTainted;
}

// A combine that makes a value depend on >=3 distinct rows of one cb buffer
// through mul/mad/add/dp4 is a possible matrix (a PS-side projection, an
// inverse, a ray basis). Two-row blends (lerps, origin + t*direction rays)
// are not matrices and never fire; movc/select/min/max and dp3 are not
// combines at all.
inline void noteArithmeticMerge(Form& f) {
    for (uint8_t i = 0; i < f.termCount; ++i) {
        uint32_t distinctRows = 1;  // the row of term i itself
        for (uint8_t j = 0; j < f.termCount; ++j) {
            if (j == i || f.terms[j].slot != f.terms[i].slot) continue;
            bool seen = false;
            for (uint8_t k = 0; k < j; ++k)
                if (f.terms[k].slot == f.terms[i].slot && f.terms[k].row == f.terms[j].row) {
                    seen = true; break;
                }
            if (!seen) ++distinctRows;
        }
        if (distinctRows >= 3) {
            f.arithMultiRow = true;
            return;
        }
    }
    if (f.overflowed && f.termCount >= 3) f.arithMultiRow = true;
}

inline Term makeRowTerm(const Operand& cb) {
    Term t;
    t.slot = cb.reg; t.row = cb.reg2;
    return t;
}

inline Term makeScaledRowTerm(const Operand& cb, const Operand& scalar, uint32_t c) {
    Term t;
    t.slot = cb.reg; t.row = cb.reg2;
    if (scalar.type == kOperandTemp || scalar.type == kOperandInput) {
        t.coefKind = 1; t.coefType = scalar.type == kOperandInput ? 1 : 0;
        t.coefReg = static_cast<uint16_t>(scalar.reg);
        t.coefComp = static_cast<uint8_t>(sourceComponent(scalar, c));
    } else {
        t.coefKind = 3; // immediates and cb scalars: opaque, never pattern-match
    }
    return t;
}

inline Term makeVectorRowTerm(const Operand& cb, const Operand& src) {
    Term t;
    t.slot = cb.reg; t.row = cb.reg2;
    t.coefKind = 2; t.coefType = src.type == kOperandInput ? 1 : 0;
    t.coefReg = static_cast<uint16_t>(src.reg);
    t.identity = isIdentityVectorRead(src);
    return t;
}

// A coefficient that is itself a combination of >=2 rows of one slot makes
// the product/dot a row x row product, never a forward idiom.
inline bool coefficientIsRowCombination(const Form& carried, uint32_t slot) {
    uint32_t rows = 0;
    for (uint8_t i = 0; i < carried.termCount; ++i)
        if (carried.terms[i].slot == slot) ++rows;
    return rows >= 2;
}

// One register file of forms: temps or outputs, 4 components each.
struct FormBank {
    std::vector<Form> forms; // [reg*4 + component]
    explicit FormBank(uint32_t count) : forms(size_t(count) * 4) {}
    Form& at(uint32_t reg, uint32_t comp) { return forms[size_t(reg) * 4 + comp]; }
    const Form& at(uint32_t reg, uint32_t comp) const { return forms[size_t(reg) * 4 + comp]; }
};

// The source form of operand `opIdx` at dest component `c`; cb row reads
// contribute one literal-coefficient term, everything else is empty.
inline Form sourceForm(const Instr& in, uint8_t opIdx, uint32_t c, const FormBank& temps,
                       const FormBank& outputs, uint32_t tempCount) {
    Form empty;
    if (opIdx >= in.opCount) return empty;
    const Operand& op = in.ops[opIdx];
    if (op.type == kOperandTemp) {
        if (op.reg >= tempCount) return empty;
        return temps.at(op.reg, sourceComponent(op, c));
    }
    if (op.type == kOperandOutput) {
        if (op.reg >= kMaxOutputs) return empty;
        return outputs.at(op.reg, sourceComponent(op, c));
    }
    if (isCbRowRead(op)) {
        Form f;
        formAddTerm(f, makeRowTerm(op));
        return f;
    }
    return empty;
}

inline bool isVectorish(const Operand& op) {
    return op.type == kOperandTemp || op.type == kOperandInput;
}

inline bool isTwoDestOpcode(uint32_t op) {
    return op == kOpSincos || op == kOpImul || op == kOpUmul || op == 78 || op == 132 || op == 133;
}

// Forward dataflow over the instruction list, filling `temps` and `outputs`.
inline void buildForms(const std::vector<Instr>& instrs, const ProgramFacts& facts,
                       FormBank& temps, FormBank& outputs) {
    for (const Instr& in : instrs) {
        if (isDeclaration(in.opcode) || !in.opCount) continue;
        if (in.parseError) continue; // the whole shader is refused elsewhere
        if (in.unmodelable) {
            // The dest value is unknown: poison it so a stale form cannot
            // pattern-match later.
            if (in.ops[0].ok) {
                const Operand& dest = in.ops[0];
                if (dest.type == kOperandTemp && dest.reg < facts.tempCount) {
                    for (uint32_t c = 0; c < 4; ++c)
                        if (dest.mask & (1u << c)) temps.at(dest.reg, c) = Form{};
                } else if (dest.type == kOperandOutput && dest.reg < kMaxOutputs) {
                    for (uint32_t c = 0; c < 4; ++c)
                        if (dest.mask & (1u << c)) outputs.at(dest.reg, c) = Form{};
                }
            }
            continue;
        }
        const Operand& dest = in.ops[0];
        const bool destIsTemp = dest.type == kOperandTemp && dest.reg < facts.tempCount;
        const bool destIsOutput = dest.type == kOperandOutput && dest.reg < kMaxOutputs;
        if (isTwoDestOpcode(in.opcode) && in.ops[1].ok) {
            const Operand& dest2 = in.ops[1];
            if (dest2.type == kOperandTemp && dest2.reg < facts.tempCount) {
                for (uint32_t c = 0; c < 4; ++c)
                    if (dest2.mask & (1u << c)) temps.at(dest2.reg, c) = Form{};
            } else if (dest2.type == kOperandOutput && dest2.reg < kMaxOutputs) {
                for (uint32_t c = 0; c < 4; ++c)
                    if (dest2.mask & (1u << c)) outputs.at(dest2.reg, c) = Form{};
            }
        }
        if (!destIsTemp && !destIsOutput) continue;
        FormBank& bank = destIsTemp ? temps : outputs;
        const uint32_t destReg = dest.reg;
        for (uint32_t c = 0; c < 4; ++c) {
            if (!(dest.mask & (1u << c))) continue;
            Form out{};
            out.cfTainted = in.cfDepth != 0;
            switch (in.opcode) {
            case kOpMov:
                out = sourceForm(in, 1, c, temps, outputs, facts.tempCount);
                out.cfTainted |= in.cfDepth != 0;
                break;
            case kOpMovc: case kOpSwapc: case kOpMin: case kOpMax: {
                Form a = sourceForm(in, 2, c, temps, outputs, facts.tempCount);
                Form b = sourceForm(in, 3, c, temps, outputs, facts.tempCount);
                formUnion(out, a); formUnion(out, b);
                break;
            }
            case kOpAdd: {
                Form a = sourceForm(in, 1, c, temps, outputs, facts.tempCount);
                Form b = sourceForm(in, 2, c, temps, outputs, facts.tempCount);
                formUnion(out, a); formUnion(out, b);
                noteArithmeticMerge(out);
                break;
            }
            case kOpMul: {
                const Operand& opA = in.ops[1];
                const Operand& opB = in.ops[2];
                const bool rowA = isCbRowRead(opA), rowB = isCbRowRead(opB);
                if (rowA && rowB) {
                    formAddTerm(out, makeRowTerm(opA));
                    formAddTerm(out, makeRowTerm(opB));
                    out.arithMultiRow = true; // element-wise cb x cb is never a matrix idiom
                } else if (rowA || rowB) {
                    const Operand& row = rowA ? opA : opB;
                    const Operand& scalar = rowA ? opB : opA;
                    formAddTerm(out, makeScaledRowTerm(row, scalar, c));
                    // The coefficient is an opaque scalar; only a coefficient
                    // that is itself a same-slot row combination makes the
                    // product a row x row product, never a forward idiom.
                    Form carried = sourceForm(in, rowA ? 2 : 1, c, temps, outputs, facts.tempCount);
                    if (coefficientIsRowCombination(carried, row.reg)) out.arithMultiRow = true;
                } else {
                    Form a = sourceForm(in, 1, c, temps, outputs, facts.tempCount);
                    Form b = sourceForm(in, 2, c, temps, outputs, facts.tempCount);
                    // Only a component-wise product of two vectors is a row x
                    // row product; a scalar times a vector stays linear.
                    if (a.termCount && b.termCount && isVectorOperand(opA) && isVectorOperand(opB))
                        out.arithMultiRow = true;
                    formUnion(out, a); formUnion(out, b);
                }
                noteArithmeticMerge(out);
                break;
            }
            case kOpMad: {
                const Operand& opA = in.ops[1];
                const Operand& opB = in.ops[2];
                const Operand& opC = in.ops[3];
                const bool rowA = isCbRowRead(opA), rowB = isCbRowRead(opB), rowC = isCbRowRead(opC);
                if (rowA && rowB) {
                    // Both multiplicands are rows: element-wise product.
                    formAddTerm(out, makeRowTerm(opA));
                    formAddTerm(out, makeRowTerm(opB));
                    out.arithMultiRow = true;
                    Form acc = sourceForm(in, 3, c, temps, outputs, facts.tempCount);
                    formUnion(out, acc);
                } else if ((rowA || rowB) && rowC) {
                    // scalar*row + row: the origin + t*direction ray idiom,
                    // not a matrix combine.
                    const Operand& row = rowA ? opA : opB;
                    const Operand& scalar = rowA ? opB : opA;
                    formAddTerm(out, makeScaledRowTerm(row, scalar, c));
                    formAddTerm(out, makeRowTerm(opC));
                } else if (rowA || rowB) {
                    const Operand& row = rowA ? opA : opB;
                    const Operand& scalar = rowA ? opB : opA;
                    formAddTerm(out, makeScaledRowTerm(row, scalar, c));
                    Form acc = sourceForm(in, 3, c, temps, outputs, facts.tempCount);
                    formUnion(out, acc);
                } else if (rowC) {
                    Form a = sourceForm(in, 1, c, temps, outputs, facts.tempCount);
                    Form b = sourceForm(in, 2, c, temps, outputs, facts.tempCount);
                    if (a.termCount && b.termCount && isVectorOperand(opA) && isVectorOperand(opB))
                        out.arithMultiRow = true;
                    formUnion(out, a); formUnion(out, b);
                    formAddTerm(out, makeRowTerm(opC));
                } else {
                    Form a = sourceForm(in, 1, c, temps, outputs, facts.tempCount);
                    Form b = sourceForm(in, 2, c, temps, outputs, facts.tempCount);
                    if (a.termCount && b.termCount && isVectorOperand(opA) && isVectorOperand(opB))
                        out.arithMultiRow = true;
                    formUnion(out, a); formUnion(out, b);
                    Form acc = sourceForm(in, 3, c, temps, outputs, facts.tempCount);
                    formUnion(out, acc);
                }
                noteArithmeticMerge(out);
                break;
            }
            case kOpDp4: {
                const Operand& opA = in.ops[1];
                const Operand& opB = in.ops[2];
                const bool rowA = isCbRowRead(opA), rowB = isCbRowRead(opB);
                if (rowA != rowB && isVectorish(rowA ? opB : opA)) {
                    const Operand& row = rowA ? opA : opB;
                    formAddTerm(out, makeVectorRowTerm(row, rowA ? opB : opA));
                    // The dot source is an opaque vector; only a source that
                    // is itself a same-slot row combination makes the dot a
                    // row x row product, never a forward idiom.
                    Form carried = sourceForm(in, rowA ? 2 : 1, c, temps, outputs, facts.tempCount);
                    if (coefficientIsRowCombination(carried, row.reg)) { out.arithMultiRow = true;
                    }
                } else {
                    Form a = sourceForm(in, 1, c, temps, outputs, facts.tempCount);
                    Form b = sourceForm(in, 2, c, temps, outputs, facts.tempCount);
                    if (a.termCount && b.termCount) out.arithMultiRow = true;
                    formUnion(out, a); formUnion(out, b);
                }
                noteArithmeticMerge(out);
                break;
            }
            case kOpIneg:
                out = sourceForm(in, 1, c, temps, outputs, facts.tempCount);
                out.cfTainted |= in.cfDepth != 0;
                break;
            default:
                // Every other opcode consumes or clears row terms; the
                // safety scan only fires on mul/mad/add/dp4 combines.
                break;
            }
            bank.at(destReg, c) = out;
        }
    }
}

// ---------------------------------------------------------------------------
// VS classification
// ---------------------------------------------------------------------------

inline bool opcodeInSliceWhitelist(uint32_t op) {
    switch (op) {
    case kOpMov: case kOpMul: case kOpMad: case kOpAdd: case kOpDp4: case kOpMin:
    case kOpMax: case kOpMovc: case kOpFrc: case kOpExp: case kOpLog: case kOpRsq:
    case kOpSqrt: case kOpRcp: case kOpDiv: case kOpFtoi: case kOpFtou: case kOpItof:
    case kOpUtof: case kOpRoundNe: case kOpRoundNi: case kOpRoundPi: case kOpRoundZ:
    case kOpAnd: case kOpOr: case kOpXor: case kOpIshl: case kOpIshr: case kOpUshr:
        return true;
    default:
        return false;
    }
}

// Depth-bounded backward slice through temp definitions: every instruction
// writing `reg` before `readAt` must be in the whitelist, control-flow free,
// modelable, and recurse into its temp sources.
inline bool sliceClean(uint32_t reg, size_t readAt, const std::vector<Instr>& instrs,
                       const std::vector<std::vector<uint32_t>>& defs, uint32_t depth) {
    if (depth > 8) return false;
    for (uint32_t idx : defs[reg]) {
        if (idx >= readAt) continue;
        const Instr& in = instrs[idx];
        if (in.cfDepth || in.parseError || in.unmodelable) return false;
        if (!opcodeInSliceWhitelist(in.opcode)) return false;
        for (uint8_t i = 1; i < in.opCount; ++i) {
            const Operand& op = in.ops[i];
            if (op.type == kOperandTemp && !sliceClean(op.reg, idx, instrs, defs, depth + 1))
                return false;
        }
    }
    return true;
}

// Backward chain check for the columns idiom. Every link must be a mov, an
// accumulator-scaling mul, a mad whose accumulator operand already carries
// rows, or an add merging two row-carrying values or a cb row. An additive
// constant or input anywhere in the chain breaks the jitter identity.
enum ChainResult { chainBad, chainFamily };

inline ChainResult columnsChainInstr(const Instr& in, size_t idx, const std::vector<Instr>& instrs,
                                     const std::vector<std::vector<uint32_t>>& defs, uint32_t depth);

inline ChainResult columnsChain(uint32_t reg, size_t readAt, const std::vector<Instr>& instrs,
                                const std::vector<std::vector<uint32_t>>& defs, uint32_t depth) {
    if (depth > 16) return chainBad;
    int found = -1;
    for (uint32_t idx : defs[reg])
        if (idx < readAt) found = static_cast<int>(idx);
    if (found < 0) return chainBad;
    return columnsChainInstr(instrs[found], static_cast<size_t>(found), instrs, defs, depth);
}

inline ChainResult columnsChainInstr(const Instr& in, size_t idx, const std::vector<Instr>& instrs,
                                     const std::vector<std::vector<uint32_t>>& defs, uint32_t depth) {
    if (in.cfDepth || in.parseError || in.unmodelable) return chainBad;
    auto recurse = [&](const Operand& op) -> ChainResult {
        if (op.type == kOperandTemp) return columnsChain(op.reg, idx, instrs, defs, depth + 1);
        if (isCbRowRead(op)) return chainFamily;
        return chainBad; // additive input/immediate/constant
    };
    switch (in.opcode) {
    case kOpMov: case kOpIneg:
        return recurse(in.ops[1]);
    case kOpMul: {
        const bool rowA = isCbRowRead(in.ops[1]), rowB = isCbRowRead(in.ops[2]);
        if (rowA || rowB) return chainFamily; // the other operand is the opaque coefficient
        ChainResult a = recurse(in.ops[1]), b = recurse(in.ops[2]);
        if (a == chainFamily && b == chainFamily) return chainBad; // product of two carriers
        return a == chainFamily ? chainFamily : b;
    }
    case kOpMad: {
        const bool rowA = isCbRowRead(in.ops[1]), rowB = isCbRowRead(in.ops[2]);
        if (rowA && rowB) return chainBad;
        if (rowA || rowB) {
            // The cb row is scaled by the opaque coefficient; the
            // accumulator must already carry rows.
            return recurse(in.ops[3]);
        }
        ChainResult a = recurse(in.ops[1]), b = recurse(in.ops[2]);
        if (a == chainFamily && b == chainFamily) return chainBad;
        return recurse(in.ops[3]) == chainFamily ? chainFamily : chainBad;
    }
    case kOpAdd: {
        ChainResult a = recurse(in.ops[1]), b = recurse(in.ops[2]);
        if (a == chainBad || b == chainBad) return chainBad;
        return chainFamily;
    }
    default:
        return chainBad;
    }
}

// The position component forms must be exactly the four-row pattern: rows
// R..R+3 of one slot, three rows scaled by distinct components of one
// source register, the fourth with coefficient 1 (or a source component).
inline bool matchForwardColumns(const Form& f, uint32_t& slot, uint32_t& row) {
    if (f.termCount != 4 || f.overflowed) return false;
    uint32_t minRow = 0xFFFFFFFFu;
    for (uint8_t i = 0; i < 4; ++i) minRow = minRow < f.terms[i].row ? minRow : f.terms[i].row;
    if (minRow > kMaxCbRow) return false;
    uint32_t coefReg = 0; uint8_t coefType = 0; bool haveCoef = false;
    uint32_t compUsed = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        const Term& t = f.terms[i];
        if (t.slot != f.terms[0].slot) return false;
        if (t.row < minRow || t.row > minRow + 3) return false;
        const uint32_t k = t.row - minRow;
        if (k < 3) {
            if (t.coefKind != 1) return false;
            if (!haveCoef) { coefReg = t.coefReg; coefType = t.coefType; haveCoef = true; }
            else if (t.coefReg != coefReg || t.coefType != coefType) return false;
            if (compUsed & (1u << t.coefComp)) return false; // distinct components
            compUsed |= 1u << t.coefComp;
        } else {
            if (t.coefKind == 0) continue;
            if (t.coefKind == 1 && haveCoef && t.coefReg == coefReg && t.coefType == coefType) continue;
            return false;
        }
    }
    slot = f.terms[0].slot; row = minRow;
    return true;
}

// Each position component is one dp4 against the same identity-read source
// vector, rows R+component.
inline bool matchForwardDp4Component(const Form& f, uint32_t comp, uint32_t& slot, uint32_t& row,
                                     uint16_t& srcReg, uint8_t& srcType) {
    if (f.termCount != 1 || f.overflowed) return false;
    const Term& t = f.terms[0];
    if (t.coefKind != 2 || !t.identity || t.row < comp) return false;
    slot = t.slot; row = t.row - comp;
    if (row > kMaxCbRow) return false;
    srcReg = t.coefReg; srcType = t.coefType;
    return true;
}

// Priority among the "position forms matched neither idiom" family of
// refusal reasons: the higher-ranked reason is the more informative one
// (we got further before failing), so the columns and dp4 attempts both
// report through this and the best of the two survives.
inline int forwardReasonRank(FlatClassifierReason r) {
    switch (r) {
    case FlatClassifierReason::ControlFlow: return 1;
    case FlatClassifierReason::ChainBreak:
    case FlatClassifierReason::SliceNotClean: return 2;
    case FlatClassifierReason::SecondMatrix: return 3;
    default: return 0; // NotForward (the default) and anything else
    }
}

struct VsAnalysis {
    FlatVsProjectionClass cls = FlatVsProjectionClass::Unclassified;
    uint32_t slot = 0, row = 0;
    bool exportsClipXyw = false;
    FlatClassifierReason reason = FlatClassifierReason::None;
    uint16_t unknownOpcode = 0; // valid when reason == UnknownOpcode
};

inline VsAnalysis analyzeVs(const std::vector<uint32_t>& t,
                            const std::vector<dxbc_container::SignatureElement>& osgn) {
    VsAnalysis result;
    std::vector<Instr> instrs;
    ProgramFacts facts;
    if (!walkProgram(t, instrs, facts)) { result.reason = FlatClassifierReason::Walk; return result; }
    if (facts.hasIndexableTemp) { result.reason = FlatClassifierReason::IndexableTemp; return result; }
    if (facts.tempCount > kMaxTemps) { result.reason = FlatClassifierReason::TempCount; return result; }
    if (facts.sawUnknownOpcode) {
        result.reason = FlatClassifierReason::UnknownOpcode;
        result.unknownOpcode = facts.firstUnknownOpcode;
        return result;
    }
    if (facts.operandOutOfRange) { result.reason = FlatClassifierReason::OperandRange; return result; }
    for (const Instr& in : instrs)
        if (in.parseError) { result.reason = FlatClassifierReason::ParseError; return result; }
    // The SV_Position output register: OSGN system value or name, with the
    // dcl_output_siv declaration as fallback; disagreement is ambiguous.
    int32_t posReg = -1;
    uint32_t posMask = 0;
    for (const auto& e : osgn) {
        if (e.systemValue == kSystemValuePosition || dxbc_container::equalName(e.name, "SV_Position")) {
            if (posReg >= 0) { result.reason = FlatClassifierReason::NoPosition; return result; }
            posReg = static_cast<int32_t>(e.registerIndex);
            posMask = e.masks & 15u;
        }
    }
    if (posReg >= 0 && facts.posOutputRegister >= 0 && facts.posOutputRegister != posReg) {
        result.reason = FlatClassifierReason::NoPosition;
        return result;
    }
    if (posReg < 0) { posReg = facts.posOutputRegister; posMask = 0xF; }
    if (posReg < 0 || posReg >= static_cast<int32_t>(kMaxOutputs)) {
        result.reason = FlatClassifierReason::NoPosition;
        return result;
    }
    if (!posMask) posMask = 0xF;

    // def lists for the chain and slice checks
    std::vector<std::vector<uint32_t>> defs(facts.tempCount ? facts.tempCount : 1);
    for (size_t i = 0; i < instrs.size(); ++i) {
        const Instr& in = instrs[i];
        if (isDeclaration(in.opcode) || in.parseError || !in.opCount) continue;
        const Operand& dest = in.ops[0];
        if (dest.type == kOperandTemp && dest.reg < defs.size())
            defs[dest.reg].push_back(static_cast<uint32_t>(i));
    }

    FormBank temps(facts.tempCount ? facts.tempCount : 1);
    FormBank outputs(kMaxOutputs);
    buildForms(instrs, facts, temps, outputs);

    // The position output must be written.
    bool written = false;
    for (const Instr& in : instrs) {
        if (isDeclaration(in.opcode) || in.parseError || !in.opCount) continue;
        const Operand& dest = in.ops[0];
        if (dest.type == kOperandOutput && dest.reg == static_cast<uint32_t>(posReg)) written = true;
    }
    if (!written) { result.reason = FlatClassifierReason::NoPosition; return result; }

    // InertNoCB: no constant buffer anywhere and the position chain to the
    // inputs is control-flow free and modelable.
    if (!facts.hasConstantBuffer) {
        bool clean = true;
        for (size_t i = 0; i < instrs.size() && clean; ++i) {
            const Instr& in = instrs[i];
            if (isDeclaration(in.opcode) || in.parseError || !in.opCount) continue;
            const Operand& dest = in.ops[0];
            if (dest.type != kOperandOutput || dest.reg != static_cast<uint32_t>(posReg)) continue;
            if (in.cfDepth || in.unmodelable || !opcodeInSliceWhitelist(in.opcode)) { clean = false; break; }
            for (uint8_t o = 1; o < in.opCount; ++o) {
                const Operand& op = in.ops[o];
                if (op.type == kOperandTemp && !sliceClean(op.reg, i, instrs, defs, 0)) { clean = false; break; }
                if (op.type == kOperandCb) { clean = false; break; }
            }
        }
        if (clean) result.cls = FlatVsProjectionClass::InertNoCB;
        else result.reason = FlatClassifierReason::InertNotClean;
        return result;
    }

    // Forward patterns on the final per-component position forms. Only the
    // x, y, w components carry the jitter identity (clip.xy += jitter*clip.W);
    // z is free (the patch never touches it), so z is not evaluated and a
    // z-only bias (e.g. 361C's depth bias) does not refuse the pair.
    uint32_t slot = 0, row = 0;
    bool columns = (posMask & 0xBu) == 0xBu;  // x, y, w all present
    bool firstComponent = true;
    bool columnsCfTainted = false;
    for (uint32_t c = 0; c < 4 && columns; ++c) {
        if (c == 2 || !(posMask & (1u << c))) continue;
        uint32_t s = 0, r = 0;
        if (!matchForwardColumns(outputs.at(posReg, c), s, r)) { columns = false; break; }
        if (outputs.at(posReg, c).cfTainted) { columns = false; columnsCfTainted = true; break; }
        if (firstComponent) { slot = s; row = r; firstComponent = false; }
        else if (s != slot || r != row) { columns = false; break; }
    }
    // Across both idiom attempts below, keep the most informative refusal
    // reason (see forwardReasonRank) and report it only if neither succeeds.
    FlatClassifierReason bestForwardReason = FlatClassifierReason::NotForward;
    auto noteForwardReason = [&](FlatClassifierReason reason) {
        if (forwardReasonRank(reason) > forwardReasonRank(bestForwardReason)) bestForwardReason = reason;
    };
    if (columnsCfTainted) noteForwardReason(FlatClassifierReason::ControlFlow);
    if (columns) {
        // The chain must be exactly the row-combine idiom: no additive
        // constant or input anywhere between the rows and the position.
        bool chainOk = true;
        for (size_t i = 0; i < instrs.size() && chainOk; ++i) {
            const Instr& in = instrs[i];
            if (isDeclaration(in.opcode) || in.parseError || !in.opCount) continue;
            const Operand& dest = in.ops[0];
            if (dest.type != kOperandOutput || dest.reg != static_cast<uint32_t>(posReg)) continue;
            if (!(dest.mask & 0xBu)) continue;  // z-only writes are free
            if (columnsChainInstr(in, i, instrs, defs, 0) != chainFamily) chainOk = false;
        }
        // Safety rule: no arithmetic multi-row combine outside the matrix
        // row family anywhere else in the shader.
        bool violation = false;
        for (size_t reg = 0; reg < temps.forms.size() / 4 && !violation; ++reg) {
            for (uint32_t c = 0; c < 4 && !violation; ++c) {
                const Form& f = temps.at(static_cast<uint32_t>(reg), c);
                if (!f.arithMultiRow) continue;
                for (uint8_t i = 0; i < f.termCount; ++i)
                    if (f.terms[i].slot != slot || f.terms[i].row < row || f.terms[i].row > row + 3) {
                        violation = true; break;
                    }
                if (f.overflowed) violation = true;
            }
        }
        for (uint32_t reg = 0; reg < kMaxOutputs && !violation; ++reg) {
            if (reg == static_cast<uint32_t>(posReg)) continue;
            for (uint32_t c = 0; c < 4 && !violation; ++c) {
                const Form& f = outputs.at(reg, c);
                if (!f.arithMultiRow) continue;
                for (uint8_t i = 0; i < f.termCount; ++i)
                    if (f.terms[i].slot != slot || f.terms[i].row < row || f.terms[i].row > row + 3) {
                        violation = true; break;
                    }
                if (f.overflowed) violation = true;
            }
        }
        if (chainOk && !violation) {
            result.cls = FlatVsProjectionClass::ForwardColumns; result.slot = slot; result.row = row;
        } else {
            if (!chainOk) noteForwardReason(FlatClassifierReason::ChainBreak);
            if (violation) noteForwardReason(FlatClassifierReason::SecondMatrix);
        }
    }
    if (result.cls == FlatVsProjectionClass::Unclassified) {
        bool dp4 = (posMask & 0xBu) == 0xBu;  // x, y, w all present
        uint32_t s = 0, r = 0;
        uint16_t src = 0; uint8_t srcType = 0;
        bool first = true;
        bool dp4CfTainted = false;
        for (uint32_t c = 0; c < 4 && dp4; ++c) {
            if (c == 2 || !(posMask & (1u << c))) continue;
            uint32_t cs = 0, cr = 0; uint16_t cg = 0; uint8_t ct = 0;
            if (!matchForwardDp4Component(outputs.at(posReg, c), c, cs, cr, cg, ct)) { dp4 = false; break; }
            if (outputs.at(posReg, c).cfTainted) { dp4 = false; dp4CfTainted = true; break; }
            if (first) { s = cs; r = cr; src = cg; srcType = ct; first = false; }
            else if (cs != s || cr != r || cg != src || ct != srcType) { dp4 = false; break; }
        }
        if (dp4CfTainted) noteForwardReason(FlatClassifierReason::ControlFlow);
        if (dp4 && !first) {
            // Each position write is a dp4 (or a mov of one) whose shared
            // source vector traces to inputs through the bounded whitelist.
            bool clean = true;
            for (size_t i = 0; i < instrs.size() && clean; ++i) {
                const Instr& in = instrs[i];
                if (isDeclaration(in.opcode) || in.parseError || !in.opCount) continue;
                const Operand& dest = in.ops[0];
                if (dest.type != kOperandOutput || dest.reg != static_cast<uint32_t>(posReg)) continue;
                if (in.opcode == kOpDp4) {
                    for (uint8_t o = 1; o < in.opCount && clean; ++o) {
                        const Operand& op = in.ops[o];
                        if (op.type == kOperandTemp && op.reg == src &&
                            !sliceClean(op.reg, i, instrs, defs, 0)) clean = false;
                    }
                } else if (in.opcode == kOpMov) {
                    const Operand& op = in.ops[1];
                    if (op.type != kOperandTemp) { clean = false; break; }
                    // The mov source must itself be a dp4 result (or a mov chain).
                    int found = -1;
                    for (uint32_t idx : defs[op.reg])
                        if (idx < i) found = static_cast<int>(idx);
                    if (found < 0) { clean = false; break; }
                    const Instr& def = instrs[found];
                    if (def.opcode == kOpDp4) {
                        for (uint8_t o = 1; o < def.opCount && clean; ++o) {
                            const Operand& sop = def.ops[o];
                            if (sop.type == kOperandTemp && sop.reg == src &&
                                !sliceClean(sop.reg, static_cast<size_t>(found), instrs, defs, 0)) clean = false;
                        }
                    } else if (def.opcode != kOpMov || def.ops[1].type != kOperandTemp) {
                        clean = false;
                    } else {
                        clean = false; // deeper mov chains are not the emitted idiom
                    }
                } else {
                    clean = false;
                }
            }
            if (clean) {
                bool violation = false;
                for (size_t reg = 0; reg < temps.forms.size() / 4 && !violation; ++reg)
                    for (uint32_t c = 0; c < 4 && !violation; ++c) {
                        const Form& f = temps.at(static_cast<uint32_t>(reg), c);
                        if (!f.arithMultiRow) continue;
                        for (uint8_t i = 0; i < f.termCount; ++i)
                            if (f.terms[i].slot != s || f.terms[i].row < r || f.terms[i].row > r + 3) {
                                violation = true; break;
                            }
                        if (f.overflowed) violation = true;
                    }
                for (uint32_t reg = 0; reg < kMaxOutputs && !violation; ++reg) {
                    if (reg == static_cast<uint32_t>(posReg)) continue;
                    for (uint32_t c = 0; c < 4 && !violation; ++c) {
                        const Form& f = outputs.at(reg, c);
                        if (!f.arithMultiRow) continue;
                        for (uint8_t i = 0; i < f.termCount; ++i)
                            if (f.terms[i].slot != s || f.terms[i].row < r || f.terms[i].row > r + 3) {
                                violation = true; break;
                            }
                        if (f.overflowed) violation = true;
                    }
                }
                if (!violation) {
                    result.cls = FlatVsProjectionClass::ForwardDp4; result.slot = s; result.row = r;
                } else {
                    noteForwardReason(FlatClassifierReason::SecondMatrix);
                }
            } else {
                noteForwardReason(FlatClassifierReason::SliceNotClean);
            }
        }
    }
    if (result.cls == FlatVsProjectionClass::Unclassified) {
        result.reason = bestForwardReason;
        return result;
    }

    // Another output whose form stays inside the same matrix family exports
    // the clip xyw varying (the decal idiom).
    for (uint32_t reg = 0; reg < kMaxOutputs; ++reg) {
        if (reg == static_cast<uint32_t>(posReg)) continue;
        for (uint32_t c = 0; c < 4; ++c) {
            const Form& f = outputs.at(reg, c);
            if (!f.termCount || f.cfTainted) continue;
            bool family = true;
            for (uint8_t i = 0; i < f.termCount; ++i)
                if (f.terms[i].slot != result.slot || f.terms[i].row < result.row ||
                    f.terms[i].row > result.row + 3) { family = false; break; }
            if (family) { result.exportsClipXyw = true; break; }
        }
        if (result.exportsClipXyw) break;
    }
    return result;
}

// ---------------------------------------------------------------------------
// PS classification
// ---------------------------------------------------------------------------

// Taint: 0 clean, 1 integer-domain (jitter-invariant), 2 float age 0 (raw
// vPos), 3+ float age (capped). Float reaching anything but a texture
// coordinate or the integer domain is a projection consumer.
inline uint8_t taintUnion(uint8_t a, uint8_t b) {
    if (a == 0) return b;
    if (b == 0) return a;
    if (a >= 2 && b >= 2) return a > b ? a : b;
    return a > b ? a : b;
}
inline uint8_t taintNext(uint8_t t) { return t >= 2 ? (t < 10 ? t + 1 : t) : t; }

inline bool opcodeIsTextureFetch(uint32_t op) {
    return (op >= kOpSample && op <= kOpSampleB) || op == kOpGather4 || op == kOpLd ||
           op == 46 || op == kOpLdRaw || op == kOpLdStructured || op == 163 || op == 164 ||
           op == 166;
}

// Operand positions of a texture fetch that count as texture coordinates
// (allowed consumers of vPos-derived values). resinfo's lod is address
// math, never a coordinate.
inline bool textureCoordOperand(uint32_t op, uint8_t idx) {
    if (op == kOpLdStructured) return idx == 1 || idx == 2;
    return idx == 1;
}

struct PsAnalysis {
    FlatPsProjectionSafety safety = FlatPsProjectionSafety::NoBytecode;
    FlatClassifierReason reason = FlatClassifierReason::None;
    uint16_t unknownOpcode = 0; // valid when reason == UnknownOpcode
    FlatVposConsumerSubcode consumerSubcode = FlatVposConsumerSubcode::None; // valid when reason == VposConsumer
};

inline bool analyzePs(const std::vector<uint32_t>& t, PsAnalysis& out) {
    std::vector<Instr> instrs;
    ProgramFacts facts;
    if (!walkProgram(t, instrs, facts)) { out.reason = FlatClassifierReason::Walk; return false; }
    if (facts.hasIndexableTemp) { out.reason = FlatClassifierReason::IndexableTemp; return false; }
    if (facts.tempCount > kMaxTemps) { out.reason = FlatClassifierReason::TempCount; return false; }
    if (facts.sawUnknownOpcode) {
        out.reason = FlatClassifierReason::UnknownOpcode;
        out.unknownOpcode = facts.firstUnknownOpcode;
        return false;
    }
    if (facts.operandOutOfRange) { out.reason = FlatClassifierReason::OperandRange; return false; }
    if (facts.depthOutput) { out.reason = FlatClassifierReason::DepthOutput; return false; }
    const int32_t vpos = facts.vposRegister;

    FormBank temps(facts.tempCount ? facts.tempCount : 1);
    FormBank outputs(kMaxOutputs);
    buildForms(instrs, facts, temps, outputs);
    // A temp or output combining >=2 distinct rows of one cb buffer with >=2
    // non-literal coefficients is a possible PS-side matrix.
    for (const Form& f : temps.forms)
        if (f.arithMultiRow) {
            out.reason = FlatClassifierReason::MultiRowTemp;
            return false;
        }
    for (const Form& f : outputs.forms)
        if (f.arithMultiRow) {
            out.reason = FlatClassifierReason::MultiRowOutput;
            return false;
        }

    // vPos taint / origin dataflow.
    std::vector<uint8_t> taint(size_t(facts.tempCount ? facts.tempCount : 1) * 4, 0);
    std::vector<uint8_t> origin(size_t(facts.tempCount ? facts.tempCount : 1) * 4, 0);
    auto taintAt = [&](const Operand& op, uint32_t c) -> uint8_t {
        if (op.type == kOperandTemp)
            return op.reg < facts.tempCount ? taint[size_t(op.reg) * 4 + sourceComponent(op, c)] : 0;
        if (op.type == kOperandInput && static_cast<int32_t>(op.reg) == vpos) return 2;
        return 0;
    };
    auto originAt = [&](const Operand& op, uint32_t c) -> uint8_t {
        if (op.type == kOperandTemp)
            return op.reg < facts.tempCount ? origin[size_t(op.reg) * 4 + sourceComponent(op, c)] : 0;
        if (op.type == kOperandInput) return 1;
        return 0;
    };
    for (const Instr& in : instrs) {
        if (isDeclaration(in.opcode) || !in.opCount) continue;
        if (in.parseError || in.unmodelable) {
            // Unproven when it involves cb, vPos or an output write. A
            // parse error means the operands cannot even be enumerated.
            bool involves = in.parseError;
            const Operand& dest = in.ops[0];
            if (dest.type == kOperandOutput || dest.type == 12 || dest.type == 38 || dest.type == 39)
                involves = true;
            for (uint8_t i = 0; i < in.opCount && !involves; ++i) {
                const Operand& op = in.ops[i];
                if (op.type == kOperandCb) involves = true;
                if (op.type == kOperandInput && static_cast<int32_t>(op.reg) == vpos) involves = true;
            }
            if (involves) {
                out.reason = FlatClassifierReason::Unmodelable;
                return false;
            }
            continue;
        }
        const Operand& dest = in.ops[0];
        const bool writesOutput = dest.type == kOperandOutput || dest.type == 12 ||
                                  dest.type == 38 || dest.type == 39;
        const bool destIsTemp = dest.type == kOperandTemp && dest.reg < facts.tempCount;
        uint8_t srcTaint[6][4];
        uint8_t srcOrigin[6][4];
        for (uint8_t i = 0; i < in.opCount; ++i)
            for (uint32_t c = 0; c < 4; ++c) {
                srcTaint[i][c] = taintAt(in.ops[i], c);
                srcOrigin[i][c] = originAt(in.ops[i], c);
            }
        uint8_t destTaint[4] = {0, 0, 0, 0};
        bool consumer = false;
        FlatVposConsumerSubcode consumerSubcode = FlatVposConsumerSubcode::None;
        // First rule to trip `consumer` for this instruction wins the subcode
        // (cheap, deterministic; a second rule tripped by the same
        // instruction is not recorded -- see FlatVposConsumerSubcode).
        auto trip = [&](FlatVposConsumerSubcode sc) {
            consumer = true;
            if (consumerSubcode == FlatVposConsumerSubcode::None) consumerSubcode = sc;
        };
        const uint32_t op = in.opcode;
        const bool transit = op == kOpAdd || op == kOpMul || op == kOpMad || op == kOpMin ||
                             op == kOpMax || op == kOpFrc || op == kOpAnd || op == kOpOr ||
                             op == kOpXor || op == kOpIshl || op == kOpIshr;
        for (uint32_t c = 0; c < 4; ++c) {
            uint8_t tv = 0;
            for (uint8_t i = 1; i < in.opCount; ++i) tv = taintUnion(tv, srcTaint[i][c]);
            if (op == kOpMov) {
                tv = srcTaint[1][c];
            } else if (op == kOpMovc || op == kOpSwapc) {
                if (srcTaint[1][c] >= 2) trip(FlatVposConsumerSubcode::MovcCondition); // select condition is comparison-like
                tv = taintUnion(srcTaint[2][c], srcTaint[3][c]);
            } else if (op == kOpFtou || op == kOpFtoi || op == kOpRoundNe || op == kOpRoundNi ||
                       op == kOpRoundPi || op == kOpRoundZ) {
                // Conversions to the integer domain (round*, ftou, ftoi) are
                // jitter-invariant: the tile/pixel-grid lookups they feed are
                // unaffected by subpixel jitter.
                tv = srcTaint[1][c] >= 2 ? 1 : srcTaint[1][c];
            } else if (op == kOpItof || op == kOpUtof) {
                tv = srcTaint[1][c];
            } else if (op == kOpDiv || op == kOpRcp) {
                // A vPos-derived value (past raw vPos) reaching a divide is
                // the depth-UV reconstruction idiom; raw vPos scaled by a
                // constant stays jitter-invariant (tile lookups).
                const uint8_t a = srcTaint[1][c];
                const uint8_t b = op == kOpDiv ? srcTaint[2][c] : 0;
                if (a >= 3 || b >= 3) trip(FlatVposConsumerSubcode::AgedDivRcp);
                if (op == kOpDiv && srcOrigin[1][c] == 1 && srcOrigin[2][c] == 1) trip(FlatVposConsumerSubcode::DivOrigin);
                tv = taintUnion(a, b);
                if (tv >= 2) tv = taintNext(tv);
            } else if (opcodeIsTextureFetch(op)) {
                tv = 0;
            } else if (!transit && tv >= 2) {
                // Anything else a float-tainted value can reach is a consumer.
                trip(FlatVposConsumerSubcode::OtherOp);
            }
            if (transit && tv >= 2) tv = taintNext(tv);
            destTaint[c] = tv;
        }
        // Texture fetches: only the coordinate/address operands may be
        // tainted; the load itself is fresh (untracked, Load origin).
        if (opcodeIsTextureFetch(op)) {
            for (uint8_t i = 1; i < in.opCount; ++i) {
                if (textureCoordOperand(op, i)) continue;
                for (uint32_t c = 0; c < 4; ++c)
                    if (srcTaint[i][c] >= 2) trip(FlatVposConsumerSubcode::TextureNonCoord);
            }
            for (uint32_t c = 0; c < 4; ++c) destTaint[c] = 0;
        }
        if (writesOutput) {
            for (uint32_t c = 0; c < 4; ++c)
                if (destTaint[c] >= 2) trip(FlatVposConsumerSubcode::OutputWrite);
        }
        if (consumer) {
            out.reason = FlatClassifierReason::VposConsumer;
            out.consumerSubcode = consumerSubcode;
            return false;
        }
        if (destIsTemp || (isTwoDestOpcode(op) && in.ops[1].ok && in.ops[1].type == kOperandTemp &&
                           in.ops[1].reg < facts.tempCount)) {
            const uint32_t regs[2] = {destIsTemp ? dest.reg : 0xFFFFFFFFu,
                                      isTwoDestOpcode(op) ? in.ops[1].reg : 0xFFFFFFFFu};
            for (uint32_t r = 0; r < 2; ++r) {
                if (regs[r] == 0xFFFFFFFFu) continue;
                const uint32_t mask = r == 0 ? dest.mask : in.ops[1].mask;
                for (uint32_t c = 0; c < 4; ++c) {
                    if (!(mask & (1u << c))) continue;
                    taint[size_t(regs[r]) * 4 + c] = r == 0 ? destTaint[c] : 0;
                    origin[size_t(regs[r]) * 4 + c] = 0;
                }
            }
        }
        // Origins: mov/movc/swapc copy provenance (a select is Input if
        // either arm is); loads mark Load; the rest Other.
        if (destIsTemp) {
            for (uint32_t c = 0; c < 4; ++c) {
                if (!(dest.mask & (1u << c))) continue;
                uint8_t o = 0;
                if (op == kOpMov) o = srcOrigin[1][c];
                else if (op == kOpMovc || op == kOpSwapc)
                    o = static_cast<uint8_t>(srcOrigin[2][c] == 1 || srcOrigin[3][c] == 1 ? 1 :
                                             (srcOrigin[2][c] == 2 && srcOrigin[3][c] == 2 ? 2 : 0));
                else if (opcodeIsTextureFetch(op)) o = 2;
                origin[size_t(dest.reg) * 4 + c] = o;
            }
        }
    }
    out.safety = FlatPsProjectionSafety::Clean;
    return true;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

inline VsAnalysis classifyVsBytes(const void* bytes, size_t len) {
    VsAnalysis result;
    if (!bytes || !len) { result.reason = FlatClassifierReason::NoBytecode; return result; }
    try {
        auto chunks = parseContainer(bytes, len, kVs50);
        const std::vector<BYTE>* program = nullptr;
        std::vector<dxbc_container::SignatureElement> osgn;
        for (const auto& chunk : chunks) {
            if (chunk.tag == kTagShex || chunk.tag == kTagShdr) program = &chunk.bytes;
            else if (chunk.tag == kTagOsgn) osgn = parseSignature(chunk.bytes);
        }
        if (!program || program->size() < 8 || (program->size() & 3)) {
            result.reason = FlatClassifierReason::Container;
            return result;
        }
        std::vector<uint32_t> t(program->size() / 4);
        std::memcpy(t.data(), program->data(), t.size() * 4);
        return analyzeVs(t, osgn);
    } catch (...) {
        result.reason = FlatClassifierReason::Container;
        return result;
    }
}

inline PsAnalysis classifyPsBytes(const void* bytes, size_t len) {
    PsAnalysis result;
    if (!bytes || !len) {
        result.safety = FlatPsProjectionSafety::NoBytecode;
        result.reason = FlatClassifierReason::NoBytecode;
        return result;
    }
    try {
        auto chunks = parseContainer(bytes, len, kPs50);
        const std::vector<BYTE>* program = nullptr;
        for (const auto& chunk : chunks)
            if (chunk.tag == kTagShex || chunk.tag == kTagShdr) program = &chunk.bytes;
        if (!program || program->size() < 8 || (program->size() & 3)) {
            result.safety = FlatPsProjectionSafety::Consumer;
            result.reason = FlatClassifierReason::Container;
            return result;
        }
        std::vector<uint32_t> t(program->size() / 4);
        std::memcpy(t.data(), program->data(), t.size() * 4);
        if (!analyzePs(t, result)) {
            result.safety = FlatPsProjectionSafety::Consumer;
            return result;
        }
        return result;
    } catch (...) {
        result.safety = FlatPsProjectionSafety::Consumer;
        result.reason = FlatClassifierReason::Container;
        return result;
    }
}

// Test/sweep hook: the container parses, the checksum verifies, and the
// walker consumes exactly the program's declared dword count (no desync).
// Unmodelable instructions are a per-analysis concern, not a desync.
inline bool walksClean(const void* bytes, size_t len, uint32_t programType) {
    if (!bytes || !len) return false;
    try {
        auto chunks = parseContainer(bytes, len, programType);
        const std::vector<BYTE>* program = nullptr;
        for (const auto& chunk : chunks)
            if (chunk.tag == kTagShex || chunk.tag == kTagShdr) program = &chunk.bytes;
        if (!program || program->size() < 8 || (program->size() & 3)) return false;
        std::vector<uint32_t> t(program->size() / 4);
        std::memcpy(t.data(), program->data(), t.size() * 4);
        std::vector<Instr> instrs;
        ProgramFacts facts;
        return walkProgram(t, instrs, facts);
    } catch (...) {
        return false;
    }
}

} // namespace flat_shader_classifier_detail

// Classify one (VS,PS) pair from creation-time bytecode. A null or empty
// stage is NoBytecode for that stage; a corrupt container is Unclassified
// (VS) or Consumer (PS): unproven is always refused.
inline FlatShaderPairClassification classifyFlatShaderPair(const void* vsBytes, size_t vsLen,
                                                           const void* psBytes, size_t psLen) {
    FlatShaderPairClassification out;
    if (vsBytes && vsLen) {
        const auto vs = flat_shader_classifier_detail::classifyVsBytes(vsBytes, vsLen);
        out.vs = vs.cls; out.vsSlot = vs.slot; out.vsRow = vs.row;
        out.vsExportsClipXyw = vs.exportsClipXyw;
        out.vsReason = vs.reason; out.vsUnknownOpcode = vs.unknownOpcode;
    } else {
        out.vsReason = FlatClassifierReason::NoBytecode;
    }
    if (psBytes && psLen) {
        const auto ps = flat_shader_classifier_detail::classifyPsBytes(psBytes, psLen);
        out.ps = ps.safety;
        out.psReason = ps.reason; out.psUnknownOpcode = ps.unknownOpcode;
        out.psConsumerSubcode = ps.consumerSubcode;
    } else {
        out.psReason = FlatClassifierReason::NoBytecode;
    }
    return out;
}

} // namespace edvr

#!/usr/bin/env python3
"""The mutation proof for tools\\intro_curve_test: the rig fails when a rule of the intro panel's module is flipped.

The rig (intro_curve_test.cpp) runs the REAL src\\d3d11\\intro_panel.cpp on a WARP device, one process per scenario (the module's latches
cannot be reset), and prints "FAIL: <scenario>.<what>" for every check that breaks. A rig that passes proves little until it is seen to FAIL
on a module that breaks the rule it pins. This tool does that: for each mutation below it copies intro_panel.cpp into a temp directory OUTSIDE
the repo, applies one textual edit (or a few that belong together), compiles the module against that copy, links the rig with the unmutated
common sources, runs it, and requires it to fail on a scenario that belongs to the rule (a FAIL label that starts with one of the mutation's
scenario ids and a dot). Nothing is written inside the repo; the temp directory is removed at the end.

  python tools\\intro_curve_test\\mutants.py --self-test       text only: every anchor is found exactly once in the module as it is now,
                                                              every scenario named is in the rig, every scenario of the rig has a mutation
                                                              or a stated reason it has none, and build.bat compiles the rig the way this
                                                              tool does (add --build-bat PATH to check another copy)
  python tools\\intro_curve_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]
  python tools\\intro_curve_test\\mutants.py --list
  python tools\\intro_curve_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does); build\\gen is added to the include
path when there is one. It takes a few minutes: the four common sources and the rig are compiled once, then each mutation compiles one file and
links, and the rig runs its scenarios. --self-test runs in build.bat's rig and is what keeps an edit of the module from silently orphaning a
mutation: if an anchor stops matching, the build fails and this file says which.
"""
import argparse
import concurrent.futures
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SRC = ROOT / "src" / "d3d11"
MODULE = SRC / "intro_panel.cpp"
RIG = HERE / "intro_curve_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_intro_curve_test"
COMMON = [ROOT / "src" / "common" / n for n in ("config.cpp", "log.cpp", "guard.cpp", "proxy.cpp")]
LIBS = ["user32.lib", "version.lib"]

# How build.bat compiles the rig; --self-test checks that the label still says the same.
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
RUN_TIMEOUT = 300.0


class Mutant:
    def __init__(self, name, caught, edits, why):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # scenario ids whose checks must report it
        self.edits = list(edits)                                                # (old, new) pairs, applied in order
        self.why = why


def M(name, caught, edits, why):
    return Mutant(name, caught, edits, why)


def drop(old):
    return [(old, "")]


# ---- the anchors: text of the production module, verbatim (the self-test finds each exactly once) -----------------------------
HALF_W = "constexpr float kScreenHalfW = 4.44444f;\n"
HALF_H = "constexpr float kScreenHalfH = 2.5f;\n"
DIST_DEFAULT = "constexpr float kScreenDistDefault = 3.35f;\n"
HALF_IPD = "constexpr float kHalfIpd = 0.0315f;\n"
SETTLE = "constexpr uint32_t kSettleFrames = 4;\n"
VS_SLOT = "constexpr uint32_t kVsSlot = 2;\n"
MAX_SLOTS = "constexpr uint32_t kMaxSlots = 2;\n"
SPLASH_W = "constexpr float kSplashHalfWidthNdc = 1.0440f;\n"
MAX_SIZE = "constexpr float kMaxSize = 8.0f;\n"
EYE_EPS = "                constexpr float kEyeCentreEps = 0.02f;\n"
LOOKS = "bool looksScreenSpace(const float* f) {\n"
SS_W12 = "    if (!zero(f[7]) || !zero(f[11])) return false;       // cb2[1].w, cb2[2].w\n"
SS_LOOP = "    for (uint32_t i = 12; i < 16; ++i) {                 // cb2[3] entirely\n"
SS_W = "    if (f[19] < 0.999f || f[19] > 1.001f) return false;  // cb2[4].w == 1\n"
SS_SCALE = "    if (f[0] < 16.0f || f[1] < 16.0f) return false;\n"
CX = "        cx[i] = At(i, 0) * -kScreenHalfW;\n"
CY = "        cy[i] = At(i, 1) * kScreenHalfH;\n"
AT = "    auto At = [&](int i, int j) { return R[j * 4 + i]; };   // A[i][j] = R[j][i]\n"
TRANSLATION = "    const float tx = R[3], ty = R[7], tz = R[11];\n"
EYE_X = "    const float ex = leftEye ? -kHalfIpd : kHalfIpd;\n"
C0_X = "    c0[0] -= ex;\n"
LT = "    const float lt = leftEye ? -outer : -inner;\n"
RT = "    const float rt = leftEye ? inner : outer;\n"
M0 = "    const float m00 = 2.0f / (rt - lt), m02 = (rt + lt) / (rt - lt);\n"
M1 = "    const float m11 = 2.0f / (bt - tp), m12 = (bt + tp) / (bt - tp);\n"
TP = "        tp = -topMag;\n"
BT = "        bt = botMag;\n"
COL_Z = "        dst[2] = 0.5f * dst[3];\n"
COL_W = "        dst[3] = -v[2];\n"
COL_X = "        dst[0] = m00 * v[0] + m02 * v[2];\n"
COL_Y = "        dst[1] = m11 * v[1] + m12 * v[2];\n"
OUT1 = "    out[1] = 1.0f;\n"
COLS = "    col(cx, false, out + 4);\n    col(cy, false, out + 8);\n"
COL_C0 = "    col(c0, true, out + 16);\n"
MEMCPY = "                memcpy(m.pData, world, kCbBytes);\n"
NO_POSE = '    if (!headPose(pose)) { *why = "no head pose has been published"; return false; }\n'
NO_TANGENTS = '    if (!eyeTangents(&outer, &inner)) {\n        *why = "no eye tangents have been published";\n        return false;\n    }\n'
SPAN = '    if (span < 1e-3f) { *why = "the published tangents are degenerate"; return false; }\n'
VERT = '               "are present -- a partial publish";\n        return false;\n'
BEHIND = "    if (!(c0[2] < 0.0f)) {\n"
FINITE = "        if (!isFiniteF(cx[i]) || !isFiniteF(cy[i]) || !isFiniteF(c0[i])) {\n"
VIEWPORT = "                if (nvp == 0 || vp.Width <= 0.0f || vp.Height <= 0.0f ||\n"
RESTORE = "        ctx->VSSetConstantBuffers(kVsSlot, 1, &orig);\n        g_restore = nullptr;\n"
BIND = "            ctx->VSSetConstantBuffers(kVsSlot, 1, &ours);\n"
G_RESTORE = "            g_restore = cb;\n"
STAGE_RETURN = "        if (s->stage || s->dueFrame) { cb->Release(); return; }\n"
BOX = "                box.right = kCbBytes;\n"
MIN_BYTES = "        const bool ok = bindingResolveResource(cb, &info) && info.isBuffer &&\n                        info.a >= kCbBytes;\n"
DUE = "        if (!s.stage || !s.dueFrame || g_frame < s.dueFrame) continue;\n"
RETIRE = "    if (sceneFrame && !g_retired) {\n"
UP_SHUTDOWN = "        introUpscaleShutdown();\n"
REFUSE = '                g_refused = true;\n                Log::get().note(\n                    "intro video size: the panel\'s constants do not read as a "\n'
WANTS_T = "        (g_matchSplash || g_size != 1.0f || g_worldLock) && !g_refused;\n"
WANTS_R = "    return (wantsTransform || introUpscaleWants()) && !g_retired;\n"
CFG_SPLASH = "    g_matchSplash = mode.screen;\n"
CFG_SIZE = "    g_size = mode.screen ? 0.0f : 1.0f;   // derived at readback when on\n"
CFG_LOCK = "    g_worldLock = mode.worldLock;\n"
CFG_FIRST = "    const float wasSize = g_size;\n"
DIST_LO = "    if (g_screenDist < 1.0f) g_screenDist = 1.0f;\n"
DIST_HI = "    if (g_screenDist > 20.0f) g_screenDist = 20.0f;\n"
FIRST = "    if (!introPanelWants() || !ctx) return false;\n"
SHAPE = "    if (kind != 'X' || count != 6 || instances != 1) return false;\n"
RECENTRE = "    if (!g_recentreRequested && !sceneArrived()) {\n"
FILL = "    if (g_fillFrame != g_frame || !g_fillW) return false;\n"
FILL_SIZE = "    if (srvW != g_fillW || srvH != g_fillH) return false;\n"
LOCK_ONLY = "        if (!g_matchSplash && g_size == 1.0f) {\n"
EYES_REFUSED = "                if (!s->eyeKnown || !eyesDisagree) {\n"
DISAGREE = "                    if (o.leftEye == s->leftEye) eyesDisagree = false;\n"
LEFT = "                s.leftEye = f[16] > 0.0f;\n"
KNOWN = "                s.eyeKnown = (f[16] > kEyeCentreEps) || (f[16] < -kEyeCentreEps);\n"
DYNAMIC = "            bd.Usage = D3D11_USAGE_DYNAMIC;\n"
DISCARD = "                if (FAILED(ctx->Map(s->ours, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) ||\n"
SCALE_CALC = "                scale = halfNdc > 1e-6f ? kSplashHalfWidthNdc / halfNdc : 1.0f;\n"
SCALE_LO = "                if (scale < 1.0f) scale = 1.0f;\n"
SCALE_HI = "                if (scale > kMaxSize) scale = kMaxSize;\n"
NOTED_EYES = ('                        g_lockRefusedNoted = true;\n                        Log::get().note(\n'
              '                            "intro video lock: cannot tell the eyes apart "\n')
NOTED_LOCK = ('                        g_lockRefusedNoted = true;\n                        Log::get().note(\n'
              '                            "intro video lock: %s. The movie stays as the "\n')
ANCHORED = "                    g_anchored = true;\n"
ENGAGED = "            if (++g_applied == 1) {\n"
OURS_SLOT = "            s->key = cb;\n"

MUTANTS = [
    # ---- golden: the bytes of cb2 -----------------------------------------------------------------------------------------------
    M("half-width-4", "golden", [(HALF_W, "constexpr float kScreenHalfW = 4.0f;\n")], "the panel's half-width is 4 units, not the splash's 4.44444"),
    M("half-height-2", "golden", [(HALF_H, "constexpr float kScreenHalfH = 2.0f;\n")], "the panel's half-height is 2 units, not the splash's 2.5"),
    M("distance-default-3", ("golden", "distance"), [(DIST_DEFAULT, "constexpr float kScreenDistDefault = 3.0f;\n")], "the default distance is 3 m, not 3.35"),
    M("half-ipd-doubled", "golden", [(HALF_IPD, "constexpr float kHalfIpd = 0.063f;\n")], "the eye offset is twice the half-IPD"),
    M("x-axis-mirrored", "golden", [(CX, "        cx[i] = At(i, 0) * kScreenHalfW;\n")], "the panel's +x points to the viewer's right: the picture comes out mirrored"),
    M("y-axis-flipped", "golden", [(CY, "        cy[i] = At(i, 1) * -kScreenHalfH;\n")], "the panel's +y is flipped: the picture comes out upside down"),
    M("rotation-not-transposed", "golden", [(AT, "    auto At = [&](int i, int j) { return R[i * 4 + j]; };   // A[i][j] = R[i][j]\n")], "the head's inverse is the rotation, not its transpose"),
    M("translation-index", "golden", [(TRANSLATION, "    const float tx = R[3], ty = R[7], tz = R[10];\n")], "the pose's z translation is read from the rotation's last entry"),
    M("centre-offset-sign", "golden", [(C0_X, "    c0[0] += ex;\n")], "the eye's lateral offset is applied the other way"),
    M("eye-offset-swapped", "golden", [(EYE_X, "    const float ex = leftEye ? kHalfIpd : -kHalfIpd;\n")], "the left eye takes the right eye's offset"),
    M("frustum-left-edge", "golden", [(LT, "    const float lt = leftEye ? -inner : -outer;\n")], "the left eye's left edge is the nasal tangent"),
    M("frustum-right-edge", "golden", [(RT, "    const float rt = leftEye ? outer : inner;\n")], "the left eye's right edge is the temporal tangent"),
    M("m02-sign", "golden", [(M0, "    const float m00 = 2.0f / (rt - lt), m02 = (lt + rt) / (lt - rt);\n")], "the horizontal frustum centre has the wrong sign"),
    M("m12-sign", "golden-asymmetric", [(M1, "    const float m11 = 2.0f / (bt - tp), m12 = (tp + bt) / (tp - bt);\n")], "the vertical frustum centre has the wrong sign (invisible on a symmetric one)"),
    M("vertical-top-sign", "golden-asymmetric", [(TP, "        tp = topMag;\n")], "the top tangent is not negated"),
    M("vertical-bottom-sign", "golden-asymmetric", [(BT, "        bt = -botMag;\n")], "the bottom tangent is negated"),
    M("clip-z-full-w", "golden", [(COL_Z, "        dst[2] = dst[3];\n")], "z is w, not w/2: ndc.z is 1, on the far plane"),
    M("clip-w-sign", "golden", [(COL_W, "        dst[3] = v[2];\n")], "w is z, not -z: the panel is behind the eye"),
    M("clip-x-sign", "golden", [(COL_X, "        dst[0] = m00 * v[0] - m02 * v[2];\n")], "the frustum shift enters x with the wrong sign"),
    M("clip-y-ignores-shift", "golden-asymmetric", [(COL_Y, "        dst[1] = m11 * v[1];\n")], "the vertical frustum shift is left out"),
    M("cb2-0-y-half", "golden", [(OUT1, "    out[1] = 0.5f;\n")], "cb2[0].y is 0.5: the panel is half as tall as it should be"),
    M("columns-swapped", "golden", [(COLS, "    col(cx, false, out + 8);\n    col(cy, false, out + 4);\n")], "x and y columns are written into each other's rows"),
    M("cb2-3-written", "golden", [(COL_C0, COL_C0 + "    out[14] = 1.0f;\n")], "cb2[3] is no longer left zero"),
    M("write-short", "golden", [(MEMCPY, "                memcpy(m.pData, world, kCbBytes - 16);\n")], "the last float4 of cb2 is not written"),
    M("slot-1", "golden", [(VS_SLOT, "constexpr uint32_t kVsSlot = 1;\n")], "the placement goes into VS b1, not b2"),
    M("bind-skipped", "golden", [(BIND, "")], "our constants are built and never bound"),
    M("ours-immutable", "golden", [(DYNAMIC, "            bd.Usage = D3D11_USAGE_DEFAULT;\n")], "our buffer is not dynamic, so it cannot be written every draw"),
    M("map-plain-write", "golden", [(DISCARD, "                if (FAILED(ctx->Map(s->ours, 0, D3D11_MAP_WRITE, 0, &m)) ||\n")], "our dynamic buffer is mapped with a flag it refuses"),
    # ---- golden: the sequence ---------------------------------------------------------------------------------------------------
    M("settle-3", "golden", [(SETTLE, "constexpr uint32_t kSettleFrames = 3;\n")], "the readback is mapped after three frames, not four"),
    M("settle-5", "golden", [(SETTLE, "constexpr uint32_t kSettleFrames = 5;\n")], "the readback is mapped after five frames, not four"),
    M("due-one-late", "golden", [(DUE, "        if (!s.stage || !s.dueFrame || g_frame <= s.dueFrame) continue;\n")], "a settled readback is retired a frame after its due frame"),
    M("copy-short", "golden", [(BOX, "                box.right = kCbBytes - 4;\n")], "the readback copies 76 bytes: cb2[4].w reads back zero"),
    M("restore-skipped", "golden", [(RESTORE, "        g_restore = nullptr;\n")], "the game's constant buffer is never put back"),
    M("restore-pointer-kept", "golden", [(RESTORE, "        ctx->VSSetConstantBuffers(kVsSlot, 1, &orig);\n")], "the restore target is not cleared: an unpaired EndDraw puts back a stale buffer"),
    M("restore-wrong-slot", "golden", [(RESTORE, "        ctx->VSSetConstantBuffers(kVsSlot + 1, 1, &orig);\n        g_restore = nullptr;\n")], "the game's buffer goes back into the slot after"),
    M("restore-target-ours", "golden", [(G_RESTORE, "            g_restore = s->ours;\n")], "the restore target is our own buffer"),
    M("settle-leaks-a-reference", "golden", [(STAGE_RETURN, "        if (s->stage || s->dueFrame) { return; }\n")], "a composite that waits on the readback keeps a reference on the game's buffer"),
    M("bind-clears-ps-slot", "golden", [(BIND, BIND + "            { ID3D11ShaderResourceView* none = nullptr; ctx->PSSetShaderResources(0, 1, &none); }\n")], "binding ours also unbinds the pixel shader's resource"),
    M("endDraw-clears-cb3", "golden", [(RESTORE, RESTORE + "        { ID3D11Buffer* none = nullptr; ctx->VSSetConstantBuffers(3, 1, &none); }\n")], "restoring the game's buffer also unbinds VS b3"),
    M("anchored-line-repeats", "golden", [(ANCHORED, "")], "the 'holding' line is written at every draw"),
    M("engaged-line-repeats", "golden", [(ENGAGED, "            if (++g_applied >= 1) {\n")], "the 'engaged' line is written at every draw"),
    # ---- head: the splash-sized panel -------------------------------------------------------------------------------------------
    M("scale-inverted", "head", [(SCALE_CALC, "                scale = halfNdc > 1e-6f ? halfNdc / kSplashHalfWidthNdc : 1.0f;\n")], "the factor is the panel's width over the splash's"),
    M("splash-width-1", "head", [(SPLASH_W, "constexpr float kSplashHalfWidthNdc = 1.0f;\n")], "the splash's half-width in NDC is 1, not 1.0440"),
    M("max-size-4", "head", [(MAX_SIZE, "constexpr float kMaxSize = 4.0f;\n")], "the factor is held at 4 rather than 8"),
    M("max-size-removed", "head-clamp-high", [(SCALE_HI, "")], "the factor has no ceiling"),
    M("min-size-removed", "head-clamp-low", [(SCALE_LO, "")], "the panel may shrink below its own size"),
    # ---- splash, refuse-*: the screen-space test ---------------------------------------------------------------------------------
    M("ss-always-true", "splash", [(LOOKS, LOOKS + "    return true;\n")], "every buffer reads as screen-space, the splash's included"),
    M("ss-drop-cb1-w", "refuse-cb2-1-w", [(SS_W12, "    if (!zero(f[11])) return false;\n")], "cb2[1].w is not looked at"),
    M("ss-drop-cb2-w", "refuse-cb2-2-w", [(SS_W12, "    if (!zero(f[7])) return false;\n")], "cb2[2].w is not looked at"),
    M("ss-cb3-skips-x", "refuse-cb2-3-x", [(SS_LOOP, "    for (uint32_t i = 13; i < 16; ++i) {                 // cb2[3] entirely\n")], "cb2[3].x is not looked at"),
    M("ss-cb3-skips-w", "refuse-cb2-3-w", [(SS_LOOP, "    for (uint32_t i = 12; i < 15; ++i) {                 // cb2[3] entirely\n")], "cb2[3].w is not looked at"),
    M("ss-cb3-skips-zw", "refuse-cb2-3-z", [(SS_LOOP, "    for (uint32_t i = 12; i < 14; ++i) {                 // cb2[3] entirely\n")], "cb2[3].z and .w are not looked at"),
    M("ss-cb3-x-only", "refuse-cb2-3-y", [(SS_LOOP, "    for (uint32_t i = 12; i < 13; ++i) {                 // cb2[3] entirely\n")], "only cb2[3].x is looked at"),
    M("ss-w-window-wide", ("refuse-w-low", "refuse-w-high"), [(SS_W, "    if (f[19] < 0.9f || f[19] > 1.1f) return false;  // cb2[4].w == 1\n")], "cb2[4].w may be anywhere within a tenth of 1"),
    M("ss-w-window-narrow", ("accept-w-in-low", "accept-w-in-high"), [(SS_W, "    if (f[19] < 0.99999f || f[19] > 1.00001f) return false;  // cb2[4].w == 1\n")], "cb2[4].w must be 1 to five decimals"),
    M("ss-scale-floor-1", ("refuse-scale-x", "refuse-scale-y"), [(SS_SCALE, "    if (f[0] < 1.0f || f[1] < 1.0f) return false;\n")], "a scale of 8 reads as a plausible half-size in pixels"),
    M("ss-scale-x-only", "refuse-scale-y", [(SS_SCALE, "    if (f[0] < 16.0f) return false;\n")], "cb2[0].y is not looked at"),
    M("ss-scale-y-only", "refuse-scale-x", [(SS_SCALE, "    if (f[1] < 16.0f) return false;\n")], "cb2[0].x is not looked at"),
    M("refusal-not-for-the-session", ("splash", "refuse-cb2-3-x"), [(REFUSE, REFUSE.replace("                g_refused = true;\n", ""))], "a buffer that does not read as screen-space is not remembered"),
    M("wants-ignores-refusal", "splash", [(WANTS_T, "        (g_matchSplash || g_size != 1.0f || g_worldLock);\n")], "the panel is still wanted after a refusal"),
    # ---- the lock's refusals ------------------------------------------------------------------------------------------------------
    M("no-pose-not-refused", "no-pose", [(NO_POSE, "    headPose(pose);\n")], "a missing pose is not a reason to leave the movie alone"),
    M("no-tangents-not-refused", "no-tangents", [(NO_TANGENTS, "    eyeTangents(&outer, &inner);\n")], "missing tangents are not a reason to leave the movie alone"),
    M("degenerate-span-accepted", "degenerate-tangents", [(SPAN, '    if (span < 0.0f) { *why = "the published tangents are degenerate"; return false; }\n')], "a span of nothing is a frustum"),
    M("vertical-derived-again", "no-vertical", [(VERT, '               "are present -- a partial publish";\n        tp = -0.5f * span;\n        bt = 0.5f * span;\n')], "a missing vertical pair is derived as symmetric, as it used to be"),
    M("behind-not-refused", "behind", [(BEHIND, "    if (false) {\n")], "a panel behind the eye is drawn (and clipped away: the movie vanishes)"),
    M("behind-inverted", ("golden", "behind"), [(BEHIND, "    if (c0[2] < 0.0f) {\n")], "a panel in front of the eye is refused"),
    M("non-finite-not-refused", "non-finite", [(FINITE, "        if (false) {\n")], "a NaN reaches the constant buffer"),
    M("viewport-not-checked", "viewport", [(VIEWPORT, "                if (false ||\n")], "a degenerate viewport does not stop the lock"),
    M("lock-line-repeats", "no-pose", [(NOTED_LOCK, NOTED_LOCK.replace("                        g_lockRefusedNoted = true;\n", ""))], "the lock's refusal is logged at every draw"),
    M("eyes-line-repeats", "eyes-alike", [(NOTED_EYES, NOTED_EYES.replace("                        g_lockRefusedNoted = true;\n", ""))], "the eye refusal is logged at every draw"),
    # ---- the eye ----------------------------------------------------------------------------------------------------------------------
    M("eye-eps-0", "eyes-near-symmetric", [(EYE_EPS, "                constexpr float kEyeCentreEps = 0.0f;\n")], "any non-zero frustum centre names the eye"),
    M("eye-eps-0.2", "golden", [(EYE_EPS, "                constexpr float kEyeCentreEps = 0.2f;\n")], "the Pimax's +-0.1939 is too faint to name the eye"),
    M("eye-eps-0.04", "eyes-faint-known", [(EYE_EPS, "                constexpr float kEyeCentreEps = 0.04f;\n")], "a centre of +-0.03 is too faint to name the eye"),
    M("eye-always-known", ("eyes-symmetric", "eyes-near-symmetric"), [(KNOWN, "                s.eyeKnown = true;\n")], "the eye is always known, a symmetric headset's included"),
    M("eye-sign-inverted", ("golden", "eyes-swapped"), [(LEFT, "                s.leftEye = f[16] < 0.0f;\n")], "the left eye is the one with the negative centre"),
    M("eyes-agreeing-accepted", "eyes-alike", [(EYES_REFUSED, "                if (!s->eyeKnown) {\n")], "two buffers that read the same eye are both drawn"),
    M("eyes-disagree-inverted", "golden", [(DISAGREE, "                    if (o.leftEye != s->leftEye) eyesDisagree = false;\n")], "two buffers that name different eyes are refused"),
    # ---- config -----------------------------------------------------------------------------------------------------------------------
    M("stock-matches-splash", "config", [(CFG_SPLASH, "    g_matchSplash = true;\n")], "stock still matches the splash"),
    M("stock-resizes", "config", [(CFG_SIZE, "    g_size = 0.0f;   // derived at readback when on\n")], "stock still resizes"),
    M("head-locks", "head", [(CFG_LOCK, "    g_worldLock = true;\n")], "head mode still holds the panel on the world"),
    M("wants-needs-both", "config", [(WANTS_R, "    return (wantsTransform && introUpscaleWants()) && !g_retired;\n")], "the transform and the resampler must both be wanted"),
    M("wants-ignores-retirement", ("retire-used", "retire-unseen"), [(WANTS_R, "    return (wantsTransform || introUpscaleWants());\n")], "the panel is still wanted after the intro is over"),
    M("distance-floor-removed", "distance", [(DIST_LO, "")], "the distance may be under a metre"),
    M("distance-ceiling-removed", "distance", [(DIST_HI, "")], "the distance may be over twenty metres"),
    # ---- stock-off, gates, scene-arrived: what reaches the work ------------------------------------------------------------------
    M("wants-not-asked", "stock-off", [(FIRST, "    if (!ctx) return false;\n")], "an unwanted panel still works the composite"),
    M("null-context", "gates", [(FIRST, "    if (!introPanelWants()) return false;\n")], "a null context reaches the resampler"),
    M("shape-kind", "gates", [(SHAPE, "    if (count != 6 || instances != 1) return false;\n")], "any kind of draw is a composite"),
    M("shape-count", "gates", [(SHAPE, "    if (kind != 'X' || instances != 1) return false;\n")], "any index count is a composite"),
    M("shape-instances", "gates", [(SHAPE, "    if (kind != 'X' || count != 6) return false;\n")], "any instance count is a composite"),
    M("fill-frame-ignored", "gates", [(FILL, "    if (!g_fillW) return false;\n")], "a fill from an earlier frame is this frame's movie"),
    M("fill-no-size", "gates", [(FILL_SIZE, "")], "any surface is the fill's surface"),
    M("fill-width-only", "gates", [(FILL_SIZE, "    if (srvW != g_fillW) return false;\n")], "the surface's height is not compared"),
    M("fill-not-needed", "splash-no-fill", [(FILL, "")], "the splash is read without a fill"),
    M("lock-only-read", "stock", [(LOCK_ONLY, "        if (false) {\n")], "at stock the constants are read back and replaced like the movie's"),
    M("recentre-every-time", ("golden", "gates"), [(RECENTRE, "    if (!sceneArrived()) {\n")], "every composite asks the vr half to recentre"),
    M("recentre-after-the-scene", "scene-arrived", [(RECENTRE, "    if (!g_recentreRequested) {\n")], "the recentre is asked for after the scene has arrived"),
    M("third-buffer-served", "third-buffer", [(MAX_SLOTS, "constexpr uint32_t kMaxSlots = 3;\n")], "a third buffer gets a slot"),
    M("small-buffer-read", "small-cb", [(MIN_BYTES, "        const bool ok = bindingResolveResource(cb, &info) && info.isBuffer;\n")], "a buffer too small for cb2 is copied"),
    # ---- retire-*, shutdown-relearn -----------------------------------------------------------------------------------------------
    M("never-retires", ("retire-used", "retire-settling", "retire-unseen"), [(RETIRE, "    if (false) {\n")], "the first rendered scene does not end the intro"),
    M("resampler-left-running", ("retire-used", "retire-unseen"), [(UP_SHUTDOWN, "")], "the resample chain stays resident after the intro"),
    M("reconfigure-revives", ("retire-used", "retire-unseen"), [(CFG_FIRST, "    g_retired = false;\n" + CFG_FIRST)], "a reload of the config brings the panel back after the intro"),
    M("slot-key-lost", "shutdown-relearn", [(OURS_SLOT, "                s->key = nullptr;\n")], "a slot cannot find its buffer again"),
]

# Scenarios no mutation is tied to, and why: they pin something no one-rule edit of the module can break.
NO_MUTANT = {
    "golden-zero": "the module does not read fix.panel_curvature today, so it is golden with the key set to 0; it exists for the change that will",
    "head-symmetric": "head mode never reads the eye: its absence of a refusal is what a refusal would have to add",
}


# ---- applying an edit ----------------------------------------------------------------------------------------------------
def apply_edits(text, edits, name="?"):
    """The text with each (old, new) applied in order; each `old` must occur exactly once in the text as it stands then."""
    for old, new in edits:
        count = text.count(old)
        if count != 1:
            raise ValueError("mutation %s: an anchor occurs %d times (want 1): %r" % (name, count, old[:90]))
        if old == new:
            raise ValueError("mutation %s: an edit changes nothing: %r" % (name, old[:90]))
        text = text.replace(old, new)
    return text


def read_source(path):
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def fail_labels(output):
    """Every check label the rig printed on a 'FAIL: <scenario>.<what>' line (the text up to ' -- ', ' [' or the end of the line); the
    rig's summary lines ('FAIL: golden: 3 of 143 checks failed', 'FAIL: intro curve: ...') are not labels."""
    labels = []
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            label = re.split(r" -- | \[", line[len("FAIL: "):], 1)[0].strip()
            if re.match(r"[a-z][a-z0-9-]*\.", label):
                labels.append(label)
    return labels


def rig_scenarios(rig_text):
    return set(re.findall(r'\{"([a-z][a-z0-9-]*)", scn\w+(?:<\d+>)?\}', rig_text))


def label_block(bat_text, label):
    """The lines of one build.bat subroutine, continuation lines (a trailing ^) joined; None when the label is absent."""
    lines = bat_text.replace("\r\n", "\n").split("\n")
    start = next((i for i, l in enumerate(lines) if l.strip().lower() == label.lower() or l.lower().startswith(label.lower() + " ")), None)
    if start is None:
        return None
    out, joined = [], ""
    for line in lines[start + 1:]:
        if line.startswith(":") and not line.startswith("::"):
            break
        if line.rstrip().endswith("^"):
            joined += line.rstrip()[:-1] + " "
            continue
        out.append(joined + line)
        joined = ""
    return out


def find_gen():
    """The generated-headers directory: this checkout's build\\gen, else the main checkout's (a worktree is .claude\\worktrees\\<name>)."""
    for candidate in (ROOT / "build" / "gen", ROOT.parents[2] / "build" / "gen" if len(ROOT.parents) > 2 else None):
        if candidate and candidate.is_dir():
            return candidate
    return None


# ---- the toolchain -----------------------------------------------------------------------------------------------------------
class Toolchain:
    """cl.exe and link.exe by absolute path (Windows looks a bare name up on THIS process's PATH, not on the env passed), with the
    environment they need: this one if cl is on PATH, else the one vcvars64.bat makes, found with vswhere as build.bat does."""

    def __init__(self):
        found = shutil.which("cl.exe")
        if found:
            self.env = os.environ.copy()
        else:
            vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
            vs = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                 "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
            bat = Path(vs) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if not vs or not bat.is_file():
                raise RuntimeError("cl.exe is not on PATH and no Visual Studio with the x64 C++ tools was found")
            dump = subprocess.run('cmd.exe /s /c ""%s" >nul && set"' % bat, capture_output=True, text=True).stdout
            self.env = {}
            for line in dump.splitlines():
                if "=" in line:
                    key, value = line.split("=", 1)
                    self.env[key] = value
            path = next((v for k, v in self.env.items() if k.upper() == "PATH"), "")
            found = shutil.which("cl.exe", path=path)
            if not found:
                raise RuntimeError("vcvars64.bat did not put cl.exe on PATH")
        self.cl = str(found)
        self.link = str(Path(found).with_name("link.exe"))


def compile_obj(tc, source, outdir, includes, obj_name=None):
    """Compile one source to outdir\\<obj_name, else the source's stem>.obj; (exit code, output, obj path)."""
    obj = outdir / ((obj_name or Path(source).stem) + ".obj")
    cmd = [tc.cl] + CL_FLAGS + ["/c"] + ["/I" + str(i) for i in includes] + ["/Fo" + str(obj), str(source)]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, (done.stdout + done.stderr), obj


def link_exe(tc, objs, exe, outdir):
    cmd = [tc.link, "/nologo", "/INCREMENTAL:NO", "/OUT:" + str(exe)] + [str(o) for o in objs] + LIBS
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, (done.stdout + done.stderr)


def run_rig(exe, tc):
    """(outcome, labels, tail) of one run of the rig: 'pass', 'fail', 'crash', 'timeout'."""
    try:
        done = subprocess.run([str(exe), "--self-test"], capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "timeout", [], "no result within %d s" % RUN_TIMEOUT
    out = done.stdout + "\n" + done.stderr
    if done.returncode == 0:
        return "pass", [], ""
    labels = fail_labels(out)
    if not labels:
        return "crash", [], "exit 0x%08X" % (done.returncode & 0xFFFFFFFF)
    return "fail", labels, ""


# ---- the run ---------------------------------------------------------------------------------------------------------------------
def select(only):
    if not only:
        return MUTANTS
    wanted = [n for n in only.split(",") if n]
    unknown = [n for n in wanted if n not in {m.name for m in MUTANTS}]
    if unknown:
        raise ValueError("no such mutation: " + ", ".join(unknown))
    return [m for m in MUTANTS if m.name in wanted]


def plan_text(mutants):
    lines = ["%d mutation(s); each is compiled with the rig in a temp directory outside the repo and must fail on a scenario named:" % len(mutants)]
    for m in mutants:
        lines.append("  %-30s caught by %-28s %s" % (m.name, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, jobs=None, keep=False, dry_run=False, verbose=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    gen = find_gen()
    tc = Toolchain()
    module_text = read_source(MODULE)
    work = Path(tempfile.mkdtemp(prefix="icm_"))
    workers = jobs or min(4, os.cpu_count() or 2)
    try:
        # The control: the unmutated module and the rig, built the way every mutation is, and every common source compiled once.
        base_inc = ([gen] if gen else []) + [SRC]
        common_dir = work / "c"
        common_dir.mkdir()
        control_dir = work / "k"
        control_dir.mkdir()
        (control_dir / "intro_panel.cpp").write_text(module_text, encoding="utf-8", newline="\n")
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(workers, 2)) as pool:
            futures = [pool.submit(compile_obj, tc, c, common_dir, base_inc) for c in COMMON]
            futures.append(pool.submit(compile_obj, tc, control_dir / "intro_panel.cpp", control_dir, [control_dir] + base_inc))
            futures.append(pool.submit(compile_obj, tc, RIG, common_dir, base_inc, "rig"))
            built = [f.result() for f in futures]
        for code, text, obj in built:
            if code != 0:
                print("control: nocompile %s\n%s" % (obj.name, text.strip()[-1500:]), file=out)
                return 1
        common_objs = [b[2] for b in built[:len(COMMON)]]
        module_obj = built[len(COMMON)][2]
        rig_obj = common_dir / "rig.obj"
        code, text = link_exe(tc, [rig_obj, module_obj] + common_objs, control_dir / "rig.exe", control_dir)
        if code != 0:
            print("control: nolink\n" + text.strip()[-1500:], file=out)
            return 1
        outcome, labels, tail = run_rig(control_dir / "rig.exe", tc)
        print("control (the unmutated module, every scenario): %s %s" % (outcome, tail or " ".join(labels[:3])), file=out)
        if outcome != "pass":
            print("the rig does not pass on the unmutated module when built this way; nothing below means anything", file=out)
            return 1

        def one(index, m):
            try:
                mutated = apply_edits(module_text, m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            d = work / ("m%02d" % index)
            d.mkdir()
            (d / "intro_panel.cpp").write_text(mutated, encoding="utf-8", newline="\n")
            code, text, mobj = compile_obj(tc, d / "intro_panel.cpp", d, [d] + base_inc)
            if code != 0:
                return m, "nocompile", (text.strip().splitlines() or [""])[-1]
            exe = d / "rig.exe"
            code, text = link_exe(tc, [rig_obj, mobj] + common_objs, exe, d)
            if code != 0:
                return m, "nolink", (text.strip().splitlines() or [""])[-1]
            outcome, labels, tail = run_rig(exe, tc)
            if outcome == "fail":
                if any(l.startswith(c + ".") for l in labels for c in m.caught):
                    if verbose:
                        return m, "caught", "%d failing label(s): %s" % (len(labels), ", ".join(labels))
                    return m, "caught", "%d failing label(s), first %s" % (len(labels), labels[0])
                return m, "OTHER", "failed on %s only" % ", ".join(sorted({l.split(".")[0] for l in labels}))
            return m, {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT"}[outcome], tail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            for m, verdict, detail in pool.map(lambda im: one(*im), list(enumerate(mutants))):
                results.append((m, verdict, detail))
                print("%-9s %-30s %-24s %s" % (verdict, m.name, "/".join(m.caught)[:24], detail if verbose else detail[:150]), file=out)
        bad = [r for r in results if r[1] != "caught"]
        print("%d mutation(s): %d caught by their own scenario, %d not" % (len(results), len(results) - len(bad), len(bad)), file=out)
        for m, verdict, detail in bad:
            print("  NOT CAUGHT: %s (%s): wanted %s, got %s %s" % (m.name, m.why, "/".join(m.caught), verdict, detail[:120]), file=out)
        return 0 if not bad else 1
    finally:
        if keep:
            print("kept: %s" % work, file=out)
        else:
            shutil.rmtree(work, ignore_errors=True)


# ---- the self-test ------------------------------------------------------------------------------------------------------------------
def self_test(build_bat=BUILD_BAT):
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # the tool's own pieces
    check(fail_labels("a\nFAIL: golden.left-bytes -- x [y]\nFAIL: refuse-w-low.refused\nFAIL: golden: 3 of 143 checks failed, 2 distinct\nFAIL: intro curve: 1 of 45 scenarios failed\n")
          == ["golden.left-bytes", "refuse-w-low.refused"], "fail_labels reads the label of every check's FAIL: line, and not the summaries")
    check(fail_labels("PASS: 12 intro curve checks\n") == [], "fail_labels reads nothing from a pass")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_scenarios('{"golden", scnGolden}, {"refuse-w-low", scnVariant<8>}') == {"golden", "refuse-w-low"}, "rig_scenarios reads the scenario table")

    # every mutation against the module as it is now, and against the rig
    module = read_source(MODULE)
    rig = read_source(RIG)
    scenarios = rig_scenarios(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 14, "the mutation list did not shrink below 14 (%d)" % len(MUTANTS))
    for m in MUTANTS:
        try:
            mutated = apply_edits(module, m.edits, m.name)
            check(mutated != module, "%s changes the module" % m.name)
        except ValueError as error:
            failures.append(str(error))
        for c in m.caught:
            check(c in scenarios, "%s: the rig has no scenario %s" % (m.name, c))
    covered = {c for m in MUTANTS for c in m.caught}
    uncovered = sorted(scenarios - covered - set(NO_MUTANT))
    check(not uncovered, "every scenario of the rig has a mutation or a stated reason it has none; without: %s" % uncovered)
    for s in NO_MUTANT:
        check(s in scenarios, "NO_MUTANT names %s, which the rig does not have" % s)
        check(s not in covered, "%s has a mutation and is also listed as having none" % s)

    # the module still has the sources and headers the rig stubs against
    check(MODULE.is_file() and (SRC / "intro_panel.h").is_file() and (SRC / "intro_upscale.h").is_file(), "the module's headers are where the rig includes them from")

    # build.bat compiles the rig the way this tool does, and runs this tool's self-test
    bat = Path(build_bat).read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        for src in ("tools\\intro_curve_test\\intro_curve_test.cpp", "src\\d3d11\\intro_panel.cpp", "src\\common\\config.cpp", "src\\common\\log.cpp",
                    "src\\common\\guard.cpp", "src\\common\\proxy.cpp"):
            check(src in cl, "build.bat's rig compile has %s (the sources this tool links)" % src)
        check('/I"src\\d3d11"' in cl, "build.bat's rig compile finds the headers through /I src\\d3d11")
        for lib in LIBS:
            check(lib in cl, "build.bat's rig link has %s" % lib)
        check("d3d11.lib" not in cl, "build.bat's rig does not link d3d11.lib (it takes System32's device through src\\common\\system_d3d11.h)")
        check('intro_curve_test.exe" --dry-run' in text and 'intro_curve_test.exe" --self-test' in text, "build.bat runs the rig's --dry-run and --self-test")
        check('mutants.py" --self-test' in text, "build.bat runs this tool's --self-test")

    # --dry-run starts nothing and writes nothing
    calls = []
    real_mkdtemp, real_run = tempfile.mkdtemp, subprocess.run
    tempfile.mkdtemp = lambda *a, **k: calls.append("mkdtemp") or real_mkdtemp(*a, **k)
    subprocess.run = lambda *a, **k: calls.append("subprocess") or real_run(*a, **k)
    try:
        sink = io.StringIO()
        code = run_all(dry_run=True, out=sink)
    finally:
        tempfile.mkdtemp, subprocess.run = real_mkdtemp, real_run
    check(code == 0 and not calls and "dry-run" in sink.getvalue(), "--dry-run starts no process and makes no directory (saw %s)" % calls)
    try:
        select("no-such-mutation")
        check(False, "--only names an unknown mutation")
    except ValueError:
        pass

    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("PASS: intro_curve_test mutants.py self-test (%d mutations over %d scenarios, %d more with a stated reason, every anchor found once, build.bat wired)"
          % (len(MUTANTS), len(covered), len(NO_MUTANT)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--keep", action="store_true", help="leave the temp directory (printed) for a look at a mutant")
    parser.add_argument("--verbose", action="store_true", help="print every label a mutant made the rig fail on, not just the first")
    parser.add_argument("--build-bat", default=str(BUILD_BAT), help="the build.bat the self-test reads the rig's label from")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test(args.build_bat)
    if args.list:
        print(plan_text(MUTANTS))
        return 0
    if args.run:
        try:
            return run_all(only=args.only, jobs=args.jobs or None, keep=args.keep, dry_run=args.dry_run, verbose=args.verbose)
        except (ValueError, RuntimeError) as error:
            print("error: %s" % error, file=sys.stderr)
            return 2
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())

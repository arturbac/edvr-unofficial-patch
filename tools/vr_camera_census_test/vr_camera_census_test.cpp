// The VR camera census's rig (src/d3d11/vr_camera_census_core.h, vr_camera_census.cpp, and the observe-only mode of
// flat_camera_inject.cpp; design doc section 82, "Pre-build findings and the stop").
//
// What runs here is the pure half the DLL compiles (the decisions, the tables, the budget, every line's text), checked
// against the game-camera model the rest of the camera work is checked against (tools\c2_derive_test), plus SOURCE PINS
// for what no rig can run because it needs the game: that with the key off every entry point returns before touching
// anything, that the detour's observe branch ends the call ahead of every write, and that the only store observe mode makes
// is the return slot. Each pin is counted against the same text with the needle removed, so the scan is known to be able to
// fail. A last group holds the log lines the DLL writes to the lines tools\edvr_log.py's --camera-census fixture carries.
//
// --self-test runs everything and prints "vr camera census: PASS" only when every check holds.
// --print-lines prints the golden lines (what the reader's fixture holds). --dry-run runs nothing.
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../c2_derive_test/c2_derive_model.h"
#include "../../src/d3d11/flat_camera_phase.h"
#include "../../src/d3d11/vr_camera_census_core.h"

namespace {

using namespace edvr;

int g_failures = 0;
void check(bool ok, const char* what) {
    if (ok) { std::printf("  ok    %s\n", what); return; }
    ++g_failures;
    std::printf("  FAIL  %s\n", what);
}
bool within(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

std::string slurp(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
unsigned count(const std::string& text, const std::string& needle) {
    unsigned n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}
// The text of the function whose definition line starts with `head`, up to the line "}" that closes it at column 0.
std::string functionBody(const std::string& text, const std::string& head) {
    const size_t at = text.find(head);
    if (at == std::string::npos) return std::string();
    const size_t end = text.find("\n}\n", at);
    return end == std::string::npos ? std::string() : text.substr(at, end + 3 - at);
}
// True when `body` (a functionBody starting with `head`) runs `statements` first, straight after the opening brace.
bool startsWith(const std::string& body, const std::string& head, const std::string& statements) {
    return !body.empty() && body.compare(0, head.size(), head) == 0 && body.compare(head.size(), statements.size(), statements) == 0;
}

// A scripted camera in the game's own layout: the derive model's struct, derived, so its projection and rows are the ones
// the game's functions would leave.
struct Model {
    c2derive::Cam cam;
    Model(uint32_t kind, float fov, float aspect, float bx, float by, float nearZ = 0.025f, float farZ = 50000.0f) {
        c2derive::makeCamera(cam);
        c2derive::camU(cam, c2derive::kCamKind) = kind;
        c2derive::camF(cam, c2derive::kCamAngular) = fov;
        c2derive::camF(cam, 0x260) = aspect;
        c2derive::camF(cam, c2derive::kCamBoundX) = bx;
        c2derive::camF(cam, c2derive::kCamBoundY) = by;
        c2derive::camF(cam, c2derive::kCamNear) = nearZ;
        c2derive::camF(cam, c2derive::kCamFar) = farZ;
        c2derive::derive(cam);
    }
    VrCensusSnap snap() const {
        VrCensusSnap s;
        std::memcpy(s.bytes, cam.b + kVrCensusSnapFrom, kVrCensusSnapBytes);
        return s;
    }
};

// ---------------------------------------------------------------------------
// The key and the decision to run.
// ---------------------------------------------------------------------------
void testKey() {
    std::printf("key\n");
    bool on = true, off = true;
    for (const char* t : {"on", "ON", "On", "oN"}) on &= vrCameraCensusKeyFromText(t) == VrCameraCensusKey::On;
    for (const char* t : {"off", "", "onn", "o", "yes", "1", "true", "auto", " on", "on "}) off &= vrCameraCensusKeyFromText(t) == VrCameraCensusKey::Off;
    check(on, "'on' in any case reads as on");
    check(off && vrCameraCensusKeyFromText(nullptr) == VrCameraCensusKey::Off,
          "everything else reads as off: absent, empty, a typo, 'yes', '1', 'auto', stray spaces (a typo never installs a hook)");
    check(vrCameraCensusWantedFor(true, VrCameraCensusKey::On) && !vrCameraCensusWantedFor(false, VrCameraCensusKey::On) &&
          !vrCameraCensusWantedFor(true, VrCameraCensusKey::Off) && !vrCameraCensusWantedFor(false, VrCameraCensusKey::Off),
          "the census runs only with the key on AND the VR profile: the flat profile never does, key or no key");
    auto ignoresProfile = [](bool, VrCameraCensusKey key) { return key == VrCameraCensusKey::On; };
    check(ignoresProfile(false, VrCameraCensusKey::On) && !vrCameraCensusWantedFor(false, VrCameraCensusKey::On),
          "control: a decision that ignored the profile would run in flat, and the row above sees it");
    check(std::strcmp(vrCameraCensusKeyName(VrCameraCensusKey::On), "on") == 0 &&
          std::strcmp(vrCameraCensusKeyName(VrCameraCensusKey::Off), "off") == 0, "the key's names are the ini's words");
}

// ---------------------------------------------------------------------------
// The camera struct's layout against the model, and the rows the composer writes.
// ---------------------------------------------------------------------------
void testLayout(const std::string& injectCpp) {
    std::printf("layout\n");
    check(kVrCensusAxes == c2derive::kCamAxes && kVrCensusProj == 0x1D0 && kVrCensusFlags == c2derive::kCamFlags &&
          kVrCensusNear == c2derive::kCamNear && kVrCensusFar == c2derive::kCamFar && kVrCensusKind == c2derive::kCamKind &&
          kVrCensusFov == c2derive::kCamAngular && kVrCensusBoundX == c2derive::kCamBoundX &&
          kVrCensusBoundY == c2derive::kCamBoundY && kVrCensusViewportW == c2derive::kCamViewportW &&
          kVrCensusViewportH == c2derive::kCamViewportH && kVrCensusFlagProj == c2derive::kFlagProj,
          "the census's camera offsets are the model's typed table (+0x250 flags, +0x254 near, +0x258 far, +0x264 kind, +0x280 fov, +0x28C bound, +0x2A0 viewport)");
    // The projection block: the model's projSlot(c, i) is helper +0x1B0 + 4 i, camera +0x1D0 + 4 i. The aspect has no named constant; the
    // model's own camera maker writes it at +0x260 and the projection builder reads it there.
    Model m(3, 1.1f, 1.6f, 0.0f, 0.0f);
    const VrCensusSnap s = m.snap();
    check(s.f(kVrCensusAspect) == 1.6f && s.f(kVrCensusFov) == 1.1f && s.u(kVrCensusKind) == 3 &&
          s.f(kVrCensusProj) == *c2derive::projSlot(m.cam, 0) && s.f(kVrCensusProj + 4 * 5) == *c2derive::projSlot(m.cam, 5),
          "a model camera read through the census's snapshot gives its own aspect, fov, kind and projection terms");
    check(kVrCensusSnapFrom + kVrCensusSnapBytes > kVrCensusViewportH + 4 && kVrCensusSnapFrom <= kVrCensusAxes &&
          kVrCensusSnapFrom + kVrCensusSnapBytes > kVrCensusProj + 64,
          "one snapshot of the camera covers every field the census reads");
    // The injector's own constants, read from its source: the kind and the bound pair are the same addresses.
    check(injectCpp.find("constexpr uint32_t kCamKind = 0x264;") != std::string::npos &&
          injectCpp.find("constexpr uint32_t kCamBoundX = 0x28C;") != std::string::npos &&
          injectCpp.find("constexpr uint32_t kCamBoundY = 0x290;") != std::string::npos &&
          injectCpp.find("constexpr uint32_t kCamFlags = 0x250;") != std::string::npos,
          "the injector's kind, bound pair and flag word are the addresses the census reads");
}

void testComposeRows() {
    std::printf("composed rows\n");
    struct Case { uint32_t kind; float fov, aspect, bx, by, nearZ, farZ; };
    const Case cases[] = {
        {3, 1.1f, 1.6f, 0.0f, 0.0f, 0.025f, 50000.0f},          // the flat world camera's shape
        {3, 1.0122f, 5040.0f / 2835.0f, 0.0f, 0.0f, 0.025f, 50000.0f},
        {3, 1.3f, 0.9f, -0.03f, 0.011f, 0.0675f, 50000.0f},     // an asymmetric HMD-eye shape
        {3, 1.2f, 0.93f, 0.0021f, -0.0004f, 0.05f, 20000.0f},
        {0, 0.9f, 1.5f, 0.0f, 0.0f, 0.5f, 100000.0f},
        {0, 1.4f, 1.0f, 0.02f, -0.02f, 0.2f, 5000.0f},
    };
    unsigned exact = 0, total = 0;
    for (const Case& k : cases) {
        Model m(k.kind, k.fov, k.aspect, k.bx, k.by, k.nearZ, k.farZ);
        // Turn the camera: the rows carry the rotation, so a sweep of yaws and pitches moves every term.
        for (int step = 0; step < 4; ++step) {
            float* a = &c2derive::camF(m.cam, c2derive::kCamAxes);
            const float yaw = 0.35f + 0.6f * step, pitch = -0.12f + 0.2f * step;
            const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
            a[0] = cy; a[1] = 0.0f; a[2] = -sy; a[3] = 0.0f;
            a[4] = sy * sp; a[5] = cp; a[6] = cy * sp; a[7] = 0.0f;
            a[8] = sy * cp; a[9] = -sp; a[10] = cy * cp;
            c2derive::camU(m.cam, c2derive::kCamFlags) |= c2derive::kFlagProj | c2derive::kFlagVP;
            c2derive::derive(m.cam);
            float want[16], got[16];
            c2derive::composeSceneCb(m.cam, want);
            const bool ok = vrCensusComposeRows(m.snap(), got);
            ++total;
            if (ok && std::memcmp(want, got, sizeof(want)) == 0) ++exact;
        }
    }
    check(exact == total && total == 24,
          "the census's composed rows are the model's composeSceneCb to the bit, over six cameras (symmetric, asymmetric, kinds 0 and 3) and four turns each");
    Model m(3, 1.1f, 1.6f, 0.0f, 0.0f);
    float rows[16];
    VrCensusSnap dirty = m.snap();
    uint32_t flags = dirty.u(kVrCensusFlags) | kVrCensusFlagProj;
    std::memcpy(dirty.bytes + (kVrCensusFlags - kVrCensusSnapFrom), &flags, 4);
    check(!vrCensusComposeRows(dirty, rows), "a camera whose projection is still dirty after the body has no rows to offer");
    VrCensusSnap poisoned = m.snap();
    const float nan = std::nanf("");
    std::memcpy(poisoned.bytes + (kVrCensusProj - kVrCensusSnapFrom), &nan, 4);
    check(!vrCensusComposeRows(poisoned, rows), "a non-finite projection yields no rows (so nothing is joined on garbage)");
    // What the rows join on: the same camera again matches within 1e-5, another camera does not, the tolerance is a boundary.
    float a[16], b[16], c[16];
    Model eye(3, 1.3f, 0.9f, -0.03f, 0.011f, 0.0675f), world(3, 1.0122f, 5040.0f / 2835.0f, 0.0f, 0.0f);
    vrCensusComposeRows(eye.snap(), a);
    vrCensusComposeRows(eye.snap(), b);
    vrCensusComposeRows(world.snap(), c);
    check(vrCensusRowsMatch(a, b, 1.0e-5f) && !vrCensusRowsMatch(a, c, 1.0e-5f),
          "the join: a camera's rows match themselves and do not match another camera's");
    std::memcpy(b, a, sizeof(a));
    b[7] += 9.0e-6f;
    const bool inside = vrCensusRowsMatch(a, b, 1.0e-5f);
    b[7] = a[7] + 1.1e-5f;
    check(inside && !vrCensusRowsMatch(a, b, 1.0e-5f), "the join's tolerance is 1e-5: 9e-6 apart matches, 1.1e-5 apart does not");
    b[7] = nan;
    check(!vrCensusRowsMatch(a, b, 1.0e-5f), "a non-finite row never matches");
}

void testTangentsAndLeak() {
    std::printf("tangents and the leak measure\n");
    struct Case { float fov, aspect, bx, by; };
    const Case cases[] = {{1.1f, 1.6f, 0.0f, 0.0f}, {1.0122f, 5040.0f / 2835.0f, 0.0f, 0.0f}, {1.3f, 0.9f, -0.03f, 0.011f}, {1.2f, 0.93f, 0.0021f, -0.0004f}};
    bool tangentsOk = true, measureOk = true, expectOk = true;
    for (const Case& k : cases) {
        Model m(3, k.fov, k.aspect, k.bx, k.by);
        VrCensusSig sig;
        vrCensusSigFromSnap(m.snap(), &sig);
        float t[4];
        if (!vrCensusTangents(sig, t)) { tangentsOk = false; continue; }
        const double th = std::tan(static_cast<double>(k.fov * 0.5f)), w = th * k.aspect;
        const double want[4] = {-w * (1 + 2 * k.bx), w * (1 - 2 * k.bx), -th * (1 + 2 * k.by), th * (1 - 2 * k.by)};
        for (int i = 0; i < 4; ++i) if (!within(t[i], want[i], 2.0e-6 * (1 + std::fabs(want[i])))) tangentsOk = false;
        // The eye rows measure (p8, p9) = (2 boundX, 2 boundY), and the expectation built from the tangents agrees.
        float rows[16];
        vrCensusComposeRows(m.snap(), rows);
        float six[6][4] = {};
        std::memcpy(six, rows, sizeof(rows));
        double mx = 0, my = 0, ex = 0, ey = 0;
        if (!flatCameraMeasureRowShift(six, mx, my)) measureOk = false;
        if (!within(mx, 2.0 * k.bx, 1.0e-6) || !within(my, 2.0 * k.by, 1.0e-6)) measureOk = false;
        if (!vrCensusExpectedMeasure(t, 0.0f, 0.0f, &ex, &ey) || !within(ex, mx, 1.0e-6) || !within(ey, my, 1.0e-6)) expectOk = false;
    }
    check(tangentsOk, "a camera's tangents, read from its derived projection, are tan(fov/2) x aspect and tan(fov/2) with the bound shift, four cameras");
    check(measureOk, "flatCameraMeasureRowShift on a camera's composed rows reads (2 boundX, 2 boundY): the off-centre terms");
    check(expectOk, "the expectation built from a camera's own tangents equals what its rows measure");
    VrCensusSig ortho;
    ortho.kind = 1; ortho.p0 = 1; ortho.p5 = 1;
    float t[4];
    check(!vrCensusTangents(ortho, t), "an orthographic camera has no tangents to offer");
    VrCensusSig degenerate;
    degenerate.kind = 3;
    check(!vrCensusTangents(degenerate, t), "a degenerate projection (p0 = 0) has none either");

    // The leak measure, end to end: the eye's frustum as EDVR advertises it, plus the tangent shift, built into a camera,
    // composed, measured; the expectation from the same frustum and shift must agree, and a world phase added to the bound
    // pair (jx/R_w, -jy/R_h, the injector's own rule) must show up as exactly 2 x that in the leak.
    const float frustum[4] = {-1.2f, 0.7f, -0.9f, 1.1f};   // {left, right, down, up}, native_temporal_test's eye 0
    const float shift[2] = {0.00037f, -0.00021f};
    const double l = frustum[0] + shift[0], r = frustum[1] + shift[0], d = frustum[2] + shift[1], u = frustum[3] + shift[1];
    const double wHalf = (r - l) / 2, tanHalf = (u - d) / 2;
    const float bx = static_cast<float>(-(r + l) / (4 * wHalf)), by = static_cast<float>(-(u + d) / (4 * tanHalf));
    const float aspect = static_cast<float>(wHalf / tanHalf), fov = static_cast<float>(2 * std::atan(tanHalf));
    auto leakOf = [&](float extraBx, float extraBy, double* lx, double* ly) {
        Model eye(3, fov, aspect, bx + extraBx, by + extraBy);
        float rows[16];
        vrCensusComposeRows(eye.snap(), rows);
        float six[6][4] = {};
        std::memcpy(six, rows, sizeof(rows));
        double mx = 0, my = 0, sx = 0, sy = 0;
        flatCameraMeasureRowShift(six, mx, my);
        vrCensusExpectedMeasure(frustum, shift[0], shift[1], &sx, &sy);
        *lx = mx - sx;
        *ly = my - sy;
    };
    double lx = 0, ly = 0;
    leakOf(0.0f, 0.0f, &lx, &ly);
    check(within(lx, 0.0, 2.0e-6) && within(ly, 0.0, 2.0e-6),
          "an eye camera built from the advertised frustum and shift leaks nothing: the measured shift is the expected one");
    const float jx = 0.5f, jy = -0.25f;
    leakOf(jx / 5040.0f, -jy / 2835.0f, &lx, &ly);
    check(within(lx, 2.0 * jx / 5040.0, 3.0e-6) && within(ly, 2.0 * (-jy / 2835.0), 3.0e-6) && std::fabs(lx) > 1.0e-4,
          "a world phase of (0.5, -0.25) px at 5040x2835 leaking into the eye camera reads as (1.98e-4, 1.76e-4) of leak: far above the baseline");
    double trueX = 0, trueY = 0;
    const float flat4[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    check(vrCensusExpectedMeasure(frustum, 0.0f, 0.0f, &trueX, &trueY) && within(trueX, 0.5 / 1.9, 1.0e-6) && within(trueY, -0.1, 1.0e-6) &&
          !vrCensusExpectedMeasure(flat4, 0.0f, 0.0f, &trueX, &trueY),
          "the expectation for {-1.2, 0.7, -0.9, 1.1} is (0.2632, -0.1), and a degenerate frustum (zero width) has none");
}

// ---------------------------------------------------------------------------
// The bounded tables.
// ---------------------------------------------------------------------------
VrCensusSig sigOf(uint32_t kind, float aspect, float nearZ, float fov, float bx = 0, float by = 0) {
    VrCensusSig s;
    s.kind = kind; s.aspect = aspect; s.nearZ = nearZ; s.farZ = 50000.0f; s.fov = fov; s.boundX = bx; s.boundY = by;
    s.viewportW = 5040; s.viewportH = 2835; s.p0 = 0.6f; s.p5 = 1.0f; s.p8 = 2 * bx; s.p9 = 2 * by;
    return s;
}

// The call that reports a camera to the table: the frame, where the call fell, and its view and context.
VrCensusCameraTable::Call callAt(uint64_t frame, uint32_t caller = 0, uint32_t ordinal = 1, uint32_t draw = 0, bool drawKnown = false,
                                 VrCensusTone tone = VrCensusTone::None, uintptr_t view = 0, uintptr_t ctx = 0) {
    VrCensusCameraTable::Call c;
    c.frame = frame; c.callerRva = caller; c.ordinal = ordinal; c.draw = draw; c.drawKnown = drawKnown; c.tone = tone;
    c.view = view; c.ctx = ctx;
    return c;
}

void testCameraTable() {
    std::printf("camera table\n");
    VrCensusCameraTable table;
    const float noTan[4] = {};
    const VrCensusSig world = sigOf(3, 1.7778f, 0.025f, 1.0122f);
    check(table.note(0x1000, world, noTan, false, callAt(1, 0x594E13, 1, 0, false, VrCensusTone::None, 0xA000, 0xA100)) == VrCensusCameraTable::Event::New &&
          table.used() == 1 && table.at(0).linePending && table.at(0).calls == 1 && table.at(0).firstFrame == 1,
          "a camera pointer seen for the first time is a new row with its line pending");
    check(table.note(0x1000, world, noTan, false, callAt(1, 0x594E13, 2, 5, true, VrCensusTone::Before, 0xB000, 0xB100)) == VrCensusCameraTable::Event::Known &&
          table.used() == 1 && table.at(0).calls == 2 && table.at(0).firstOrdinal == 1 && table.at(0).firstView == 0xA000 && table.at(0).firstCtx == 0xA100,
          "the same camera again is known: one row, its first call's ordinal, view and context kept");
    check(table.takeCameraLine(0) && !table.takeCameraLine(0), "a first-sight line prints once");
    // Changes.
    VrCensusSig moved = world;
    moved.boundX = 5.0e-8f;   // a rounding: not a change
    check(table.note(0x1000, moved, noTan, false, callAt(2)) == VrCensusCameraTable::Event::Known,
          "a bound that moves by less than 1e-7 is not a change");
    VrCensusSig noisy = world;
    noisy.aspect = std::nextafter(world.aspect, 2.0f);
    noisy.fov = world.fov * (1.0f + 1.0e-7f);
    noisy.viewportW = world.viewportW + 0.001f;
    VrCensusSig realMove = world;
    realMove.aspect = world.aspect * 1.0001f;
    check(!vrCensusSigDiffers(world, noisy) && vrCensusSigDiffers(world, realMove),
          "an aspect or field of view that differs only in its last bits is not a change (an eye camera re-derives them every frame), a 1e-4 move is");
    VrCensusSig eyeMoved = world;
    eyeMoved.boundX = 0.0002f;
    check(table.note(0x1000, eyeMoved, noTan, false, callAt(3)) == VrCensusCameraTable::Event::Changed &&
          table.at(0).changes == 1 && table.at(0).changePending && table.at(0).changeFrame == 3 && table.at(0).changeFrom.boundX == 0.0f,
          "a bound that moves is a change: the row keeps what it was and the frame it moved in");
    VrCensusSig nearMoved = eyeMoved;
    nearMoved.nearZ = 0.03f;
    table.note(0x1000, nearMoved, noTan, false, callAt(4));
    check(table.at(0).changes == 2 && table.at(0).changeFrom.boundX == 0.0f && table.at(0).changeFrame == 3,
          "a second change before the boundary prints keeps the FIRST pending change (what the line will say)");
    // The per-camera cap: four lines, then counted.
    unsigned printed = 0;
    VrCensusSig s = world;
    for (unsigned i = 0; i < 12; ++i) {
        s.nearZ = 0.1f + 0.01f * i;
        table.note(0x1000, s, noTan, false, callAt(10 + i));
        if (table.takeChangeLine(0)) ++printed;
    }
    check(printed + 1 >= kVrCensusMaxChangesPerCamera && table.at(0).changeLines == kVrCensusMaxChangesPerCamera && table.at(0).changes >= 12,
          "a camera prints at most four 'changed:' lines; every change past them is counted in the row");
    // Capacity.
    VrCensusCameraTable full;
    for (uintptr_t i = 0; i < VrCensusCameraTable::kCapacity; ++i)
        full.note(0x10000 + i * 0x100, world, noTan, false, callAt(1));
    check(full.used() == 64 && full.overflow() == 0, "the table holds the first 64 cameras of a session");
    check(full.note(0x99999, world, noTan, false, callAt(1)) == VrCensusCameraTable::Event::Full &&
          full.used() == 64 && full.overflow() == 1 &&
          full.note(0x10000, world, noTan, false, callAt(1)) == VrCensusCameraTable::Event::Known && full.overflow() == 1,
          "a 65th camera is counted in overflow, not held, and a camera already held is still known");
    // The tangents and the first call's place are kept for the line.
    VrCensusCameraTable t2;
    const float tan4[4] = {-0.5f, 0.5f, -0.3f, 0.3f};
    t2.note(0x2000, world, tan4, true, callAt(7, 0x58DE73, 4, 1234, true, VrCensusTone::After, 0xE000, 0xE100));
    check(t2.at(0).tanValid && t2.at(0).tan[1] == 0.5f && t2.at(0).callerRva == 0x58DE73 && t2.at(0).firstDraw == 1234 &&
          t2.at(0).firstTone == VrCensusTone::After && t2.at(0).firstView == 0xE000 && t2.at(0).firstCtx == 0xE100,
          "a new row keeps the tangents, the caller, the draw, the tone and the view and context of its first call");
}

void testFrameBuffer() {
    std::printf("frame buffer\n");
    VrCensusFrame f;
    bool added = true;
    for (uint32_t i = 0; i < VrCensusFrame::kCapacity; ++i) {
        VrCensusCall* c = f.add();
        if (!c) { added = false; break; }
        c->camera = 0x1000 + i;
    }
    check(added && f.calls == 160 && f.recorded == 160 && f.truncated() == 0, "a frame records its first 160 calls");
    check(f.add() == nullptr && f.add() == nullptr && f.calls == 162 && f.recorded == 160 && f.truncated() == 2,
          "a call past 160 is counted, not recorded, and the frame says how many it cut");
    f.toneSeen = true;
    f.reset();
    check(f.calls == 0 && f.recorded == 0 && !f.toneSeen, "a boundary empties the frame and its tone flag");
    f.call[0].rowsValid = true;
    VrCensusCall* c = f.add();
    check(c && !c->rowsValid && c->camera == 0 && c->tone == VrCensusTone::None, "a record handed out is initialised (it never carries the last frame's rows)");
}

void testWindow() {
    std::printf("5 s window\n");
    VrCensusWindow w;
    w.noteCall(true, 3, 0x594E13, 0xA0, VrCensusTone::Before, false, true);    // an injected world call
    w.noteCall(true, 3, 0x594EAB, 0xA0, VrCensusTone::After, false, false);    // not injected (after the trigger)
    w.noteCall(true, 0, 0x594E13, 0xB0, VrCensusTone::After, true, false);
    w.noteCall(false, 0, 0x58DE73, 0xC0, VrCensusTone::None, false, false);
    w.noteCall(true, 9, 0x111111, 0xB0, VrCensusTone::None, false, true);      // (a kind the detour never injects, but the window only counts)
    check(w.calls == 5 && w.stale == 1 && w.kinds[3] == 2 && w.kinds[0] == 1 && w.kinds[7] == 1 && w.kinds[6] == 1 &&
          w.cameraCount == 3 && w.callerCount == 4 && w.toneBefore == 1 && w.toneAfter == 2 && w.toneNone == 2,
          "a window counts calls, stale calls, kinds (other and unreadable apart), distinct cameras and callers, and tone positions");
    check(w.injCalls == 2, "a window counts the calls the observer heard with willInject set (inj-calls)");
    VrCensusWindowText text;
    text.hook = "installed";
    text.camerasTotal = 3;
    char line[kVrCensusLineBytes + 1];
    vrCensusFormatWindow(line, sizeof(line), w, text);
    check(std::strstr(line, "kinds=0:1,3:2,other:1,unreadable:1") != nullptr && std::strstr(line, "callers=+0x594E13:2,+0x594EAB:1,+0x58DE73:1,+0x111111:1") != nullptr &&
          std::strstr(line, "cameras-seen=3 cameras-total=3 tone=1/2/2") != nullptr && std::strstr(line, " stale=1 inj-calls=2 kinds=") != nullptr,
          "the 5 s line names the kinds and the callers busiest first, and the tone split, and the injected calls beside the stale ones");
    VrCensusWindow empty;
    vrCensusFormatWindow(line, sizeof(line), empty, VrCensusWindowText{});
    check(std::strstr(line, "frames=0 calls=0 posts=0 off-thread=0 stale=0 inj-calls=0 kinds=- callers=- cameras-seen=0") != nullptr &&
          std::strstr(line, "progress=no hook=pending windows=0") != nullptr,
          "an empty window still prints every field (zeros included): an absent line is what 'the census never ran' looks like");
    // The worst case still fits a log line: sixteen callers, every kind, six-digit counts.
    VrCensusWindow big;
    for (uint32_t i = 0; i < 40; ++i) big.noteCall(i % 9 != 8, i % 9, 0x594E13 + i, 0x1000 + i, static_cast<VrCensusTone>(i % 3), (i & 1) != 0, (i % 3) == 0);
    big.frames = big.calls = big.posts = big.injCalls = 99999999999ull;
    big.onFootFrames = big.eyeDraws = big.eyeOnFoot = 99999999999ull;
    big.windows = 12;
    VrCensusWindowText bigText;
    bigText.hook = "installed"; bigText.offThread = 99999999999ull; bigText.offThreadOverflow = 99999999999ull;
    bigText.camerasTotal = 64; bigText.cameraTableOverflow = 99999999999ull;
    const int n = vrCensusFormatWindow(line, sizeof(line), big, bigText);
    check(n > 0 && static_cast<size_t>(n) <= kVrCensusLineBytes && std::strlen(line) <= kVrCensusLineBytes && std::strstr(line, ",+more:") != nullptr,
          "the worst-case 5 s line (forty callers, every kind, eleven-digit counts) is at most 400 characters and says how many callers it left out");
    // Thinning: every window for the first three minutes, then one line per twelve windows covering all of them.
    unsigned early = 0;
    for (uint32_t t = 1; t <= kVrCensusEveryWindow; ++t) if (vrCensusWindowPrints(t)) ++early;
    unsigned hour = 0;
    for (uint32_t t = 1; t <= 720; ++t) if (vrCensusWindowPrints(t)) ++hour;
    check(early == kVrCensusEveryWindow && !vrCensusWindowPrints(kVrCensusEveryWindow + 1) && vrCensusWindowPrints(48) &&
          !vrCensusWindowPrints(47) && vrCensusWindowPrints(720) && hour == 36 + 57,
          "the first 36 windows (three minutes) each print; after that one in twelve (a minute), so an hour is 93 lines");
    bool fits = true;
    VrCensusBudget b;
    for (uint32_t t = 1; t <= 720 * 2; ++t) if (vrCensusWindowPrints(t)) fits &= b.take(VrCensusLines::Window) || t > 720;
    check(fits && b.used[static_cast<size_t>(VrCensusLines::Window)] <= VrCensusBudget::kCap[static_cast<size_t>(VrCensusLines::Window)],
          "the first hour of 5 s lines fits the class's cap of 100");
}

void testBudget() {
    std::printf("line budget\n");
    VrCensusBudget b;
    unsigned taken = 0;
    while (b.take(VrCensusLines::Camera)) ++taken;
    check(taken == 64 && b.suppressed[static_cast<size_t>(VrCensusLines::Camera)] == 1 && b.take(VrCensusLines::Changed),
          "a class is capped on its own (cameras 64) and a full class does not starve the others");
    unsigned calls = 0;
    while (b.take(VrCensusLines::Call)) ++calls;
    check(calls == 480 && VrCensusBudget::capTotal() == 100 + 64 + 24 + 480 + 16 + 16 + 24,
          "call lines are capped at 480 (three sequences of the 160-call frame) and the caps add up to 724");
    std::printf("  note  a hard worst case of %u lines, about 400 in a real session (see the budget's comment)\n", VrCensusBudget::capTotal());
    // A realistic session: ten minutes, twenty cameras, three sequences of 110 calls, eight eye draws.
    VrCensusBudget real;
    unsigned session = 0;
    for (uint32_t t = 1; t <= 120; ++t) if (vrCensusWindowPrints(t) && real.take(VrCensusLines::Window)) ++session;
    for (int i = 0; i < 20; ++i) if (real.take(VrCensusLines::Camera)) ++session;
    for (int i = 0; i < 3 * (1 + 110); ++i) if (real.take(VrCensusLines::Call)) ++session;
    for (int i = 0; i < 16; ++i) if (real.take(VrCensusLines::Eye)) ++session;
    for (int i = 0; i < 6; ++i) if (real.take(VrCensusLines::Info)) ++session;
    check(session >= 380 && session <= 430, "a ten-minute session that reaches on-foot (twenty cameras, 110-call frames) logs about 400 lines");
    std::printf("  note  that session: %u lines\n", session);
    check(vrCensusPrintsSequence(true, 0) && vrCensusPrintsSequence(true, 2) && !vrCensusPrintsSequence(true, 3) && !vrCensusPrintsSequence(false, 0),
          "a call sequence prints for an on-foot frame (the tone was seen), the first three only");
    VrCensusEyeBudget eye;
    bool ok = true;
    for (uint64_t frame = 10; frame < 14; ++frame) { ok &= eye.take(frame); ok &= eye.take(frame); }
    check(ok && eye.draws() == 8 && eye.frames() == 4 && !eye.take(10) && !eye.take(99),
          "eye draws are read back for the first four on-foot frames, eight draws in all, and a ninth or a fifth frame is refused");
    VrCensusEyeBudget early;
    bool e = early.take(5) && early.take(5) && early.take(5) && early.take(6) && early.take(7) && early.take(8) && !early.take(9);
    check(e && early.draws() == 6, "three draws in one frame still count against the frame allowance of four, not its own");
}

void testOffThread() {
    std::printf("calls on other threads\n");
    VrCensusOffThread t;
    t.note(11, 0x1000, 3, true, 0x594E13);
    t.note(11, 0x1000, 3, true, 0x594E13);
    t.note(12, 0x1000, 3, true, 0x594E13);
    t.note(11, 0x2000, 0, false, 0x58DE73);
    check(t.total() == 4 && t.used() == 3 && t.overflow() == 0, "a repeated tuple is one entry; another thread, another camera or caller is another");
    VrCensusOffThread::Entry e;
    uint64_t seen = 0;
    unsigned entries = 0;
    bool hasUnreadable = false;
    while (t.takeNew(&e)) { ++entries; seen += e.calls; hasUnreadable |= !e.kindReadable && e.camera == 0x2000; }
    check(entries == 3 && seen == 4 && hasUnreadable && !t.takeNew(&e), "the boundary takes each entry once, with its count, and an unreadable kind says so");
    for (uint32_t i = 0; i < 40; ++i) t.note(100 + i, 0x5000 + i, 3, true, 0x594E13);
    check(t.used() == VrCensusOffThread::kCapacity && t.overflow() == 40 - (VrCensusOffThread::kCapacity - 3) && t.total() == 44,
          "the table holds sixteen tuples; the calls it cannot name are counted in overflow");
    // Concurrency: four threads, one camera each, ten thousand calls each; the totals are exact and every call is somewhere.
    VrCensusOffThread c;
    std::vector<std::thread> threads;
    for (uint32_t k = 0; k < 4; ++k)
        threads.emplace_back([&c, k] { for (int i = 0; i < 10000; ++i) c.note(500 + k, 0x9000 + k, 3, true, 0x594E13 + (i & 1)); });
    for (auto& th : threads) th.join();
    uint64_t sum = 0;
    VrCensusOffThread::Entry x;
    while (c.takeNew(&x)) sum += x.calls;
    check(c.total() == 40000 && sum + c.overflow() == 40000 && c.used() == 8,
          "four threads noting ten thousand calls each lose none: the entries' counts and the overflow add to the total");
}

// ---------------------------------------------------------------------------
// The text of every line.
// ---------------------------------------------------------------------------
struct Golden { const char* name; std::string text; };

// The lines tools\edvr_log.py's --camera-census fixture carries are built here from fixed inputs, so a drift of either
// side breaks a build: this rig pins the exact text, and the next group finds each line in the reader's own source.
std::vector<Golden> goldenLines() {
    std::vector<Golden> out;
    char line[kVrCensusLineBytes + 1];
    auto add = [&](const char* name) { out.push_back({name, line}); };

    VrCensusWindow w;
    w.frames = 450; w.calls = 10012; w.posts = 10012; w.stale = 0; w.injCalls = 6750;
    w.kinds[0] = 1800; w.kinds[1] = 600; w.kinds[3] = 7612;
    w.callers[0] = {0x594E13, 3337}; w.callers[1] = {0x594EAB, 3337}; w.callers[2] = {0x594FE1, 3337}; w.callers[3] = {0x58DE73, 1}; w.callerCount = 4;
    w.cameraCount = 14; w.toneBefore = 9000; w.toneAfter = 1012; w.toneNone = 0;
    w.toneFrames = 450; w.onFootFrames = 450; w.eyeDraws = 900; w.eyeOnFoot = 900; w.windows = 1; w.progressSeen = true;
    VrCensusWindowText text;
    text.hook = "installed"; text.camerasTotal = 14; text.foot = VrCensusFoot::Yes;
    vrCensusFormatWindow(line, sizeof(line), w, text);
    add("window");

    VrCensusCamera world;
    world.camera = 0x241dc2e2960;
    world.firstSig = sigOf(3, 5040.0f / 2835.0f, 0.025f, 1.0122f);
    world.firstSig.p0 = 0.9f; world.firstSig.flags = 0;
    world.tanValid = true; world.tan[0] = -0.5625f; world.tan[1] = 0.5625f; world.tan[2] = -0.3164f; world.tan[3] = 0.3164f;
    world.callerRva = 0x594E13; world.firstOrdinal = 1; world.firstDraw = 0; world.firstDrawKnown = true;
    world.firstTone = VrCensusTone::Before; world.firstFrame = 1;
    world.firstView = 0x241dd00a000; world.firstCtx = 0x241dd00a100;
    vrCensusFormatCamera(line, sizeof(line), world);
    add("camera-world");

    VrCensusCamera eye;
    eye.camera = 0x241df6d0bb0;
    eye.firstSig = sigOf(3, 0.9f, 0.05f, 1.3f, -0.03f, 0.011f);
    eye.firstSig.viewportW = 2620; eye.firstSig.viewportH = 2533;
    eye.tanValid = true; eye.tan[0] = -1.2f; eye.tan[1] = 0.7f; eye.tan[2] = -0.9f; eye.tan[3] = 1.1f;
    eye.callerRva = 0x594FE1; eye.firstOrdinal = 98; eye.firstDraw = 8210; eye.firstDrawKnown = true;
    eye.firstTone = VrCensusTone::After; eye.firstFrame = 1;
    eye.firstView = 0x241dd00e000; eye.firstCtx = 0x241dd00e100;
    vrCensusFormatCamera(line, sizeof(line), eye);
    add("camera-eye");

    VrCensusCamera moved = eye;
    moved.changeFrom = eye.firstSig;
    moved.sig = eye.firstSig;
    moved.sig.boundX = -0.0297f; moved.sig.nearZ = 0.06f;
    moved.changeFrame = 2; moved.changes = 7;
    vrCensusFormatChanged(line, sizeof(line), moved);
    add("changed");

    const VrCensusPhase jitter{true, 0.252f, -0.126f};
    vrCensusFormatSequence(line, sizeof(line), 4, 1, VrCensusFoot::Yes, jitter, 107, 107);
    add("sequence");
    vrCensusFormatSequence(line, sizeof(line), 5, 2, VrCensusFoot::Off, VrCensusPhase{}, 107, 100);
    add("sequence-no-route");

    VrCensusCall call;
    call.camera = 0x241dc2e2960; call.view = 0x241dd00a000; call.kind = 3; call.kindReadable = true; call.callerRva = 0x594E13; call.draw = 6500; call.drawKnown = true;
    call.tone = VrCensusTone::Before; call.preFlags = 0x1C; call.postFlags = 0; call.postSeen = true; call.rowsValid = true;
    call.willInject = true; call.role = 0;
    const float rowsWorld[16] = {0.5625f, 0, 0, 0.8f, 0, 1.0f, 0, -0.1f, 0.2f, 0, 1.0f, 0.6f, 0, 0, 0.025f, 0};
    std::memcpy(call.rows, rowsWorld, sizeof(rowsWorld));
    vrCensusFormatCall(line, sizeof(line), 4, 1, call);
    add("call-world");
    VrCensusCall firstPerson = call;
    firstPerson.role = 1;
    vrCensusFormatCall(line, sizeof(line), 4, 13, firstPerson);
    add("call-first-person");
    VrCensusCall aux = call;
    aux.willInject = false; aux.role = 2; aux.callerRva = 0x58DE73;
    vrCensusFormatCall(line, sizeof(line), 4, 14, aux);
    add("call-aux");
    VrCensusCall eyeCall = call;
    eyeCall.camera = 0x241df6d0bb0; eyeCall.view = 0x241dd00e000; eyeCall.callerRva = 0x594FE1; eyeCall.draw = 8210; eyeCall.tone = VrCensusTone::After;
    eyeCall.kind = 5; eyeCall.willInject = false; eyeCall.role = kVrCensusRoleNone;
    const float rowsEye[16] = {1.1f, 0, 0, 0.8f, 0, 1.0f, 0, -0.1f, 0.2f, 0, 1.0f, 0.6f, 0, 0, 0.05f, 0};
    std::memcpy(eyeCall.rows, rowsEye, sizeof(rowsEye));
    vrCensusFormatCall(line, sizeof(line), 4, 98, eyeCall);
    add("call-eye");
    VrCensusCall ortho = call;
    ortho.camera = 0x241de000100; ortho.view = 0x241dd00c000; ortho.kind = 1; ortho.callerRva = 0x58DE73; ortho.draw = 0; ortho.drawKnown = false;
    ortho.tone = VrCensusTone::None; ortho.postSeen = false; ortho.rowsValid = false; ortho.willInject = false; ortho.role = kVrCensusRoleNone;
    vrCensusFormatCall(line, sizeof(line), 4, 2, ortho);
    add("call-ortho");

    vrCensusFormatEye(line, sizeof(line), 0, 4, VrCensusFoot::Yes, jitter, true, 8213, 0x1eb2e751e20ull, 0, 5376, rowsEye, true, 0.0002, -0.0001, nullptr);
    add("eye");
    vrCensusFormatEye(line, sizeof(line), 1, 4, VrCensusFoot::Yes, VrCensusPhase{}, true, 8220, 0x1eb2e751e20ull, 0, 5376, nullptr, false, 0, 0, "map");
    add("eye-failed");
    const float frustum[4] = {-1.2f, 0.7f, -0.9f, 1.1f}, shift[2] = {0.0f, 0.0f};
    double trueX = 0, trueY = 0;
    vrCensusExpectedMeasure(frustum, 0.0f, 0.0f, &trueX, &trueY);   // the rows of an unshifted, unleaked eye measure exactly this
    vrCensusFormatEyeGeometry(line, sizeof(line), 0, 4, true, 4711, frustum, shift, true, trueX, trueY);
    add("eye-geometry");
    vrCensusFormatEyeGeometry(line, sizeof(line), 1, 4, false, 0, nullptr, nullptr, false, 0, 0);
    add("eye-geometry-unavailable");

    VrCensusOffThread::Entry other;
    other.thread = 4321; other.camera = 0x241dc2e2960; other.kind = 3; other.kindReadable = true; other.callerRva = 0x594E13; other.calls = 57;
    vrCensusFormatOtherThread(line, sizeof(line), other);
    add("other-thread");
    return out;
}

void testFormats() {
    std::printf("line text\n");
    const std::vector<Golden> g = goldenLines();
    auto at = [&](const char* name) -> std::string {
        for (const Golden& x : g) if (std::strcmp(x.name, name) == 0) return x.text;
        return std::string("<missing ") + name + ">";
    };
    check(at("window") ==
              "vr camera census 5s: frames=450 calls=10012 posts=10012 off-thread=0 stale=0 inj-calls=6750 kinds=0:1800,1:600,3:7612 "
              "callers=+0x594E13:3337,+0x594EAB:3337,+0x594FE1:3337,+0x58DE73:1 cameras-seen=14 cameras-total=14 tone=9000/1012/0 "
              "tone-frames=450 on-foot-frames=450 foot=yes eye-draws=900/900 progress=yes hook=installed windows=1 cam-overflow=0 thread-overflow=0",
          "the 5 s line: frames, calls, posts, off-thread, stale, injected calls, kinds, callers, cameras, tone split, tone frames, sampled frames, the journal, eye draws, progress, hook");
    check(at("camera-world") ==
              "vr camera census: camera=0x241dc2e2960 kind=3 caller=+0x594E13 thread=owner aspect=1.777778 near=0.025 far=50000 fov=1.0122 "
              "bound=(0,0) offcentre=(0,0) viewport=(5040,2835) tan=(-0.5625,0.5625,-0.3164,0.3164) view=0x241dd00a000 vctx=0x241dd00a100 "
              "first-call=1 draw=0 tone=before frame=1",
          "the per-camera line: kind, caller, thread, aspect, near, far, fov, bound, off-centre, viewport, tangents, view, context, first call, draw, tone, frame");
    check(at("camera-eye") ==
              "vr camera census: camera=0x241df6d0bb0 kind=3 caller=+0x594FE1 thread=owner aspect=0.9 near=0.05 far=50000 fov=1.3 "
              "bound=(-0.03,0.011) offcentre=(-0.06,0.022) viewport=(2620,2533) tan=(-1.2,0.7,-0.9,1.1) view=0x241dd00e000 vctx=0x241dd00e100 "
              "first-call=98 draw=8210 tone=after frame=1",
          "an asymmetric eye camera's line shows its bound pair and off-centre terms");
    check(at("changed") ==
              "vr camera census: changed: camera=0x241df6d0bb0 frame=2 n=7 near=0.05->0.06 bound=(-0.03,0.011)->(-0.0297,0.011)",
          "the changed line names only the fields that moved, old->new");
    check(at("sequence") == "vr camera census: sequence frame=4 index=1/3 foot=yes phase=0.2520,-0.1260 calls=107 recorded=107 truncated=0",
          "the sequence header names what the journal said and the phase the route chose for the frame (render pixels, four decimals)");
    check(at("sequence-no-route") == "vr camera census: sequence frame=5 index=2/3 foot=off phase=- calls=107 recorded=100 truncated=7",
          "a frame the route was not jittering has phase=-: a zero phase of a warm-up frame (0.0000,0.0000) and no route at all read differently");
    check(at("call-world") ==
              "vr camera census: call frame=4 n=1 camera=0x241dc2e2960 kind=3 caller=+0x594E13 draw=6500 tone=before inj=1 role=scene fl=0x1C>0x0 "
              "view=0x241dd00a000 rows=[0.5625,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.025,0]",
          "a call line: ordinal, camera, kind, caller, draw, tone, what the detour decided (inj, role), dirty flags before>after, the view, the sixteen composed floats");
    check(at("call-first-person") ==
              "vr camera census: call frame=4 n=13 camera=0x241dc2e2960 kind=3 caller=+0x594E13 draw=6500 tone=before inj=1 role=fp fl=0x1C>0x0 "
              "view=0x241dd00a000 rows=[0.5625,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.025,0]",
          "the first-person role prints as fp");
    check(at("call-aux") ==
              "vr camera census: call frame=4 n=14 camera=0x241dc2e2960 kind=3 caller=+0x58DE73 draw=6500 tone=before inj=0 role=aux fl=0x1C>0x0 "
              "view=0x241dd00a000 rows=[0.5625,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.025,0]",
          "an excluded kind-3 call prints inj=0 role=aux");
    check(at("call-eye") ==
              "vr camera census: call frame=4 n=98 camera=0x241df6d0bb0 kind=5 caller=+0x594FE1 draw=8210 tone=after inj=0 role=- fl=0x1C>0x0 "
              "view=0x241dd00e000 rows=[1.1,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.05,0]",
          "an eye camera's call (kind 5), after the tone, is never injected and has no role");
    check(at("call-ortho") ==
              "vr camera census: call frame=4 n=2 camera=0x241de000100 kind=1 caller=+0x58DE73 draw=- tone=none inj=0 role=- fl=0x1C>- view=0x241dd00c000 rows=-",
          "a call with no draw progress, no post half and no rows prints dashes, never a guess");
    check(at("eye") ==
              "vr camera census: eye=0 frame=4 foot=yes phase=0.2520,-0.1260 draw=8213 b1=0x1eb2e751e20 first=0 bytes=5376 "
              "rows=[1.1,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.05,0] meas=(0.0002,-0.0001)",
          "an eye draw's line: the frame's phase, the b1 buffer, its rows 270..273, the measured shift");
    check(at("eye-failed") == "vr camera census: eye=1 frame=4 foot=yes phase=- draw=8220 b1=0x1eb2e751e20 first=0 bytes=5376 rows=- meas=- why=map",
          "a failed readback says why and never prints rows");
    check(at("eye-geometry") ==
              "vr camera census: eye-geometry eye=0 frame=4 seq=4711 frustum=[-1.2,0.7,-0.9,1.1] shift=(0,0) expect=(0.2631579,-0.1) "
              "expect-shifted=(0.2631579,-0.1) leak=(0.000e+00,0.000e+00)",
          "the eye geometry line: the advertised frustum and shift, the expectation and the leak");
    check(at("eye-geometry-unavailable") == "vr camera census: eye-geometry eye=1 frame=4 geometry=unavailable",
          "no advertised geometry is said, not invented");
    check(at("other-thread") == "vr camera census: other-thread tid=4321 camera=0x241dc2e2960 kind=3 caller=+0x594E13 calls=57",
          "a call on another thread: thread, camera, kind, caller, count");
    bool lengthOk = true;
    for (const Golden& x : g) if (x.text.size() > kVrCensusLineBytes) lengthOk = false;
    check(lengthOk, "every line above is at most 400 characters");
    // The worst case of the long lines at realistic maxima: sixteen floats at their widest (-1.234568e-05), a user-mode pointer
    // for the camera and the view (twelve hex digits), a caller inside a 256 MB image, a six-digit draw, nine-digit frame.
    VrCensusCall worst;
    worst.camera = 0x7FFFFFFFFFFFull; worst.view = 0x7FFFFFFFFFFFull; worst.kind = 5; worst.kindReadable = true; worst.callerRva = 0xFFFFFFF;
    worst.draw = 999999; worst.drawKnown = true; worst.tone = VrCensusTone::Before; worst.preFlags = worst.postFlags = 0xFF; worst.postSeen = worst.rowsValid = true;
    worst.willInject = true; worst.role = 0;   // the longest words the new tokens take: inj=1 role=scene
    for (int i = 0; i < 16; ++i) worst.rows[i] = -1.2345678e-05f * (i + 1);
    char line[kVrCensusLineBytes + 1];
    const int n = vrCensusFormatCall(line, sizeof(line), 999999999ull, 160, worst);
    check(n > 0 && std::strlen(line) <= kVrCensusLineBytes && std::strstr(line, "rows=[") != nullptr && line[std::strlen(line) - 1] == ']' &&
              std::strstr(line, " inj=1 role=scene ") != nullptr,
          "the widest realistic call line (pointers, a long frame number, inj and role, and sixteen long floats) still fits 400 characters whole");
    std::printf("  note  widest realistic call line: %zu characters\n", std::strlen(line));
    // The eye line at its widest realistic: sixteen long floats, a b1 pointer, a long frame, the journal's longest word, a phase of a
    // pixel either way, and the measured shift at its longest.
    {
        float eyeRows[16];
        for (int i = 0; i < 16; ++i) eyeRows[i] = -1.2345678e-05f * (i + 1);
        const VrCensusPhase wide{true, -0.9999f, -0.9999f};
        char eyeLine[kVrCensusLineBytes + 1];
        const int en = vrCensusFormatEye(eyeLine, sizeof(eyeLine), 1, 999999999ull, VrCensusFoot::Unknown, wide, true, 999999, 0x7FFFFFFFFFFFull, 0,
                                         16777216, eyeRows, true, -1.2345678e-05, -1.2345678e-05, nullptr);
        check(en > 0 && std::strlen(eyeLine) <= kVrCensusLineBytes && eyeLine[std::strlen(eyeLine) - 1] == ')' && std::strstr(eyeLine, "phase=-0.9999,-0.9999 ") != nullptr,
              "the widest realistic eye line (phase included) still fits 400 characters whole, its measured shift last");
        std::printf("  note  widest realistic eye line: %zu characters\n", std::strlen(eyeLine));
        char seqLine[kVrCensusLineBytes + 1];
        const int sn = vrCensusFormatSequence(seqLine, sizeof(seqLine), 999999999ull, 3, VrCensusFoot::Unknown, wide, 160, 160);
        check(sn > 0 && std::strlen(seqLine) <= kVrCensusLineBytes && std::strstr(seqLine, " phase=-0.9999,-0.9999 calls=160 recorded=160 truncated=0") != nullptr,
              "a sequence header at its widest fits, and ends with its counts");
        const VrCensusPhase odd[] = {{true, -0.00002f, 0.00004f}, {true, std::nanf(""), 0.5f}, {true, -0.0f, 0.0f}};
        char oddLine[kVrCensusLineBytes + 1];
        vrCensusFormatSequence(oddLine, sizeof(oddLine), 1, 1, VrCensusFoot::Yes, odd[0], 1, 1);
        const bool tiny = std::strstr(oddLine, " phase=0.0000,0.0000 ") != nullptr;
        vrCensusFormatSequence(oddLine, sizeof(oddLine), 1, 1, VrCensusFoot::Yes, odd[1], 1, 1);
        const bool notANumber = std::strstr(oddLine, " phase=nan,0.5000 ") != nullptr;
        vrCensusFormatSequence(oddLine, sizeof(oddLine), 1, 1, VrCensusFoot::Yes, odd[2], 1, 1);
        const bool negZero = std::strstr(oddLine, " phase=0.0000,0.0000 ") != nullptr && std::strstr(oddLine, "-0.0000") == nullptr;
        check(tiny && notANumber && negZero,
              "a phase that rounds to zero prints 0.0000 (never -0.0000), a jittering warm-up frame's zero is 0.0000,0.0000, and a NaN axis prints nan");
    }
    worst.rows[3] = std::nanf("");
    vrCensusFormatCall(line, sizeof(line), 1, 1, worst);
    check(std::strstr(line, ",nan,") != nullptr && std::strstr(line, "-nan") == nullptr && std::strstr(line, "(ind)") == nullptr,
          "a non-finite float prints as 'nan' (the compiler's spelling of a NaN never reaches the log)");
    // A line never holds a space inside a (..) or [..] value, so the reader can split on spaces.
    bool noSpaces = true;
    for (const Golden& x : g) {
        int depth = 0;
        for (char ch : x.text) { if (ch == '(' || ch == '[') ++depth; else if (ch == ')' || ch == ']') --depth; else if (ch == ' ' && depth > 0) noSpaces = false; }
    }
    check(noSpaces, "no value holds a space inside its brackets: a line splits into key=value tokens");
}

// ---------------------------------------------------------------------------
// The glue, without D3D: the same calls in the same order the DLL makes, through the core's own tables, so the order of the
// log and every cap is exercised end to end.
// ---------------------------------------------------------------------------
struct Sim {
    VrCensusFrame frame;
    VrCensusCameraTable cameras;
    VrCensusBudget budget;
    VrCensusFoot foot = VrCensusFoot::Yes;   // what the journal says; the glue reads it once a boundary
    VrCensusPhase phase;                     // what the route chose for the frame in progress, latched at the boundary that opened it
    bool progressAvailable = true;           // false: vrWorldRouteDrawProgress answers false (the skeleton, a route that does not watch)
    uint32_t sequencesLogged = 0;
    uint64_t frameNo = 1, toneFrames = 0, sampledFrames = 0;
    std::vector<std::string> log;
    void say(VrCensusLines c, const char* line) { if (budget.take(c)) log.emplace_back(line); }
    void call(uintptr_t camera, uint32_t kind, uint32_t caller, bool tone, const VrCensusSig& sig, bool inject = false,
              uint8_t role = kVrCensusRoleNone) {
        const bool recording = vrCensusMayRecord(foot, phase) && vrCensusPrintsSequence(true, sequencesLogged);
        VrCensusCall* rec = recording ? frame.add() : (++frame.calls, nullptr);
        if (progressAvailable) { frame.progress = true; if (tone) frame.toneSeen = true; }
        const VrCensusTone where = !progressAvailable ? VrCensusTone::None : tone ? VrCensusTone::After : VrCensusTone::Before;
        if (rec) { rec->camera = camera; rec->view = camera + 0x5000; rec->kind = kind; rec->kindReadable = true; rec->callerRva = caller;
                   rec->drawKnown = progressAvailable; rec->draw = frame.calls; rec->tone = where; rec->postSeen = true; rec->rowsValid = true;
                   rec->willInject = inject; rec->role = role; }
        const float noTan[4] = {};
        cameras.note(camera, sig, noTan, false, callAt(frameNo, caller, frame.calls, frame.calls, progressAvailable, where,
                                                       camera + 0x5000, camera + 0x5100));
    }
    // The Present boundary: the frame that ended is judged under the phase latched when it began, then the next frame's is latched.
    void boundary(const VrCensusPhase& next = VrCensusPhase{}) {
        char line[kVrCensusLineBytes + 1];
        const bool sampled = vrCensusSamplesFrame(frame.toneSeen, frame.progress, foot, phase);
        if (frame.toneSeen) ++toneFrames;
        if (sampled) ++sampledFrames;
        for (size_t i = 0; i < cameras.used(); ++i) {
            if (cameras.takeCameraLine(i)) { vrCensusFormatCamera(line, sizeof(line), cameras.at(i)); say(VrCensusLines::Camera, line); }
            if (cameras.takeChangeLine(i)) { vrCensusFormatChanged(line, sizeof(line), cameras.at(i)); say(VrCensusLines::Changed, line); }
        }
        if (vrCensusPrintsSequence(sampled, sequencesLogged) && frame.recorded > 0) {
            ++sequencesLogged;
            vrCensusFormatSequence(line, sizeof(line), frameNo, sequencesLogged, foot, phase, frame.calls, frame.recorded);
            say(VrCensusLines::Call, line);
            for (uint32_t i = 0; i < frame.recorded; ++i) { vrCensusFormatCall(line, sizeof(line), frameNo, i + 1, frame.call[i]); say(VrCensusLines::Call, line); }
        }
        frame.reset();
        ++frameNo;
        phase = next;
    }
};

void testSession() {
    std::printf("a scripted session through the core\n");
    Sim sim;
    const VrCensusSig world = sigOf(3, 1.7778f, 0.025f, 1.0122f), eye = sigOf(3, 0.9f, 0.05f, 1.3f, -0.03f, 0.011f), ui = sigOf(1, 1.0f, 0.1f, 0.0f);
    // Ten frames in the cockpit (no tone), then six on foot: world calls before the tone, eye calls after it.
    sim.foot = VrCensusFoot::No;
    for (int f = 0; f < 10; ++f) { for (int i = 0; i < 40; ++i) sim.call(0x1000, 3, 0x594E13, false, world); sim.boundary(); }
    const size_t cockpitLines = sim.log.size();
    check(cockpitLines == 1 && sim.sequencesLogged == 0, "frames without the tone print only the camera's first-sight line, never a call sequence");
    // Ten more in the cockpit WITH the tone (the detector fires for every frame the game draws the same chain): the journal
    // says not on foot, so nothing is recorded and no sequence is spent on them.
    for (int f = 0; f < 10; ++f) {
        for (int i = 0; i < 30; ++i) sim.call(0x1000, 3, 0x594E13, false, world);
        for (int i = 0; i < 4; ++i) sim.call(0x3000, 3, 0x594FE1, true, eye);
        check(sim.frame.recorded == 0 && sim.frame.calls == 34, "a cockpit frame with the tone counts its calls and records none");
        sim.boundary();
    }
    check(sim.sequencesLogged == 0 && sim.toneFrames == 10 && sim.sampledFrames == 0,
          "ten cockpit frames with the tone draw are tone frames and none is sampled: the journal's word keeps the budget for the commander on foot");
    sim.foot = VrCensusFoot::Unknown;
    for (int i = 0; i < 4; ++i) sim.call(0x3000, 3, 0x594FE1, true, eye);
    sim.boundary();
    check(sim.sequencesLogged == 0 && sim.sampledFrames == 0, "a menu frame (the journal read, no Flags2) is not sampled either");
    sim.foot = VrCensusFoot::Yes;
    for (int f = 0; f < 6; ++f) {
        for (int i = 0; i < 60; ++i) sim.call(0x1000, 3, i % 3 ? 0x594EAB : 0x594E13, false, world);
        for (int i = 0; i < 5; ++i) sim.call(0x2000, 3, 0x594FE1, false, ui);   // a kind-1 camera drawn before the tone
        for (int i = 0; i < 4; ++i) sim.call(0x3000, 3, 0x594FE1, true, f == 2 ? sigOf(3, 0.9f, 0.05f, 1.3f, -0.0298f, 0.011f) : eye);
        sim.boundary();
    }
    unsigned sequences = 0, callLines = 0, cameraLines = 0, changed = 0;
    size_t firstCall = 0, lastCamera = 0;
    for (size_t i = 0; i < sim.log.size(); ++i) {
        const std::string& l = sim.log[i];
        if (l.find("sequence frame=") != std::string::npos) ++sequences;
        if (l.find(": call frame=") != std::string::npos) { ++callLines; if (!firstCall) firstCall = i; }
        if (l.find("camera census: camera=0x") != std::string::npos) { ++cameraLines; lastCamera = i; }
        if (l.find("changed:") != std::string::npos) ++changed;
    }
    check(sequences == 3 && callLines == 3 * (60 + 5 + 4) && sim.sequencesLogged == 3,
          "six on-foot frames print the call sequence of the first three only, every call of each (69 lines a frame)");
    check(cameraLines == 3 && lastCamera < firstCall, "each camera's line was printed once, before the first sequence that names it");
    check(sim.toneFrames == 17 && sim.sampledFrames == 6, "seventeen tone frames in all, six of them sampled (the six on foot)");
    check(changed >= 1 && changed <= 4, "the eye camera's bound that moved in frame 3 printed a changed line (bounded)");
    check(sim.budget.total() == sim.log.size() && sim.log.size() < VrCensusBudget::capTotal(), "every printed line was taken from the budget");
    // After three sequences the recording stops: a further frame's calls are counted and no record is written.
    for (int i = 0; i < 30; ++i) sim.call(0x1000, 3, 0x594E13, true, world);
    check(sim.frame.recorded == 0 && sim.frame.calls == 30, "once three sequences are out, a call is counted and nothing is recorded");
    // The journal flips to on foot AT a boundary (the glue reads it before it rolls the frame that ended): that frame was
    // recorded under the old word, so it has no calls; it is sampled by the rule but prints no empty sequence and spends
    // none of the three. The next frame, recorded under the new word, prints.
    Sim flip;
    flip.foot = VrCensusFoot::No;
    for (int i = 0; i < 4; ++i) flip.call(0x3000, 3, 0x594FE1, true, eye);
    flip.foot = VrCensusFoot::Yes;
    flip.boundary();
    check(flip.sequencesLogged == 0 && flip.sampledFrames == 1 && flip.log.size() == 1,
          "the frame the journal flips at is sampled by the rule but recorded nothing: no empty sequence header, none of the three spent");
    for (int i = 0; i < 4; ++i) flip.call(0x3000, 3, 0x594FE1, true, eye);
    flip.boundary();
    check(flip.sequencesLogged == 1 && flip.log.back().find("call frame=2 n=4 ") != std::string::npos,
          "the next frame, recorded under the new word, prints its whole sequence");
    // With the journal not read at all the tone alone decides, as the brief has it: the first tone frame prints a sequence.
    Sim bare;
    bare.foot = VrCensusFoot::Off;
    for (int i = 0; i < 20; ++i) bare.call(0x1000, 3, 0x594E13, false, world);
    for (int i = 0; i < 4; ++i) bare.call(0x3000, 3, 0x594FE1, true, eye);
    bare.boundary();
    check(bare.sequencesLogged == 1 && bare.sampledFrames == 1 && bare.log.back().find("call frame=1 n=24 ") != std::string::npos,
          "with no journal the tone alone decides: the first frame with the tone prints its whole sequence");
    bool any = false;
    for (const std::string& l : bare.log) any = any || l.find("foot=off") != std::string::npos;
    check(any, "...and its header says foot=off, so the reader knows the journal was not consulted");
    // The world route reports no draw progress at all (the skeleton, or a route that does not watch): there is no tone to see,
    // so the journal alone decides, and only a journal that positively says on foot samples.
    Sim mute;
    mute.progressAvailable = false;
    mute.foot = VrCensusFoot::Yes;
    for (int i = 0; i < 20; ++i) mute.call(0x1000, 3, 0x594E13, false, world);
    for (int i = 0; i < 4; ++i) mute.call(0x3000, 3, 0x594FE1, true, eye);
    mute.boundary();
    bool dashes = false;
    for (const std::string& l : mute.log) dashes = dashes || (l.find(": call frame=1 n=24 ") != std::string::npos && l.find("draw=- tone=none") != std::string::npos);
    check(mute.sequencesLogged == 1 && mute.sampledFrames == 1 && mute.toneFrames == 0 && dashes,
          "with no draw progress and a journal that says on foot the frame is sampled, and every call prints draw=- tone=none");
    for (VrCensusFoot f : {VrCensusFoot::Off, VrCensusFoot::Unknown, VrCensusFoot::No}) {
        Sim none;
        none.progressAvailable = false;
        none.foot = f;
        for (int i = 0; i < 24; ++i) none.call(0x1000, 3, 0x594E13, false, world);
        none.boundary();
        check(none.sequencesLogged == 0 && none.sampledFrames == 0,
              "with no draw progress and no positive word from the journal (off, a menu, a ship) nothing is sampled");
    }
}

// STAGE 2: the world route jitters. Its first frames are a warm-up with a ZERO phase (vrWorldRouteWorldPhase answers true and 0,0);
// an eye camera with no phase in the world cameras has nothing to leak, so those frames must not spend the samples (flight 1 spent
// all of them there). The frames after carry a phase and are sampled. One frame's phase is latched at the boundary that opens it.
void testPhaseSession() {
    std::printf("a scripted session with the route jittering: a zero-phase warm-up, then phases\n");
    const VrCensusSig world = sigOf(3, 1.7778f, 0.025f, 1.0122f), eye = sigOf(3, 0.95f, 0.025f, 1.5997f, 0.0891f, 0.0f);
    const VrCensusPhase zero{true, 0.0f, 0.0f};
    const VrCensusPhase moving[] = {{true, 0.252f, -0.126f}, {true, -0.189f, 0.063f}, {true, 0.126f, 0.252f},
                                    {true, -0.252f, -0.063f}, {true, 0.063f, 0.189f}};
    // One on-foot frame: the injected scene calls and the first-person ones before the tone, an auxiliary one (excluded), the eyes' after.
    auto frame = [&](Sim& s) {
        for (int i = 0; i < 12; ++i) s.call(0x1000, 3, i % 3 ? 0x594EAB : 0x594E13, false, world, s.phase.jittering && vrCensusPhaseNonZero(s.phase), 0);
        for (int i = 0; i < 3; ++i) s.call(0x1000, 3, 0x594E13, false, world, s.phase.jittering && vrCensusPhaseNonZero(s.phase), 1);
        s.call(0x2000, 3, 0x58DE73, false, sigOf(3, 1.0f, 0.1f, 1.5f), false, 2);
        for (int i = 0; i < 6; ++i) s.call(0x3000 + 0x100 * (i / 3), 5, 0x594FE1, true, eye);
    };
    Sim jit;
    jit.foot = VrCensusFoot::Yes;
    jit.phase = zero;   // frame 1's choice, latched at the boundary before it
    for (int f = 0; f < 3; ++f) {   // the warm-up: jittering, zero phase
        frame(jit);
        check(jit.frame.recorded == 0 && jit.frame.calls == 22 && jit.frame.toneSeen,
              "a warm-up frame (the route jitters, the phase is zero) counts its 22 calls and records none: the tone was seen and the commander is on foot, but nothing can leak");
        jit.boundary(f < 2 ? zero : moving[0]);
    }
    check(jit.sequencesLogged == 0 && jit.sampledFrames == 0 && jit.toneFrames == 3,
          "three warm-up frames are tone frames and none is sampled: no sequence is spent on them");
    bool noSequence = true;
    for (const std::string& l : jit.log) noSequence = noSequence && l.find("sequence frame=") == std::string::npos;
    check(noSequence, "...and the log holds no sequence header for them");
    for (int f = 0; f < 5; ++f) {   // frames 4..8 carry a phase
        frame(jit);
        const bool records = f < 3;
        check((jit.frame.recorded == 22) == records, "a frame with a non-zero phase records its calls while sequences are still wanted (the first three)");
        jit.boundary(f < 4 ? moving[f + 1] : zero);
    }
    check(jit.sequencesLogged == 3 && jit.sampledFrames == 5,
          "the five frames that carry a phase are sampled and the first three of them print their sequence: the budget is spent on frames that can leak");
    std::vector<std::string> headers, calls;
    for (const std::string& l : jit.log) {
        if (l.find("sequence frame=") != std::string::npos) headers.push_back(l);
        if (l.find(": call frame=") != std::string::npos) calls.push_back(l);
    }
    check(headers.size() == 3 &&
              headers[0] == "vr camera census: sequence frame=4 index=1/3 foot=yes phase=0.2520,-0.1260 calls=22 recorded=22 truncated=0" &&
              headers[1] == "vr camera census: sequence frame=5 index=2/3 foot=yes phase=-0.1890,0.0630 calls=22 recorded=22 truncated=0" &&
              headers[2] == "vr camera census: sequence frame=6 index=3/3 foot=yes phase=0.1260,0.2520 calls=22 recorded=22 truncated=0",
          "each sequence header carries the phase of ITS frame, latched at the boundary that opened it (not the next frame's)");
    unsigned scene = 0, fp = 0, aux = 0, eyes = 0, injected = 0;
    for (const std::string& l : calls) {
        if (l.find(" kind=3 ") != std::string::npos && l.find(" inj=1 role=scene ") != std::string::npos) { ++scene; ++injected; }
        if (l.find(" kind=3 ") != std::string::npos && l.find(" inj=1 role=fp ") != std::string::npos) { ++fp; ++injected; }
        if (l.find(" kind=3 ") != std::string::npos && l.find(" inj=0 role=aux ") != std::string::npos) ++aux;
        if (l.find(" kind=5 ") != std::string::npos && l.find(" inj=0 role=- ") != std::string::npos) ++eyes;
    }
    check(calls.size() == 66 && scene == 36 && fp == 9 && aux == 3 && eyes == 18 && injected == 45,
          "the logged calls say what the detour decided: 36 injected scene, 9 injected first-person, 3 excluded auxiliary, and the eyes' 18 kind-5 calls never injected");
    // With the route NOT jittering the same frames are sampled the old way: the first three print, phase=-.
    Sim plain;
    plain.foot = VrCensusFoot::Yes;
    for (int f = 0; f < 5; ++f) { frame(plain); plain.boundary(); }
    bool dashes = plain.sequencesLogged == 3 && plain.sampledFrames == 5;
    for (const std::string& l : plain.log) if (l.find("sequence frame=") != std::string::npos) dashes = dashes && l.find(" foot=yes phase=- calls=22 ") != std::string::npos;
    check(dashes, "with the route not jittering the rule is exactly what it was: the first three on-foot frames print (phase=-), five are sampled");
    // The rule the census would run if it ignored the phase: it samples the warm-up (the control the row above sees).
    const VrCensusPhase notJittering{};
    check(vrCensusSamplesFrame(true, true, VrCensusFoot::Yes, notJittering) && !vrCensusSamplesFrame(true, true, VrCensusFoot::Yes, zero),
          "control: a sampler that ignored the phase would sample a warm-up frame, and this one does not");
    // The route stops jittering mid-session (idle, released): the frames after are judged by today's rule again.
    Sim idle;
    idle.foot = VrCensusFoot::Yes;
    idle.phase = moving[0];
    frame(idle); idle.boundary(notJittering);   // frame 1 carried a phase and is sampled
    frame(idle); idle.boundary(notJittering);   // frame 2: the route is not jittering, today's rule samples it
    check(idle.sampledFrames == 2 && idle.sequencesLogged == 2, "a route that stops jittering leaves the frames judged by the old rule: both are sampled");
}

void testSampling() {
    std::printf("which frames are sampled\n");
    check(vrCensusFootFrom(false, false, false) == VrCensusFoot::Off && vrCensusFootFrom(false, true, true) == VrCensusFoot::Off &&
          vrCensusFootFrom(true, false, false) == VrCensusFoot::Unknown && vrCensusFootFrom(true, false, true) == VrCensusFoot::Unknown &&
          vrCensusFootFrom(true, true, false) == VrCensusFoot::No && vrCensusFootFrom(true, true, true) == VrCensusFoot::Yes,
          "the journal's state: off when it is not read, unknown (a menu) without Flags2, no in a ship, yes on foot");
    const VrCensusPhase none{};   // the route is not jittering: the phase has no say
    bool samples = true, never = true, record = true;
    for (int foot = 0; foot < 4; ++foot) {
        const VrCensusFoot f = static_cast<VrCensusFoot>(foot);
        const bool allows = f == VrCensusFoot::Yes || f == VrCensusFoot::Off;
        if (vrCensusSamplesFrame(true, true, f, none) != allows) samples = false;     // progress available, tone seen
        if (vrCensusSamplesFrame(false, true, f, none)) never = false;                // progress available, no tone: never
        if (vrCensusSamplesFrame(false, false, f, none) != (f == VrCensusFoot::Yes)) samples = false;   // no progress: the journal alone
        if (vrCensusSamplesFrame(true, false, f, none) != (f == VrCensusFoot::Yes)) samples = false;
        if (vrCensusMayRecord(f, none) != allows) record = false;
    }
    check(samples && never && record,
          "with the route not jittering, with draw progress a frame is sampled when the tone was seen and the journal says on foot or is not read, and never "
          "without the tone; with none only a journal that says on foot samples (exactly the rule before stage 2)");
    auto toneOnly = [](bool tone, VrCensusFoot) { return tone; };   // the brief's rule, as the census would run without the journal
    check(toneOnly(true, VrCensusFoot::No) && !vrCensusSamplesFrame(true, true, VrCensusFoot::No, none),
          "control: the tone alone would sample a cockpit frame, and the journal's word keeps it out");

    // THE PHASE: the truth table. While the route jitters a frame is sampled only with a non-zero phase; the journal, the tone and the
    // progress keep their say on top of it (the phase can only take a sample away). Every cell is checked against the rule written out.
    struct PhaseCase { const char* what; VrCensusPhase p; bool allows; };
    const PhaseCase phases[] = {
        {"not jittering, zero", {false, 0.0f, 0.0f}, true},
        {"not jittering, a phase the route left behind", {false, 0.25f, -0.1f}, true},
        {"jittering, zero (a warm-up frame)", {true, 0.0f, 0.0f}, false},
        {"jittering, negative zero", {true, -0.0f, -0.0f}, false},
        {"jittering, x only", {true, 0.25f, 0.0f}, true},
        {"jittering, y only", {true, 0.0f, -0.1f}, true},
        {"jittering, both", {true, -0.25f, 0.1f}, true},
        {"jittering, a tiny phase", {true, 1.0e-6f, 0.0f}, true},
        {"jittering, a NaN axis and a zero", {true, std::nanf(""), 0.0f}, false},
    };
    bool table = true, phaseOnly = true, never2 = true;
    unsigned cells = 0;
    for (const PhaseCase& pc : phases) {
        if (vrCensusPhaseAllowsSample(pc.p) != pc.allows) { table = false; std::printf("  note  phase case '%s' disagrees\n", pc.what); }
        for (int foot = 0; foot < 4; ++foot) {
            const VrCensusFoot f = static_cast<VrCensusFoot>(foot);
            for (int tone = 0; tone < 2; ++tone) {
                for (int progress = 0; progress < 2; ++progress) {
                    const bool base = progress ? (tone && (f == VrCensusFoot::Yes || f == VrCensusFoot::Off)) : f == VrCensusFoot::Yes;
                    const bool want = base && pc.allows;
                    if (vrCensusSamplesFrame(tone != 0, progress != 0, f, pc.p) != want) table = false;
                    if (!base && vrCensusSamplesFrame(tone != 0, progress != 0, f, pc.p)) never2 = false;   // a phase never ADDS a sample
                    ++cells;
                }
            }
            const bool mayRecord = (f == VrCensusFoot::Yes || f == VrCensusFoot::Off) && pc.allows;
            if (vrCensusMayRecord(f, pc.p) != mayRecord) record = false;
        }
        // The same frame with and without the phase differs exactly when the route jitters with nothing to show.
        if (vrCensusSamplesFrame(true, true, VrCensusFoot::Yes, pc.p) != pc.allows) phaseOnly = false;
    }
    check(table && record && cells == 9 * 4 * 2 * 2,
          "the phase truth table: nine phase states x four journal states x tone x progress (144 cells) are sampled exactly when the old rule says so AND the route is "
          "not jittering or its phase is non-zero; calls are recorded under the same condition");
    check(phaseOnly && never2,
          "a frame on foot with the tone seen is sampled exactly when the phase allows it, and a phase never adds a sample the old rule refused");
    check(vrCensusPhaseNonZero({true, 0.0f, 1.0e-30f}) && !vrCensusPhaseNonZero({true, 0.0f, 0.0f}) && !vrCensusPhaseNonZero({true, -0.0f, 0.0f}) &&
              vrCensusAbs(-2.5f) == 2.5f && vrCensusAbs(2.5f) == 2.5f && vrCensusAbs(-0.0f) == 0.0f,
          "a phase is non-zero when either axis is: |x| + |y| > 0, however small, of either sign");
    // The control the rows above see: two wrong rules, each of which the table catches. One ignores the phase (the warm-up would be
    // sampled again); the other applies the zero test even when the route is not jittering (the frames of a route that is off would
    // starve, which is not "exactly today's rule").
    auto mutantIgnoresPhase = [](const VrCensusPhase&) { return true; };
    auto mutantZeroTestAlways = [](const VrCensusPhase& p) { return vrCensusPhaseNonZero(p); };
    auto mutantBackwards = [](const VrCensusPhase& p) { return p.jittering || vrCensusPhaseNonZero(p); };
    bool ignoreCaught = false, zeroCaught = false, backwardsCaught = false;
    for (const PhaseCase& pc : phases) {
        if (mutantIgnoresPhase(pc.p) != pc.allows) ignoreCaught = true;
        if (mutantZeroTestAlways(pc.p) != pc.allows) zeroCaught = true;
        if (mutantBackwards(pc.p) != pc.allows) backwardsCaught = true;
    }
    check(ignoreCaught && zeroCaught && backwardsCaught,
          "control: a rule that ignored the phase, one that applied the zero test to a route that is not jittering, and one with the jittering test inverted "
          "each disagree with the table at a cell, so the table can fail");
    check(std::strcmp(vrCensusFootName(VrCensusFoot::Yes), "yes") == 0 && std::strcmp(vrCensusFootName(VrCensusFoot::No), "no") == 0 &&
          std::strcmp(vrCensusFootName(VrCensusFoot::Unknown), "unknown") == 0 && std::strcmp(vrCensusFootName(VrCensusFoot::Off), "off") == 0,
          "the journal's states have the words the log uses");
}

// ---------------------------------------------------------------------------
// Source pins: what no rig can run because it needs the game.
// ---------------------------------------------------------------------------
struct Pin { const std::string* text; const char* needle; unsigned times; const char* what; };

void runPins(const std::vector<Pin>& pins, const char* control) {
    for (const Pin& pin : pins) {
        check(count(*pin.text, pin.needle) == pin.times, pin.what);
        std::string without = *pin.text;
        for (size_t at = without.find(pin.needle); at != std::string::npos; at = without.find(pin.needle))
            without.erase(at, std::strlen(pin.needle));
        if (count(without, pin.needle) != 0) check(false, control);
    }
}

void testSourcePins(const std::string& injectCpp) {
    std::printf("source pins\n");
    const std::string censusCpp = slurp("src/d3d11/vr_camera_census.cpp");
    const std::string censusH = slurp("src/d3d11/vr_camera_census.h");
    const std::string phaseH = slurp("src/d3d11/flat_camera_phase.h");
    check(!censusCpp.empty() && !injectCpp.empty() && !censusH.empty() && !phaseH.empty(),
          "the census, injector and admission sources are readable from the repo root");

    // KEY OFF = NOTHING.
    const std::string eyeHead = "void vrCameraCensusEyeDraw(ID3D11DeviceContext* ctx, uint32_t eye) {";
    const std::string boundaryHead = "void vrCameraCensusFrameBoundary() {";
    const std::string readHead = "bool readWanted() {";
    const std::string eyeDraw = functionBody(censusCpp, eyeHead);
    const std::string boundary = functionBody(censusCpp, boundaryHead);
    const std::string readWanted = functionBody(censusCpp, readHead);
    check(!eyeDraw.empty() && !boundary.empty() && !readWanted.empty() &&
          censusCpp.find("bool vrCameraCensusWanted() { return g_wanted; }") != std::string::npos,
          "the census's entry points can be delimited in the source");
    check(startsWith(eyeDraw, eyeHead, "\n    if (!g_wanted) return;\n"),
          "KEY OFF: the eye-draw hook's first statement is 'if (!g_wanted) return;', ahead of any other work");
    const size_t offReturn = boundary.find("        return;\n    }\n");
    check(startsWith(boundary, boundaryHead, "\n    const bool wanted = readWanted();\n    if (!wanted) {\n        if (g_wanted) deactivate();") &&
          offReturn != std::string::npos && offReturn < boundary.find("new (std::nothrow) State") &&
          boundary.find("new (std::nothrow) State") > boundary.find("if (!wanted) {") &&
          boundary.find("flatCameraInjectDisarm()") > boundary.find("if (!wanted) {") &&
          boundary.find("flatCameraInjectObserveFrame()") > boundary.find("if (!wanted) {"),
          "KEY OFF: the boundary reads the key, and with it off returns before the state is allocated, the owner is set or the hook is asked for");
    check(startsWith(readWanted, readHead, "\n    if (!runtimeVrProfile()) return false;") &&
          readWanted.find("getString(\"advanced.vr_camera_census\", \"off\")") > readWanted.find("runtimeVrProfile()"),
          "KEY OFF outside VR: the flat profile returns before it asks for the key (and Config reads the key off there anyway)");
    std::string censusCode;   // the glue without its // comments, so prose that says "new" is not an allocation
    {
        size_t from = 0;
        while (from < censusCpp.size()) {
            size_t to = censusCpp.find('\n', from);
            if (to == std::string::npos) to = censusCpp.size();
            std::string ln = censusCpp.substr(from, to - from);
            const size_t slashes = ln.find("//");
            if (slashes != std::string::npos) ln.erase(slashes);
            censusCode += ln;
            censusCode += '\n';
            from = to + 1;
        }
    }
    check(count(censusCode, "new (std::nothrow) State") == 1 && count(censusCode, "new ") == 1 && count(censusCode, "malloc") == 0 &&
          count(censusCode, "std::vector") == 0 && count(censusCode, "std::string ") == 1,
          "the only allocation in the census's code is the one State, made at the first boundary with the key on (the key's one std::string is a three-character small string)");
    const std::string sampleAsk = "if (!vrCensusSamplesFrame(toneSeen, have, s->foot, phase)) return;";
    check(count(censusCpp, "journalOnFootKnown()") == 1 && count(censusCpp, "journalOnFoot()") == 1 &&
          boundary.find("s->foot = currentFoot();") != std::string::npos && boundary.find("s->foot = currentFoot();") < boundary.find("rollFrame(s);") &&
          eyeDraw.find(sampleAsk) != std::string::npos &&
          eyeDraw.find(sampleAsk) < eyeDraw.find("s->eye.take(s->frame)") &&
          eyeDraw.find(sampleAsk) < eyeDraw.find("readEyeRows("),
          "the journal is asked in one place (currentFoot), once a boundary ahead of the frame's roll, and an eye draw the journal or the route's phase rules "
          "out is returned before it spends the eye budget or stalls on a readback");
    // THE PHASE (stage 2): the census asks the route for it in one place, latches it at the boundary AFTER the frame that ended has been
    // judged under the previous one, and reads it at the eye draw before the sampling decision uses it.
    const std::string rollBody = functionBody(censusCpp, "void rollFrame(State* s) {");
    const std::string preBody = functionBody(censusCpp, "bool observePre(const FlatCameraObserveCall& call) noexcept {");
    check(!rollBody.empty() && !preBody.empty() && count(censusCpp, "vrWorldRouteWorldPhase(") == 1 && count(censusCpp, "readPhase()") == 3,
          "the route's phase is asked in one place, readPhase(), which the boundary and the eye draw call (definition plus two calls)");
    check(boundary.find("rollFrame(s);") != std::string::npos && boundary.find("s->phase = readPhase();") != std::string::npos &&
          boundary.find("rollFrame(s);") < boundary.find("s->phase = readPhase();") &&
          boundary.find("s->phase = readPhase();") < boundary.find("flatCameraInjectObserveFrame()") &&
          boundary.find("s->phase = readPhase();") > boundary.find("if (!wanted) {"),
          "the boundary judges the frame that ended under the phase latched when it began (rollFrame), THEN latches the next frame's, before the window opens; "
          "with the key off it returns before asking the route for anything");
    check(rollBody.find("vrCensusSamplesFrame(s->current.toneSeen, s->current.progress, s->foot, s->phase)") != std::string::npos &&
          rollBody.find("readPhase()") == std::string::npos &&
          censusCpp.find("vrCensusFormatSequence(line, kVrCensusLineBytes + 1, s->frame, s->sequencesLogged, s->foot, s->phase, f.calls, f.recorded);") != std::string::npos,
          "the roll and the sequence header use the phase latched for that frame, never a fresh ask (the route's boundary has moved on by then)");
    check(eyeDraw.find("const VrCensusPhase phase = readPhase();") != std::string::npos &&
          eyeDraw.find("const VrCensusPhase phase = readPhase();") > eyeDraw.find("if (!g_wanted) return;") &&
          eyeDraw.find("const VrCensusPhase phase = readPhase();") < eyeDraw.find(sampleAsk) &&
          eyeDraw.find("vrCensusFormatEye(line, kVrCensusLineBytes + 1, eye, s->frame, s->foot, phase,") != std::string::npos,
          "the eye draw reads the running frame's phase after the key-off return and before it decides, and prints that same phase on its line");
    check(preBody.find("vrCensusMayRecord(s->foot, s->phase)") != std::string::npos &&
          preBody.find("call.willInject") != std::string::npos && preBody.find("record->willInject = call.willInject;") != std::string::npos &&
          preBody.find("record->role = call.role;") != std::string::npos && preBody.find("vrWorldRouteWorldPhase") == std::string::npos &&
          preBody.find("readPhase") == std::string::npos,
          "the refresh's pre half decides recording from the latched phase and copies what the detour decided (willInject, role) into the record and the window; "
          "it never asks the route (no call into the route on the hot path)");
    check(count(censusCpp, "flatCameraInjectSetObserver(&g_observer)") == 1 && count(censusCpp, "flatCameraInjectObserveFrame()") == 1 &&
          count(censusCpp, "flatCameraInjectPause(") == 2,
          "the hook is asked for from one place (the boundary, after the key is on), and paused and unpaused from the two edges of it");
    check(censusCpp.find("flatRuntime") == std::string::npos && injectCpp.find("flatRuntimePhaseState(&jx, &jy, &rw, &rh, &applied);") != std::string::npos,
          "the census never calls the flat runtime (no phase source is needed: observe mode asks for no phase)");
    // A second place that reads the key would need its own off path.
    check(count(slurp("src/d3d11/vscreen.cpp") + slurp("src/d3d11/device_hook.cpp"), "advanced.vr_camera_census") == 0,
          "no other hook reads the key: the census's own boundary is the only place the decision is made");

    // The detour.
    const std::string pre = functionBody(injectCpp, "void refreshPre(uintptr_t r0, uintptr_t ctx, uintptr_t p2, uintptr_t camera) noexcept {");
    const std::string post = functionBody(injectCpp, "void refreshPost() noexcept {");
    const std::string observeCall = functionBody(injectCpp, "void observeCall(");
    const std::string observeOff = functionBody(injectCpp, "void observeOffThread(uintptr_t r0, uintptr_t camera) noexcept {");
    check(!pre.empty() && !post.empty() && !observeCall.empty() && !observeOff.empty(), "the detour's halves can be delimited in the source");
    const size_t branch = pre.find("if (observe) {\n        observeCall(");
    const size_t firstWrite = std::min({pre.find("flushCamera(camera)"), pre.find("sehWriteF32("), pre.find("sehWriteU32("), pre.find("sehWriteU64("),
                                        pre.find("flatRuntimeNoteCameraApplied"), pre.find("injected.noteInjected")});
    check(branch != std::string::npos && firstWrite != std::string::npos && branch < firstWrite,
          "OBSERVE-ONLY: the detour's observe branch ends the call ahead of the first flush, bound write, flag write or return-slot write of the inject path");
    check(pre.find("        observeCall(r0, ctx, p2, camera, callNo, readable, kind, callerRva, gate);\n        return;\n    }\n    // A camera this session injected, now not") != std::string::npos,
          "...and the branch is followed directly by the flush code it skips");
    const size_t phaseAsk = pre.find("flatRuntimePhaseState(");
    check(phaseAsk != std::string::npos && pre.rfind("if (!observe) {", phaseAsk) != std::string::npos && pre.rfind("if (!observe) {", phaseAsk) > pre.find("if (readable && kind == 3) {"),
          "OBSERVE-ONLY: the flat runtime's phase is asked for only under 'if (!observe)'");
    check(count(observeCall, "sehWriteU64(r0, static_cast<uint64_t>(g_stubB))") == 1 && count(observeCall, "sehWrite") == 1 &&
          observeCall.find("flushCamera") == std::string::npos && observeCall.find("flatRuntime") == std::string::npos,
          "OBSERVE-ONLY: observeCall's one write is the body's return slot (the post half's hook); no camera byte, flag or flush");
    check(observeOff.find("sehWrite") == std::string::npos && observeOff.find("flush") == std::string::npos,
          "OBSERVE-ONLY: the off-thread report reads and counts, it never writes");
    const size_t observeBranch = post.find("if (g_refreshTls.observe) {");
    const size_t observeEnd = post.find("        return;\n    }\n", observeBranch);
    check(observeBranch != std::string::npos && observeEnd != std::string::npos &&
          post.substr(observeBranch, observeEnd - observeBranch).find("sehWrite") == std::string::npos &&
          post.substr(observeBranch, observeEnd - observeBranch).find("flatRuntimeNoteCameraApplied") == std::string::npos &&
          post.find("sehWriteF32(camera + kCamBoundX, g_refreshTls.entryX);") > observeBranch,
          "OBSERVE-ONLY: the post half's observe branch restores nothing and notes nothing applied, and returns ahead of the restore");
    // The install and the switch.
    const std::string observeFrame = functionBody(injectCpp, "bool flatCameraInjectObserveFrame() {");
    check(count(injectCpp, "observeOnly.store(true") == 1 && count(injectCpp, "installRefreshHook(true)") == 1 &&
          observeFrame.find("observeOnly.store(true") < observeFrame.find("installRefreshHook(true)") &&
          count(injectCpp, "installRefreshHook(false)") == 1,
          "the switch is set in one place, ahead of the install it guards, and the flat path installs through the same function with it off");
    check(count(injectCpp, "admission.observeOnly = observe;") == 1 && count(phaseH, "if (in.observeOnly) return FlatCameraAdmit::Observed;") == 1,
          "the detour hands the switch to the admission table, and the table answers Observed for it");
    check(count(injectCpp, "flatcpu::Scope timed(flatcpu::kInject);") == 2,
          "the detour's CPU timer is still in its two halves, and the observe path added none");

    // The native temporal getter.
    const std::string nativeCpp = slurp("src/d3d11/native_temporal.cpp");
    const std::string geometry = functionBody(nativeCpp, "bool nativeTemporalEyeGeometry(");
    check(!geometry.empty() && geometry.find("if (t_insideTreat) return false;") != std::string::npos &&
          geometry.find("std::lock_guard<std::mutex> lock(mutex);") != std::string::npos &&
          geometry.find("t_insideTreat") < geometry.find("lock_guard") &&
          geometry.find("|| !current->begun || eye > 1") != std::string::npos,
          "nativeTemporalEyeGeometry takes the channel's mutex the way nativeTemporalDrawJitter does: never inside treat(), only with a begun frame, eye 0 or 1");
    check(count(censusH, "bool nativeTemporalEyeGeometry(uint32_t eye, uint64_t* sequence, float frustum[4], float shift[2]);") == 1,
          "the getter's declaration is in the census header, where native_temporal.cpp includes it");

    const std::vector<Pin> pins = {
        {&censusCpp, "FlatComputeInternalScope internal;", 1, "the eye readback's copy, Map and Unmap run inside FlatComputeInternalScope (the hooks step aside)"},
        {&censusCpp, "D3D11_USAGE_STAGING", 1, "the readback goes through one staging buffer"},
        {&censusCpp, "box{offset, 0, 0, offset + 64u, 1, 1}", 1, "the copy is the 64 bytes of rows 270..273 only"},
        {&censusCpp, "s->eye.take(s->frame)", 1, "an eye draw is read back only when the eye budget takes it"},
    };
    runPins(pins, "source pin control: a source with the line removed no longer contains it");
}

// ---------------------------------------------------------------------------
// The reader's fixture: a synthetic flight log written by the very formatters the DLL compiles, from cameras built by the
// derive model, so the rows the eye draws read back are the rows a camera's call composed. tools\edvr_log.py's
// --camera-census self-test reads tools\camera_census_fixture.log; this rig holds that file to exactly what the formatters
// write (--print-fixture regenerates it), so a drift of either the writer or the reader breaks a build.
//
// The story is flight 1's, one stage on: an on-foot session with the world route jittering (stage 2).
//   - the WORLD camera is one object that was the left EYE camera in the cockpit (its first-sight line says kind 5) and is the
//     kind-3 world camera on foot; it is refreshed with two projections, the scene's (near 0.025) and the first-person weapon
//     camera's (a tighter field of view, near 0.0675), both carrying the frame's phase in their bound pair (the injector's own
//     rule: bound += (jx / W, -jy / H), so the rows measure flatProjectionJitter's shift);
//   - an ortho UI camera (kind 1), an AUXILIARY kind-3 camera (excluded: inj=0 role=aux) that was a kind-5 call in the first logged
//     frame (one camera, two kinds in two frames), and an eye camera per eye (kind 5, three call sites each, after the tone, never
//     injected, the eye shift off because the route owns the world);
//   - four on-foot frames 4..7, each with a NON-ZERO phase (a zero-phase frame is not sampled while the route jitters): the first three
//     carry a call sequence, all four an eye draw per eye;
//   - the route's own two 5 s lines, back to back, before each census 5 s line (the route's side is written here as text).
// ---------------------------------------------------------------------------
struct FixtureCam {
    uintptr_t ptr;
    uint32_t kind;
    float fov, aspect, nearZ;
    uint32_t caller;
    float viewportW, viewportH;
    uintptr_t view;   // the pass object the refresh is handed with the camera
};
std::string fixtureLog() {
    std::string out;
    int ms = 0;
    auto put = [&](const std::string& text) {
        char stamp[32];
        const int total = 43200000 + ms;   // 12:00:00 and up
        std::snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d.%03d] ", total / 3600000, total / 60000 % 60, total / 1000 % 60, total % 1000);
        out += stamp;
        out += text;
        out += "\n";
        ms += 37;
    };
    char line[kVrCensusLineBytes + 1];
    put("version 0.18.0-rc.4-31-g0a1b2c3d (build 68C0A1F2) -- synthetic fixture for edvr_log.py --camera-census");
    put("vr camera census: on (advanced.vr_camera_census); observe-only, nothing is written to any camera; owner thread 4321; "
        "5 s line per window for 36 windows, then one per 12; cameras first 64, call sequences first 3 and eye draws first 4 "
        "on-foot frames (the tone drawn while the journal, read=yes, says on foot; while the world route jitters, only a frame "
        "whose phase is non-zero); line budget 724");

    const float renderW = 5040.0f, renderH = 2835.0f;
    const FixtureCam world{0x241dc2e2960, 3, 1.0122f, renderW / renderH, 0.025f, 0x594E13, renderW, renderH, 0x241dd00a000};
    const float firstPersonFov = 0.8453f, firstPersonNear = 0.0675f;   // x1.23 tighter, a larger near plane: flight 1's weapon camera
    const uintptr_t worldViews[3] = {0x241dd00a000, 0x241dd00a800, 0x241dd00b000};
    const FixtureCam ui{0x241de000100, 1, 0.0f, 1.0f, 0.1f, 0x58DE73, renderW, renderH, 0x241dd00c000};
    const uintptr_t auxPtr = 0x241de100200, auxView = 0x241dd00c800;
    const float frustumOf[2][4] = {{-1.2f, 0.7f, -0.9f, 1.1f}, {-0.7f, 1.2f, -0.9f, 1.1f}};
    const uintptr_t eyePtr[2] = {0x241df6d0bb0, 0x241df6d0ff0};
    const uintptr_t eyeView[2] = {0x241dd00e000, 0x241dd00f000};
    const uint32_t callers3[3] = {0x594E13, 0x594EAB, 0x594FE1};
    // What the route chose for each on-foot frame 4..7: the raster phase in render pixels, positive right/down (about 1e-4 NDC).
    const VrCensusPhase phaseOf[4] = {{true, 0.2520f, -0.1260f}, {true, -0.1890f, 0.0630f}, {true, 0.1260f, 0.2520f}, {true, -0.2520f, -0.0630f}};
    // The eye cameras: built from EDVR's advertised frustum with the eye shift OFF (the route owns the world), so the bound pair is the
    // frustum's own and does not move from frame to frame.
    struct EyeCam { float bx, by, aspect, fov; };
    EyeCam eyeCam[2];
    for (int e = 0; e < 2; ++e) {
        const double l = frustumOf[e][0], r = frustumOf[e][1], d = frustumOf[e][2], u = frustumOf[e][3];
        const double wHalf = (r - l) / 2, tanHalf = (u - d) / 2;
        eyeCam[e].bx = static_cast<float>(-(r + l) / (4 * wHalf));
        eyeCam[e].by = static_cast<float>(-(u + d) / (4 * tanHalf));
        eyeCam[e].aspect = static_cast<float>(wHalf / tanHalf);
        eyeCam[e].fov = static_cast<float>(2 * std::atan(tanHalf));
    }
    const EyeCam auxEye{0.0f, 0.0f, 1.0f, 1.5708f};   // the auxiliary camera's kind-5 face in the first logged frame
    // The injector's own rule: the frame's phase goes into the bound pair as (jx / W, -jy / H); the rows then measure 2x that.
    auto phaseBound = [&](const VrCensusPhase& p, float* bx, float* by) { *bx = p.x / renderW; *by = -p.y / renderH; };

    // Two lines of the world route's 5 s window, as the route writes them (the route's side of the contract; the route's own formatter is
    // not part of the census): the route line with the stage 2 tokens between last= and last-trigger=, then the inject line.
    auto routeWindow = [&](const char* state, const char* gate, const char* counters, const char* last, const char* jitter, const char* phase,
                           const char* rows, const char* fpMode, const char* vs, const char* ps, const char* target, const char* hdr,
                           const char* selection, const char* inject) {
        char text[1200];
        std::snprintf(text, sizeof(text),
                      "vr world route 5s: key=auto state=%s layer=live gate=%s %s (last=none) scene-resets=0 late-hdr-writes=0 (in 0 frames) "
                      "last=%s jitter=%s phase=%s rows=%s fp-mode=%s last-trigger=VS=%s PS=%s target=%s hdr=%s selection=%s",
                      state, gate, counters, last, jitter, phase, rows, fpMode, vs, ps, target, hdr, selection);
        put(text);
        std::snprintf(text, sizeof(text), "vr world route inject 5s: %s", inject);
        put(text);
    };

    // The first window: a commander in a ship. The route is idle, the tone is drawn every frame, the journal says not on foot, so
    // nothing is sampled.
    routeWindow("observing", "no",
                "frames=448 gate-frames=448 gate-flips=0 hdr-frames=0 trigger=0 none=0 ambiguous=0 treated=0 declined=0 owned-frames=0 "
                "eye-takes=0 door-layer-only=0 enters=0 releases=0",
                "none", "idle", "0.0000,0.0000", "0.0000,0.0000", "0/0/0", "0000000000000000", "0000000000000000", "0x0", "0x0", "none",
                "inj-scene=0 inj-fp=0 inj-refused=0 warming=0 aux=0 after=0 unsupported=0 other-kind=0 unreadable=0 off-thread=0 write-fail=0 "
                "inj-kinds=none pair-checked=0 pair-bad=0 inj-unnamed=0 inj-shut=0");
    {
        VrCensusWindow w;
        w.frames = 448; w.calls = 41788; w.posts = 41788;
        w.kinds[1] = 896; w.kinds[3] = 38204; w.kinds[5] = 2688;
        w.callers[0] = {0x594E13, 13895}; w.callers[1] = {0x594EAB, 13895}; w.callers[2] = {0x594FE1, 13895}; w.callers[3] = {0x58DE73, 103}; w.callerCount = 4;
        w.cameraCount = 3; w.toneBefore = 32000; w.toneAfter = 9788; w.toneFrames = 448; w.windows = 1; w.progressSeen = true;
        w.eyeDraws = 896;
        VrCensusWindowText t;
        t.hook = "installed"; t.camerasTotal = 3; t.foot = VrCensusFoot::No;
        vrCensusFormatWindow(line, sizeof(line), w, t);
        put(line);
    }

    // Camera lines: the first sight of each, in the order the boundary prints them. A kind-5 camera (an eye's) is the game's custom matrix:
    // the bound pair is zero, the off-centre terms are the matrix's own, there are no tangents and no viewport (as flight 1's lines were).
    auto cam5Line = [&](uintptr_t ptr, uint32_t caller, uintptr_t view, const EyeCam& ec, VrCensusTone tone, uint32_t ordinal, bool drawKnown,
                        uint32_t draw, uint64_t frame) {
        VrCensusCamera row;
        row.camera = ptr;
        VrCensusSig& s = row.firstSig;
        s.kind = 5; s.aspect = ec.aspect; s.nearZ = 0.025f; s.farZ = 50000.0f; s.fov = ec.fov;
        s.boundX = 0.0f; s.boundY = 0.0f; s.viewportW = 0.0f; s.viewportH = 0.0f; s.p8 = 2.0f * ec.bx; s.p9 = 2.0f * ec.by;
        row.tanValid = false;
        row.callerRva = caller; row.firstOrdinal = ordinal; row.firstDraw = draw; row.firstDrawKnown = drawKnown; row.firstTone = tone; row.firstFrame = frame;
        row.firstView = view; row.firstCtx = 0x241dce749f40ull;
        vrCensusFormatCamera(line, sizeof(line), row);
        put(line);
    };
    auto camLine = [&](const FixtureCam& c, float bx, float by, VrCensusTone tone, uint32_t ordinal, uint32_t draw, uint64_t frame) {
        Model m(c.kind, c.fov, c.aspect, bx, by, c.nearZ, 50000.0f);
        c2derive::camF(m.cam, c2derive::kCamViewportW) = c.viewportW;   // not part of the derivation: no re-derive needed
        c2derive::camF(m.cam, c2derive::kCamViewportH) = c.viewportH;
        VrCensusCamera row;
        row.camera = c.ptr;
        vrCensusSigFromSnap(m.snap(), &row.firstSig);
        row.tanValid = vrCensusTangents(row.firstSig, row.tan);
        row.callerRva = c.caller; row.firstOrdinal = ordinal; row.firstDraw = draw; row.firstDrawKnown = true; row.firstTone = tone; row.firstFrame = frame;
        row.firstView = c.view; row.firstCtx = c.view + 0x100;
        vrCensusFormatCamera(line, sizeof(line), row);
        put(line);
    };
    // The world camera's object was the LEFT EYE camera in the cockpit: its first-sight line, at the first frame, says kind 5 and is the
    // eye's (tone none: the route reported no progress yet).
    cam5Line(world.ptr, 0x594E13, eyeView[0], eyeCam[0], VrCensusTone::None, 19, false, 0, 1);
    camLine(ui, 0.0f, 0.0f, VrCensusTone::Before, 2, 0, 1);
    // The on-foot frames 4..7: the cameras first seen on foot (an auxiliary camera that is a kind-5 call in frame 4, and the two eyes).
    const uint64_t firstOnFoot = 4;
    cam5Line(auxPtr, 0x594FE1, auxView, auxEye, VrCensusTone::After, 97, true, 8209, firstOnFoot);
    for (int e = 0; e < 2; ++e) cam5Line(eyePtr[e], 0x594E13, eyeView[e], eyeCam[e], VrCensusTone::After, 103 + 3 * e, true, 8210 + static_cast<uint32_t>(e), firstOnFoot);

    // One 'changed:' line as the boundary prints it: the signature the camera had, the one it has now.
    auto changedLine = [&](uintptr_t ptr, uint64_t frame, uint32_t n, const VrCensusSig& from, const VrCensusSig& to) {
        VrCensusCamera row;
        row.camera = ptr;
        row.changeFrom = from; row.sig = to; row.changeFrame = frame; row.changes = n;
        vrCensusFormatChanged(line, sizeof(line), row);
        put(line);
    };
    auto sigFor = [&](uint32_t kind, float aspect, float nearZ, float fov, float bx, float by, float vw, float vh) {
        VrCensusSig s = sigOf(kind, aspect, nearZ, fov, bx, by);
        s.viewportW = vw; s.viewportH = vh;
        return s;
    };
    auto axesOf = [](Model& m, float yaw, float pitch) {
        float* a = &c2derive::camF(m.cam, c2derive::kCamAxes);
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        a[0] = cy; a[1] = 0.0f; a[2] = -sy; a[3] = 0.0f;
        a[4] = sy * sp; a[5] = cp; a[6] = cy * sp; a[7] = 0.0f;
        a[8] = sy * cp; a[9] = -sp; a[10] = cy * cp;
        c2derive::camU(m.cam, c2derive::kCamFlags) |= c2derive::kFlagProj | c2derive::kFlagVP;
        c2derive::derive(m.cam);
    };
    // The camera a call composed its rows from: a perspective model with the call's own terms, the head turned as the frame turns it. The
    // world's first-person camera shares the world camera's object, so it turns with it; the eyes follow the head, not the world.
    auto composeCall = [&](float fov, float aspect, float bx, float by, float nearZ, float yaw, float rows[16]) {
        Model m(3, fov, aspect, bx, by, nearZ);
        axesOf(m, yaw, -0.12f);
        return vrCensusComposeRows(m.snap(), rows);
    };

    // One sequence per on-foot frame (4, 5, 6), then the eye draws of frames 4..7 with their geometry and leak.
    uint32_t worldChanges = 0;
    for (int f = 0; f < 4; ++f) {
        const uint64_t frame = firstOnFoot + f;
        float bx = 0.0f, by = 0.0f, prevBx = 0.0f, prevBy = 0.0f;
        phaseBound(phaseOf[f], &bx, &by);
        if (f > 0) phaseBound(phaseOf[f - 1], &prevBx, &prevBy);
        // The 'changed:' lines of a camera whose terms moved since the last frame: the world camera's kind and field at the first on-foot frame,
        // its bound pair (the phase) in every frame; the auxiliary camera's kind when it stopped being a kind-5 call.
        if (f < 3) {
            worldChanges += 9;
            const VrCensusSig from = f == 0 ? sigFor(5, eyeCam[0].aspect, 0.025f, eyeCam[0].fov, 0.0f, 0.0f, 0.0f, 0.0f)
                                            : sigFor(3, world.aspect, 0.025f, world.fov, prevBx, prevBy, renderW, renderH);
            changedLine(world.ptr, frame, worldChanges, from, sigFor(3, world.aspect, 0.025f, world.fov, bx, by, renderW, renderH));
            if (f == 1) changedLine(auxPtr, frame, 1, sigFor(5, auxEye.aspect, 0.025f, auxEye.fov, 0.0f, 0.0f, 0.0f, 0.0f),
                                    sigFor(3, 1.0f, 0.1f, 1.5708f, 0.0f, 0.0f, 2048.0f, 2048.0f));
        }
        if (f < 3) {
            struct Call { uintptr_t ptr, view; uint32_t kind; float fov, aspect, bx, by, nearZ, yaw; uint32_t caller, draw; bool rows; VrCensusTone tone; bool inject; uint8_t role; };
            std::vector<Call> calls;
            const float worldYaw = 0.35f + 0.004f * static_cast<float>(f), eyeYaw = 0.02f * static_cast<float>(f);
            uint32_t draw = 40;
            for (int i = 0; i < 12; ++i) {
                draw += 310 + 17 * i;
                calls.push_back({world.ptr, worldViews[i % 3], 3, world.fov, world.aspect, bx, by, world.nearZ, worldYaw, callers3[i % 3], draw, true,
                                 VrCensusTone::Before, true, 0});
            }
            for (int i = 0; i < 3; ++i) {
                draw += 55;
                calls.push_back({world.ptr, worldViews[2], 3, firstPersonFov, world.aspect, bx, by, firstPersonNear, worldYaw, 0x594E13, draw, true,
                                 VrCensusTone::Before, true, 1});
            }
            for (int i = 0; i < 2; ++i) calls.push_back({ui.ptr, ui.view, 1, ui.fov, ui.aspect, 0.0f, 0.0f, ui.nearZ, worldYaw, 0x58DE73, draw + 9, false,
                                                         VrCensusTone::Before, false, kVrCensusRoleNone});
            if (f == 0) calls.push_back({auxPtr, auxView, 5, auxEye.fov, auxEye.aspect, 0.0f, 0.0f, 0.025f, eyeYaw, 0x594FE1, 8209, true,
                                         VrCensusTone::After, false, kVrCensusRoleNone});
            else calls.push_back({auxPtr, auxView, 3, 1.5708f, 1.0f, 0.0f, 0.0f, 0.1f, worldYaw, 0x58DE73, draw + 12, true, VrCensusTone::Before, false, 2});
            for (int i = 0; i < 6; ++i) {
                const int e = i / 3;
                calls.push_back({eyePtr[e], eyeView[e], 5, eyeCam[e].fov, eyeCam[e].aspect, eyeCam[e].bx, eyeCam[e].by, 0.025f, eyeYaw, callers3[i % 3],
                                 8210u + static_cast<uint32_t>(i), true, VrCensusTone::After, false, kVrCensusRoleNone});
            }
            vrCensusFormatSequence(line, sizeof(line), frame, static_cast<uint32_t>(f + 1), VrCensusFoot::Yes, phaseOf[f],
                                   static_cast<uint32_t>(calls.size()), static_cast<uint32_t>(calls.size()));
            put(line);
            uint32_t ordinal = 0;
            for (const Call& c : calls) {
                ++ordinal;
                VrCensusCall rec;
                rec.camera = c.ptr; rec.view = c.view; rec.kind = c.kind; rec.kindReadable = true; rec.callerRva = c.caller;
                rec.draw = c.draw; rec.drawKnown = true;
                rec.tone = c.tone; rec.preFlags = 0x1C; rec.postSeen = true;
                rec.willInject = c.inject; rec.role = c.role;
                rec.rowsValid = c.rows && composeCall(c.fov, c.aspect, c.bx, c.by, c.nearZ, c.yaw, rec.rows);
                rec.postFlags = c.rows ? 0x0 : 0x4;
                vrCensusFormatCall(line, sizeof(line), frame, ordinal, rec);
                put(line);
            }
        }
        // The eye draws of this frame: the rows the same camera composed in the sequence, read back from b1.
        for (int e = 0; e < 2; ++e) {
            float rows[16];
            composeCall(eyeCam[e].fov, eyeCam[e].aspect, eyeCam[e].bx, eyeCam[e].by, 0.025f, 0.02f * static_cast<float>(f), rows);
            float six[6][4] = {};
            std::memcpy(six, rows, sizeof(rows));
            double mx = 0, my = 0;
            const bool measured = flatCameraMeasureRowShift(six, mx, my);
            vrCensusFormatEye(line, sizeof(line), static_cast<uint32_t>(e), frame, VrCensusFoot::Yes, phaseOf[f], true, 8213u + 7u * static_cast<uint32_t>(e),
                              0x1eb2e751e20ull, 0, 5376, rows, measured, mx, my, nullptr);
            put(line);
            const float noShift[2] = {0.0f, 0.0f};
            vrCensusFormatEyeGeometry(line, sizeof(line), static_cast<uint32_t>(e), frame, true, 4700 + frame, frustumOf[e], noShift, measured, mx, my);
            put(line);
        }
    }
    // The on-foot window: the route's two lines, then the census's 5 s line, which says how many calls the detour injected.
    routeWindow("owned", "held",
                "frames=450 gate-frames=450 gate-flips=1 hdr-frames=450 trigger=450 none=0 ambiguous=0 treated=450 declined=0 owned-frames=450 "
                "eye-takes=900 door-layer-only=900 enters=1 releases=0",
                "treated", "on", "-0.2520,-0.0630", "0.1260,0.2520", "0/450/0", "DFED8E1C9E191BEC", "143AAE0597E2F7BF", "2520x1417", "5040x2835",
                "selected:450",
                "inj-scene=5400 inj-fp=1350 inj-refused=0 warming=0 aux=450 after=0 unsupported=2700 other-kind=900 unreadable=0 off-thread=0 write-fail=0 "
                "inj-kinds=3:6750 pair-checked=448 pair-bad=0 inj-unnamed=0 inj-shut=0");
    {
        VrCensusWindow w;
        w.frames = 450; w.calls = 10800; w.posts = 10800; w.stale = 0; w.injCalls = 6750;
        w.kinds[1] = 900; w.kinds[3] = 7200; w.kinds[5] = 2700;
        w.callers[0] = {0x594E13, 3150}; w.callers[1] = {0x594EAB, 3150}; w.callers[2] = {0x594FE1, 3150}; w.callers[3] = {0x58DE73, 1350}; w.callerCount = 4;
        w.cameraCount = 5; w.toneBefore = 8100; w.toneAfter = 2700; w.toneFrames = 450; w.onFootFrames = 450; w.eyeDraws = 900; w.eyeOnFoot = 900;
        w.windows = 1; w.progressSeen = true;
        VrCensusWindowText t;
        t.hook = "installed"; t.camerasTotal = 5; t.foot = VrCensusFoot::Yes;
        vrCensusFormatWindow(line, sizeof(line), w, t);
        put(line);
    }
    return out;
}


// Two log texts are the same when they differ at most in the last digits of the numbers in them: every line's text is
// identical and every number is within 1e-5 (relative above one). The scripted session builds its cameras with the
// library's tan, cos, sin and atan, which may round their last bit differently on another CPU, so the rows it prints can
// differ in the seventh digit from one machine to the next; the text the formatters write cannot.
bool sameWithinRounding(const std::string& a, const std::string& b, std::string* why) {
    size_t i = 0, j = 0;
    auto number = [](const std::string& s, size_t at, size_t* end, double* value) {
        size_t k = at;
        if (k < s.size() && s[k] == '-') ++k;
        if (k >= s.size() || !std::isdigit(static_cast<unsigned char>(s[k]))) return false;
        // A hex literal (0x241dc2e2960, +0x594E13, fl=0x1C) is text, not a number.
        if (s[k] == '0' && k + 1 < s.size() && s[k + 1] == 'x') return false;
        while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
        if (k + 1 < s.size() && s[k] == '.' && std::isdigit(static_cast<unsigned char>(s[k + 1]))) {
            ++k;
            while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
        }
        if (k + 1 < s.size() && (s[k] == 'e' || s[k] == 'E') &&
            (std::isdigit(static_cast<unsigned char>(s[k + 1])) ||
             ((s[k + 1] == '-' || s[k + 1] == '+') && k + 2 < s.size() && std::isdigit(static_cast<unsigned char>(s[k + 2]))))) {
            k += 2;
            while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
        }
        *end = k;
        *value = std::strtod(s.substr(at, k - at).c_str(), nullptr);
        return true;
    };
    auto hexEnd = [](const std::string& s, size_t at) {   // the end of a 0x literal starting at `at`, or `at` when there is none
        if (at + 1 >= s.size() || s[at] != '0' || s[at + 1] != 'x') return at;
        size_t k = at + 2;
        while (k < s.size() && std::isxdigit(static_cast<unsigned char>(s[k]))) ++k;
        return k;
    };
    while (i < a.size() && j < b.size()) {
        const size_t ha = hexEnd(a, i), hb = hexEnd(b, j);
        if (ha != i || hb != j) {   // a hex literal is text, every digit of it
            if (a.compare(i, ha - i, b, j, hb - j) != 0 || ha == i || hb == j) { if (why) *why = "hex differs near: " + a.substr(i, 40); return false; }
            i = ha; j = hb;
            continue;
        }
        size_t ea = 0, eb = 0;
        double va = 0, vb = 0;
        const bool na = number(a, i, &ea, &va), nb = number(b, j, &eb, &vb);
        if (na != nb) { if (why) *why = "a number against text near: " + a.substr(i, 40); return false; }
        if (na) {
            const double scale = std::max(1.0, std::max(std::fabs(va), std::fabs(vb)));
            if (!(std::fabs(va - vb) <= 1.0e-5 * scale)) { if (why) *why = "numbers differ near: " + a.substr(i, 40); return false; }
            i = ea; j = eb;
            continue;
        }
        if (a[i] != b[j]) { if (why) *why = "text differs near: " + a.substr(i, 40); return false; }
        ++i; ++j;
    }
    if (i != a.size() || j != b.size()) { if (why) *why = "one text is longer"; return false; }
    return true;
}

void testReaderFixture() {
    std::printf("the reader's fixture\n");
    const std::string fixture = slurp("tools/camera_census_fixture.log");
    const std::string built = fixtureLog();
    std::string normalised;
    for (char ch : fixture) if (ch != '\r') normalised += ch;
    check(!fixture.empty(), "tools/camera_census_fixture.log is readable from the repo root");
    std::string why;
    const bool same = sameWithinRounding(normalised, built, &why);
    if (!same) std::printf("  note  %s\n", why.c_str());
    check(same,
          "the reader's fixture file is what the DLL's formatters write for the scripted session, to the last digits of its numbers (python tools\\edvr_log.py --camera-census reads it; regenerate with --print-fixture)");
    check(sameWithinRounding("a 1.0000001 b 0x1F", "a 1.0000002 b 0x1F", nullptr) && !sameWithinRounding("a 1.0001 b", "a 1.0002 b", nullptr) &&
          !sameWithinRounding("a 1 b 0x1F", "a 1 b 0x1E", nullptr) && !sameWithinRounding("a 1 b", "a 1 c", nullptr) && !sameWithinRounding("a 1", "a 1 ", nullptr) &&
          sameWithinRounding("x=-0 y=1e-09", "x=0 y=3e-09", nullptr),
          "the comparison tolerates the seventh digit and a hex literal is text: a digit off at 1e-4, a changed word or hex, a longer line are all different");
    // The golden lines are pinned above; the fixture carries lines of the same classes from the scripted session (the off-thread line is
    // not in it: the fixture is a clean flight, so the reader's verdict on it can PASS, and the reader's own self-test adds that line).
    const char* classes[] = {"vr camera census 5s: frames=", "vr camera census: camera=0x", "vr camera census: changed: camera=0x",
                             "vr camera census: sequence frame=", "vr camera census: call frame=", "vr camera census: eye=",
                             "vr camera census: eye-geometry eye=",
                             "vr world route 5s: key=auto state=owned", "vr world route inject 5s: inj-scene=5400"};
    unsigned found = 0;
    for (const char* cls : classes) if (built.find(cls) != std::string::npos) ++found;
    check(found == 9, "the fixture holds a line of every class the rig pins: 5 s, camera, changed, sequence, call, eye, eye-geometry, and the route's two lines");
    // The stage 2 tokens are in the fixture's census lines, from the formatters: a sampled frame's phase, what the detour decided for a call.
    check(built.find(" phase=0.2520,-0.1260 calls=24 recorded=24 truncated=0") != std::string::npos &&
              built.find(" inj=1 role=scene ") != std::string::npos && built.find(" inj=1 role=fp ") != std::string::npos &&
              built.find(" inj=0 role=aux ") != std::string::npos && built.find(" kind=5 caller=+0x594FE1 draw=8212 tone=after inj=0 role=- ") != std::string::npos &&
              built.find(" stale=0 inj-calls=6750 kinds=1:900,3:7200,5:2700 ") != std::string::npos,
          "the fixture carries the new tokens: a sequence's phase, injected scene and first-person calls, an excluded auxiliary call, kind-5 eye calls with no role, inj-calls");
    std::string mutated = normalised;
    const size_t at = mutated.find("vr camera census 5s:");
    if (at != std::string::npos) mutated[at + 3] = '#';
    check(!sameWithinRounding(mutated, built, nullptr), "control: a fixture file with one character altered is no longer the formatters' output, so the row above can fail");
    std::string nudged = normalised;
    const size_t num = nudged.find("phase=0.2520,-0.1260");
    if (num != std::string::npos) nudged.replace(num, 20, "phase=0.2521,-0.1260");
    check(num != std::string::npos && !sameWithinRounding(nudged, built, nullptr),
          "control: a number moved in its fourth decimal is no longer the formatters' output either");
    std::string toggled = normalised;
    const size_t inj = toggled.find(" inj=1 role=scene ");
    if (inj != std::string::npos) toggled[inj + 5] = '0';
    check(inj != std::string::npos && !sameWithinRounding(toggled, built, nullptr),
          "control: a call line whose inj= was flipped is no longer the formatters' output");
}

int runSelfTest() {
    const std::string injectCpp = slurp("src/d3d11/flat_camera_inject.cpp");
    testKey();
    testLayout(injectCpp);
    testComposeRows();
    testTangentsAndLeak();
    testCameraTable();
    testFrameBuffer();
    testWindow();
    testBudget();
    testOffThread();
    testFormats();
    testSession();
    testPhaseSession();
    testSampling();
    testSourcePins(injectCpp);
    testReaderFixture();
    if (g_failures == 0) {
        std::printf("vr camera census: PASS\n");
        return 0;
    }
    std::printf("vr camera census: %d FAILED check(s)\n", g_failures);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return runSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "--print-lines") == 0) {
        for (const Golden& g : goldenLines()) std::printf("%-26s %s\n", g.name, g.text.c_str());
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--print-fixture") == 0) {
        const std::string text = fixtureLog();
        _setmode(_fileno(stdout), _O_BINARY);               // LF only: the fixture file is this, byte for byte
        std::fwrite(text.data(), 1, text.size(), stdout);   // tools\camera_census_fixture.log
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("vr camera census test: dry run (no checks run)\n");
        return 0;
    }
    std::printf("usage: vr_camera_census_test --self-test|--print-lines|--print-fixture|--dry-run\n");
    return 2;
}

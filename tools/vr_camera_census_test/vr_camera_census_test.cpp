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

void testCameraTable() {
    std::printf("camera table\n");
    VrCensusCameraTable table;
    const float noTan[4] = {};
    const VrCensusSig world = sigOf(3, 1.7778f, 0.025f, 1.0122f);
    check(table.note(0x1000, world, noTan, false, 1, 0x594E13, 1, 0, false, VrCensusTone::None) == VrCensusCameraTable::Event::New &&
          table.used() == 1 && table.at(0).linePending && table.at(0).calls == 1 && table.at(0).firstFrame == 1,
          "a camera pointer seen for the first time is a new row with its line pending");
    check(table.note(0x1000, world, noTan, false, 1, 0x594E13, 2, 5, true, VrCensusTone::Before) == VrCensusCameraTable::Event::Known &&
          table.used() == 1 && table.at(0).calls == 2 && table.at(0).firstOrdinal == 1,
          "the same camera again is known: one row, its first call's ordinal kept");
    check(table.takeCameraLine(0) && !table.takeCameraLine(0), "a first-sight line prints once");
    // Changes.
    VrCensusSig moved = world;
    moved.boundX = 5.0e-8f;   // a rounding: not a change
    check(table.note(0x1000, moved, noTan, false, 2, 0, 1, 0, false, VrCensusTone::None) == VrCensusCameraTable::Event::Known,
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
    check(table.note(0x1000, eyeMoved, noTan, false, 3, 0, 1, 0, false, VrCensusTone::None) == VrCensusCameraTable::Event::Changed &&
          table.at(0).changes == 1 && table.at(0).changePending && table.at(0).changeFrame == 3 && table.at(0).changeFrom.boundX == 0.0f,
          "a bound that moves is a change: the row keeps what it was and the frame it moved in");
    VrCensusSig nearMoved = eyeMoved;
    nearMoved.nearZ = 0.03f;
    table.note(0x1000, nearMoved, noTan, false, 4, 0, 1, 0, false, VrCensusTone::None);
    check(table.at(0).changes == 2 && table.at(0).changeFrom.boundX == 0.0f && table.at(0).changeFrame == 3,
          "a second change before the boundary prints keeps the FIRST pending change (what the line will say)");
    // The per-camera cap: four lines, then counted.
    unsigned printed = 0;
    VrCensusSig s = world;
    for (unsigned i = 0; i < 12; ++i) {
        s.nearZ = 0.1f + 0.01f * i;
        table.note(0x1000, s, noTan, false, 10 + i, 0, 1, 0, false, VrCensusTone::None);
        if (table.takeChangeLine(0)) ++printed;
    }
    check(printed + 1 >= kVrCensusMaxChangesPerCamera && table.at(0).changeLines == kVrCensusMaxChangesPerCamera && table.at(0).changes >= 12,
          "a camera prints at most four 'changed:' lines; every change past them is counted in the row");
    // Capacity.
    VrCensusCameraTable full;
    for (uintptr_t i = 0; i < VrCensusCameraTable::kCapacity; ++i)
        full.note(0x10000 + i * 0x100, world, noTan, false, 1, 0, 1, 0, false, VrCensusTone::None);
    check(full.used() == 64 && full.overflow() == 0, "the table holds the first 64 cameras of a session");
    check(full.note(0x99999, world, noTan, false, 1, 0, 1, 0, false, VrCensusTone::None) == VrCensusCameraTable::Event::Full &&
          full.used() == 64 && full.overflow() == 1 &&
          full.note(0x10000, world, noTan, false, 1, 0, 1, 0, false, VrCensusTone::None) == VrCensusCameraTable::Event::Known && full.overflow() == 1,
          "a 65th camera is counted in overflow, not held, and a camera already held is still known");
    // The tangents and the first call's place are kept for the line.
    VrCensusCameraTable t2;
    const float tan4[4] = {-0.5f, 0.5f, -0.3f, 0.3f};
    t2.note(0x2000, world, tan4, true, 7, 0x58DE73, 4, 1234, true, VrCensusTone::After);
    check(t2.at(0).tanValid && t2.at(0).tan[1] == 0.5f && t2.at(0).callerRva == 0x58DE73 && t2.at(0).firstDraw == 1234 &&
          t2.at(0).firstTone == VrCensusTone::After,
          "a new row keeps the tangents, the caller and the draw and tone of its first call");
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
    w.noteCall(true, 3, 0x594E13, 0xA0, VrCensusTone::Before, false);
    w.noteCall(true, 3, 0x594EAB, 0xA0, VrCensusTone::After, false);
    w.noteCall(true, 0, 0x594E13, 0xB0, VrCensusTone::After, true);
    w.noteCall(false, 0, 0x58DE73, 0xC0, VrCensusTone::None, false);
    w.noteCall(true, 9, 0x111111, 0xB0, VrCensusTone::None, false);
    check(w.calls == 5 && w.stale == 1 && w.kinds[3] == 2 && w.kinds[0] == 1 && w.kinds[7] == 1 && w.kinds[6] == 1 &&
          w.cameraCount == 3 && w.callerCount == 4 && w.toneBefore == 1 && w.toneAfter == 2 && w.toneNone == 2,
          "a window counts calls, stale calls, kinds (other and unreadable apart), distinct cameras and callers, and tone positions");
    VrCensusWindowText text;
    text.hook = "installed";
    text.camerasTotal = 3;
    char line[kVrCensusLineBytes + 1];
    vrCensusFormatWindow(line, sizeof(line), w, text);
    check(std::strstr(line, "kinds=0:1,3:2,other:1,unreadable:1") != nullptr && std::strstr(line, "callers=+0x594E13:2,+0x594EAB:1,+0x58DE73:1,+0x111111:1") != nullptr &&
          std::strstr(line, "cameras-seen=3 cameras-total=3 tone=1/2/2") != nullptr,
          "the 5 s line names the kinds and the callers busiest first, and the tone split");
    VrCensusWindow empty;
    vrCensusFormatWindow(line, sizeof(line), empty, VrCensusWindowText{});
    check(std::strstr(line, "frames=0 calls=0 posts=0 off-thread=0 stale=0 kinds=- callers=- cameras-seen=0") != nullptr &&
          std::strstr(line, "progress=no hook=pending windows=0") != nullptr,
          "an empty window still prints every field (zeros included): an absent line is what 'the census never ran' looks like");
    // The worst case still fits a log line: sixteen callers, every kind, six-digit counts.
    VrCensusWindow big;
    for (uint32_t i = 0; i < 40; ++i) big.noteCall(i % 9 != 8, i % 9, 0x594E13 + i, 0x1000 + i, static_cast<VrCensusTone>(i % 3), (i & 1) != 0);
    big.frames = big.calls = big.posts = 99999999999ull;
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
    w.frames = 450; w.calls = 10012; w.posts = 10012; w.stale = 0;
    w.kinds[0] = 1800; w.kinds[1] = 600; w.kinds[3] = 7612;
    w.callers[0] = {0x594E13, 3337}; w.callers[1] = {0x594EAB, 3337}; w.callers[2] = {0x594FE1, 3337}; w.callers[3] = {0x58DE73, 1}; w.callerCount = 4;
    w.cameraCount = 14; w.toneBefore = 9000; w.toneAfter = 1012; w.toneNone = 0;
    w.onFootFrames = 450; w.eyeDraws = 900; w.eyeOnFoot = 900; w.windows = 1; w.progressSeen = true;
    VrCensusWindowText text;
    text.hook = "installed"; text.camerasTotal = 14;
    vrCensusFormatWindow(line, sizeof(line), w, text);
    add("window");

    VrCensusCamera world;
    world.camera = 0x241dc2e2960;
    world.firstSig = sigOf(3, 5040.0f / 2835.0f, 0.025f, 1.0122f);
    world.firstSig.p0 = 0.9f; world.firstSig.flags = 0;
    world.tanValid = true; world.tan[0] = -0.5625f; world.tan[1] = 0.5625f; world.tan[2] = -0.3164f; world.tan[3] = 0.3164f;
    world.callerRva = 0x594E13; world.firstOrdinal = 1; world.firstDraw = 0; world.firstDrawKnown = true;
    world.firstTone = VrCensusTone::Before; world.firstFrame = 1;
    vrCensusFormatCamera(line, sizeof(line), world);
    add("camera-world");

    VrCensusCamera eye;
    eye.camera = 0x241df6d0bb0;
    eye.firstSig = sigOf(3, 0.9f, 0.05f, 1.3f, -0.03f, 0.011f);
    eye.firstSig.viewportW = 2620; eye.firstSig.viewportH = 2533;
    eye.tanValid = true; eye.tan[0] = -1.2f; eye.tan[1] = 0.7f; eye.tan[2] = -0.9f; eye.tan[3] = 1.1f;
    eye.callerRva = 0x594FE1; eye.firstOrdinal = 98; eye.firstDraw = 8210; eye.firstDrawKnown = true;
    eye.firstTone = VrCensusTone::After; eye.firstFrame = 1;
    vrCensusFormatCamera(line, sizeof(line), eye);
    add("camera-eye");

    VrCensusCamera moved = eye;
    moved.changeFrom = eye.firstSig;
    moved.sig = eye.firstSig;
    moved.sig.boundX = -0.0297f; moved.sig.nearZ = 0.06f;
    moved.changeFrame = 2; moved.changes = 7;
    vrCensusFormatChanged(line, sizeof(line), moved);
    add("changed");

    vrCensusFormatSequence(line, sizeof(line), 4, 1, 107, 107);
    add("sequence");

    VrCensusCall call;
    call.camera = 0x241dc2e2960; call.kind = 3; call.kindReadable = true; call.callerRva = 0x594E13; call.draw = 6500; call.drawKnown = true;
    call.tone = VrCensusTone::Before; call.preFlags = 0x1C; call.postFlags = 0; call.postSeen = true; call.rowsValid = true;
    const float rowsWorld[16] = {0.5625f, 0, 0, 0.8f, 0, 1.0f, 0, -0.1f, 0.2f, 0, 1.0f, 0.6f, 0, 0, 0.025f, 0};
    std::memcpy(call.rows, rowsWorld, sizeof(rowsWorld));
    vrCensusFormatCall(line, sizeof(line), 4, 1, call);
    add("call-world");
    VrCensusCall eyeCall = call;
    eyeCall.camera = 0x241df6d0bb0; eyeCall.callerRva = 0x594FE1; eyeCall.draw = 8210; eyeCall.tone = VrCensusTone::After;
    const float rowsEye[16] = {1.1f, 0, 0, 0.8f, 0, 1.0f, 0, -0.1f, 0.2f, 0, 1.0f, 0.6f, 0, 0, 0.05f, 0};
    std::memcpy(eyeCall.rows, rowsEye, sizeof(rowsEye));
    vrCensusFormatCall(line, sizeof(line), 4, 98, eyeCall);
    add("call-eye");
    VrCensusCall ortho = call;
    ortho.camera = 0x241de000100; ortho.kind = 1; ortho.callerRva = 0x58DE73; ortho.draw = 0; ortho.drawKnown = false;
    ortho.tone = VrCensusTone::None; ortho.postSeen = false; ortho.rowsValid = false;
    vrCensusFormatCall(line, sizeof(line), 4, 2, ortho);
    add("call-ortho");

    vrCensusFormatEye(line, sizeof(line), 0, 4, true, 8213, 0x1eb2e751e20ull, 0, 5376, rowsEye, true, 0.0002, -0.0001, nullptr);
    add("eye");
    vrCensusFormatEye(line, sizeof(line), 1, 4, true, 8220, 0x1eb2e751e20ull, 0, 5376, nullptr, false, 0, 0, "map");
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
              "vr camera census 5s: frames=450 calls=10012 posts=10012 off-thread=0 stale=0 kinds=0:1800,1:600,3:7612 "
              "callers=+0x594E13:3337,+0x594EAB:3337,+0x594FE1:3337,+0x58DE73:1 cameras-seen=14 cameras-total=14 tone=9000/1012/0 "
              "on-foot-frames=450 eye-draws=900/900 progress=yes hook=installed windows=1 cam-overflow=0 thread-overflow=0",
          "the 5 s line: frames, calls, posts, off-thread, stale, kinds, callers, cameras, tone split, on-foot frames, eye draws, progress, hook");
    check(at("camera-world") ==
              "vr camera census: camera=0x241dc2e2960 kind=3 caller=+0x594E13 thread=owner aspect=1.777778 near=0.025 far=50000 fov=1.0122 "
              "bound=(0,0) offcentre=(0,0) viewport=(5040,2835) tan=(-0.5625,0.5625,-0.3164,0.3164) first-call=1 draw=0 tone=before frame=1",
          "the per-camera line: kind, caller, thread, aspect, near, far, fov, bound, off-centre, viewport, tangents, first call, draw, tone, frame");
    check(at("camera-eye") ==
              "vr camera census: camera=0x241df6d0bb0 kind=3 caller=+0x594FE1 thread=owner aspect=0.9 near=0.05 far=50000 fov=1.3 "
              "bound=(-0.03,0.011) offcentre=(-0.06,0.022) viewport=(2620,2533) tan=(-1.2,0.7,-0.9,1.1) first-call=98 draw=8210 tone=after frame=1",
          "an asymmetric eye camera's line shows its bound pair and off-centre terms");
    check(at("changed") ==
              "vr camera census: changed: camera=0x241df6d0bb0 frame=2 n=7 near=0.05->0.06 bound=(-0.03,0.011)->(-0.0297,0.011)",
          "the changed line names only the fields that moved, old->new");
    check(at("sequence") == "vr camera census: sequence frame=4 index=1/3 calls=107 recorded=107 truncated=0", "the sequence header");
    check(at("call-world") ==
              "vr camera census: call frame=4 n=1 camera=0x241dc2e2960 kind=3 caller=+0x594E13 draw=6500 tone=before fl=0x1C>0x0 "
              "rows=[0.5625,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.025,0]",
          "a call line: ordinal, camera, kind, caller, draw, tone, dirty flags before>after, the sixteen composed floats");
    check(at("call-eye") ==
              "vr camera census: call frame=4 n=98 camera=0x241df6d0bb0 kind=3 caller=+0x594FE1 draw=8210 tone=after fl=0x1C>0x0 "
              "rows=[1.1,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.05,0]",
          "an eye camera's call, after the tone");
    check(at("call-ortho") ==
              "vr camera census: call frame=4 n=2 camera=0x241de000100 kind=1 caller=+0x58DE73 draw=- tone=none fl=0x1C>- rows=-",
          "a call with no draw progress, no post half and no rows prints dashes, never a guess");
    check(at("eye") ==
              "vr camera census: eye=0 frame=4 draw=8213 b1=0x1eb2e751e20 first=0 bytes=5376 "
              "rows=[1.1,0,0,0.8,0,1,0,-0.1,0.2,0,1,0.6,0,0,0.05,0] meas=(0.0002,-0.0001)",
          "an eye draw's line: the b1 buffer, its rows 270..273, the measured shift");
    check(at("eye-failed") == "vr camera census: eye=1 frame=4 draw=8220 b1=0x1eb2e751e20 first=0 bytes=5376 rows=- meas=- why=map",
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
    // The worst case of the long lines: sixteen floats at their widest, every field at its largest.
    VrCensusCall worst;
    worst.camera = 0xFFFFFFFFFFFFull; worst.kind = 4294967295u; worst.kindReadable = true; worst.callerRva = 0xFFFFFF; worst.draw = 4294967295u;
    worst.drawKnown = true; worst.tone = VrCensusTone::Before; worst.preFlags = worst.postFlags = 0xFFFFFFFFu; worst.postSeen = worst.rowsValid = true;
    for (int i = 0; i < 16; ++i) worst.rows[i] = -1.2345678e-05f * (i + 1);
    char line[kVrCensusLineBytes + 1];
    const int n = vrCensusFormatCall(line, sizeof(line), 99999999999ull, 4294967295u, worst);
    check(n > 0 && std::strlen(line) <= kVrCensusLineBytes && std::strstr(line, "rows=[") != nullptr && line[std::strlen(line) - 1] == ']',
          "the widest call line (every field and sixteen long floats) still fits 400 characters whole");
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
    uint32_t sequencesLogged = 0;
    uint64_t frameNo = 1;
    std::vector<std::string> log;
    void say(VrCensusLines c, const char* line) { if (budget.take(c)) log.emplace_back(line); }
    void call(uintptr_t camera, uint32_t kind, uint32_t caller, bool tone, const VrCensusSig& sig) {
        const bool recording = vrCensusPrintsSequence(true, sequencesLogged);
        VrCensusCall* rec = recording ? frame.add() : (++frame.calls, nullptr);
        if (tone) frame.toneSeen = true;
        if (rec) { rec->camera = camera; rec->kind = kind; rec->kindReadable = true; rec->callerRva = caller; rec->drawKnown = true; rec->draw = frame.calls;
                   rec->tone = tone ? VrCensusTone::After : VrCensusTone::Before; rec->postSeen = true; rec->rowsValid = true; }
        const float noTan[4] = {};
        cameras.note(camera, sig, noTan, false, frameNo, caller, frame.calls, frame.calls, true, tone ? VrCensusTone::After : VrCensusTone::Before);
    }
    void boundary() {
        char line[kVrCensusLineBytes + 1];
        for (size_t i = 0; i < cameras.used(); ++i) {
            if (cameras.takeCameraLine(i)) { vrCensusFormatCamera(line, sizeof(line), cameras.at(i)); say(VrCensusLines::Camera, line); }
            if (cameras.takeChangeLine(i)) { vrCensusFormatChanged(line, sizeof(line), cameras.at(i)); say(VrCensusLines::Changed, line); }
        }
        if (vrCensusPrintsSequence(frame.toneSeen, sequencesLogged)) {
            ++sequencesLogged;
            vrCensusFormatSequence(line, sizeof(line), frameNo, sequencesLogged, frame.calls, frame.recorded);
            say(VrCensusLines::Call, line);
            for (uint32_t i = 0; i < frame.recorded; ++i) { vrCensusFormatCall(line, sizeof(line), frameNo, i + 1, frame.call[i]); say(VrCensusLines::Call, line); }
        }
        frame.reset();
        ++frameNo;
    }
};

void testSession() {
    std::printf("a scripted session through the core\n");
    Sim sim;
    const VrCensusSig world = sigOf(3, 1.7778f, 0.025f, 1.0122f), eye = sigOf(3, 0.9f, 0.05f, 1.3f, -0.03f, 0.011f), ui = sigOf(1, 1.0f, 0.1f, 0.0f);
    // Ten frames in the cockpit (no tone), then six on foot: world calls before the tone, eye calls after it.
    for (int f = 0; f < 10; ++f) { for (int i = 0; i < 40; ++i) sim.call(0x1000, 3, 0x594E13, false, world); sim.boundary(); }
    const size_t cockpitLines = sim.log.size();
    check(cockpitLines == 1 && sim.sequencesLogged == 0, "frames without the tone print only the camera's first-sight line, never a call sequence");
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
    check(changed >= 1 && changed <= 4, "the eye camera's bound that moved in frame 3 printed a changed line (bounded)");
    check(sim.budget.total() == sim.log.size() && sim.log.size() < VrCensusBudget::capTotal(), "every printed line was taken from the budget");
    // After three sequences the recording stops: a further frame's calls are counted and no record is written.
    for (int i = 0; i < 30; ++i) sim.call(0x1000, 3, 0x594E13, true, world);
    check(sim.frame.recorded == 0 && sim.frame.calls == 30, "once three sequences are out, a call is counted and nothing is recorded");
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
    check(count(censusCpp, "new (std::nothrow) State") == 1 && count(censusCpp, "new ") == 1 && count(censusCpp, "malloc") == 0,
          "the only allocation in the census is the one State, made at the first boundary with the key on");
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
// The story: an on-foot session with a world camera, a weapon camera, an ortho UI camera and an eye camera per eye. The
// world and the weapon are symmetric (near 0.025 and 0.0675); the eyes are asymmetric, call from the caller the world's
// third call site also uses, and are refreshed only after the tone. Four on-foot frames: the first three carry a call
// sequence, all four an eye draw per eye.
// ---------------------------------------------------------------------------
struct FixtureCam {
    uintptr_t ptr;
    uint32_t kind;
    float fov, aspect, nearZ;
    uint32_t caller;
    float viewportW, viewportH;
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
        "5 s line per window for 36 windows, then one per 12; cameras first 64, call sequences first 3 on-foot frames, "
        "eye draws first 4 on-foot frames; line budget 724");
    put("flat camera inject: refresh hook installed at EliteDangerous64.exe+0x592200 in OBSERVE-ONLY mode (the VR camera census): "
        "no camera is ever written");

    const FixtureCam world{0x241dc2e2960, 3, 1.0122f, 5040.0f / 2835.0f, 0.025f, 0x594E13, 5040.0f, 2835.0f};
    const FixtureCam weapon{0x241de100200, 3, 0.8236f, 5040.0f / 2835.0f, 0.0675f, 0x594E13, 5040.0f, 2835.0f};
    const FixtureCam ui{0x241de000100, 1, 0.0f, 1.0f, 0.1f, 0x58DE73, 5040.0f, 2835.0f};
    const float frustumOf[2][4] = {{-1.2f, 0.7f, -0.9f, 1.1f}, {-0.7f, 1.2f, -0.9f, 1.1f}};
    const uintptr_t eyePtr[2] = {0x241df6d0bb0, 0x241df6d0ff0};
    // The first 5 s line: before the on-foot frames, cockpit-like traffic with no tone.
    {
        VrCensusWindow w;
        w.frames = 448; w.calls = 39100; w.posts = 39100;
        w.kinds[1] = 896; w.kinds[3] = 38204;
        w.callers[0] = {0x594E13, 12999}; w.callers[1] = {0x594EAB, 12999}; w.callers[2] = {0x594FE1, 12999}; w.callers[3] = {0x58DE73, 103}; w.callerCount = 4;
        w.cameraCount = 3; w.toneNone = 39100; w.windows = 1; w.progressSeen = true;
        w.eyeDraws = 896;
        VrCensusWindowText t;
        t.hook = "installed"; t.camerasTotal = 3;
        vrCensusFormatWindow(line, sizeof(line), w, t);
        put(line);
    }
    // Camera lines: the first sight of each, in the order the boundary prints them.
    auto camLine = [&](const FixtureCam& c, float bx, float by, VrCensusTone tone, uint32_t ordinal, uint32_t draw, uint64_t frame) {
        Model m(c.kind, c.fov, c.aspect, bx, by, c.nearZ, 50000.0f);
        c2derive::camF(m.cam, c2derive::kCamViewportW) = c.viewportW;   // not part of the derivation: no re-derive needed
        c2derive::camF(m.cam, c2derive::kCamViewportH) = c.viewportH;
        VrCensusCamera row;
        row.camera = c.ptr;
        vrCensusSigFromSnap(m.snap(), &row.firstSig);
        row.tanValid = vrCensusTangents(row.firstSig, row.tan);
        row.callerRva = c.caller; row.firstOrdinal = ordinal; row.firstDraw = draw; row.firstDrawKnown = true; row.firstTone = tone; row.firstFrame = frame;
        vrCensusFormatCamera(line, sizeof(line), row);
        put(line);
    };
    struct EyeFrame { float bx[2], by[2], aspect[2], fov[2]; float shift[2][2]; };
    EyeFrame eyeFrame[4];
    for (int f = 0; f < 4; ++f) {
        for (int e = 0; e < 2; ++e) {
            eyeFrame[f].shift[e][0] = 0.00011f * static_cast<float>((f + 1) % 4) - 0.0002f;
            eyeFrame[f].shift[e][1] = -0.00007f * static_cast<float>((f + 2) % 3) + 0.00005f;
            const double l = frustumOf[e][0] + eyeFrame[f].shift[e][0], r = frustumOf[e][1] + eyeFrame[f].shift[e][0];
            const double d = frustumOf[e][2] + eyeFrame[f].shift[e][1], u = frustumOf[e][3] + eyeFrame[f].shift[e][1];
            const double wHalf = (r - l) / 2, tanHalf = (u - d) / 2;
            eyeFrame[f].bx[e] = static_cast<float>(-(r + l) / (4 * wHalf));
            eyeFrame[f].by[e] = static_cast<float>(-(u + d) / (4 * tanHalf));
            eyeFrame[f].aspect[e] = static_cast<float>(wHalf / tanHalf);
            eyeFrame[f].fov[e] = static_cast<float>(2 * std::atan(tanHalf));
        }
    }
    camLine(world, 0.0f, 0.0f, VrCensusTone::Before, 1, 0, 1);
    camLine(ui, 0.0f, 0.0f, VrCensusTone::Before, 2, 0, 1);
    // The on-foot frames 4..7: the cameras of the first three are first seen in frame 4.
    const uint64_t firstOnFoot = 4;
    camLine(weapon, 0.0f, 0.0f, VrCensusTone::Before, 13, 2100, firstOnFoot);
    for (int e = 0; e < 2; ++e) {
        const FixtureCam eyeCam{eyePtr[e], 3, eyeFrame[0].fov[e], eyeFrame[0].aspect[e], 0.05f, 0x594FE1, 2620.0f, 2533.0f};
        camLine(eyeCam, eyeFrame[0].bx[e], eyeFrame[0].by[e], VrCensusTone::After, 18 + e, 8210 + e, firstOnFoot);
    }
    // One sequence per on-foot frame (4, 5, 6), then the eye draws of frames 4..7 with their geometry and leak.
    for (int f = 0; f < 4; ++f) {
        const uint64_t frame = firstOnFoot + f;
        // The "changed:" line of an eye camera whose bound moved since the last frame.
        if (f > 0 && f < 3) {
            for (int e = 0; e < 2; ++e) {
                VrCensusCamera row;
                row.camera = eyePtr[e];
                Model before(3, eyeFrame[f - 1].fov[e], eyeFrame[f - 1].aspect[e], eyeFrame[f - 1].bx[e], eyeFrame[f - 1].by[e], 0.05f);
                Model after(3, eyeFrame[f].fov[e], eyeFrame[f].aspect[e], eyeFrame[f].bx[e], eyeFrame[f].by[e], 0.05f);
                vrCensusSigFromSnap(before.snap(), &row.changeFrom);
                vrCensusSigFromSnap(after.snap(), &row.sig);
                row.changeFrame = frame; row.changes = static_cast<uint32_t>(f * 9);
                vrCensusFormatChanged(line, sizeof(line), row);
                put(line);
            }
        }
        if (f < 3) {
            struct Call { const FixtureCam* cam; float bx, by, fov, aspect; uint32_t caller, draw; bool rows; VrCensusTone tone; };
            std::vector<Call> calls;
            const uint32_t callers3[3] = {0x594E13, 0x594EAB, 0x594FE1};
            uint32_t draw = 40;
            for (int i = 0; i < 12; ++i) { draw += 310 + 17 * i; calls.push_back({&world, 0, 0, world.fov, world.aspect, callers3[i % 3], draw, true, VrCensusTone::Before}); }
            for (int i = 0; i < 3; ++i) { draw += 55; calls.push_back({&weapon, 0, 0, weapon.fov, weapon.aspect, 0x594E13, draw, true, VrCensusTone::Before}); }
            for (int i = 0; i < 2; ++i) { calls.push_back({&ui, 0, 0, ui.fov, ui.aspect, 0x58DE73, draw + 9, false, VrCensusTone::Before}); }
            for (int i = 0; i < 4; ++i) {
                const int e = i % 2;
                calls.push_back({nullptr, eyeFrame[f].bx[e], eyeFrame[f].by[e], eyeFrame[f].fov[e], eyeFrame[f].aspect[e], 0x594FE1, 8210u + static_cast<uint32_t>(i), true, VrCensusTone::After});
            }
            vrCensusFormatSequence(line, sizeof(line), frame, static_cast<uint32_t>(f + 1), static_cast<uint32_t>(calls.size()), static_cast<uint32_t>(calls.size()));
            put(line);
            uint32_t ordinal = 0;
            for (const Call& c : calls) {
                ++ordinal;
                const bool isEye = c.cam == nullptr;
                // The last four calls are the eyes', left then right twice.
                const uintptr_t ptr = isEye ? eyePtr[(ordinal - static_cast<uint32_t>(calls.size() - 4) - 1) % 2] : c.cam->ptr;
                const uint32_t kind = isEye ? 3u : c.cam->kind;
                const float near0 = isEye ? 0.05f : c.cam->nearZ;
                Model m(kind, c.fov, c.aspect, c.bx, c.by, near0);
                // The head and the world do not turn alike: the world's first-person camera drifts, the eyes follow the head.
                float* a = &c2derive::camF(m.cam, c2derive::kCamAxes);
                const float yaw = (isEye ? 0.02f * static_cast<float>(f) : 0.35f + 0.004f * static_cast<float>(f)), pitch = -0.12f;
                const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
                a[0] = cy; a[1] = 0.0f; a[2] = -sy; a[3] = 0.0f;
                a[4] = sy * sp; a[5] = cp; a[6] = cy * sp; a[7] = 0.0f;
                a[8] = sy * cp; a[9] = -sp; a[10] = cy * cp;
                c2derive::camU(m.cam, c2derive::kCamFlags) |= c2derive::kFlagProj | c2derive::kFlagVP;
                c2derive::derive(m.cam);
                VrCensusCall rec;
                rec.camera = ptr; rec.kind = kind; rec.kindReadable = true; rec.callerRva = c.caller; rec.draw = c.draw; rec.drawKnown = true;
                rec.tone = c.tone; rec.preFlags = 0x1C; rec.postSeen = true;
                rec.rowsValid = c.rows && vrCensusComposeRows(m.snap(), rec.rows);
                rec.postFlags = c.rows ? 0x0 : 0x4;
                vrCensusFormatCall(line, sizeof(line), frame, ordinal, rec);
                put(line);
            }
        }
        // The eye draws of this frame: the rows the same camera composed in the sequence, read back from b1.
        for (int e = 0; e < 2; ++e) {
            Model m(3, eyeFrame[f].fov[e], eyeFrame[f].aspect[e], eyeFrame[f].bx[e], eyeFrame[f].by[e], 0.05f);
            float* a = &c2derive::camF(m.cam, c2derive::kCamAxes);
            const float yaw = 0.02f * static_cast<float>(f), pitch = -0.12f;
            const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
            a[0] = cy; a[1] = 0.0f; a[2] = -sy; a[3] = 0.0f;
            a[4] = sy * sp; a[5] = cp; a[6] = cy * sp; a[7] = 0.0f;
            a[8] = sy * cp; a[9] = -sp; a[10] = cy * cp;
            c2derive::camU(m.cam, c2derive::kCamFlags) |= c2derive::kFlagProj | c2derive::kFlagVP;
            c2derive::derive(m.cam);
            float rows[16];
            vrCensusComposeRows(m.snap(), rows);
            float six[6][4] = {};
            std::memcpy(six, rows, sizeof(rows));
            double mx = 0, my = 0;
            const bool measured = flatCameraMeasureRowShift(six, mx, my);
            vrCensusFormatEye(line, sizeof(line), static_cast<uint32_t>(e), frame, true, 8213u + 7u * static_cast<uint32_t>(e), 0x1eb2e751e20ull, 0, 5376, rows,
                              measured, mx, my, nullptr);
            put(line);
            vrCensusFormatEyeGeometry(line, sizeof(line), static_cast<uint32_t>(e), frame, true, 4700 + frame, frustumOf[e], eyeFrame[f].shift[e], measured, mx, my);
            put(line);
        }
    }
    // A call on another thread and a later 5 s line (the on-foot one), so the reader sees both shapes.
    VrCensusOffThread::Entry other;
    other.thread = 4321; other.camera = 0x241dc2e2960; other.kind = 3; other.kindReadable = true; other.callerRva = 0x594E13; other.calls = 57;
    vrCensusFormatOtherThread(line, sizeof(line), other);
    put(line);
    {
        VrCensusWindow w;
        w.frames = 450; w.calls = 48600; w.posts = 48600; w.stale = 0; w.kinds[1] = 900; w.kinds[3] = 47700;
        w.callers[0] = {0x594E13, 16200}; w.callers[1] = {0x594EAB, 16200}; w.callers[2] = {0x594FE1, 16200}; w.callers[3] = {0x58DE73, 0}; w.callerCount = 3;
        w.cameraCount = 5; w.toneBefore = 40500; w.toneAfter = 8100; w.onFootFrames = 450; w.eyeDraws = 900; w.eyeOnFoot = 900;
        w.windows = 1; w.progressSeen = true;
        VrCensusWindowText t;
        t.hook = "installed"; t.camerasTotal = 5; t.offThread = 57;
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
    // The golden lines are pinned above; the fixture carries lines of the same classes from the scripted session.
    const char* classes[] = {"vr camera census 5s: frames=", "vr camera census: camera=0x", "vr camera census: changed: camera=0x",
                             "vr camera census: sequence frame=", "vr camera census: call frame=", "vr camera census: eye=",
                             "vr camera census: eye-geometry eye=", "vr camera census: other-thread tid="};
    unsigned found = 0;
    for (const char* cls : classes) if (built.find(cls) != std::string::npos) ++found;
    check(found == 8, "the fixture holds a line of every class the rig pins: 5 s, camera, changed, sequence, call, eye, eye-geometry, other-thread");
    std::string mutated = normalised;
    const size_t at = mutated.find("vr camera census 5s:");
    if (at != std::string::npos) mutated[at + 3] = '#';
    check(!sameWithinRounding(mutated, built, nullptr), "control: a fixture file with one character altered is no longer the formatters' output, so the row above can fail");
    std::string nudged = normalised;
    const size_t num = nudged.find("aspect=1.777778");
    if (num != std::string::npos) nudged.replace(num, 15, "aspect=1.777879");
    check(num != std::string::npos && !sameWithinRounding(nudged, built, nullptr),
          "control: a number moved in its fifth digit is no longer the formatters' output either");
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

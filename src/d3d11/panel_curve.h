// Bending the on-foot screen, by replacing the quad it is drawn on.
//
// WHY THIS WORKS THE WAY IT DOES
//
// A flat quad cannot curve, whatever its transform says. So the panel
// composite has to be re-issued over a finer mesh with the bend baked into
// its local-space positions -- and the whole art of it is changing nothing
// else. The game's vertex shader, pixel shader, input layout, samplers,
// blend and rasterizer state all stay bound and untouched; only the vertex
// buffer, the index buffer and the topology are swapped for one draw, and
// put back immediately after.
//
// That is possible because of what the field sessions measured (all of it in
// docs/screen-curvature.md): the composite draws the CANONICAL UNIT QUAD --
// four vertices of float3 position plus float2 UV at stride 20, corners at
// plus and minus one, z = 0 -- through a 208-byte constant buffer that sizes
// it, places it at its distance and projects it per eye. Bending coordinates
// in that space is bending the screen in its own space, and the game's own
// transform carries it through per eye for free. EDVR never learns what the
// 208 bytes mean.
//
// It composes with the panel distance fix rather than competing with it:
// that fix substitutes the CONSTANT BUFFER for the same draw, this one
// substitutes the GEOMETRY, and the substituted transform serves the
// substituted mesh exactly as it served the flat quad.
//
// THE STAGED PROOF, which is why segments is a setting and not a constant.
// The strip is numbered bottom row first and its triangles are wound with
// the game's own index pattern, so at segments = 1 and curvature = 0 the
// buffers this builds are BYTE-IDENTICAL to the game's quad and index
// buffer. That splits one ambiguous black screen into three failures that
// can be told apart:
//
//   segments = 1,  curvature = 0   the substitution MECHANISM only
//   segments = 64, curvature = 0   the grid GENERATOR
//   curvature > 0                  the bend, and the sign of z
//
// Off by default. At curvature = 0 with the default segment count nothing is
// built, nothing is bound and the composite path does not call in.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// The game's real DrawIndexedInstanced, passed in rather than looked up.
//
// The substituted draw MUST go through the original function pointer. Issuing
// it through the context's vtable would re-enter our own draw thunk, which
// would recognise the composite again and substitute again, without end. The
// pointer lives in vscreen's state; this module is handed it rather than
// keeping a second copy that could drift from it.
// Spelled in plain C++ types rather than UINT/INT so this header needs no
// windows.h. They are the same types -- UINT is unsigned int and INT is int --
// so vscreen's PFN_DrawIndexedInstanced assigns to this without a cast, which
// is the point: a cast here would paper over a signature that had drifted.
typedef void(__stdcall* PanelCurveDrawFn)(ID3D11DeviceContext*, unsigned int,
                                          unsigned int, unsigned int, int,
                                          unsigned int);

// Reads fix.panel_curvature and the two advanced keys. Both config paths,
// live -- the whole staged proof above depends on being able to walk the
// three cases without restarting the game.
void panelCurveConfigure(Config& cfg);

// Is a substitution wanted at all? False when curvature is 0 and the segment
// count is the default, and false for the rest of the session once the fault
// budget has stood the feature down.
//
// Inline: asked per draw, and the build has no /GL to fold a cross-TU
// getter for four scalar loads. kDefaultSegments lives here too, so this
// can compare against it; the .cpp's segment default and clamp keep using
// it unqualified through a using-declaration.
namespace detail {
constexpr int kDefaultSegments = 64;
extern bool  g_panelCurveStoodDown;
extern float g_panelCurveCurvature;
extern int   g_panelCurveSegments;
// What the VR world route's lines say about the curve (vr_world_route.cpp, panelCurveInfo below). Written only by panel_curve.cpp:
// the depth gain the strip in hand was built with (0 until one is), whether the substitution drew its strip at its last attempt, and how
// many strips the route's layer has re-issued (panelCurveReissue).
extern float    g_panelCurveGain;
extern bool     g_panelCurveReady;
extern uint64_t g_panelCurveReissues;
}  // namespace detail
// __forceinline, not inline: beginPanelOverride is large enough that MSVC's
// inliner declined this one and called an out-of-line copy per eye draw
// (21 innermost samples of the 1355-frame parked-5 window; the built DLL of
// 2026-09-22 round three still called it).
__forceinline bool panelCurveWants() {
    if (detail::g_panelCurveStoodDown) return false;
    // Curvature 0 at the default segment count is the shipped state and does
    // nothing at all. A non-default segment count at curvature 0 is the
    // deliberate identity test, which has to substitute in order to prove
    // anything -- so it counts as wanting.
    return detail::g_panelCurveCurvature > 0.0f ||
           detail::g_panelCurveSegments != detail::kDefaultSegments;
}

// Replace one recognised composite draw with the bent strip: save the input
// assembler state actually touched, bind ours, issue the equivalent draw,
// put the saved state back.
//
// Returns whether the game's own draw must now be SWALLOWED. False means
// nothing was substituted and the caller must forward the draw as usual --
// which is the honest answer when the buffers could not be built, and is why
// a failure here is a flat screen rather than a missing one.
//
// withMotion: also issue the screen's per-eye motion pass (screenMotionDraw) with the strip, as the game's own draw's tail does for a flat
// screen. The VR world route passes false for a frame it owns: the eye it re-issues into the layer (panelCurveReissue) goes through the
// layer-only door, no temporal pass runs for it, and the motion would be drawn for nothing -- the flat screen's tail skips it for the
// same frames (vscreen.cpp). The default is the substitution as it always was.
bool panelCurveSubstitute(ID3D11DeviceContext* ctx, PanelCurveDrawFn draw, bool withMotion = true);

// The VR world route's re-issue of a CURVED screen (ui_layer.h uiLayerWorldReissueBegin/End): the very strip panelCurveSubstitute has
// just drawn for the game's own draw, drawn once more by the same code (one helper binds the strip and issues the draw, so the two
// cannot differ), into whatever the caller has bound -- the eye's layer, the mipped screen at PS slot 0 -- with every other binding the
// game's still bound: its vertex shader, its placement constants (the panel-distance override's included, while the caller is inside
// that bracket), its rasterizer state. Nothing is learned or built here: the strip exists because the substitution just used it.
//
// panelCurveReissueReady: the strip in hand is the one the current configuration asks for and the feature has not stood down. Asked
// BEFORE the layer's bracket is opened, so a re-issue that cannot happen never opens one. panelCurveReissue returns whether it drew;
// a fault stands the whole feature down (the substitution's policy: the first fault, for the session) and returns false.
bool panelCurveReissueReady();
bool panelCurveReissue(ID3D11DeviceContext* ctx, PanelCurveDrawFn draw);

// What the route's 5 s line and its OWNS line say. Inline over the detail state, so a rig that does not link panel_curve.cpp
// (tools\vr_world_route_gpu_test) supplies the variables the way it supplies the three above.
struct PanelCurveInfo {
    bool wanted = false;       // panelCurveWants(): a substitution is asked for (curvature above 0, or the identity test's segment count)
    bool standDown = false;    // the feature stood itself down for the session (a fault, or a SIZE that cannot be a panel's)
    bool ready = false;        // the substitution drew its strip at its last attempt (false while it is still learning the panel's SIZE)
    float curvature = 0.0f;    // fix.panel_curvature as read
    int segments = 0;          // advanced.panel_curvature_segments as read
    float gain = 0.0f;         // the depth gain the strip in hand was built with, in the panel's model units (0 before the first)
    uint64_t reissues = 0;     // strips the route's layer has re-issued, cumulative
};
inline PanelCurveInfo panelCurveInfo() {
    PanelCurveInfo i;
    i.wanted = panelCurveWants();
    i.standDown = detail::g_panelCurveStoodDown;
    i.ready = detail::g_panelCurveReady;
    i.curvature = detail::g_panelCurveCurvature;
    i.segments = detail::g_panelCurveSegments;
    i.gain = detail::g_panelCurveGain;
    i.reissues = detail::g_panelCurveReissues;
    return i;
}

// Releases the grid buffers. From the vScreen shutdown, which is the only
// place that knows the device is still alive.
void panelCurveShutdown();

}  // namespace edvr

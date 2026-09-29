#pragma once

// Fixed extra HLSL: verbatim source extracted from runtime callers.

#include <string>

#include "fsr_hlsl_gen.h"

#include "particle_vs.h"

#include "flare_vs.h"

#include "sunglare_vs.h"

namespace edvr { namespace fixed_extra_source {

inline std::string joinChunks(const char* const* chunks) { std::string s; for (; *chunks; ++chunks) s += *chunks; return s; }

namespace backdrop_fix {

constexpr char kBackdropCsHlsl[] = R"HLSL(
Texture2D<float4>   S : register(t0);
RWTexture2D<float4> O : register(u0);
cbuffer P : register(b0) {
    float4 p;   // x = radius in texels, y = flatness threshold, z = dither
};

float mx3(float3 v) { return max(max(v.x, v.y), v.z); }

// Interleaved gradient noise: a pure function of position. The bake is
// therefore deterministic, and since ONE texture feeds both eyes the dither
// cannot differ between them -- the failure mode the FSS arc spent forty
// rounds on, absent here by construction rather than by care.
float ign(float2 q) {
    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    O.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;

    int2 c0 = int2(id.xy);
    int  r  = int(p.x);
    int2 lo = int2(0, 0);
    int2 hi = int2(int(w) - 1, int(h) - 1);

    float4 c  = S[c0];
    float3 n0 = S[clamp(c0 + int2( r,  0), lo, hi)].rgb;
    float3 n1 = S[clamp(c0 + int2(-r,  0), lo, hi)].rgb;
    float3 n2 = S[clamp(c0 + int2( 0,  r), lo, hi)].rgb;
    float3 n3 = S[clamp(c0 + int2( 0, -r), lo, hi)].rgb;

    // The threshold is the whole difference between a deband and a blur. A
    // star, a hull edge, any real structure exceeds it and passes through
    // untouched; only a neighbourhood already flat to within a quantization
    // step or two is averaged -- which is exactly where the step between two
    // block endpoints shows as a contour.
    float d = max(max(mx3(abs(n0 - c.rgb)), mx3(abs(n1 - c.rgb))),
                  max(mx3(abs(n2 - c.rgb)), mx3(abs(n3 - c.rgb))));
    // A SOFT weight, not a hard switch. "d < threshold ? average : centre"
    // makes adjacent pixels land on opposite sides of a cliff, and five
    // chained passes bake each cliff in and re-average it -- which the first
    // field run saw as blotches that are not in the source. Fading the
    // average out as the neighbourhood stops being flat has no boundary to
    // see, and at d = 0 it is still the full average.
    float flat = saturate(1.0 - d / max(p.y, 1e-6));
    float3 o = lerp(c.rgb, (n0 + n1 + n2 + n3) * 0.25, flat);

    // Dither on the final pass only: about one LSB, enough to break the last
    // residual contour and far below what reads as noise.
    if (p.z > 0.0) o += (ign(float2(c0)) - 0.5) * p.z;

    O[id.xy] = float4(saturate(o), c.a);
}
)HLSL";

}

namespace sharpen_pass {

const char kSharpenMain[] =
    "Texture2D<float4> Src : register(t0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) { uint4 con; int4 region; int2 outSize; int2 pad0; };\n"
    "AF4 FsrRcasLoadF(ASU2 p) {\n"
    "    int2 q = clamp(int2(p), region.xy, region.zw - 1);\n"
    "    return Src.Load(int3(q, 0));\n"
    "}\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= (uint)outSize.x || id.y >= (uint)outSize.y) return;\n"
    "    int2 ip = int2(id.xy) + region.xy;\n"
    "    AF3 c;\n"
    "    FsrRcasF(c.r, c.g, c.b, AU2(ip), con);\n"
    "    Dst[id.xy] = float4(c, Src.Load(int3(ip, 0)).a);\n"
    "}\n";

const char kGpuPrologue[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n";

const std::string kRcasSource = std::string(kGpuPrologue) + joinChunks(kFfxAChunks) + "#define FSR_RCAS_F 1\n" + joinChunks(kFfxFsr1Chunks) + kSharpenMain;

}

namespace menu_panel {

constexpr char kCompositeCs[] = R"HLSL(
Texture2D<float4> S : register(t0);
Texture2D<float4> P : register(t1);
SamplerState L : register(s0);
RWTexture2D<float4> O : register(u0);
cbuffer C : register(b0) {
    int4   region;     // the pixels of S this eye owns (x1, y1 exclusive)
    int2   outSize;
    int    flipV;      // the submit's rows run bottom-up
    int    linearOut;  // the frame is linear light: linearise the panel
    int4   box;        // the output pixels this dispatch covers (x1, y1 exclusive)
    float4 tans;       // left, right, top, bottom tangent magnitudes
    float4 m0;         // current-head -> anchor rotation rows; .w = origin
    float4 m1;
    float4 m2;
    float4 geom;       // dist, curve, halfW, halfH
    float4 misc;       // alpha
};
float3 toLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint2 id = uint2(box.x + tid.x, box.y + tid.y);
    if (id.x >= (uint)box.z || id.y >= (uint)box.w) return;
    float4 src = S.Load(int3(region.x + id.x, region.y + id.y, 0));
    float u = (id.x + 0.5) / outSize.x;
    float v = (id.y + 0.5) / outSize.y;
    if (misc.z > 0.5) u = 1.0 - u;
    if (flipV) v = 1.0 - v;
    float tx = lerp(-tans.x, tans.y, u);
    float ty = lerp(tans.z, -tans.w, v);
    float3 dv = float3(tx, ty, -1.0);
    float3 df = float3(dot(m0.xyz, dv), dot(m1.xyz, dv), dot(m2.xyz, dv));
    float3 org = float3(m0.w, m1.w, m2.w);
    float dist = geom.x, curve = geom.y, halfW = geom.z, halfH = geom.w;
    float su = -1.0, sv = -1.0;
    if (curve > 0.005) {
        float R = dist / curve;
        float zc = R - dist;
        float a = df.x * df.x + df.z * df.z;
        float b = 2.0 * (org.x * df.x + (org.z - zc) * df.z);
        float c = org.x * org.x + (org.z - zc) * (org.z - zc) - R * R;
        float disc = b * b - 4.0 * a * c;
        if (disc > 0 && a > 1e-8) {
            float t = (-b + sqrt(disc)) / (2.0 * a);
            if (t > 0) {
                float3 hit = org + t * df;
                float th = atan2(hit.x, zc - hit.z);
                su = (th * R - misc.y + halfW) / (2.0 * halfW);
                sv = (hit.y + halfH) / (2.0 * halfH);
            }
        }
    } else if (df.z < -1e-4) {
        float t = (-dist - org.z) / df.z;
        if (t > 0) {
            float3 hit = org + t * df;
            su = (hit.x - misc.y + halfW) / (2.0 * halfW);
            sv = (hit.y + halfH) / (2.0 * halfH);
        }
    }
    float4 outc = src;
    if (su >= 0 && su <= 1 && sv >= 0 && sv <= 1) {
        float4 p = P.SampleLevel(L, float2(su, 1.0 - sv), 0);
        if (linearOut) {
            float pa = max(p.a, 1e-4);
            p.rgb = toLinear(p.rgb / pa) * pa;
        }
        float a = p.a * misc.x;
        outc = float4(src.rgb * (1.0 - a) + p.rgb * misc.x, src.a);
    }
    O[id.xy] = outc;
}
)HLSL";

}

namespace hud_sprite {

const char kGpuPrologue[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n";

const char kEasuMain[] =
    "Texture2D<float4> Src : register(t0);\n"
    "SamplerState Smp : register(s0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) {\n"
    "    uint4 con0; uint4 con1; uint4 con2; uint4 con3; uint2 dstSize;\n"
    "};\n"
    "AF4 FsrEasuRF(AF2 p) { return Src.GatherRed(Smp, p); }\n"
    "AF4 FsrEasuGF(AF2 p) { return Src.GatherGreen(Smp, p); }\n"
    "AF4 FsrEasuBF(AF2 p) { return Src.GatherBlue(Smp, p); }\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    AF3 c;\n"
    "    FsrEasuF(c, id.xy, con0, con1, con2, con3);\n"
    "    // ALPHA MATTERS HERE, unlike the intro movie. These are cut-out\n"
    "    // sprites on a transparent atlas, and EASU has no alpha path -- so\n"
    "    // alpha is resampled with a plain bilinear tap at the same place.\n"
    "    float2 uv = (float2(id.xy) + 0.5) / float2(dstSize);\n"
    "    float a = Src.SampleLevel(Smp, uv, 0).a;\n"
    "    Dst[id.xy] = float4(c, a);\n"
    "}\n";

const char kRcasMain[] =
    "Texture2D<float4> Src : register(t0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) { uint4 con; uint2 dstSize; };\n"
    "AF4 FsrRcasLoadF(ASU2 p) { return Src.Load(int3(p, 0)); }\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    AF3 c;\n"
    "    FsrRcasF(c.r, c.g, c.b, id.xy, con);\n"
    "    Dst[id.xy] = float4(c, Src.Load(int3(id.xy, 0)).a);\n"
    "}\n";

const std::string kEasuSource = std::string(kGpuPrologue) + joinChunks(kFfxAChunks) + "#define FSR_EASU_F 1\n" + joinChunks(kFfxFsr1Chunks) + kEasuMain;

const std::string kRcasSource = std::string(kGpuPrologue) + joinChunks(kFfxAChunks) + "#define FSR_RCAS_F 1\n" + joinChunks(kFfxFsr1Chunks) + kRcasMain;

}

namespace panel_upscale {

const char kGpuPrologue[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n";

const char kEasuMain[] =
    "Texture2D<float4> Src : register(t0);\n"
    "SamplerState Smp : register(s0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) {\n"
    "    uint4 con0; uint4 con1; uint4 con2; uint4 con3; uint2 dstSize;\n"
    "};\n"
    "AF4 FsrEasuRF(AF2 p) { return Src.GatherRed(Smp, p); }\n"
    "AF4 FsrEasuGF(AF2 p) { return Src.GatherGreen(Smp, p); }\n"
    "AF4 FsrEasuBF(AF2 p) { return Src.GatherBlue(Smp, p); }\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    AF3 c;\n"
    "    FsrEasuF(c, id.xy, con0, con1, con2, con3);\n"
    "    // The panel is composited over the cockpit, so ALPHA is its shape.\n"
    "    // EASU has no alpha path; a bilinear tap at the same place keeps\n"
    "    // the coverage the game drew.\n"
    "    float2 uv = (float2(id.xy) + 0.5) / float2(dstSize);\n"
    "    Dst[id.xy] = float4(c, Src.SampleLevel(Smp, uv, 0).a);\n"
    "}\n";

const char kRcasMain[] =
    "Texture2D<float4> Src : register(t0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) { uint4 con; uint2 dstSize; };\n"
    "AF4 FsrRcasLoadF(ASU2 p) { return Src.Load(int3(p, 0)); }\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    AF3 c;\n"
    "    FsrRcasF(c.r, c.g, c.b, id.xy, con);\n"
    "    Dst[id.xy] = float4(c, Src.Load(int3(id.xy, 0)).a);\n"
    "}\n";

const std::string kEasuSource = std::string(kGpuPrologue) + joinChunks(kFfxAChunks) + "#define FSR_EASU_F 1\n" + joinChunks(kFfxFsr1Chunks) + kEasuMain;

const std::string kRcasSource = std::string(kGpuPrologue) + joinChunks(kFfxAChunks) + "#define FSR_RCAS_F 1\n" + joinChunks(kFfxFsr1Chunks) + kRcasMain;

}

namespace intro_upscale {

const char kDebandHlsl[] =
    "Texture2D<float4> S : register(t0);\n"
    "RWTexture2D<float4> O : register(u0);\n"
    "cbuffer P : register(b0) { float4 p; };\n"
    "float mx3(float3 v) { return max(max(v.x, v.y), v.z); }\n"
    "float ign(float2 q) {\n"
    "    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));\n"
    "}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    uint w, h;\n"
    "    O.GetDimensions(w, h);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    int2 c0 = int2(id.xy);\n"
    "    int r = int(p.x);\n"
    "    int2 lo = int2(0, 0);\n"
    "    int2 hi = int2(int(w) - 1, int(h) - 1);\n"
    "    float4 c = S[c0];\n"
    "    float3 n0 = S[clamp(c0 + int2( r, 0), lo, hi)].rgb;\n"
    "    float3 n1 = S[clamp(c0 + int2(-r, 0), lo, hi)].rgb;\n"
    "    float3 n2 = S[clamp(c0 + int2( 0, r), lo, hi)].rgb;\n"
    "    float3 n3 = S[clamp(c0 + int2( 0,-r), lo, hi)].rgb;\n"
    "    float d = max(max(mx3(abs(n0 - c.rgb)), mx3(abs(n1 - c.rgb))),\n"
    "                  max(mx3(abs(n2 - c.rgb)), mx3(abs(n3 - c.rgb))));\n"
    "    float flatness = saturate(1.0 - d / max(p.y, 1e-6));\n"
    "    float3 o = lerp(c.rgb, (n0 + n1 + n2 + n3) * 0.25, flatness);\n"
    "    if (p.z > 0.0) o += (ign(float2(c0)) - 0.5) * p.z;\n"
    "    O[id.xy] = float4(saturate(o), c.a);\n"
    "}\n";

const char kEasuMain[] =
    "Texture2D<float4> Src : register(t0);\n"
    "SamplerState Smp : register(s0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) {\n"
    "    uint4 con0; uint4 con1; uint4 con2; uint4 con3; uint2 dstSize;\n"
    "};\n"
    "AF4 FsrEasuRF(AF2 p) { return Src.GatherRed(Smp, p); }\n"
    "AF4 FsrEasuGF(AF2 p) { return Src.GatherGreen(Smp, p); }\n"
    "AF4 FsrEasuBF(AF2 p) { return Src.GatherBlue(Smp, p); }\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    AF3 c;\n"
    "    FsrEasuF(c, id.xy, con0, con1, con2, con3);\n"
    "    Dst[id.xy] = float4(c, 1.0);\n"
    "}\n";

const char kRcasMain[] =
    "Texture2D<float4> Src : register(t0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) { uint4 con; uint2 dstSize; };\n"
    "AF4 FsrRcasLoadF(ASU2 p) { return Src.Load(int3(p, 0)); }\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    AF3 c;\n"
    "    FsrRcasF(c.r, c.g, c.b, id.xy, con);\n"
    "    Dst[id.xy] = float4(c, 1.0);\n"
    "}\n";

const char kGpuPrologue[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n";

const char kCubicHlsl[] =
    "Texture2D<float4> Src : register(t0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) { uint2 srcSize; uint2 dstSize; };\n"
    "float w(float t) {\n"
    "    t = abs(t);\n"
    "    if (t <= 1.0) return 1.5*t*t*t - 2.5*t*t + 1.0;\n"
    "    if (t <  2.0) return -0.5*t*t*t + 2.5*t*t - 4.0*t + 2.0;\n"
    "    return 0.0;\n"
    "}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    float2 sp = (float2(id.xy) + 0.5) *\n"
    "                (float2(srcSize) / float2(dstSize)) - 0.5;\n"
    "    int2 b = int2(floor(sp));\n"
    "    float2 f = sp - float2(b);\n"
    "    float wx[4], wy[4];\n"
    "    [unroll] for (int k = 0; k < 4; ++k) {\n"
    "        wx[k] = w(float(k - 1) - f.x);\n"
    "        wy[k] = w(float(k - 1) - f.y);\n"
    "    }\n"
    "    float4 acc = 0.0;\n"
    "    float sum = 0.0;\n"
    "    [unroll] for (int j = 0; j < 4; ++j) {\n"
    "        [unroll] for (int i = 0; i < 4; ++i) {\n"
    "            int2 q = clamp(b + int2(i - 1, j - 1), int2(0, 0),\n"
    "                           int2(srcSize) - 1);\n"
    "            float cw = wx[i] * wy[j];\n"
    "            acc += Src.Load(int3(q, 0)) * cw;\n"
    "            sum += cw;\n"
    "        }\n"
    "    }\n"
    "    if (sum > 0.0) acc /= sum;\n"
    "    Dst[id.xy] = float4(saturate(acc.rgb), acc.a);\n"
    "}\n";

const std::string kEasuSource = std::string(kGpuPrologue) + joinChunks(kFfxAChunks) + "#define FSR_EASU_F 1\n" + joinChunks(kFfxFsr1Chunks) + kEasuMain;

const std::string kRcasSource = std::string(kGpuPrologue) + joinChunks(kFfxAChunks) + "#define FSR_RCAS_F 1\n" + joinChunks(kFfxFsr1Chunks) + kRcasMain;

}

namespace eye_mask {

constexpr char kRingVsHlsl[] = R"HLSL(
cbuffer EyeMaskCB : register(b0) {
    float2 centre;
    float2 axes;
    float outerScale;
    float depthValue;
    float segments;
    float pad0;
};
float4 main(uint id : SV_VertexID) : SV_POSITION {
    uint seg = id / 2;
    uint parity = id - seg * 2;
    float theta = float(seg) * (6.283185307179586 / segments);
    float c = cos(theta);
    float s = sin(theta);
    float scale = parity == 0 ? 1.0 : outerScale;
    float2 p = centre + float2(axes.x * c, axes.y * s) * scale;
    return float4(p, depthValue, 1.0);
}
)HLSL";

}

namespace splash_dim {

constexpr char kPsHlsl[] =
    "float4 main() : SV_Target { return float4(0.0, 0.0, 0.0, 0.4); }";

}

namespace fss_heal {

constexpr char kHealCsHlsl[] = R"HLSL(
Texture2D<float4> L : register(t0);
Texture2D<float4> R : register(t1);
RWTexture2D<float4> O : register(u0);
cbuffer P : register(b0) {
    float4 p;   // p.x = dx pixels; p.yz = screen AABB min (u,v);
    float4 q;   // p.w,q.x = AABB max -- packed: yz=min, w+q.x=max
}
// Wireframe blue: the blue channel meaningfully ahead of red. Floorless,
// so dim antialiased edges are caught; red-relative, so neutral and warm
// content (ring, body, white bloom) is not.
bool isWire(float4 c) { return c.b > c.r * 1.35 + 0.02; }
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    O.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 l = L[id.xy];
    float4 o = l;
    // Round 48e, the SPATIAL scope: the fill exists only inside the
    // scanner screen's rectangle -- the squares live on the body inside
    // it, the near-field neon frame lives around it, and filling beside
    // the neon at the infinity disparity painted offset twins.
    float u = (id.x + 0.5) / w;
    float v = (id.y + 0.5) / h;
    bool inScreen = u >= p.y && u <= p.w && v >= p.z && v <= q.x;
    if (!inScreen) {
        O[id.xy] = o;
        return;
    }
    // Round 48b, the WINDOW-ERA classifier: one test. The heal now exists
    // only inside the zoom-press arrival window -- body at optical
    // infinity, virtually no UI -- so the v5 gate stack (interior,
    // square-scale, right-lit, bright-region) that protected menus in
    // its always-on life is pure fill-suppression here: it left the
    // squares over DIM ring content black and speckled tile boundaries.
    // A hard-black left pixel takes the right's pixel at the infinity
    // shift, whatever it is: filling space-black with space-black is a
    // no-op, and bright chrome is never hard-black.
    if (dot(l.rgb, float3(0.299, 0.587, 0.114)) < 0.004) {
        int rx = int(id.x) - int(round(p.x));
        uint rw, rh;
        R.GetDimensions(rw, rh);
        if (rx >= 0 && rx < int(rw)) {
            float4 rp = R[uint2(uint(rx), id.y)];
            // The neon wireframe lives in PLAYER space, not at the
            // body's optical infinity -- its right-eye pixels are the
            // wrong disparity for this shift, and stamping them paints
            // offset twins of the blue lines (the field's report, three
            // times now). During the zoom TRANSIT the source region is
            // void plus wireframe and nothing else, so every visible
            // fill in those ~3 s is contamination by definition. Round
            // 49's veto (b > 0.10 and b > 1.6r) let two tails through:
            // dim antialiased bar edges under the 0.10 floor, and
            // bloom-brightened cores whose lifted red defeats the
            // ratio. The test is now floorless and red-relative, and a
            // core that blooms to near-white is caught by its GLOW: the
            // four axis neighbours at 4 px are tested too -- a bar is
            // thinner than 8 px, so some neighbour is always still
            // blue. A vetoed source keeps the left's black -- the stock
            // look, never a new artifact. (Known cost: strongly
            // blue-dominant body content can keep its squares;
            // preferred over ever painting the wireframe.)
            bool wire = isWire(rp) ||
                        isWire(R[uint2(min(uint(rx) + 4u, rw - 1u), id.y)]) ||
                        isWire(R[uint2(uint(max(rx - 4, 0)), id.y)]) ||
                        isWire(R[uint2(uint(rx), min(id.y + 4u, rh - 1u))]) ||
                        isWire(R[uint2(uint(rx), uint(max(int(id.y) - 4, 0)))]);
            if (!wire) o = rp;
        }
    }
    O[id.xy] = o;
}
)HLSL";

constexpr char kMirrorCsHlsl[] = R"HLSL(
Texture2D<float4> L : register(t0);
Texture2D<float4> R : register(t1);
RWTexture2D<float4> O : register(u0);
cbuffer P : register(b0) { float4 p; }   // p.x = dx pixels (left minus right)
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    O.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 r = R[id.xy];
    float4 o = r;
    float rl = dot(r.rgb, float3(0.299, 0.587, 0.114));
    if (rl > 0.02) {
        uint lw, lh;
        L.GetDimensions(lw, lh);
        int lx = int(id.x) + int(round(p.x));
        if (lx >= 0 && lx < int(lw)) {
            const int dys[3] = {0, -16, 16};
            [unroll] for (int i = 0; i < 3; ++i) {
                int ly = int(id.y) + dys[i];
                if (ly < 0 || ly >= int(lh)) continue;
                if (dot(L[uint2(uint(lx), uint(ly))].rgb,
                        float3(0.299, 0.587, 0.114)) >= 0.004) continue;
                // interior of a black region in the left...
                bool blk = true;
                int2 c = int2(lx, ly);
                int2 offs[4] = {int2(-2, 0), int2(2, 0), int2(0, -2),
                                int2(0, 2)};
                [unroll] for (int k = 0; k < 4; ++k) {
                    int2 q = c + offs[k];
                    if (q.x < 0 || q.y < 0 || q.x >= int(lw) ||
                        q.y >= int(lh)) continue;
                    if (dot(L[uint2(q)].rgb,
                            float3(0.299, 0.587, 0.114)) >= 0.004) {
                        blk = false;
                        break;
                    }
                }
                if (!blk) continue;
                // ...at square scale, not panel background or space
                bool farLit = false;
                int2 far4[4] = {int2(-10, 0), int2(10, 0), int2(0, -10),
                                int2(0, 10)};
                [unroll] for (int k2 = 0; k2 < 4; ++k2) {
                    int2 q2 = c + far4[k2];
                    if (q2.x < 0 || q2.y < 0 || q2.x >= int(lw) ||
                        q2.y >= int(lh)) continue;
                    if (dot(L[uint2(q2)].rgb,
                            float3(0.299, 0.587, 0.114)) >= 0.004) {
                        farLit = true;
                        break;
                    }
                }
                if (farLit) o = float4(0, 0, 0, r.a);
                break;
            }
        }
    }
    O[id.xy] = o;
}
)HLSL";

}

namespace fss_theater {

constexpr char kTheaterCsHlsl[] = R"HLSL(
Texture2D<float4> C : register(t0);
Texture2D<float4> C2 : register(t1);   // the RIGHT eye, for the stitch
SamplerState S0 : register(s0);
RWTexture2D<float4> O : register(u0);
cbuffer P : register(b0) {
    float4 tans;   // x = left-extent tan, y = right-extent tan,
                   // z = vertical half tan, w = panel distance
    float4 m0;     // delta rotation rows (current-head -> frozen-head);
    float4 m1;     // the rows' .w = this eye's ray origin in frozen-head
    float4 m2;     // space, translation and eye offset folded in
    float4 misc;   // x = vertical band (fraction of the texture's height
                   // the screen shows, centre-cropped), y = half width
                   // along the surface, z = half height, w = curve
    float4 hA;     // square->quad homography for the scanner screen's
    float4 hB;     // quad in the LEFT eye: pu=(hA.x sx + hA.y sy +
                   // hA.z)/den, pv=(hA.w sx + hB.x sy + hB.y)/den,
                   // den=hB.z sx + hB.w sy + 1. All-zero hA = no quad;
                   // the centred band applies.
    float4 hA2;    // the RIGHT eye's homography. Where valid, the panel's
    float4 hB2;    // right half samples the right eye instead -- each eye
                   // is clean on its temporal side, so the nose-mask
                   // cutouts never reach the screen. All-zero = left only.
}
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    O.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float u = (id.x + 0.5) / w;
    float v = (id.y + 0.5) / h;
    // View-space ray, OpenVR convention: -Z forward, +Y up, +X right;
    // pixel rows run top to bottom.
    float tx = lerp(-tans.x, tans.y, u);
    float ty = lerp(tans.z, -tans.z, v);
    float3 dv = float3(tx, ty, -1.0);
    float3 df = float3(dot(m0.xyz, dv), dot(m1.xyz, dv), dot(m2.xyz, dv));
    float3 org = float3(m0.w, m1.w, m2.w);
    float4 outc = float4(0, 0, 0, 1);
    float su = -1.0, sv = -1.0;
    if (misc.w > 0.005) {
        // Curved screen: a vertical cylinder of radius dist/curve whose
        // arc centre sits at the screen distance; u runs along the arc so
        // the content keeps its width. The viewer is always inside the
        // cylinder (zc < R), so the ray's forward intersection is the +
        // root of the quadratic.
        float R = tans.w / misc.w;
        float zc = R - tans.w;
        float a = df.x * df.x + df.z * df.z;
        float b = 2.0 * (org.x * df.x + (org.z - zc) * df.z);
        float c = org.x * org.x + (org.z - zc) * (org.z - zc) - R * R;
        float disc = b * b - 4.0 * a * c;
        if (disc > 0 && a > 1e-8) {
            float t = (-b + sqrt(disc)) / (2.0 * a);
            if (t > 0) {
                float3 hit = org + t * df;
                float th = atan2(hit.x, zc - hit.z);
                su = (th * R + misc.y) / (2.0 * misc.y);
                sv = (hit.y + misc.z) / (2.0 * misc.z);
            }
        }
    } else if (df.z < -1e-4) {
        float t = (-tans.w - org.z) / df.z;
        if (t > 0) {
            float3 hit = org + t * df;
            su = (hit.x + misc.y) / (2.0 * misc.y);
            sv = (hit.y + misc.z) / (2.0 * misc.z);
        }
    }
    if (su >= 0 && su <= 1 && sv >= 0 && sv <= 1) {
        // Screen fractions -> content coordinates. With a derived quad,
        // the square->quad homography rectifies the scanner's screen --
        // level and fully framed whatever its tilt or the head's pose at
        // engage; otherwise the centred band. sv runs bottom-up in
        // space; the homography's sy runs top-down like the texture.
        float pu, pv;
        if (hA.x != 0.0 || hA.y != 0.0 || hA.z != 0.0) {
            float sx = su;
            float sy = 1.0 - sv;
            // The stitch: the panel's left half from the left eye, right
            // half from the right eye when its homography is valid --
            // each eye clean on its temporal side, so the nasal
            // hidden-area cutouts (the nose shadow) never reach the
            // screen. The screen sits at optical-far depth, so the two
            // eyes' images agree at the seam.
            bool useR = sx >= 0.5 &&
                        (hA2.x != 0.0 || hA2.y != 0.0 || hA2.z != 0.0);
            float den, s2;
            if (useR) {
                den = hB2.z * sx + hB2.w * sy + 1.0;
                pu = (hA2.x * sx + hA2.y * sy + hA2.z) / den;
                pv = (hA2.w * sx + hB2.x * sy + hB2.y) / den;
            } else {
                den = hB.z * sx + hB.w * sy + 1.0;
                pu = (hA.x * sx + hA.y * sy + hA.z) / den;
                pv = (hA.w * sx + hB.x * sy + hB.y) / den;
            }
            // A sample past the rendered eye means this strip was never
            // drawn: honest black, not a clamped smear.
            if (pu < 0.0 || pu > 1.0 || pv < 0.0 || pv > 1.0) {
                O[id.xy] = float4(0, 0, 0, 1);
                return;
            }
            outc = useR
                       ? float4(C2.SampleLevel(S0, float2(pu, pv), 0).rgb, 1)
                       : float4(C.SampleLevel(S0, float2(pu, pv), 0).rgb, 1);
            O[id.xy] = outc;
            return;
        } else {
            pu = su;
            pv = 0.5 + (0.5 - sv) * misc.x;
        }
        outc = float4(C.SampleLevel(S0, float2(pu, pv), 0).rgb, 1);
    }
    O[id.xy] = outc;
}
)HLSL";

}

namespace fss_ring {

constexpr char kCleanPsHlsl[] = R"HLSL(
// The FSS ring quad's pixel shader (ps 7CECABDE34FFBE9E), transcribed
// mechanically from docs/shaders/fss-ring-ps.asm with ONE semantic change: the
// flag byte's bit 4 -- the dissolve's hard-black "unrevealed" state, which
// collapses the exposure lerp to zero and paints the black squares -- is
// treated as never set. The soft cb2[46] fade-in and every lighting term
// are transcribed unchanged.

Texture2D<float4> t0 : register(t0);   // x = ao, yz = packed normal
Texture2D<float4> t1 : register(t1);   // rgb = albedo, a*255 = flag byte
Texture2D<float4> t2 : register(t2);   // material (loaded .yzxw)
Texture2D<float4> t3 : register(t3);   // illumination
cbuffer CB2 : register(b2) { float4 c[47]; }

struct PsIn {
    float2 uv   : TEXCOORD0;
    float3 view : TEXCOORD1;
};
struct PsOut {
    float3 c0 : SV_Target0;
    float  c1 : SV_Target1;
};

PsOut main(PsIn i) {
    float2 fp = floor(i.uv * c[1].xy);
    uint2  lim = uint2(c[1].xy + float2(-1.0, -1.0));
    uint2  p = min(lim, uint2(fp));

    float4 alb = t1.Load(int3(p, 0));
    uint flags = (uint)(alb.w * 255.0 + 0.5);

    float3 na = t0.Load(int3(p, 0)).xyz;
    float2 n2 = na.yz * 4.0 - 2.0;
    float  d2 = dot(n2, n2);
    float  tz = sqrt(max(1.0 - d2 * 0.25, 0.0));
    float3 ns = normalize(float3(n2 * tz, d2 * 0.5 - 1.0));
    float3 N = ns.x * c[2].xyz + ns.y * c[3].xyz + ns.z * c[4].xyz;
    float  ao = clamp(na.x, 0.05, 1.0);

    float4 mat = t2.Load(int3(p, 0)).yzxw;

    float atten, attenS;
    if (flags & 128u) {
        atten = 1.0;
        attenS = 1.0;
    } else {
        if (flags & 64u) mat.xy = mat.zz;
        float fade = 1.0 - mat.w;
        atten  = 1.0 - fade * c[46].x;
        attenS = 1.0 - fade * c[46].y;
    }

    float3 V = normalize(i.view);

    // Stock: float blackFlag = (flags & 4u) ? 0.0 : 1.0;  -- THE SQUARES.
    const float blackFlag = 1.0;

    if (((flags & 32u) != 0u) && dot(-c[42].xyz, N) < 0.0) N = -N;

    float3 H = normalize(-V - c[42].xyz);
    float NdL = saturate(dot(N, -c[42].xyz));
    float NdV = saturate(dot(N, -V));
    float NdH = saturate(dot(N, H));

    float  dif = NdL * 0.318310;
    float3 diffuse = alb.xyz * dif;

    float ao2 = ao * ao;
    float ao4 = ao2 * ao2;
    float den = min((NdH * ao4 - NdH) * NdH + 1.000001, 1.0);
    den = den * den * 3.141592;
    float D = ao4 / den;

    float fh = 1.0 - abs(dot(-c[42].xyz, H));
    float f2 = fh * fh;
    float fres = fh * (f2 * f2);
    float3 F = mat.zxy + (1.0 - mat.zxy) * fres;

    float visL = NdL * (2.0 - ao2) + ao2;
    float visV = NdV * (2.0 - ao2) + ao2;
    float3 spec = (D / (visL * visV)) * F * NdL * attenS;

    float3 col = (diffuse * atten + spec) * c[40].xyz;
    float3 aux = dif * c[40].xyz;

    // Stock: lum = dot(t3.Load(p).xyz, c[44].xyz) -- t3 is the per-eye
    // illumination map the game fills tile by tile across the build, and
    // scale = lerp(lum, blackFlag, c[36].x) * c[36].y collapses unfilled
    // tiles to black. The ring's CONTENT (t1) is complete from the first
    // frame; only this map staggers in. Full illumination, always:
    float scale = c[36].y;

    PsOut o;
    o.c0 = scale * col * c[0].x;
    o.c1 = dot(scale * aux, float3(0.308600, 0.609400, 0.082000)) * c[0].x;
    return o;
}
)HLSL";

constexpr char kHoldCsHlsl[] = R"HLSL(
Texture2D<float4> cur  : register(t0);
Texture2D<float4> prev : register(t1);
RWTexture2D<float4> outt : register(u0);
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    cur.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 c = cur[id.xy];
    float4 p = prev[id.xy];
    bool valid = dot(abs(c.xyz), 1.0.xxx) > 0.02;
    outt[id.xy] = valid ? c : p;
}
)HLSL";

}

namespace fss_dump {

constexpr char kSeriesCsHlsl[] = R"HLSL(
Texture2D<float4> src : register(t0);
RWTexture2D<float> outt : register(u0);
cbuffer P : register(b0) { uint4 off; }   // x = yOffset, y = tw, z = th
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= off.y || id.y >= off.z) return;
    float s = 0;
    for (uint j = 0; j < 16; ++j)
        for (uint i = 0; i < 16; ++i) {
            float4 c = src.Load(int3(id.x * 16 + i, id.y * 16 + j, 0));
            s += dot(c.rgb, float3(0.299, 0.587, 0.114));
        }
    outt[uint2(id.x, off.x + id.y)] = s / 256.0;
}
)HLSL";

}

namespace depth_probe {

constexpr char kSampleCsHlsl[] = R"HLSL(
Texture2D<float> D : register(t0);
RWStructuredBuffer<float> O : register(u0);
cbuffer P : register(b0) { uint2 size; uint2 pad0; };
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 16 || id.y >= 16) return;
    uint2 p = uint2((id.x * 2 + 1) * size.x / 32, (id.y * 2 + 1) * size.y / 32);
    p = min(p, size - 1);
    O[id.y * 16 + id.x] = D.Load(int3(int2(p), 0));
}
)HLSL";

}

namespace resolve_probe {

constexpr char kProbePsHlsl[] = R"HLSL(
Texture2D<float4> gt0 : register(t0);
Texture2D<float4> gt1 : register(t1);
Texture2D<float4> gt2 : register(t2);
Texture2D<float4> gt3 : register(t3);

cbuffer Resolve : register(b2) { float4 c[48]; };

struct VSOut {
    float2 uv  : TEXCOORD0;
    float3 ray : TEXCOORD1;
};

struct PSOut {
    float3 col : SV_TARGET0;
    float  lum : SV_TARGET1;
};

PSOut main(VSOut i) {
    PSOut o;
    o.lum = 0.0;
#ifdef PROBE_WHITE
    // Constant everywhere. The resolve covers the whole screen, so anything
    // that is NOT this colour afterwards is a region where the write was
    // rejected downstream of the pixel shader.
    o.col = float3(1.0, 1.0, 1.0);
#else
    int2 px = (int2)floor(i.uv * c[1].xy);
    px = min(px, (int2)c[1].xy - 1);
    px = max(px, int2(0, 0));
    float4 g1 = gt1.Load(int3(px, 0));
    float4 g2 = gt2.Load(int3(px, 0));
    float4 g3 = gt3.Load(int3(px, 0));
    // red = the flag byte (bit 128 bypasses the multiplier below)
    // green = t2.w, the multiplier that blacks the surface when zero
    // blue = the shadow mask
    o.col = float3(g1.w, g2.w, g3.x);
#endif
    return o;
}
)HLSL";

}

namespace foveation {

const char kProbeVs[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0);\n"
    "    return float4(p, 0.5, 1.0);\n"
    "}\n";

const char kProbePs[] =
    "RWByteAddressBuffer counts : register(u1);\n"
    "float2 main(float4 pos : SV_Position) : SV_Target {\n"
    "    uint band = min(uint(pos.y) / 64u, 7u);\n"
    "    counts.InterlockedAdd(band * 4u, 1u);\n"
    "    return pos.xy;\n"
    "}\n";

const char kProbePsDepth[] =
    "RWByteAddressBuffer counts : register(u1);\n"
    "float2 main(float4 pos : SV_Position, out float depth : SV_Depth) : SV_Target {\n"
    "    uint band = min(uint(pos.y) / 64u, 7u);\n"
    "    counts.InterlockedAdd(band * 4u, 1u);\n"
    "    depth = 0.5;\n"
    "    return pos.xy;\n"
    "}\n";

const char kProbeVsUv[] =
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V main(uint id : SV_VertexID) {\n"
    "    V v;\n"
    "    float2 p = float2(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0);\n"
    "    v.pos = float4(p, 0.5, 1.0);\n"
    "    v.uv = p * 0.5 + 0.5;\n"
    "    return v;\n"
    "}\n";

const char kProbePsSample[] =
    "RWByteAddressBuffer counts : register(u1);\n"
    "float2 main(float4 pos : SV_Position, sample float2 uv : TEXCOORD0) : SV_Target {\n"
    "    uint band = min(uint(pos.y) / 64u, 7u);\n"
    "    counts.InterlockedAdd(band * 4u, 1u);\n"
    "    return pos.xy + uv * 0.0;\n"
    "}\n";

const char kProbePsAlpha[] =
    "RWByteAddressBuffer counts : register(u1);\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "    uint band = min(uint(pos.y) / 64u, 7u);\n"
    "    counts.InterlockedAdd(band * 4u, 1u);\n"
    "    return float4(pos.xy, 0.0, 1.0);\n"
    "}\n";

const char kProbePsMrt[] =
    "RWByteAddressBuffer counts : register(u3);\n"
    "struct O { float2 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; };\n"
    "O main(float4 pos : SV_Position) {\n"
    "    uint band = min(uint(pos.y) / 64u, 7u);\n"
    "    counts.InterlockedAdd(band * 4u, 1u);\n"
    "    O o;\n"
    "    o.a = pos.xy;\n"
    "    o.b = float4(0.25, 0.5, 0.75, 1.0);\n"
    "    o.c = float4(1.0, 0.75, 0.5, 0.25);\n"
    "    return o;\n"
    "}\n";

const char kProbePsCoverage[] =
    "RWByteAddressBuffer counts : register(u1);\n"
    "float2 main(float4 pos : SV_Position, float2 uv : TEXCOORD0, uint cov : SV_Coverage) : SV_Target {\n"
    "    uint band = min(uint(pos.y) / 64u, 7u);\n"
    "    counts.InterlockedAdd(band * 4u, 1u);\n"
    "    return pos.xy + uv * 0.0 + float(cov & 0u);\n"
    "}\n";

}

} }

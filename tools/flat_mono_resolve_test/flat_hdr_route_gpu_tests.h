// The HDR route's resolver half on WARP (docs/design-flat-temporal-aa-2026-09-23.md, section 81): the game's R11G11B10F
// scene target goes in, the resolve goes back INTO it through a pixel shader, and nothing else of the game's state moves.
// The backends are the rig's stubs (they inspect their real GPU inputs, the flag they were handed and the formats of the
// textures they name); the shaders, the private input copy, the fp16 outputs and the requantisation are the shipped ones.
//
// What is pinned: the backend is told "HDR" and handed an R11G11B10F input and an fp16 output; a reset frame comes back
// as the jittered image resampled on the unjittered grid, requantised to within one ulp of the format (half an ulp
// where a device rounds to nearest; WARP truncates toward zero, which costs up to one, and the rig prints which); a frame the
// backend resolves comes back as the backend's output where the history is trusted and as the input where it is not;
// EDVR's TAA accumulates in c/(1+max3(c)) space (a hot pixel is tamed where linear blending would carry it) and never
// overshoots the input range or makes a NaN; every refusal leaves the game's texture bit for bit what it was; the game's
// pipeline state is restored on every exit; and the LDR route after an HDR frame is what it was.
#pragma once

namespace hdrgpu {
// R11G11B10_FLOAT by hand: no sign, 5 exponent bits (bias 15), 6 mantissa bits for red and green and 5 for blue.
inline double top(int mant) { return (2.0 - std::ldexp(1.0, -mant)) * 32768.0; }
inline uint32_t encode(double v, int mant) {
    const uint32_t full = (1u << mant);
    if (!(v > 0)) return 0;
    if (v >= top(mant)) return (30u << mant) | (full - 1);
    int e = 0; std::frexp(v, &e);
    int exponent = e - 1;
    if (exponent < -14) {   // denormal: mant * 2^(-14 - mant)
        const uint32_t q = uint32_t(std::floor(v / std::ldexp(1.0, -14 - mant) + 0.5));
        return q >= full ? full : q;
    }
    uint32_t q = uint32_t(std::floor((v / std::ldexp(1.0, exponent) - 1.0) * full + 0.5));
    if (q == full) { q = 0; ++exponent; }
    if (exponent > 15) return (30u << mant) | (full - 1);
    return (uint32_t(exponent + 15) << mant) | q;
}
inline double decode(uint32_t bits, int mant) {
    const uint32_t full = (1u << mant);
    const uint32_t exponent = bits >> mant, q = bits & (full - 1);
    if (exponent == 0) return std::ldexp(double(q), -14 - mant);
    return std::ldexp(1.0 + double(q) / full, int(exponent) - 15);
}
inline uint32_t pack(double r, double g, double b) { return encode(r, 6) | (encode(g, 6) << 11) | (encode(b, 5) << 22); }
inline void unpack(uint32_t p, double (&out)[3]) { out[0] = decode(p & 0x7FF, 6); out[1] = decode((p >> 11) & 0x7FF, 6); out[2] = decode(p >> 22, 5); }
// The gap to the next value the format holds above v.
inline double ulp(double v, int mant) {
    int e = 0; std::frexp(v > 0 ? v : 1e-9, &e);
    int exponent = e - 1; if (exponent < -14) exponent = -14;
    return std::ldexp(1.0, exponent - mant);
}
inline int mantissa(int channel) { return channel == 2 ? 5 : 6; }
// A texel as three doubles, after one trip through the format.
inline void representable(double r, double g, double b, double (&out)[3]) { unpack(pack(r, g, b), out); }
}  // namespace hdrgpu

inline void hdrRouteGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace hdrgpu;
    using edvr::FlatMonoResolveFrame;
    using edvr::FlatMonoResolveMode;
    const UINT w = 16, h = 16;

    // The engine inputs, as the copy route's fixture builds them: no engine slot anywhere (the camera term moves pixels),
    // a pool with one empty record, scene constants for a still camera.
    std::vector<float> z(w * h, .01f), slots(w * h * 2);
    for (size_t i = 0; i < slots.size(); i += 2) { slots[i] = -1; slots[i + 1] = .01f; }
    auto depth = texture(device, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), w * 4);
    auto slotTexture = texture(device, w, h, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, slots.data(), w * 8);
    auto depthView = view(device, depth.Get()), slotView = view(device, slotTexture.Get());
    uint32_t record[84]{};
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(record); bd.StructureByteStride = 336;
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = record;
    ComPtr<ID3D11Buffer> pool;
    check(SUCCEEDED(device->CreateBuffer(&bd, &initial, pool.GetAddressOf())), "HDR route: pool buffer");
    auto poolView = view(device, pool.Get());
    constexpr uint32_t kStamp = 77;
    float scene[277][4]{}; float cam[6][4]; camera(cam); std::memcpy(scene + 270, cam, sizeof(cam));
    { const uint32_t stamp = kStamp; std::memcpy(&scene[276][0], &stamp, 4); }
    bd = {}; bd.ByteWidth = sizeof(scene); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    initial.pSysMem = scene;
    ComPtr<ID3D11Buffer> now, old;
    check(SUCCEEDED(device->CreateBuffer(&bd, &initial, now.GetAddressOf())) &&
          SUCCEEDED(device->CreateBuffer(&bd, &initial, old.GetAddressOf())), "HDR route: engine camera buffers");

    // The game's pipeline around the call: a render target, a pixel and a vertex shader resource, a compute constant
    // buffer and a viewport, all of which the resolver must hand back untouched.
    // (The shader resource is over ANOTHER texture: one resource bound as both would have the runtime unbind one of them.)
    auto other = texture(device, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    auto sampled = texture(device, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11RenderTargetView> otherRtv; ComPtr<ID3D11ShaderResourceView> otherSrv = view(device, sampled.Get());
    check(SUCCEEDED(device->CreateRenderTargetView(other.Get(), nullptr, otherRtv.GetAddressOf())), "HDR route: original RTV");
    D3D11_VIEWPORT viewport{3, 4, 19, 21, .2f, .8f};
    auto bindOriginal = [&] {
        ID3D11RenderTargetView* rt = otherRtv.Get(); context->OMSetRenderTargets(1, &rt, nullptr);
        ID3D11ShaderResourceView* srv = otherSrv.Get(); context->PSSetShaderResources(0, 1, &srv); context->VSSetShaderResources(3, 1, &srv);
        ID3D11Buffer* cb = old.Get(); context->CSSetConstantBuffers(4, 1, &cb); context->RSSetViewports(1, &viewport);
    };
    auto restored = [&] {
        ComPtr<ID3D11RenderTargetView> rt; ComPtr<ID3D11ShaderResourceView> ps, vs; ComPtr<ID3D11Buffer> cb;
        context->OMGetRenderTargets(1, rt.GetAddressOf(), nullptr); context->PSGetShaderResources(0, 1, ps.GetAddressOf());
        context->VSGetShaderResources(3, 1, vs.GetAddressOf()); context->CSGetConstantBuffers(4, 1, cb.GetAddressOf());
        UINT count = 1; D3D11_VIEWPORT current{}; context->RSGetViewports(&count, &current);
        return rt.Get() == otherRtv.Get() && ps.Get() == otherSrv.Get() && vs.Get() == otherSrv.Get() && cb.Get() == old.Get() &&
               count == 1 && !std::memcmp(&current, &viewport, sizeof(viewport));
    };

    // The game's HDR scene target: R11G11B10F, shader and render-target bound, at the render size.
    auto makeH = [&](UINT binds = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) {
        return texture(device, w, h, DXGI_FORMAT_R11G11B10_FLOAT, binds);
    };
    auto hTexture = makeH();
    auto hSrv = view(device, hTexture.Get());
    // One frame of input: what the game rendered, already at representable values.
    std::vector<uint32_t> texels(w * h);
    std::vector<double> inR(w * h), inG(w * h), inB(w * h);
    auto fill = [&](auto colorOf) {
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                double c[3]; colorOf(x, y, c);
                double r[3]; representable(c[0], c[1], c[2], r);
                inR[y * w + x] = r[0]; inG[y * w + x] = r[1]; inB[y * w + x] = r[2];
                texels[y * w + x] = pack(r[0], r[1], r[2]);
            }
        context->UpdateSubresource(hTexture.Get(), 0, nullptr, texels.data(), w * 4, 0);
    };
    auto readH = [&](std::vector<uint32_t>& out) {
        std::vector<unsigned char> bytes;
        if (!readWhole(context, hTexture.Get(), bytes, 4)) return false;
        out.resize(w * h);
        std::memcpy(out.data(), bytes.data(), bytes.size());
        return true;
    };
    auto hHash = [&] {
        std::vector<unsigned char> bytes;
        return readWhole(context, hTexture.Get(), bytes, 4) ? fnv1a(bytes.data(), bytes.size()) : uint64_t(0);
    };
    // The jittered input resampled on the unjittered grid, in doubles: what a reset frame must give back.
    auto bilinear = [&](const std::vector<double>& channel, double fx, double fy) {
        const double tx = std::min<double>(std::max<double>(fx, 0.0), w - 1.0), ty = std::min<double>(std::max<double>(fy, 0.0), h - 1.0);
        const int x0 = int(std::floor(tx)), y0 = int(std::floor(ty));
        const int x1 = std::min<int>(x0 + 1, w - 1), y1 = std::min<int>(y0 + 1, h - 1);
        const double ax = tx - x0, ay = ty - y0;
        return (channel[y0 * w + x0] * (1 - ax) + channel[y0 * w + x1] * ax) * (1 - ay) +
               (channel[y1 * w + x0] * (1 - ax) + channel[y1 * w + x1] * ax) * ay;
    };
    // Every texel of H against `expected(x, y, channel)`, to within one unit in the format's last place plus the filter's
    // own slack (one ulp is what either rounding mode of the conversion to R11G11B10F can cost: nearest costs half, truncation
    // nearly one, and what a device does is its own); the worst miss, in ulps, is returned, with how many texels fell short
    // of the expected value (truncation) and how many went past it (rounding up).
    auto worstMiss = [&](auto expected, double slack, int* badTexels, int* below = nullptr, int* above = nullptr) {
        std::vector<uint32_t> got;
        double worst = 0; int bad = 0, lo = 0, hi = 0;
        if (!readH(got)) { if (badTexels) *badTexels = -1; return 1e9; }
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                double g[3]; unpack(got[y * w + x], g);
                for (int c = 0; c < 3; ++c) {
                    const double want = expected(x, y, c), u = ulp(want, mantissa(c));
                    const double miss = std::abs(g[c] - want) / u;
                    if (std::abs(g[c] - want) > 1.0 * u + slack) ++bad;
                    if (g[c] < want - 1e-9 * u) ++lo; else if (g[c] > want + 1e-9 * u) ++hi;
                    worst = std::max(worst, miss);
                }
            }
        if (badTexels) *badTexels = bad;
        if (below) *below = lo;
        if (above) *above = hi;
        return worst;
    };

    FlatMonoResolveFrame f{};
    f.color = hSrv.Get(); f.depth = depthView.Get(); f.renderWidth = w; f.renderHeight = h; f.outputWidth = w; f.outputHeight = h;
    f.deltaMs = 16; camera(f.camera); camera(f.previousCamera);
    f.engine = {slotView.Get(), poolView.Get(), now.Get(), old.Get()};
    f.mode = FlatMonoResolveMode::Dlss; f.frame = 100; f.hdr = true;
    auto run = [&](bool wanted, const char** why = nullptr) {
        bindOriginal();
        ComPtr<ID3D11ShaderResourceView> out; const char* reason = nullptr;
        const bool ok = edvr::flatMonoResolve(device, context, f, out.GetAddressOf(), &reason);
        if (ok != wanted) std::printf("info: HDR resolver reason %s\n", reason ? reason : "none");
        check(ok == wanted, "HDR route: resolver result");
        check(restored(), "HDR route: the game's whole pipeline restored on every exit");
        check(out == nullptr, "HDR route: no output view is returned (the result is in the game's target)");
        if (why) *why = reason;
        return ok;
    };

    // ---- 1. a reset frame: the jittered input resampled on the unjittered grid, requantised --------------------------
    // A 2-D ramp in all three channels, HDR-ranged: the bilinear resample is exact for it, so the only error is the
    // format's conversion (half an ulp rounding to nearest, up to one truncating) and the filter's fixed-point weights.
    fill([&](UINT x, UINT y, double (&c)[3]) { c[0] = 100 + 50.0 * x; c[1] = 10.0 * (y + 1); c[2] = 40.0 + 30.0 * x + 20.0 * y; });
    f.jitterX = f.previousJitterX = expectedJx = .25f; f.jitterY = f.previousJitterY = expectedJy = -.375f; f.reset = true;
    const auto before = edvr::flatMonoResolveStats();
    backendCalls = 0; observedHdr = false;
    bool ok = run(true);
    check(ok && backendCalls == 1 && observedHdr, "HDR route: the backend is called once and told it is HDR");
    check(observedColourFormat == DXGI_FORMAT_R11G11B10_FLOAT && observedOutFormat == DXGI_FORMAT_R16G16B16A16_FLOAT,
          "HDR route: the backend is handed an R11G11B10F input and an R16G16B16A16F output");
    int bad = 0, below = 0, above = 0;
    double worst = worstMiss([&](UINT x, UINT y, int c) {
        const std::vector<double>& ch = c == 0 ? inR : c == 1 ? inG : inB;
        return bilinear(ch, x + f.jitterX, y + f.jitterY);
    }, 0.5, &bad, &below, &above);
    check(bad == 0, "HDR route: a reset frame is the input resampled at the jitter, to within an ulp of the format and the filter's slack");
    std::printf("flat mono resolve: HDR reset frame: worst miss %.3f ulp of R11G11B10F over %u texels x 3 channels; %d below the exact "
                "resample, %d above (this device %s)\n", worst, w * h, below, above,
                worst <= 0.5001 ? "rounds to nearest" : (above == 0 ? "truncates toward zero" : "rounds both ways"));
    check(edvr::flatMonoResolveStats().hdrResolves == before.hdrResolves + 1, "HDR route: the resolve is counted");

    // ---- 2. a frame the backend resolves: its output where history is trusted, the input where it is not -------------
    fill([&](UINT x, UINT y, double (&c)[3]) { c[0] = 100 + 50.0 * x; c[1] = 10.0 * (y + 1); c[2] = 40.0 + 30.0 * x + 20.0 * y; });
    f.reset = false; ++f.frame; f.camera[5][0] = .3125f;   // a translation: one render pixel of motion, current to previous
    f.jitterX = f.previousJitterX = expectedJx = 0; f.jitterY = f.previousJitterY = expectedJy = 0;
    ok = run(true);
    check(ok && !backendReset && std::abs(observedMotion - 1) < .001, "HDR route: a continuing frame reaches the backend with the camera's motion");
    {
        std::vector<uint32_t> got; readH(got);
        double mid[3], edge[3];
        unpack(got[8 * w + 8], mid); unpack(got[8 * w + 15], edge);
        check(std::abs(mid[0]) < 1e-6 && std::abs(mid[1] - 1) < 1e-3 && std::abs(mid[2]) < 1e-6,
              "HDR route: where history is trusted the backend's output (green, fp16) is what goes into the game's target");
        check(std::abs(edge[0] - inR[8 * w + 15]) <= .55 * ulp(inR[8 * w + 15], 6) &&
              std::abs(edge[1] - inG[8 * w + 15]) <= .55 * ulp(inG[8 * w + 15], 6),
              "HDR route: where the reprojection left the frame the input goes through, unchanged");
    }
    f.camera[5][0] = 0;

    // ---- 3. FSR receives the flag too ------------------------------------------------------------------------------
    fill([&](UINT x, UINT, double (&c)[3]) { c[0] = 64 + 8.0 * x; c[1] = 32; c[2] = 16; });
    f.mode = FlatMonoResolveMode::Fsr; f.reset = true; ++f.frame; observedHdr = false; infiniteSeen = false;
    ok = run(true);
    check(ok && observedHdr && infiniteSeen && observedColourFormat == DXGI_FORMAT_R11G11B10_FLOAT && observedOutFormat == DXGI_FORMAT_R16G16B16A16_FLOAT,
          "HDR route: FSR is told HDR with an R11G11B10F input and an fp16 output, and keeps its explicit infinite depth");
    f.mode = FlatMonoResolveMode::Dlaa; f.reset = true; ++f.frame; observedHdr = false;
    ok = run(true);
    check(ok && observedHdr, "HDR route: DLAA is told HDR too (R = D)");

    // ---- 4. EDVR's TAA: bounded-space accumulation -----------------------------------------------------------------
    // A still camera and a still scene, jitter zero: a hot pixel appears on the second frame. In c/(1+max3(c)) space the
    // history (100) and the new value (10000) are a hair apart, the 3x3 box holds the history, and the blend at .9
    // leaves the hot pixel near 111; linear blending would leave it near 1090.
    f.mode = FlatMonoResolveMode::Taa; f.jitterX = f.jitterY = f.previousJitterX = f.previousJitterY = 0; expectedJx = expectedJy = 0;
    fill([&](UINT, UINT, double (&c)[3]) { c[0] = c[1] = c[2] = 100; });
    f.reset = true; ++f.frame; ok = run(true);
    {
        const int badStill = [&] { int b = 0; worstMiss([&](UINT, UINT, int) { return 100.0; }, 0.0, &b); return b; }();
        check(ok && badStill == 0, "HDR TAA: a reset frame is the input through the format (a flat 100)");
    }
    fill([&](UINT x, UINT y, double (&c)[3]) { c[0] = c[1] = c[2] = (x == 8 && y == 8) ? 10000 : 100; });
    f.reset = false; ++f.frame; ok = run(true);
    {
        const double cur = 10000.0 / 10001.0, hist = 100.0 / 101.0;
        const double mixed = cur + (hist - cur) * .9;
        const double want = mixed / (1 - mixed);
        std::vector<uint32_t> got; readH(got);
        double hot[3], adjacent[3], distant[3];
        unpack(got[8 * w + 8], hot); unpack(got[8 * w + 9], adjacent); unpack(got[2 * w + 2], distant);
        std::printf("flat mono resolve: HDR TAA hot pixel %.2f (compressed-space blend expects %.2f; a linear blend would give 1090), neighbour %.2f, far %.2f\n",
                    hot[0], want, adjacent[0], distant[0]);
        check(std::abs(hot[0] - want) <= .02 * want && std::abs(hot[1] - want) <= .02 * want && std::abs(hot[2] - want) <= .03 * want,
              "HDR TAA: the hot pixel is blended in c/(1+max3(c)) space, history weighted .9: near 111, not the linear 1090");
        check(std::abs(adjacent[0] - 100) <= 1 && std::abs(distant[0] - 100) <= 1, "HDR TAA: the pixels around it stay at 100");
    }
    // A jitter walk over a still firefly: every frame finite, nothing below 0 or above the brightest input, and the
    // texels far from it untouched. No ringing.
    {
        const double fire = 61440;   // representable: 1.875 * 2^15
        static const float walk[8][2] = {{.25f, -.25f}, {-.25f, .25f}, {.125f, .375f}, {-.375f, -.125f},
                                          {.375f, .125f}, {-.125f, -.375f}, {.0f, .25f}, {.25f, .0f}};
        f.reset = true;
        bool finite = true, bounded = true, calm = true;
        for (int frame = 0; frame < 8; ++frame) {
            fill([&](UINT x, UINT y, double (&c)[3]) { c[0] = c[1] = c[2] = (x == 8 && y == 8) ? fire : 0.25; });
            f.jitterX = expectedJx = walk[frame][0]; f.jitterY = expectedJy = walk[frame][1];
            f.previousJitterX = frame ? walk[frame - 1][0] : f.jitterX; f.previousJitterY = frame ? walk[frame - 1][1] : f.jitterY;
            ++f.frame; ok = run(true); f.reset = false;
            std::vector<uint32_t> got; readH(got);
            for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < w; ++x) {
                    double g[3]; unpack(got[y * w + x], g);
                    for (int c = 0; c < 3; ++c) {
                        finite = finite && std::isfinite(g[c]);
                        bounded = bounded && g[c] >= 0 && g[c] <= fire * (1 + 1e-3) && (x == 8 && y == 8 ? true : g[c] <= fire * .5 * (1.02));
                        if (std::abs(double(x) - 8) >= 3 || std::abs(double(y) - 8) >= 3) calm = calm && std::abs(g[c] - 0.25) <= 0.25 * 0.016;
                    }
                }
        }
        check(finite, "HDR TAA: a firefly under a jitter walk never makes a NaN or an infinity");
        check(bounded, "HDR TAA: a firefly never leaves an output above its own brightness, or a neighbour above half of it");
        check(calm, "HDR TAA: texels three or more away from a firefly stay at their own value in every frame");
    }
    // A flat field under a jitter walk: still flat, to within the format's rounding and the round trip of the bounded space.
    {
        bool flat = true;
        f.reset = true;
        for (int frame = 0; frame < 6; ++frame) {
            fill([&](UINT, UINT, double (&c)[3]) { c[0] = 1024; c[1] = 512; c[2] = 1536; });
            f.jitterX = expectedJx = (frame & 1) ? .25f : -.25f; f.jitterY = expectedJy = (frame & 2) ? .125f : -.125f;
            f.previousJitterX = -f.jitterX; f.previousJitterY = -f.jitterY;
            ++f.frame; ok = run(true); f.reset = false;
            std::vector<uint32_t> got; readH(got);
            for (UINT i = 0; i < w * h; ++i) {
                double g[3]; unpack(got[i], g);
                // Within an ulp of the format: the round trip through the bounded space is exact to a part in 10^4, and
                // what is left is the conversion into R11G11B10F (nearest costs half an ulp, truncation up to one).
                flat = flat && std::abs(g[0] - 1024) <= ulp(1024, 6) && std::abs(g[1] - 512) <= ulp(512, 6) &&
                       std::abs(g[2] - 1536) <= ulp(1536, 5);
            }
        }
        check(flat, "HDR TAA: a flat HDR field stays flat through the bounded space and back, frame after frame");
    }

    // ---- 5. a backend that refuses, and the spatial recovery into the game's target --------------------------------
    f.mode = FlatMonoResolveMode::Dlss; f.reset = false; ++f.frame;
    fill([&](UINT x, UINT y, double (&c)[3]) { c[0] = 100 + 50.0 * x; c[1] = 10.0 * (y + 1); c[2] = 40.0 + 30.0 * x + 20.0 * y; });
    f.jitterX = f.previousJitterX = expectedJx = .5f; f.jitterY = f.previousJitterY = expectedJy = 0;
    const uint64_t untouched = hHash();
    backendFail = true;
    const char* why = nullptr;
    ok = run(false, &why);
    backendFail = false;
    check(!ok && hHash() == untouched, "HDR route: a backend refusal leaves the game's target bit for bit what it was");
    bindOriginal();
    ComPtr<ID3D11ShaderResourceView> none; const char* fallbackReason = nullptr;
    const auto beforeSpatial = edvr::flatMonoResolveStats();
    const bool recovered = edvr::flatMonoResolveSpatialFallback(device, context, f, none.GetAddressOf(), &fallbackReason);
    check(recovered && none == nullptr && restored(), "HDR route: the spatial recovery writes into the game's target, returns no view and restores the pipeline");
    check(edvr::flatMonoResolveStats().hdrSpatial == beforeSpatial.hdrSpatial + 1, "HDR route: the spatial recovery is counted");
    int badSpatial = 0;
    worstMiss([&](UINT x, UINT y, int c) {
        const std::vector<double>& ch = c == 0 ? inR : c == 1 ? inG : inB;
        return bilinear(ch, x + f.jitterX, y + f.jitterY);
    }, 0.5, &badSpatial);
    check(badSpatial == 0, "HDR route: the spatial recovery is the jittered input resampled on the unjittered grid");

    // ---- 6. refusals that happen before anything is written ---------------------------------------------------------
    {
        fill([&](UINT x, UINT, double (&c)[3]) { c[0] = 10.0 * x + 1; c[1] = 2; c[2] = 3; });
        const uint64_t base = hHash();
        // R < D: upscaling keeps the copy route (decision (c)).
        FlatMonoResolveFrame up = f; up.outputWidth = 32; up.outputHeight = 32; up.reset = true; ++up.frame;
        bindOriginal(); ComPtr<ID3D11ShaderResourceView> out; const char* reason = nullptr;
        check(!edvr::flatMonoResolve(device, context, up, out.GetAddressOf(), &reason) && reason &&
              !std::strcmp(reason, "flat-resolve-hdr-requires-render-size-evaluation") && restored() && hHash() == base,
              "HDR route: a render below the output declines, naming why, with the target and the pipeline untouched");
        // EDVR's TAA above D evaluates on the display grid: not E = R.
        FlatMonoResolveFrame taaDown = f; taaDown.mode = FlatMonoResolveMode::Taa; taaDown.outputWidth = 8; taaDown.outputHeight = 8;
        bindOriginal(); reason = nullptr;
        check(!edvr::flatMonoResolve(device, context, taaDown, out.GetAddressOf(), &reason) && reason &&
              !std::strcmp(reason, "flat-resolve-hdr-requires-render-size-evaluation") && restored() && hHash() == base,
              "HDR route: EDVR's TAA at R > D (display-grid evaluation) stays on the copy route");
        // An LDR view over an LDR texture is not H.
        auto ldr = texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        auto ldrView = view(device, ldr.Get());
        FlatMonoResolveFrame wrong = f; wrong.color = ldrView.Get();
        bindOriginal(); reason = nullptr;
        check(!edvr::flatMonoResolve(device, context, wrong, out.GetAddressOf(), &reason) && reason &&
              !std::strcmp(reason, "flat-resolve-input-view-mismatch") && restored(),
              "HDR route: an R8G8B8A8 view is not the game's HDR target");
        // An R11G11B10F texture the game cannot render into cannot take the result.
        auto noRt = makeH(D3D11_BIND_SHADER_RESOURCE);
        auto noRtView = view(device, noRt.Get());
        FlatMonoResolveFrame readOnly = f; readOnly.color = noRtView.Get();
        bindOriginal(); reason = nullptr;
        check(!edvr::flatMonoResolve(device, context, readOnly, out.GetAddressOf(), &reason) && reason &&
              !std::strcmp(reason, "flat-resolve-input-view-mismatch") && restored(),
              "HDR route: a texture with no render-target bind cannot take the result");
        // The same gates in the spatial recovery.
        bindOriginal(); reason = nullptr;
        check(!edvr::flatMonoResolveSpatialFallback(device, context, up, out.GetAddressOf(), &reason) && hHash() == base && restored(),
              "HDR route: the spatial recovery refuses a render below the output too");
    }

    // ---- 7. the preflight knows the route ---------------------------------------------------------------------------
    {
        edvr::FlatMonoResolvePreflight planned{};
        planned.renderWidth = w; planned.renderHeight = h; planned.outputWidth = w; planned.outputHeight = h;
        planned.mode = FlatMonoResolveMode::Dlss; planned.hdr = true;
        planned.colorViewFormat = DXGI_FORMAT_R11G11B10_FLOAT; planned.depthViewFormat = DXGI_FORMAT_R32_FLOAT;
        auto ready = edvr::flatMonoResolvePreflight(device, context, planned);
        check(ready.readyForRasterJitter() && ready.spatialFallbackReady && ready.backendAvailable,
              "HDR route: a valid HDR plan preflights ready, its pixel-shader fallback included");
        auto low = planned; low.outputWidth = 32; low.outputHeight = 32;
        auto refused = edvr::flatMonoResolvePreflight(device, context, low);
        check(refused.status == edvr::FlatMonoResolvePreflightStatus::InvalidMetadata && refused.reason &&
              !std::strcmp(refused.reason, "flat-preflight-hdr-requires-render-size-evaluation"),
              "HDR route: a plan with the render below the output is refused by name");
        auto taaDown = planned; taaDown.mode = FlatMonoResolveMode::Taa; taaDown.renderWidth = 32; taaDown.renderHeight = 32;
        refused = edvr::flatMonoResolvePreflight(device, context, taaDown);
        check(refused.status == edvr::FlatMonoResolvePreflightStatus::InvalidMetadata,
              "HDR route: a TAA plan that evaluates at the display size above it is refused");
        auto ldrFormat = planned; ldrFormat.colorViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        refused = edvr::flatMonoResolvePreflight(device, context, ldrFormat);
        check(refused.status == edvr::FlatMonoResolvePreflightStatus::InvalidMetadata && refused.reason &&
              !std::strcmp(refused.reason, "flat-preflight-unsupported-source-format"),
              "HDR route: an LDR colour view in an HDR plan is refused");
        auto hdrFormatLdr = planned; hdrFormatLdr.hdr = false;
        refused = edvr::flatMonoResolvePreflight(device, context, hdrFormatLdr);
        check(refused.status == edvr::FlatMonoResolvePreflightStatus::InvalidMetadata,
              "the copy route's plan still refuses an R11G11B10F colour view");
    }

    // ---- 8. the copy route after the HDR route is the copy route -----------------------------------------------------
    {
        std::vector<uint32_t> red(w * h, 0xff0000ff);
        auto color = texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, red.data(), w * 4);
        auto colorView = view(device, color.Get());
        FlatMonoResolveFrame ldr{};
        ldr.color = colorView.Get(); ldr.depth = depthView.Get(); ldr.renderWidth = w; ldr.renderHeight = h;
        ldr.outputWidth = w; ldr.outputHeight = h; ldr.deltaMs = 16; camera(ldr.camera); camera(ldr.previousCamera);
        ldr.engine = {slotView.Get(), poolView.Get(), now.Get(), old.Get()};
        ldr.mode = FlatMonoResolveMode::Dlss; ldr.frame = f.frame + 1000; ldr.reset = true;
        expectedJx = expectedJy = 0; observedHdr = true;
        bindOriginal(); ComPtr<ID3D11ShaderResourceView> out; const char* reason = nullptr;
        const bool good = edvr::flatMonoResolve(device, context, ldr, out.GetAddressOf(), &reason);
        uint32_t px = 0;
        if (good && out) {
            ComPtr<ID3D11Resource> r; out->GetResource(r.GetAddressOf()); ComPtr<ID3D11Texture2D> t; r.As(&t);
            readPixel(context, t.Get(), &px, 4, 8, 8);
        }
        check(good && out && restored() && !observedHdr && px == 0xff0000ff,
              "after the HDR route, the copy route resolves an LDR frame as before: output view, LDR backend flag, the input's pixels");
    }
}

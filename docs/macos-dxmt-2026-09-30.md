# macOS under CrossOver: what DXMT answers about formats

On a Mac, Elite runs through CrossOver, where d3d11.dll is DXMT, a
D3D11-to-Metal layer. One rc.4 log from that setup shows the flat runtime
treating no frames at all. Read the Status block first.

## Status

- **State (2026-09-30):** cause not proven. An instrument that logs what DXMT
  answers about formats, and what Elite asks, is built and tested on Windows
  (branch `claude/format-support-log`, not merged, not flown on a Mac).
- **The finding:** in `edvr_gfx_20260930_123414.log` (v0.18.0-rc.4, CrossOver
  with DXMT, 5120x1440) Elite renders its full-size world into DXGI format 23,
  R10G10B10A2_TYPELESS. On Windows the same target is 26, R11G11B10_FLOAT. The
  half-size bloom targets are still 26.
- **Refusals in that log:** `no-observed-hdr-writes`, then `no-known-tone-pass`
  for the rest of the session. treated=0 all session. The first fits the
  format: the flat runtime's HDR checks are written for format 26
  (`flatHdrCandidateDraw` in `flat_hdr_route.h`; `k.format != 26` in
  `flat_mono_frame.h`).
- **DXMT's signature:** the same log says "47 of 49 d3d11 exports did not
  resolve": DXMT's d3d11.dll has 2 of the 49 exports EDVR's proxy forwards.
- **The damage:** black outlines on floor markings. In rc.4 engine motion's
  substitution and the overlay guard kept running on refused frames: 1,719
  full-frame copies, 101 GB in 30 s. Main's stand-down (`flat_standdown.h`,
  ce6d511a) pauses that work after 5 s of `no-known-tone-pass`, which it counts
  as structural. `no-observed-hdr-writes` is not structural, so a session that
  stays on it is not stood down.
- **Hypothesis, untested:** DXMT does not offer the R11G11B10 capability Elite
  needs for its full-size world, most likely in-place compute read-write, which
  Metal allows only for a short list of formats. Elite picks formats from the
  device's answers to format-support queries, so it falls back. Format 23 is
  the typeless resource; its row shows texture bits only on WARP, which is
  usual for a typeless format. The views over it are 24 (UNORM), so the 24 row
  says what the device lets Elite do with it.
- **Would confirm it:** the self-query row for `R11G11B10_FLOAT[26]` lacks
  `TYPED_UAV`, or `UAV_TYPED_LOAD` and `UAV_TYPED_STORE` in `support2`, while
  `R10G10B10A2_UNORM[24]` has them; and Elite's queries ask about 26 before 23
  or 24.
- **Would refute it:** DXMT reports 26 as capable of everything Windows does.
  Then the choice hangs on something else: an option flag, the adapter, a
  query the instrument does not cover (`CheckMultisampleQualityLevels` is
  not hooked).
- **Ruled out:** nothing yet.
- **Next flight:** one Mac launch with this build, either profile, then send
  the log. Read the lines below, then decide what the flat runtime does on
  DXMT.

## 2026-09-30: the instrument

Built for one Mac launch, on both profiles, always on, no setting. It never
changes an answer the game gets, and prints at most 58 lines a session.

**What it logs.** Grep the log for `D3D11 adapter:`, `format support` and
`device options`. Read them in this order; the first three say whether the
rest can be trusted.

1. One adapter line per device, the first three, written right after the
   `D3D11 device ... created` line. The created line has no vendor; this one
   says DXMT or NVIDIA or AMD in any log bundle. A real line, from a Windows
   WARP device in the rig:

       D3D11 adapter: device=000002864EC18470 (#1) description="Microsoft Basic Render Driver" vendor=0x1414 deviceId=0x008C subsys=0x00000000 revision=0 dedicatedVideoMemory=0 MiB dedicatedSystemMemory=0 MiB sharedSystemMemory=15930 MiB luid=00000000:0001610D

2. The self-query, on the first device only: 11 formats (R11G11B10_FLOAT,
   R10G10B10A2_UNORM and _TYPELESS, R16G16B16A16_FLOAT, R8G8B8A8_UNORM, _SRGB
   and _TYPELESS, R32_FLOAT, D32_FLOAT_S8X24_UINT, R32G8X24_TYPELESS,
   R24G8_TYPELESS), each with `CheckFormatSupport` (`support`) and
   `CheckFeatureSupport(FORMAT_SUPPORT2)` (`support2`) in hex and as bit names,
   then the D3D11 options. A failed call prints its HRESULT and no mask. WARP:

       format support (self-query): R11G11B10_FLOAT[26] support=0x02E6F3F3 [BUFFER|IA_VERTEX|TEX1D|TEX2D|TEX3D|CUBE|SHADER_LOAD|SHADER_SAMPLE|MIP|MIP_AUTOGEN|RENDER_TARGET|BLENDABLE|CPU_LOCKABLE|MSAA_RESOLVE|MSAA_RENDER_TARGET|MSAA_LOAD|SHADER_GATHER|TYPED_UAV] support2=0x000006C0 [UAV_TYPED_LOAD|UAV_TYPED_STORE|TILED|SHAREABLE] via-CheckFeatureSupport=same
       device options (self-query): D3D11_OPTIONS hr=0x00000000 [OMLogicOp=1 ... ExtendedResourceSharing=1] D3D11_OPTIONS2 hr=0x00000000 [PSStencilRef=0 TypedUAVLoadAdditionalFormats=1 ROVs=0 ...]

   `via-CheckFeatureSupport` compares the `FORMAT_SUPPORT` query through
   `CheckFeatureSupport` with `CheckFormatSupport`; it says `DIFFERS` and gives
   the other answer if they disagree.

3. One line saying the hooks went on, right after the hook install (or, if
   the table is too short, that they did not). Absent, the hooks were never
   installed, and another line in the log says why (`d3d11 fixes are OFF`,
   `SENTINEL TRIPPED`, `device vtable unusable`). Fixture device pointer:

       format support: CheckFormatSupport (vtable slot 29) and CheckFeatureSupport (slot 33) are hooked on the game's device 000001F2A3B4C5D0; each forwards the real call first and returns its answer untouched. The game's queries follow as "format support (game #N)" lines.

4. The game's own queries, from those two hooks on the device's
   `CheckFormatSupport` and `CheckFeatureSupport` slots (29 and 33). Each
   distinct (query, format, answer) prints once, in the order the game first
   asked, the first 40;
   `#N` is the call's number among all the game's queries, so a gap is a
   repeat. `from=exe+0x...` is the call site in EliteDangerous64.exe, which a
   disassembler can follow to the code that picks the format. Other callers
   print their raw address. EDVR's own queries are not logged.

       format support (game #1): CheckFormatSupport(R11G11B10_FLOAT[26]) -> hr=0x00000000 support=0x... [...] tid=14248 from=exe+0x2D4A1C3
       format support (game #2): CheckFeatureSupport(FORMAT_SUPPORT2, R11G11B10_FLOAT[26]) -> hr=0x00000000 support2=0x... [...] tid=14248 from=exe+0x2D4A2F0

   Other features print their name, size and answer. The closing line comes
   once the hooks have been quiet for 5 s (twice at most):

       format support (game queries): 412 call(s), 77 distinct; 40 logged, 37 suppressed past the 40-line cap; 0 untracked (table full); 0 before the log opened; hooks ran 450 time(s), 38 not reported

   `hooks ran` counts every entry into either hook; `not reported` is what they
   ran for and did not log (EDVR's own queries, another device's). If the
   hooks are never entered, the line still comes, once, after a minute of
   frames, with `hooks ran 0 time(s)`. So an empty game section has three
   readings, and the lines above tell them apart: no install line (never
   hooked), `hooks ran 0` (hooked, never reached: the game's calls go through
   another table or device), or `hooks ran N, N not reported` (reached, but
   not as the game's own device).

**Pass-through.** The two hooks forward the real call first, unguarded, with
the caller's arguments, and return its HRESULT on every path. The log is read
from that result afterwards, inside a fault budget, so a report that faults
costs the line and not the answer. No lock, no allocation; safe on any thread
and before the log is open (those calls are counted, not lost).

**Tested.** `tools\format_support_test` (in build.bat's rig gate): the decode
against the SDK's enumerators, the vtable slots against `ID3D11DeviceVtbl`,
the ledger from eight threads, every line's exact text, and, in a child
process, the hook bodies on a WARP device: 119 formats three ways and 26
features at 7 sizes, compared with and without the hook, from four threads,
with the log checked line by line; and in another, a session whose hooks are
never entered, which must stay silent for 59 ticks and say `hooks ran 0` on
the 60th.

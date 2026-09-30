# macOS under CrossOver: what DXMT answers about formats

On a Mac, Elite runs through CrossOver, where d3d11.dll is DXMT, a
D3D11-to-Metal layer. One rc.4 log from that setup showed the flat runtime
treating no frames at all; two later logs show the game exiting about 30 s in,
and a run with the HDR route's key off shows its first treatment is the
trigger. Read the Status block first.

## Status

- **State (2026-09-30):** on v0.18.0-rc.4-68-g84c8d555 (flat installer;
  CrossOver with DXMT, Apple M4 Max) the game exits about 30 s in, at the HDR
  route's first treatment, with DLSS (`edvr_gfx_20260930_144001.log`) and with
  FSR (`..._144103.log`), and leaves no crash record. With the route's key off
  (`..._144722.log`, DLSS) the session ran about 70 s and ended with Sean's own
  quit. Which step of the treatment ends the process is not known. The format
  theory is ruled out. The format instrument is on main and has flown.
- **Hypotheses:** (1) confirmed as the trigger: the HDR route's first treatment
  under DXMT, common to DLSS and FSR; which of its steps fails (the resolve,
  the prep and finish shaders, the backend with HDR flags) is open; (2) ruled
  out, below; (3) a DXMT fatal error that exits the process through Wine
  without an exception, still a possible mechanism of (1). Detail: the last two
  entries.
- **The key-off run** (`..._144722.log`, bundle
  `edvr-logs-20260930-144837.zip`): the copy route refused every frame,
  `no-observed-hdr-writes` then `no-known-tone-pass`, treated=0, with the
  stand-down at frame=3736 and the F8 warning shown. The HDR route, observing,
  would have taken every frame (`hdr-frames=130 trigger=130`), so on the Mac it
  is the only path to AA.
- **The finding, corrected:** in `edvr_gfx_20260930_123414.log` (v0.18.0-rc.4,
  CrossOver with DXMT, 5120x1440) the format-23 world target,
  R10G10B10A2_TYPELESS, is the G-buffer. Windows draws the world into an
  R10G10B10A2 G-buffer too.
- **Refusals in that log:** `no-observed-hdr-writes`, then `no-known-tone-pass`
  for the rest of the session. treated=0 all session. The flat runtime's HDR
  checks are written for format 26 (`flatHdrCandidateDraw` in
  `flat_hdr_route.h`; `k.format != 26` in `flat_mono_frame.h`). With the scene
  HDR target at 26 on the Mac, why rc.4 saw no HDR writes is open.
- **DXMT's signature:** the same log says "47 of 49 d3d11 exports did not
  resolve": DXMT's d3d11.dll has 2 of the 49 exports EDVR's proxy forwards.
  The 144103 log's adapter line reports `vendor=0x10DE`, NVIDIA's id, for
  "Apple M4 Max".
- **The damage:** black outlines on floor markings. In rc.4 engine motion's
  substitution and the overlay guard kept running on refused frames: 1,719
  full-frame copies, 101 GB in 30 s. Main's stand-down (`flat_standdown.h`,
  ce6d511a) pauses that work after 5 s of `no-known-tone-pass`, which it counts
  as structural. `no-observed-hdr-writes` is not structural, so a session that
  stays on it is not stood down.
- ruled out: "DXMT offers less for R11G11B10_FLOAT, so Elite renders its world
  in R10G10B10A2", because DXMT reports full R11G11B10_FLOAT support, the scene
  HDR target is R11G11B10_FLOAT on the Mac as on Windows, and the format-23
  target was the G-buffer. Evidence: the format-theory entry below.
- ruled out: hypothesis 2, engine motion's substitution under DXMT, because it
  ran in the key-off run with no exit: it started at frames 1737 and 3473, each
  with the overlay guard active. Evidence: the key-off entry below.
- **Decision (Sean, 2026-09-30):** no DXMT guard that turns the HDR route off
  on Apple adapters ("we'll fix crossover properly"). The route gets fixed on
  DXMT.
- **Next flight:** a build with crash-safe breadcrumbs before and after each
  step of the HDR route's first 3 treated frames (branch
  `claude/hdr-route-crumbs`, in progress). Then one Mac run with the key on
  `auto`, DLSS on, and CrossOver's debug log on if possible. The last crumb
  names the failing step.

## 2026-09-30: the instrument

Built for one Mac launch, on both profiles, always on, no setting. It never
changes an answer the game gets, and prints at most 58 lines a session. It
flew twice on the Mac; results are in the next two entries.

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

## 2026-09-30: the format theory is ruled out

ruled out: "DXMT offers less for R11G11B10_FLOAT, so Elite renders its world in
R10G10B10A2", because DXMT reports full R11G11B10_FLOAT support, the scene HDR
target is R11G11B10_FLOAT on the Mac as on Windows, and the format-23 target
was the G-buffer.

The instrument flew twice (`edvr_gfx_20260930_144001.log` and `..._144103.log`;
v0.18.0-rc.4-68-g84c8d555, CrossOver with DXMT, Apple M4 Max). Three reasons,
from the 144103 log:

(a) The self-query row: full support, typed UAV load and store included:

       format support (self-query): R11G11B10_FLOAT[26] support=0x02F6F3F3 [... RENDER_TARGET|BLENDABLE|... TYPED_UAV] support2=0x000003C0 [UAV_TYPED_LOAD|UAV_TYPED_STORE|OM_LOGIC_OP|TILED]

(b) The scene HDR target is R11G11B10_FLOAT at 5120x1440: `flat unknown
projection capture: ... fmt=26 size=5120x1440`, and `flat hdr route: first
trigger ... reads the scene HDR 5120x1440 at t0; ... selection: the route could
resolve this frame`.

(c) The format-23 target of the 123414 log (the `bank=world` contract entries,
R10G10B10A2 with depth) is the G-buffer. Section 82's census retake in
`design-flat-temporal-aa-2026-09-23.md` has Windows drawing the world into R2,
R10G10B10A2, too. The earlier comparison set a G-buffer against the HDR target.

The adapter line, where DXMT reports NVIDIA's vendor id:

       D3D11 adapter: ... description="Apple M4 Max" vendor=0x10DE deviceId=0x0000 ... dedicatedVideoMemory=55050 MiB

The game's queries: 352 calls, 140 distinct, the hooks ran 352 times, 0 not
reported.

## 2026-09-30: the game exits at the first treatment

On v0.18.0-rc.4-68-g84c8d555 the game exits about 30 s in, in both logged
sessions: 144001 with DLSS (NGX initialised on the M4 Max) and 144103 with FSR
(the fsr3 DX11 backend initialised).

The breadcrumbs end with `gfx: process exit` and there is no `UNHANDLED`
record, so no fault went through EDVR's filter; something ends the process.
DllMain writes that crumb only during process shutdown (`reserved != NULL`,
`d3d11_proxy.cpp`), which the source reads as the game exiting. The last log
lines, in both:

       engine motion: on-foot source slot target created 5120x1440 R32G32
       engine motion: substitution starts at present frame N ... the first this session
       flat hdr route: declined at frame=N seq=...: engine-source-not-ready

The frame before, `flat hdr route: first trigger ... selection: the route
could resolve this frame` was declined for the same reason. The exit falls at
the first frame EDVR would treat on DXMT, and rc.4 never treated a Mac frame,
so no treatment path had run on DXMT before. No `dlss: the feature ... created
for the flat HDR route` line and no FSR context line follow the trigger; the
gfx log may not flush its last lines on exit.

Hypotheses, none tested:

1. Leading: the first treatment's path under DXMT (the HDR route's resolve,
   the prep and finish shaders, or the backend with HDR flags), common to DLSS
   and FSR.
2. Engine motion's substitution under DXMT since rc.4. Two changes are new:
   13b1b8ee keeps the substitution bound across producer draws and puts the
   game's state back once (the lazy restore), and 515af954 answers the
   per-draw Get* questions from the runtime's own record (the query cut).
3. A DXMT fatal error that exits the process through Wine without an
   exception.

With `temporal_aa_before_post = off` the copy route stands alone and the HDR
route never treats. CrossOver's debug log is there for hypothesis 3.

## 2026-09-30: the key-off run, the HDR route's first treatment is the trigger

Sean ran the Mac with `temporal_aa_before_post = off` under `[experimental]`
(bundle `edvr-logs-20260930-144837.zip`, log `edvr_gfx_20260930_144722.log`,
v0.18.0-rc.4-68-g84c8d555, DLSS on). No exit: the session ran about 70 s and
ended with his own quit, `gfx: process exit` after two `alive` crumbs.

1. The copy route refuses every Mac frame: `no-observed-hdr-writes`, then
   `no-known-tone-pass`. treated=0 all session. The stand-down entered, and
   the F8 warning named Bloom:

       flat stand-down: entered at frame=3736: every frame for 5.0 s (265 frames) was refused for no-known-tone-pass, none treated
       flat settings warning: shown (mode=DLSS, ...): DLSS is not active: Elite's post-processing is not recognised. Turn off in Elite's graphics options: Bloom

2. The HDR route, observing with the key off, would take every frame. On the
   Mac it is the only path to AA:

       flat hdr route 5s: key=off state=observing frames=130 hdr-frames=130 trigger=130 ... last=selected last-trigger=VS=DEF19B035D5EDEDC PS=DED8796049C7BB4A target=5120x1440 ...

3. ruled out: hypothesis 2, engine motion's substitution under DXMT, because
   it ran in this session with no exit. `engine motion: substitution starts at
   present frame 1737` (14:47:49.967) and again at frame 3473 (14:48:19.779),
   each with `flat overlay guard active`. The first is about 28 s after the log
   opened, near where the key-on sessions ended.
4. Hypothesis 1 is confirmed as the trigger: the HDR route's first treatment
   ends the process, with DLSS and with FSR. The same build and backend ran
   70 s with the route off and exited at about 30 s with it on. Hypothesis 3,
   a DXMT fatal error through Wine, remains a possible mechanism of it.

Decision (Sean, 2026-09-30): no DXMT guard that turns the route off on Apple
adapters ("we'll fix crossover properly"). The route gets fixed on DXMT.

Next: a build with crash-safe breadcrumbs before and after each step of the HDR
route's first 3 treated frames (branch `claude/hdr-route-crumbs`, in progress).
Then one Mac run with the key on `auto`, DLSS on, and CrossOver's debug log if
possible. The last crumb names the failing step.

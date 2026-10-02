# Issue 65: flat on Linux/Proton spends about 25 ms a frame in `cb shadows`

## Status

*Written 2026-10-02. Update whenever this doc changes.*

- **State:** investigated read-only at v0.18.0; no fix. A reply asking for four tests is
  drafted, not posted. The leading hypothesis (H1) is unproven.
- **Report:** issue 65, flat v0.18.0 (build 6ABED11D) on Gentoo with GE-Proton 11-6
  (DXVK), Mesa 26.2.2 RADV, RX 7900 XTX, Ryzen 9 9950X; 9000x2160 in a Wine virtual
  desktop; EDVR -> EDHM -> DXVK through `advanced.real_dll`; `temporal_aa = on`. At SS
  1.0: F8 off 53 fps, TAA 30, FSR3 36. EDVR's counter reads `cb shadows` 25-28 ms per
  clocked frame (about 4500 calls). Their perf profile puts 49% of the render thread in
  EDVR's DLL: the CRT memcpy 29%, the trace ring 14%.
- **What one entry is:** at every Unmap of a pure constant buffer (16 B to 64 KB, up to
  64 tracked), the flat projection runtime copies the WHOLE buffer from the pointer Map
  returned into a 64 KB bank slot (`finishMapFull`, flat_projection_bindings.h:128-137),
  whatever range the game wrote. UpdateSubresource copies from the game's own source.
  Every such buffer is shadowed whether or not a consumer reads it; the consumers read
  about 3 buffers and 10 spans of 16-128 B. The counter spans only EDVR's observer
  bodies (never the forwarded call), counts scope entries (a Map is 2, an Unmap 1), and
  clocks 1 frame in 16.
- **Windows (Sean, Epic, RTX 5090, native D3D11, 4K):** 41-52 ns per entry, about
  0.11 ms a frame. The reporter: about 5.8 us per entry, roughly 130x.
- **H1, leading and unproven:** DXVK gives dynamic buffers host-visible device-local
  (BAR) memory, which is uncached for CPU reads, so EDVR's whole-buffer read at Unmap
  runs about 1000x slower per byte than from cached memory. It fits the memcpy loop at
  29%. Only the reporter's `d3d11.cachedDynamicResources = c` test weighs against it,
  and that setting may never have reached DXVK: dxvk.conf is read from the working
  directory, `DXVK_CONFIG_FILE` or `DXVK_CONFIG`, and DXVK's own log lists the
  effective configuration.
- **H2, open:** a slow QueryPerformanceCounter under Wine inflating the counter. The
  counter's 25-28 ms is more than the perf-implied EDVR total (about 16 ms). Their
  `flat cpu 5s` lines (the clock floor and every family) settle it.
- **H3, open:** the trace ring is always recorded and read only by an F10 dump. It
  builds a 504-byte event twice per mark into 4 frames x 4096 events x 504 B (8.26 MB).
  It costs 20-33 ns a call on Windows; its instruction count does not explain 14% (about
  4.6 ms) on Proton.
- **Ruled out:**
  - ruled out: lock contention, because the path takes no lock (an owner-thread check
    only);
  - ruled out: the bank and tracked-list scans as the difference, because the same code
    costs 10-15 ns of work per entry on Windows;
  - ruled out: DXVK's own time as the cost, because DXVK is about 1% of their render
    thread.
- **Next:** the reporter's four tests (in the reply): their `flat cpu 5s` lines with
  the `(cont.)` lines; DXVK's log for the effective configuration, then
  `cachedDynamicResources` applied for real; `[experimental] temporal_aa_jitter = off`
  as a test only (the projection runtime is released, so `cb shadows` reads 0 and the
  present time shows the cost); one run without EDHM. Locally, Windows with DXVK
  through `advanced.real_dll` (docs\dxvk-windows.md, never run against the game) would
  reproduce it if NVIDIA also gets BAR memory under DXVK. That needs Sean's OK for the
  DXVK download.
- **Fix direction, after a flight confirms:** copy only the buffers a plan consumes and
  only the spans its recipes read; probe the mapped-read bandwidth at start and skip the
  Unmap read when it is slow; record the trace ring only when armed, or as a compact
  record; remember non-constant-buffer resources in `track()` so a Map stops paying for
  QueryInterface and GetDesc.

## RVA map (the v0.18.0 release DLL)

The release DLL carries no CodeView entry (build.bat links a PDB only with
`EDVR_PROFILE_SYMBOLS=1`), so no PDB can match it. The RVAs were resolved by
disassembly, `.pdata` and the source. To get names from a PDB, build the v0.18.0 tag
with `EDVR_PROFILE_SYMBOLS=1` and compare the `.text` sha256 with the release's
(`e952a174687245f2fdecca004c8444c119890cd431ebd4f76405562b7a2c4457`, vsize 0x2deac0).

| RVA | Share | Function | Confidence |
|---|---|---|---|
| 0x2d3f90-0x2d4012 | ~29% | CRT memcpy (0x2d3dc0-0x2d442d), the 256-byte AVX loop for copies over 256 bytes | exact |
| 0x0b9900-0x0b9a11 | in ~14% | `flatTraceEventMarker` (flat_trace.h:81): zeroes one 0x1F8-byte event | exact |
| 0x0b9a20-0x0b9be4 | in ~14% | `flatTraceMark` (flat_trace.h:159): two 0x1F8 temporaries in a 0x420 frame, capped at 4096 events a frame | exact |
| 0x09e720-0x09ea0a | 1.8% | `capture(Camera&, mapped)` (flat_runtime.cpp:1238): 96 B of camera rows read from the mapped pointer | strong |
| 0x07f600 | 1.2% | `FlatProjectionRuntime::observeUnmap` (flat_projection_runtime.cpp:340), its shadow memcpy at 0x07f72a | strong |
| 0x0c6f00 | 0.9% | the "flat stand-down: still stood down" log formatting, so their window held stood-down frames | exact |

Environment note: DXVK's built-in profile for Elite sets `dxgi.customVendorId = 10de`,
so EDVR sees the reporter's AMD card as NVIDIA.

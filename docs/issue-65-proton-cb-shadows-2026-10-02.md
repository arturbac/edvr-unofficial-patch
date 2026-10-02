# Issue 65: flat on Linux/Proton spends about 25 ms a frame in `cb shadows`

## Status

*Written 2026-10-02. Update whenever this doc changes.*

- **State:** cause CONFIRMED by the reporter (comment of 2026-10-02 11:45 UTC): EDVR's
  CPU reads of the pointer `Map(WRITE_DISCARD)` returns, which DXVK places in
  write-combined, uncached memory by default. A workaround exists (below). No fix built;
  no reply posted.
- **Report:** issue 65, flat v0.18.0 (build 6ABED11D), GE-Proton 11-6 (DXVK), Mesa 26.2.2
  RADV, RX 7900 XTX, Ryzen 9 9950X, 9000x2160, EDVR -> EDHM -> DXVK through
  `advanced.real_dll`, `temporal_aa = on`. `cb shadows` 25-28 ms per clocked frame
  (about 4500 calls); 49% of the render thread in EDVR's DLL (memcpy 29%, trace ring 14%).
- **What one entry is:** at every Unmap of a pure constant buffer (16 B to 64 KB, up to
  64 tracked) the flat projection runtime copies the WHOLE buffer from the pointer Map
  returned into a 64 KB bank slot (`finishMapFull`, flat_projection_bindings.h:128-137),
  whatever range the game wrote, whether or not a consumer reads that buffer (the
  consumers read about 3 buffers and 10 spans of 16-128 B). The counter spans only
  EDVR's observer bodies, counts scope entries (a Map is 2, an Unmap 1) and clocks 1
  frame in 16. Sean's Windows flat logs (Epic, RTX 5090, native D3D11, 4K): 41-52 ns an
  entry, about 0.11 ms a frame. The reporter: about 5.8 us, roughly 130x.
- **The confirmation** (same spot, SS 0.75, only dxvk.conf differs): without the option,
  17-18 fps for the first ~60 s of passive discovery (EDVR ~52 ms a frame: cb shadows
  ~27, discovery ~23), then 30 fps (cb shadows ~26 ms, ~6 us a call). With
  `d3d11.cachedDynamicResources = c`: 60 fps with VSync, about 78 without (present p50
  12.9 ms, EDVR ~6.5 ms), cb shadows ~3.3 ms at ~4050 calls (~0.8 us a call). Their PDB
  build with AMD LBR call records: 94% of the render thread's memcpy calls come from
  `observeUnmap` -> `finishMapFull` (flat_projection_runtime.cpp:344); during discovery
  `flatTemporalUnmap` and `flatTemporalUpdate` (flat_temporal.cpp) make the same copy,
  plus `projectionHashes`. Their first test of the option was void: DXVK never loaded
  it (the Steam Linux Runtime container did not mount the config's directory).
- **Workaround (Linux/Proton):** a `dxvk.conf` next to `EliteDangerous64.exe` holding
  `d3d11.cachedDynamicResources = c`. DXVK's log then lists it under `Effective
  configuration:`.
- **Still open:** with the option, 0.8 us a call is still about 17x Windows (a slow
  QueryPerformanceCounter under Wine is the candidate; their `flat cpu 5s` lines show
  the clock floor). The trace ring (always recorded, read only by an F10 dump, a
  504-byte event built twice per mark) is 14% of the render thread without the option
  and about 5% with it, while its own counter shows about 0.3 ms. Native Windows on AMD
  is unchecked: Sean's NVIDIA logs show no such cost, but other drivers may also hand
  out write-combined memory.
- **Ruled out:**
  - ruled out: lock contention, because the path takes no lock (an owner-thread check
    only);
  - ruled out: the bank and tracked-list scans as the difference, because the same code
    costs 10-15 ns of work per entry on Windows;
  - ruled out: DXVK's own time as the cost, because DXVK is about 1% of their render
    thread.
- **Fix direction:** stop reading the mapped pointer. On `Map(WRITE_DISCARD)` hand the
  game a cached copy and write it into the real mapping at Unmap (writes to
  write-combined memory are fast), which also covers discovery's reads; the reporter is
  trying this in a local build. Check first how Elite uses NO_OVERWRITE maps and
  deferred contexts on these buffers. After that: copy only buffers a plan consumes,
  record the trace ring only when armed or as a compact record, and remember
  non-constant-buffer resources in `track()` so a Map stops paying for QueryInterface
  and GetDesc.

## RVA map (the v0.18.0 release DLL)

The release DLL carries no CodeView entry (build.bat links a PDB only with
`EDVR_PROFILE_SYMBOLS=1`), so no PDB can match it. The RVAs were resolved by
disassembly, `.pdata` and the source. To get names from a PDB, build the v0.18.0 tag
with `EDVR_PROFILE_SYMBOLS=1` and compare the `.text` sha256 with the release's
(`e952a174687245f2fdecca004c8444c119890cd431ebd4f76405562b7a2c4457`, vsize 0x2deac0).
The reporter's own PDB build agreed on the top two.

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

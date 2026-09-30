# Plugins: every fix a selectable module (design, 2026-09-30)

## Status

- **State:** design only, for the release after v0.18.0. Nothing is built.
  It extends draft PR #46 (Devin Nemec, "generic OpenXR addon and plugin
  architecture"), which this document reuses as its add-on tier (section 6).
- **Goal (Sean):** every fix and performance item belongs to one plugin,
  plugins group features logically, and the user picks which to install in
  the installer. A plugin that is not installed costs nothing.
- **Built on:** a read-only inventory of main `4296f142` (25 features, 225
  ini keys, 19 draw verdicts, the services they share; section 2), the flat
  CPU census of 2026-09-30 (hook entry about 1.4 ms over 17,180 calls a frame
  on foot; D3D call counts are what a wrapper such as ReShade multiplies),
  the landing-pad regression of 2026-09-29 (a claim order nobody had written
  down) and PR #46's diff.
- **Recommendation in one line:** a core that owns every hook and service,
  first-party plugins version-locked to it (modules inside one DLL first,
  separate DLLs only if still wanted), and PR #46's C ABI kept for third-party
  add-ons, moved into the core so flat mode gets it too.
- **Decisions for Sean:** Q1-Q6 in section 10 (phase order, the plugin list,
  default sets, the add-on tier's timing, diagnostics, settings layout).
- **Next:** Sean and Devin review sections 4 and 10; then Phase 1 (section 8):
  the registry and dispatch tables in the core, and one small plugin moved
  behind them with a byte-identical verdict replay as its gate.
- **Ruled out while designing:** loading every DLL found in a folder (DLL
  planting; the installer's receipts already know what it installed), a
  stable ABI for first-party plugins (they ship with the core; freezing
  their interface buys nothing and costs every refactor), and a per-draw
  virtual call into every plugin (section 7's measured costs forbid it).

## 1. Goals and non-goals

Goals:

1. Every fix is owned by exactly one plugin. A plugin groups features that
   share code, state or a purpose, and says so in a manifest.
2. The installer offers plugins per profile (VR, flat) with descriptions,
   dependencies and a recommended set. Not installed means: no hooks for it,
   no per-draw work, no menu rows, no ini keys, no log lines.
3. Plugins talk to the core through one declared interface. Contested draws
   are arbitrated by a declared precedence table that a rig replays.
4. Every log names the plugin set that ran, so a flight stays evidence.
5. Third parties can add overlays and panels (PR #46's MFD case) through a
   narrower, stable ABI.

Non-goals: a stable ABI for first-party plugins; loading or unloading
plugins while the game runs (install-time choice, plus the live on/off keys
there are today); any change in a fix's behaviour during the migration.

## 2. What exists today

Features by area (full table in the inventory report; switch key and
default first):

| Area | Features | Profile |
|---|---|---|
| Temporal | temporal AA (TAA/DLSS/FSR) with engine motion, camera jitter, screen/weapon/celestial/terrain motion; sharpening (RCAS); UI, smoke and hologram depth; the flat adapter | VF |
| Interface | UI quality (layer, panel scale, hologram remaps) | V |
| Cockpit visuals | sun glare, particle billboards and witchspace stars, RemLok lines, loading hologram, wake pulse, target indicator, night vision, black void | V |
| Light | exposure share and damping | V |
| Scanners | FSS eye sync (heal, reveal, panel, res), scanner body | V |
| Screens | on-foot screen (resolution, distance, curvature), intro video, backdrop, loading dim, weapon stability | V |
| Comfort | transition flash, Explorer Cam | V |
| Performance | cull guard, FOV trim, OpenXR resolution, settlement detail, static props | V |
| Always | F8 menu and FPS overlay, input gate, diagnostics (census, dumps, probes) | VF |

How it hangs together now:

- **Hooks are process-lifetime and shared.** Device, context, swap chain,
  factory, DirectInput vtables; IAT hooks (input, intro skip); code hooks
  through the kinematic relay; memory patches. A disabled feature keeps its
  hooks; only `advanced.d3d11_fixes = 0` or a sentinel trip removes them.
- **The draw ladder.** `beginPanelOverride` (vscreen.cpp) returns one of 19
  `DrawVerdict`s; the first claim wins. A new verdict touches four lists (the
  enum, `forwardVerdictBegin/End`, `alteredFixOf`, `uiLayerVerdictForwards`).
  Non-verdict features compose in `forwardWithVerdict` (UI and hologram
  depth flags, engine-motion substitution, the UI layer). The defaults keep
  the draw gate open, so every eye draw walks the ladder with shape-first
  predicates whether or not the features are on.
- **Contested draws are settled by position.** The sun glare, the radar's
  contact family and the landing pad share one shader pair (VS
  94D5C556DFD6D705 / PS 912477AEF6958379); glare wins because it comes first.
  The 2026-09-29 pad attempt changed that arbitration and broke the glare.
- **Configuration.** 225 keys, all read and documented (the contract gate);
  33 configure calls at install and 35 on reload, 28 gate terms, a shutdown
  list, 51 frame-boundary ticks a Present. The architecture review
  (reviews\architecture-review-2026-09-29.md, T1) calls the missing feature
  registry the main debt.
- **Installer.** The edition is fixed by the artifact (VR or flat
  installer); `components` is a write-only string; there is no component
  choice. F8 rows are generated from the ini's `# ui:` lines.
- **No rig** covers exposure, sun glare, particles, RemLok, the loading
  hologram, wake pulse, target indicator, black void, the on-foot screen,
  the intro, backdrop and loader panels, or the FSS panel family.

PR #46 adds `include\edvr_plugin_api.h` (API v2): `EdvrPluginRegister`,
`onInitialize/onShutdown/onUpdate/onRenderEye/onFilterInput`, and host
services `registerSetting` (an F8 "Plugins" tab) and `logNote`. The manager
lives in the OpenXR runtime and loads every DLL under `plugins\`. What it can
express: overlays drawn over the finished eye, with the pose, keyboard
capture and F8 toggles. What it cannot: any draw-level fix, tees, code hooks,
temporal AA, the UI layer, engine motion, pose or projection edits, and all
of flat mode (no XR renderer, so no manager). Defects found in the diff:

- two `PluginManager` singletons, of which only the runtime's is
  initialised, so the menu's input filter never sees a plugin;
- the runtime never opens EDVR's log, so plugin lines are dropped;
- no fault containment around callbacks;
- unsigned DLLs loaded from a folder;
- `dt` fixed at 16 ms.

The OM-unbind fix in the same PR is unrelated and should land on its own.

## 3. Three tiers

- **Core** (always installed, one per profile as today): every hook, the
  draw dispatch, and the services in 3.1. It has no fixes of its own.
- **First-party plugins:** EDVR's fixes, grouped as in section 4, built from
  the same tree and version-locked to the core (a plugin whose build stamp
  differs is refused and named in the log).
- **Add-ons:** third-party DLLs through a stable, versioned C ABI (section 6)
  with narrow extension points: overlays, panels, input, settings.

### 3.1 Services the core owns

Hooks (vtable, IAT, code, patches); draw dispatch and claim arbitration;
binding shadows and the shader registry (fnv1a64 at creation); config with
per-plugin key ownership and the contract gate; logging with a plugin prefix;
fault containment (a `FaultBudget` per plugin, the sentinel naming the
plugin); the GPU and CPU census with families grouped per plugin; the draw
census and eye dumps; the F8 menu built from plugin schemas; the input gate,
hotkeys and game bindings; the journal watch; the profile descriptor; frame
ticks; the OpenXR runtime host on the VR side; the install manifest.

Engine motion, camera jitter and the temporal backends serve only temporal
AA, so they stay inside that plugin rather than becoming services. The UI
layer's need for the temporal pass's frame state goes through a core-owned
"frame treatment" record, never a direct call between plugins.

## 4. The plugins

| Plugin | Contents | Profiles | Needs | Default VR / flat |
|---|---|---|---|---|
| temporal-aa | TAA, DLSS, FSR, engine motion, camera jitter (VR frustum, flat camera path), screen, weapon, celestial and terrain motion, UI/smoke/hologram depth, sharpening, the flat adapter (stand-down, F8 warning) | VF | NGX DLL for DLSS | installed, mode off / installed |
| interface-quality | UI layer, panel scale, hologram remaps | V | temporal-aa for the layer (panel scale runs without) | on / - |
| cockpit-visuals | sun glare, particles and witchspace stars, RemLok, loading hologram, wake pulse, target indicator, night vision, black void | V | - | on / - |
| exposure | exposure share and damping | V | - | on / - |
| scanners | FSS eye sync family, scanner body | V | - | on / - |
| screens | on-foot screen, intro video, backdrop, loading dim, weapon stability | V | temporal-aa for weapon stability's motion half (soft) | on / - |
| comfort | transition flash, Explorer Cam | V | - | on / - |
| performance | cull guard, FOV trim, OpenXR resolution, settlement detail, static props | V | - | installed, off / - |
| diagnostics | probes, eye dumps, developer instruments | VF | - | off / off |

The per-frame census lines (GPU census, flat CPU census, LONG FRAME) stay in
the core: user logs are how field reports get diagnosed, so they are never
optional. Dependencies are hard (the plugin cannot load without) or soft (a
feature inside degrades and says so); the manifest records which.

temporal-aa is one plugin on purpose: its parts share per-draw state, the
camera and the history, and splitting them would put that state on a plugin
boundary. It carries two adapters: the per-eye VR one, and the single-image
one that serves flat mode today and could serve the VR on-foot screen and the
HDR route (docs\design-flat-temporal-aa-2026-09-23.md, section 81).

## 5. The plugin contract

- **Manifest** (compiled in, and exported as JSON for the installer): id,
  name, description, profiles, requires and conflicts (hard or soft),
  default per profile, the keys it owns, its hook points, its claims, a cost
  note from the census, its rigs.
- **Lifecycle:** init(host) with a services table; configure(cfg), live as
  today; device and swap-chain creation; resize; device loss; shutdown.
- **Hook points:** per-draw classify and claim; claimed-draw begin and end;
  observers of Map/Unmap/Update, state setters, dispatch, clear and copy;
  Present before and after; frame-boundary ticks; runtime events on the VR
  side (eye submit, compose, pose); input; menu actions.
- **Claims:** each plugin declares the families it may claim (shader pairs
  or shape predicates) and a precedence. The ladder becomes a data table in
  the core. Overlapping declarations fail the build unless the table carries
  an explicit resolution entry. A rig replays recorded draw censuses through
  the table and requires the verdicts to match today's byte for byte (the
  rig the pad attempt lacked).
- **Interplay:** plugins exchange data only through core services, never by
  calling each other.

## 6. The add-on tier (PR #46 reworked)

Keep: the structSize-versioned C ABI, `EdvrPluginRegister`, `registerSetting`
and `logNote`, `onRenderEye` for overlays, `onFilterInput`. Change:

1. The manager moves into the core. It becomes one instance serving both
   profiles, so flat mode gets add-ons too. The flat overlay point is the
   final image before Present.
2. Load only add-ons named in the install manifest with matching SHA-256.
3. Wrap every callback in fault containment. A faulting add-on is disabled
   for the session and named in the log.
4. Log through `edvr::Log`.
5. Pass the real frame time.
6. Back add-on settings by the ini (an `[addon.<id>]` section), so they
   survive restarts and appear in the log bundle.
7. Versioning policy: the host supports API N and N-1.

## 7. Performance rules

Measured on 2026-09-30 (flight 090706, on foot): hook entry costs about
1.4 ms over 17,180 calls a frame, and each D3D call EDVR makes is multiplied
by a context wrapper such as ReShade (one user's frame rate came back only
when ReShade was removed). So:

1. A plugin that is not installed, or installed but switched off, registers
   nothing and costs nothing per draw. Today a disabled fix still walks the
   ladder.
2. The core evaluates interest once per draw (a shader-identity table plus
   declared shape predicates) and calls only the interested plugins; no
   per-draw fan-out.
3. Plugins read state from the core's binding shadows, never with Get*
   calls on a hot path. This extends the query cut to everything.
4. The census reports cost per plugin, and a plugin's manifest states its
   expected per-frame budget.

## 8. Migration

- **Phase 0:** this document and the decisions in section 10.
- **Phase 1:** the registry, manifests and dispatch tables in the core,
  inside the one DLL; the ladder as data; per-plugin configure and census
  families. Move night vision first (small, and it already has a rig). The
  gate is byte-identical verdicts on replayed censuses plus the existing
  rigs.
- **Phase 2:** move the other groups one at a time, each with its rigs and
  one flight: cockpit-visuals, exposure, scanners, screens, comfort,
  performance, interface-quality, temporal-aa last. Each move adds a verdict
  replay fixture for the features that have no rig today.
- **Phase 3:** installer component selection (profile-filtered, dependencies
  auto-selected, a recommended set), receipts that list plugins and
  versions, Modify without touching settings, F8 rows only for installed
  plugins, the contract gate per plugin, and a startup line naming the plugin
  set (`edvr_log.py --expect-plugins`).
- **Phase 4 (optional):** first-party plugins as separate DLLs beside the
  core. They are version-locked, with a C-style boundary: /MT gives every DLL
  its own CRT heap, so nothing is allocated on one side and freed on the
  other. They are signed like the core.
- **Phase 5:** the add-on tier of section 6.

## 9. Risks

- **Behaviour drift while moving code.** Mitigated by the verdict replays,
  the trace replays and one flight per group.
- **Coupled features split across plugins.** Kept together (temporal-aa);
  services where there is a real consumer.
- **Combinations.** Per-plugin rigs plus a small supported matrix (temporal
  on and off, interface-quality on and off, VR and flat) rather than every
  subset.
- **The installer's modify and repair paths** grow; receipts already
  fingerprint every file.
- **More signed binaries** in Phase 4 (the SignPath route).
- **Runtime-side features** (cull guard, FOV, resolution, pacing, flash) need
  hook points in the OpenXR runtime as well; the contract spans both DLLs.

## 10. Decisions for Sean

- **Q1.** Phase order: modules inside one DLL first (install means enabled),
  then separate DLLs only if still wanted? Or separate DLLs from the start?
- **Q2.** The plugin list and boundaries in section 4, especially
  temporal-aa as one plugin.
- **Q3.** Default sets per profile (the recommended preset).
- **Q4.** The add-on tier: rework PR #46 now (manager into the core,
  manifest, fault containment) or after Phase 1?
- **Q5.** Diagnostics probes excluded from the default install, with the
  census kept in the core?
- **Q6.** Settings: keep today's keys and sections with an ownership map
  (compatible), or per-plugin sections (cleaner, needs a migration)?

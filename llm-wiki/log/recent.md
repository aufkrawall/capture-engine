# llm-wiki Log

### 2026-08-21 - streamline_upgrade: CE takes a 1.x game's Streamline imports over

Second step of the generation bridge, on top of the feasibility result below. `streamline_upgrade=on`
(default off) stops treating `streamline_dll_path` as a DLL substitution and instead loads that
folder's 2.x interposer by full path as a second, CE-owned runtime, initialises it with
`pathsToPlugins` pinned and OTA off, and repoints the game's `sl.interposer` import slots at CE.
The game's own 1.x runtime stays loaded and untouched. Policy in
`hook/apis/streamline_bridge_policy.h` with 20 regression tests; runtime in
`hook/apis/streamline_bridge.{h,cpp}`.

At this stage the eight Streamline calls are owned but not yet translated - each refuses politely, so
a bridged game runs without Streamline features. The seven DXGI/D3D12 entry points the interposer
re-exports forward to the 2.x runtime (falling back to the original 1.x slot if a symbol is missing),
which is what actually puts the game's device behind the 2.x interposer.

**Two corrections to the plan this was written from.** The plan called for a narrow exemption to
`WouldRedirectDuplicateLoadedModule`; that was the wrong place, because that guard governs
redirecting the *game's own loads* and the bridge never redirects anything. The guard is unchanged.
The real requirement is the inverse - an active bridge stands the ordinary sl.* substitution down,
since both mechanisms want the same folder for opposite purposes. And the hand-rolled `sl::Preferences`
mirror (the hook DLL cannot include `sl.h`) was **wrong on first write**: `BaseStructure` puts `next`
at offset 0 and `structType` at 8, the reverse of how the declaration reads. Measuring it against the
real header caught it; every payload field was already right, so it would have failed nowhere near
its cause.

**1.x ground truth, and a wrong assumption corrected.** There is no public Streamline 1.5.6 header -
upstream's 1.x tags stop at v1.1.1 - so 1.5.6-specific values must come from W3's own binaries.
OptiScaler vendors a genuine SL1 header set (`external/streamline1/`) whose `Constants` matches
v1.1.1 and whose `Resource` independently confirms the offsets CE already encodes, but it predates
DLSS-G and must not be trusted for the feature enum. Details and the per-source trust levels are in
`frame-generation/guardrails.md`.

**Follow-up in the same session: the generation latch had to go.** Taking the imports over means two
Streamline generations are resident at once, which broke an assumption
`ResolveStreamlineGeneration` was built on - it latched the first module exporting
`slSetTag`/`slEvaluateFeature` and answered with it for every module afterwards. In a bridged process
the 1.x interposer is loaded from process start and is classified first, so the latch would either
authorise 1.x hooks on the 2.x runtime or, once the bridge made V2 authoritative, authorise
**2.x-shaped hooks on the still-resident 1.x interposer** - the exact truncation that started this
whole workstream. `ClassifyModuleGeneration` is now per module and uncached and drives the inline/IAT
hook decisions; only the GetProcAddress route, which is keyed on the symbol name alone and cannot be
per module, takes a process-wide answer from `AuthoritativeProcessGeneration`.

Still open: the actual 1.x->2.x call translation. The translation map for
`Constants` is now known (prepend the 2.x `BaseStructure`; copy `cameraViewToClip`..`reset`
verbatim; **drop** 1.x `notRenderingGameFrames`, which has no 2.x field; keep the three motion-vector
/ projection Booleans; leave 2.x `minRelativeLinearDepthObjectSeparation` at its 40.0f default rather
than zero; drop 1.x `ext`). Verifying it needs a real run of the game.

### 2026-08-21 - Hosting a Streamline 2.x runtime inside a 1.x process is feasible (measured)

Feasibility gate for the proposed `streamline_dll_path` generation upgrade - running a 2.x runtime
in a game that shipped 1.x so DLSS-G/MFG becomes available. The question was whether the two
generations can be resident at once, given that 2.x wants the same plugin **base names** 1.x already
holds. Answered with a standalone probe rather than in CE, so a failure would not be confounded by
CE's own hooks.

**Result: go.** With The Witcher 3's `sl.interposer` 1.5.6 plus `sl.common` / `sl.dlss` / `sl.dlss_g`
/ `sl.reflex` resident first, loading Talos Reawakened's 2.11.1 interposer by full path and calling
`slInit` with `Preferences::pathsToPlugins` set to that folder returns `eOk`, maps a second
`sl.common.dll` at its own base, and binds every 2.x plugin from the 2.x folder.
`slIsFeatureSupported` returns `eOk` for `kFeatureDLSS` and `kFeatureDLSS_G`; `slShutdown` returns
`eOk`; the process exits 0. A `sl2only` control run was taken first so the coexist result is
attributable. The identified risk - 2.x resolving a plugin via `GetModuleHandleW("sl.common.dll")`
and getting 1.x's image, which wins that lookup for the whole process - does not occur: 2.x binds by
path.

Three findings that constrain the implementation, all recorded in
`frame-generation/guardrails.md`: the feature enum is **not** identity across generations
(1.x `eFeatureDLSS_G` 5 vs 2.x `kFeatureDLSS_G` 1000; 1.x `eFeatureDebug` 4 collides with 2.x
`kFeaturePCL` 4) while `BufferType` **is** identity across every value 1.x can emit; SL1's
`VS_FIXEDFILEINFO` reads `1.0.0.0` against a StringFileInfo of `1.5.6.0`, so generation gating via
`DllFileMajorVersion()` is sound but minor-version pinning needs the string table; and 2.x's default
`PreferenceFlags` let OTA plugins under `C:\ProgramData\NVIDIA\NGX\models\` compete with the staged
set, so a pinned bridge should clear `eAllowOTA | eLoadDownloadedPlugins`.

Not yet done: the import takeover, the actual 1.x->2.x call translation, and making
`ResolveStreamlineGeneration` treat the bridged runtime as authoritative when both are resident. No
CE code changed for this entry - the probe lives in the session scratchpad, and the vendored 2.11.1
headers it built against are already in `build/fg_sdk_include/streamline/include`.

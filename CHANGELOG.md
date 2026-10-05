# Changelog

## Unreleased

Changes since [v0.1.6941](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6941).

### New

- **Controller C API groundwork:** added opaque handles and recording, overlay, benchmark and screenshot controls for the existing controller. An independently embeddable library, custom API configuration and complete recording statistics are still pending.

### Improved

- **Recording and FG verification:** documented the implemented ownership contracts, measured debugging context and reproducible game/capture validation; repeated DX12 frame-lock contention diagnostics are now metered.

- **DX12 drawing and capture:** resource preparation, overlay recording, submission and capture publication now follow private, named transactions, with regression protection for queue selection and ordering during frame-generation handovers.

- **DX12 overlay recovery regressions** production draw transactions now verify reset/acquisition/close failures, submission exits and exact backbuffer release behavior; deliberate historical-defect mutations must fail the same behavioral tests.

- **Recording A/V anchor commitment:** first-output timing and source sampling now have one owner, with explicit QPC, elapsed-time, audio-timestamp and frame-grid units. Deterministic regressions preserve inject output timing, authoritative WGC scheduling and exactly-once audio anchoring.

- **Recording frame retries and timing:** inject and screen-grab submission adapters now carry explicit outcomes through fresh, repeat, drain, catch-up and privacy-blackout paths, retaining deferred source leases without separate deferred queries.

- **Recording frame acceptance:** added versioned submission results that distinguish fresh output, repeats, deferred retries and committed video/audio timing while preserving existing DLL entry points. Incompatible media DLLs fail clearly and clear every resolved function pointer.

- **Recording and screenshot feedback:** controller intent, health and notification updates now reject incompatible or replaced inject mappings and release mappings on every failure. Regression tests cover reconnects and publication across Windows processes.

- **Recording start, stop and failure feedback:** controller actions now share one recording session, with regression coverage for pending cancellation, uncertain acknowledgements, late observations and audio-only fallback. A stop during child readiness cannot resume a cancelled start.

- **Recording and frame-generation regression contracts:** documented the state, resource lifetime and timing guarantees used to verify recording cancellation, overlay handover and encode recovery during the core refactor.

- **Screenshot controls use the controller API:** screenshot hotkeys now use a controller-owned engine handle and share screenshot notifications with API callers.

- **Streamline code comments and archive dated incident logs to wiki:** stripped historical session timestamps and diagnostic narratives across hook thread initialization, swapchain wrapper present tracking, DLSS frame multiplier defaults, and queue reinitialization while archiving durable architectural knowledge in `llm-wiki/log/recent.md`.

- **Modular encoder session summary structures:** decoupled capture session summary and starved episode telemetry structures into dedicated header `media_encoder_session_summary.h`, maintaining strict line ceiling limits across media encoder session state.

- **Typed descriptors for DirectX 12 focus-loss and swapchain policies:** replaced multi-boolean positional parameters across focus-loss overlay fence synchronization and non-presentable backbuffer hold checks with structured parameter descriptors `D3D12FocusLossImmediateFenceDesc` and `D3D12NonPresentableSwapchainHoldDesc`, isolating focus-loss synchronization logic into `focus_loss_policy.h`.

- **Structured D3D11 frame submission descriptor:** replaced the multi-parameter Direct3D 11 / WGC frame processing signature with structured parameter descriptor `D3D11FrameSubmissionDesc`, clarifying frame parameter passing across capture and encoding loops while retaining inline compatibility forwarders.

- **Type-safe video frame submission descriptor:** replaced the 14-parameter inject video frame processing call with structured parameter descriptor `VideoFrameSubmissionDesc`, improving readability and maintainability across encoder loops while preserving backward-compatible forwarders.

- **Encapsulate Streamline hook runtime globals:** grouped loose global interception targets, failure retry counters, viewport runtime states, and module teardown trackers into a unified `StreamlineHookState` structure.

- **Encapsulate DirectX 12 command queue interception state:** grouped loose global command queue dispatch tables, real function pointers, and command list execution tracking into a unified `DX12ECLState` structure.

- **Encapsulate DirectX 12 Post-Streamline runtime state:** grouped loose global PostSL flags, epoch tracking, cooldown counters, and synchronization mutexes into a cohesive `DX12PostSLRuntimeState` structure.

- **Encapsulate DirectX 12 overlay coverage state:** grouped loose global overlay coverage tracking variables into a cohesive, domain-owned `DX12OverlayCoverageState` structure.

- **Streamline diagnostic log metering resilience:** multi-stream change gates now resolve hash collisions across their fixed slot pool, keeping alternating call shapes independently metered.

- **Session logs are much smaller and easier to read:** lines that repeated unchanged every frame or every second (overlay submits, queue choices, frame-generation decisions, Streamline UI-tag records, controller loop timing, config re-reads for every whitelisted game) are now written when something changes, with a "(+N unchanged)" count instead of the copies. Each hook installation is one line instead of about twenty, the Steam overlay is reported once instead of on every hook pass, and FPS-limiter stats and Vulkan layer lines are no longer written to two files. Nothing that marks a change, a failure or a recovery was removed.

- **Shorter hook log lines:** `hook_debug.log` lines read `time T<thread> #<line> p<pid> message`. The line number counts per file, so a gap now always means a lost line, and each process's first line names its executable and pid.

- **UE5 Ray Reconstruction preset no longer causes foliage shimmer:** `high` and `full` now use the smooth (bilinear) Lumen screen-probe interpolation that UE's own Epic and Cinematic quality use. Before, every level forced the cheaper stochastic one, which shows as boiling on foliage in shade and on thin detail further away. `medium` and up also undo UE 5.7's noisy half-resolution GI integration (`r.Lumen.ScreenProbeGather.IntegrateDownsampleFactor=1`).

- **UE5 RR preset levels match what each setting really costs:** the expensive steps (four times the screen-probe traces, full-resolution MegaLights) moved to `full`, faster surface-cache lighting updates moved to `high`, and the free probe-direction cycle moved to `medium`. `full` now also reaches full-resolution MegaLights on UE 5.6 (`r.MegaLights.DownsampleFactor`). Two temporal switches that only overrode deliberate game tuning are no longer written by any level; they stay available in `custom_cvar_overrides`.

### Fixed

- **Recording stop feedback:** explicit child rejection now remains distinct from an uncertain acknowledgement. The recording session owns media-first/inject-fallback ordering and endpoint release; API and hotkey toggles use that same lifecycle policy.

- **DX12 frame synchronization:** normal drawing and capture now retain the frame overlay lock, releasing it only around PostSL retirement and route rechecks. This prevents unowned unlocks and concurrent resource mutation after preparation.

- **FG route confirmation** activation and rendering proof now commit through the PostSL lifecycle owner; cancelled proof cannot be restored by a callback already publishing confirmation, while temporary suspension preserves validated handover ownership.

- **FG queue retirement** selected and pinned PostSL queues now retain GPU completion evidence across delayed callbacks and replacement fences; incomplete drains cannot release those references early.

- **FG callback queue lifetime** PostSL retains selected and wrapper queues through submission and rejects delayed callbacks from retired generations before acquiring resources.

- **FG handover lifecycle** PostSL render admission now covers recording and submission together; retired epochs cannot restore current rendering confirmation.

- **Privacy-blackout repeat recovery:** a successful cached-black repeat no longer promotes the unencoded fresh candidate as the last successful source or counts it as fresh catch-up output. Candidate ownership regressions exercise real ring leases.

- **Recording retry feedback:** an inactive inject or repeat encoder attempt now clears a previous deferred-fence result, preventing rejected attempts from being reported as still waiting for that earlier frame.

- **Recording controls could report success without working:** the controller API now rejects invalid handles, wrong-thread calls and invalid recording modes, returns command failures, and reports unsupported configuration and statistics explicitly. Stopping clears the recording flag, pending start and tray state so the next toggle starts correctly; event polling handles hotkeys and quit messages without millisecond polling sleeps. The reported version follows the actual application build.

- **Recording could crash with mismatched media DLLs:** typed frame submissions now use distinct DLL exports while the original exports retain their calling contract. A rejected DLL also clears its function pointers.

- **Concurrent diagnostic streams could lose changes:** log suppression slots now keep their owners; excess streams are logged instead of resetting a gate another thread is using. Colliding streams use all free slots before overflowing, keeping their separate suppression counts.

- **Elevation service installed in the wrong folder:** the service now runs from a protected runtime inside the actual Capture Engine install folder, including custom locations. Updating migrates an existing service out of the separate `C:\Program Files\CaptureEngine` folder; uninstall also handles older registrations.

- **Overlay kept showing "DLSS 2x" after DLSS frame generation was turned off right after turning it on:** an off request that arrives during DLSS-G's startup is held until the startup is over. Depending on timing it then reached DLSS-G without Capture Engine noticing, so the overlay status stayed on. It now always goes through Capture Engine's own handling.

- **Overlay on FSR frame generation when the game switches its present callback while frame generation stays on:** Capture Engine keeps its own callback in AMD's runtime across such a switch but still ran its callback-less overlay routes next to it, and the last frame before a switch to a callback could lose its overlay. The overlay now follows the route AMD actually uses, frame by frame.

- **Overlay hand-over with FSR UI textures that AMD reads directly:** games that keep their own HUD texture per frame (instead of letting AMD copy it) now get the same exact overlay hand-over at FSR on/off as the others. For a 1x1 placeholder without AMD's copy, Capture Engine alternates two textures instead of redrawing the one AMD is reading. A game that reuses one texture every frame keeps the overlay in that texture without a hand-over.

- **Overlay drawn twice or briefly missing when turning FSR frame generation on or off (games without a present callback):** AMD still shows the previous frame while the game already prepares the next one, and Capture Engine switched its overlay route by the next frame. At every FSR enable the previous frame's outputs got the overlay twice, at every disable or swapchain switch the last frame's final output got none. The route now switches with the frame AMD actually shows.

- **Overlay vanished for seconds after turning DLSS frame generation back on:** after an FSR frame generation phase, switching DLSS-G off and on again could hide the overlay for 6.5 s, and while it was off the overlay still showed "DLSS 2x". DLSS-G's last generated frame after the switch-off no longer counts as frame generation, and the overlay's transition pause always ends completely.

- **Overlay hidden for about half a second after leaving FSR frame generation:** switching from FSR frame generation (already off) to a DLSS or native swapchain no longer pauses the overlay; it is redrawn on the new swapchain at once.

- **Possible GPU device loss when DLSS frame generation starts after FSR:** a temporary resource Capture Engine used to test the game's queue was freed before the GPU had used it.

- **Possible freeze on the first FSR frame generation frame in games with a placeholder UI texture (GTA V Enhanced style):** re-registering Capture Engine's UI texture could re-enter Capture Engine's own hook and deadlock.

- **No overlay on FSR frame generation frames in games without a UI texture:** games that let FSR frame generation interpolate their HUD (no UI resource, no present callback) now show the overlay the same way.

- **Overlay could be submitted on AMD's internal queue:** a game that creates its FSR frame generation swapchain before presenting anything else made Capture Engine mistake AMD's present queue for the game's own, the case known to crash AMD's runtime.

- **Overlay coverage diagnostics missed FSR frame generation frames:** frames presented by FSR frame generation without a present callback were never counted in the session log's overlay coverage summaries, so gaps there went unreported.

- **DirectX 12 games crashed with the D3D12 debug layer or a capture tool wrapping their queues:** Capture Engine handed its first overlay submit to a D3D12 function that only understands D3D12's own queue objects, so a game running with the debug layer (or under a tool that wraps queues the same way) crashed at once. Such queues are now submitted through their own interface; the log names each decision (`Resolved ExecuteCommandLists ... REFUSED - calling through the queue's own vtable`).

- **Overlay drawn on DLSS frame generation frames without the required resource transition:** the overlay drew onto Streamline's output while D3D12 still had it marked ready for display, an invalid use the debug layer reported on every frame. When the overlay runs on the queue that presents the frame it now switches the image to drawing and back; the log line `PostSL barrier mode` shows the choice.

- **UE5 RR preset left reflections noisy without Ray Reconstruction:** under TSR, plain DLSS SR, an RR fallback, or after RR was turned off in a game's menu, the preset still switched off Lumen's reflection denoising. Those settings now follow whether RR is actually rendering and hand the game its own values back the moment it stops. The log says `UE5 overrides: Ray Reconstruction is rendering` / `stopped rendering`.

## v0.1.6941

Changes since [v0.1.6868](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6868).

### New

- **Graphical installer and uninstaller (`captureengine-setup.exe`):** a dark, Windows 11 styled wizard that installs to `C:\Program Files\Capture Engine` by default, with optional desktop and Start menu shortcuts, start with Windows, the elevation service, always-run-as-administrator and the PawnIO driver (installed silently from the bundled, verified installer). It uses only Windows system libraries, supports `/S` silent installs and shows up in Installed Apps with its own uninstaller.

- **Updating over an existing installation:** setup closes a running Capture Engine (and stops the elevation service) before replacing files, and never terminates a game: a hook DLL loaded into a running game is renamed aside and the new one takes its place. Every file is replaced as one unit, so a failure puts the previous version back.

- **`config.ini` is never overwritten by setup:** a missing one is created; an existing one stays byte for byte as it is and the new defaults are written next to it as `config.ini.new` so new options can be compared and copied over. Uninstall keeps `config.ini`, logs and recordings unless you tick "delete my settings and logs", and never deletes recordings.

- **Program Files install works for ordinary users:** setup opens only the folders Capture Engine writes to (`logs`, `captures`, `screenshots`, `benchmarks`) and `config.ini` to the Users group; the program files stay administrator-only.

- **Hardware sensors without elevating CaptureEngine:** the optional elevation service runs PawnIO sensors and privileged display timing in a protected runtime. CaptureEngine and its ordinary workers can stay unelevated; the service stops when its last client exits.

- **Administrator launch preference:** a tray checkbox requests administrator privileges on the next launch, preserving launch arguments and the initiating user's settings. Changing it leaves the current recording running.

- **Start with Windows:** a tray checkbox manages one startup registration per user, including an approved elevated scheduled task for administrator accounts or a login UAC prompt for standard accounts.

- **Limit a game's own texture mip bias (`[Graphics] mip_bias_min` / `mip_bias_max`):** some games, e.g. with upscalers, choose a strongly negative bias, so distant textures shimmer. The game still decides which textures get a bias and how much; CaptureEngine only stops the value at a limit. For example, `mip_bias_min=-2.0` turns -3.5 into -2.0 and leaves -1.0 alone. This works in DirectX 9 to 12, OpenGL and Vulkan, also per profile, and applies after `mip_bias`. `force_mip_bias_clamp` still takes precedence. Shadow and other special-purpose samplers stay untouched. The DX12 log lists each requested-to-effective bias pair once (`sampler mip bias application=A effective=B`). Whenever the range widens it shows the most negative and most positive bias across the game's samplers, before and after the limit (`sampler mip bias range now application=[..] effective=[..]`).


### Improved

- **Download names include the version:** the installer and archives are now `captureengine-setup-0.1.<build>.exe`, `captureengine-0.1.<build>.7z`, `testapps-0.1.<build>.7z` and `ffmpeg-corresponding-source-0.1.<build>.7z`, so a downloaded file shows which release it is.

- **Recordings and screenshots default to your Videos folder:** a new `config.ini` pre-fills `output_dir=%VIDEOS%\Capture Engine` and `screenshot_dir=%VIDEOS%\Capture Engine\Screenshots`, so a Program Files install no longer buries captures in the install folder. Leaving both empty still writes to `captures` and `screenshots` beside the executable; existing `config.ini` files are untouched.

- **Benchmark reports default to your Documents folder:** a new `config.ini` pre-fills `[Benchmark] output_dir=%DOCUMENTS%\Capture Engine Benchmarks`. Leaving it empty still uses `benchmarks` beside the executable.

- **Path variables in `output_dir`, `screenshot_dir` and `[Benchmark] output_dir`:** `%VIDEOS%` and `%DOCUMENTS%` resolve to the Windows Videos and Documents folders even when they were moved (OneDrive, another drive), and `%USERPROFILE%` or any other environment variable is expanded. An undefined variable in a capture folder is logged instead of silently creating a folder named after it.

- **Clearer logs when the upgraded Streamline runtime rejects a game's calls (`streamline_upgrade=true`):** the log used to show only the first rejection of each call as a bare number (`sl::Result=38`), so it could not tell whether DLSS kept failing afterwards. It now names the error (`eErrorInvalidState`), reports how many times in a row the call failed (at the 1st, 2nd, 4th, 8th… failure, and whenever the error changes), and says when the call `succeeds again`. The game's fullscreen switches are logged too, with whether its window had focus. A switch Windows refused is reported as `never reached 2.x DLSS-G's after-hook`: Windows refuses exclusive fullscreen to a window without focus, and Streamline 2.x then leaves frame generation torn down until the next switch.

- **Tracing the overlay gap when DLSS frame generation switches on:** the overlay can vanish for a moment when frame generation starts, for example right after a save loads. The per-present `[OVERLAY HANDOFF]` trace started too late to show those presents. It now starts the moment frame generation is switched on and also records the presenting thread and time, plus the gate (`lastGate=`) that skipped a present's overlay draw.

- **Service removal and portable-folder cleanup:** tray service removal waits for owned processes to exit and removes the protected runtime. CaptureEngine confirms child exit before releasing process handles; injected games must also close before their loaded hook files can be removed.


### Fixed

- **Installer data safety:** uninstall and failed updates preserve files outside the installation when an internal folder or `logs` is a directory link.

- **Installer relocation:** updating through another path to the same folder keeps the new program files; disabling shortcuts during a move removes links to the old folder.

- **Silent uninstall completion:** setup waits for removal to finish and returns its actual result, keeps waiting installer launchers alive, and removes the temporary executable when it exits.

- **Installer Cancel button flickered while files were copied:** every progress update repainted the whole setup window, including the footer under the buttons. Only the status, bar and percentage area is repainted now.

- **Installer regression tests on deeply nested build directories:** test target paths are shortened so tests exercising long method names stay safely within the installer's 200-character directory length limit, even in nested CI runner checkouts.

- **"Recording failed" for recordings saved to a slow drive or network share:** after a stop, everything still waiting to be written goes to the file first. On a slow target this takes a while: a 6 s 4K recording left 34 MB to write to a network share that took about 1 MB/s. CaptureEngine stopped waiting after a fixed 30 s, reported the recording as failed with no file saved, and the complete file appeared on the share 12 s later. CaptureEngine now waits for as long as the writing keeps moving forward. It gives up only if nothing has been written for 45 s; a single write that hangs is still cancelled after 30 s. While it waits, the log shows `writer_finalize_progress` every 10 s.

- **Grand Theft Auto V froze for about 5 s and wrote a freeze dump when DLSS frame generation first switched on after loading a save:** with `backbuffer_count` set, CaptureEngine waited on a frame-pacing signal that belonged to DLSS frame generation, which waits on that signal itself, so the wait could only time out (1 s). A stall check then took that second for a hung game and wrote a dump, which froze the game for another 4 s while the overlay was missing from the frozen frame. CaptureEngine now paces only on signals it added itself, and the stall check no longer counts time a frame spent being presented.

- **DLSS frame generation stayed on when a menu opened right after it switched on:** seen in Witcher 3 with `streamline_upgrade=true` when the menu was opened within about 3 s of frame generation starting, for example right after a save loads. CaptureEngine holds back an "off" in that window because some games, Grand Theft Auto V among them, send stray ones while frame generation starts. Once frame generation was running steadily, CaptureEngine then discarded the held "off" as stray instead of passing it on. A held "off" is now passed on once frame generation is running steadily, unless the game has said "on" since, either by switching it on again or in the status it reports every frame (as Grand Theft Auto V does). It is passed on from the game's own thread at its next frame, so CaptureEngine's own frame-generation state follows too. The log shows `Replaying held slDLSSGSetOptions(OFF) on the title thread`.

- **DLSS frame generation stayed on in the game menu for the rest of the session:** seen in Witcher 3 with `streamline_upgrade=true` after loading a save. Some games, Grand Theft Auto V among them, send stray "frame generation off" requests while it starts up, so CaptureEngine holds back an "off" that comes right after an "on" until frame generation has proven to be running steadily. That proof counted only the game's status queries. Witcher 3 sends no status queries, only on/off changes, so after one held-back "off" every later "off" was held back as well, including every menu. The proof now also counts the frames the game renders while frame generation runs, so the hold ends after a few frames. Games that query the status every frame keep the same protection. The log shows `quiet proof reached (source=title frames ...)` when it ends.

- **DX12 overlay vanished for about 0.1 s when DLSS frame generation switched on:** seen in Witcher 3 right after a save loads. CaptureEngine skipped its overlay on the very frame where the game switched frame generation on, and DLSS then spent about 0.1 s setting itself up, keeping that frame on screen. The overlay is now drawn on that frame, and on any further frame until it has been drawn through frame generation once. This applies when the game switches frame generation on itself and renders on a single queue, without FSR frame generation used earlier in the session. The log shows `Keeping pre-SL overlay drawing through the DLSS-G toggle-on cooldown`.

- **Witcher 3 (DX12) with `streamline_upgrade=true` and DLSS frame generation: alt-tab from exclusive fullscreen crashed the game:** on alt-tab the game leaves fullscreen from its window thread while its render thread is still presenting. Streamline 2.x expects these calls one at a time: leaving fullscreen made frame generation release its copies of the game's back buffers while the present in progress was still using one, and that present crashed (the game then hung inside Streamline's crash handler until it closed). Streamline 1.x tolerated this. CaptureEngine now runs Streamline 2.x's present, fullscreen-change and resize steps one after another. A thread that must wait keeps handling window messages, so this cannot deadlock. The log shows `swapchain call serializer` at startup and `waited for another thread's swapchain call` whenever the game overlapped such calls.

- **Witcher 3 (DX12) with `streamline_upgrade=true`: brief dark flashes while moving through the world:** the game sometimes presents a second time without handing Streamline its depth and motion-vector images again. Streamline 1.x kept the previous images valid. Streamline 2.x discards them after one extra present, so frame generation lost its inputs, switched off for that frame and restarted on the next, which showed as a short dark flash. CaptureEngine now hands the game's last frame-generation inputs back to Streamline after every present, so they stay valid exactly as they did under 1.x. The log shows `re-issuing N DLSS-G input tag(s) after each present` once. That extra present also lacked the Reflex "present start" marker that Streamline 2.x requires before every present (1.x did not), so frame generation still skipped it (`ReflexNotDetectedAtRuntime` in `sl.log`). CaptureEngine now adds that marker for the game's last frame when a present arrives without one; the log shows `title presented without a Reflex PRESENT_START marker - re-marked frame N`. Occasional dark flashes still remained at exactly these extra presents. The new `unmarked present #N` log showed why: since its last frame the game had sent no camera data, no images and no upscale, so the extra present repeats a frame it already showed. Streamline 2.x reads the frame-generation images at every present, and by then the game is already drawing its next frame into them. CaptureEngine now keeps such a repeated present away from Streamline 2.x and from the display; the frame stays on screen through its generated frames. The log shows `absorbed it (a re-present ...)` and the guard's setup line names both hooked plugins. A present that carries a new frame is still shown (re-marked as before), and only the first unmarked present in a row is held back, so a game that stops sending markers never freezes. The flash did not occur with the game's own Streamline 1.x. In the first build with this change the protection never switched on (`absorbing needs ...` in the log): Streamline 2.x shares one function for both of its present hooks, so CaptureEngine's second hook on it failed. Both present paths now go through that single hook, and the guard's setup line says `one folded entry point, shared detour`. With the flashes gone, each held-back present still lines up with one long frame (a 0.1% low near 32 fps). The log now records an `absorbed-present timeline` around such a present: the game's latency markers and wait, each present, and how long frame generation's present step took. It shows that holding the present back costs no time (frame generation's present step returns within 0.1 ms either way). The irregularity comes before it: the game's real frame arrives about 20 ms early and the repeated present sits in that frame's usual slot. The timeline now starts three presents earlier, and it no longer counts each present's frame-generation step twice. The wider timeline shows that the game itself causes this. Its simulation thread is held up for about 11 ms, so the game draws its next frame without a new simulation step. When that simulation step finishes, the game presents again without drawing. The hold is the game's own stutter (for example during abrupt camera pans), not something CaptureEngine adds.

- **No PC latency in the overlay during frame generation (seen in Witcher 3 with DLSS frame generation):** while frame generation started up, a few of the game's own presents reached CaptureEngine with long gaps between them. After that, only the frame-generation runtime presented. CaptureEngine kept using those startup gaps (about 140 ms) as the game's frame time for the rest of the session. That value was too long to estimate a latency from. It also made the game's own latency markers look like they arrived at the output rate, so CaptureEngine discarded them as well. CaptureEngine now ignores the game's present rhythm once those presents stop arriving, and uses the latency markers or frame generation's reported base rate instead. The `PC latency chain` log line shows `appStream=fresh` or `appStream=stale`.

- **Witcher 3 (DX12) with `streamline_upgrade=true`: frame-generation artifacts for the first seconds after loading a save:** Streamline 1.x lets a game mark frames as "not game frames" (loading screens, fades), and 1.x then skips frame generation for them. Streamline 2.x has no such flag, and CaptureEngine dropped it when translating, so DLSS frame generation interpolated frames the game had excluded. CaptureEngine now pauses frame generation for exactly those frames and keeps its resources, so generation resumes on the next game frame without a rebuild. The log shows `title marked frame N ... as NOT a game frame` and `DLSS-G off ... (notRenderingGameFrames ...)`, plus `title requested a history reset` for the game's camera cuts.

- **Steam overlay color-space tracking (e.g. The Witcher 3):** CaptureEngine now leaves the color-space function's entry to a loaded overlay and tracks changes further inside it, so Steam can install its hook instead of failing to decode CaptureEngine's detour.

- **Witcher 3 (DX12) crashed at startup with NVIDIA Smooth Motion enabled:** to find DirectX's functions, CaptureEngine creates a small throwaway Direct3D 11 device. It used the graphics card for this, often while the game was creating its own device. That ran the NVIDIA driver's setup twice at once, and Smooth Motion stops the game when two threads set it up at the same time. CaptureEngine now uses Windows' software renderer (WARP) for this device, so the graphics driver never runs on CaptureEngine's own threads. Its DirectX 12 setup already worked this way. The log line `Temp D3D11 hook-discovery device is WARP` confirms the change.

- **Witcher 3 (DX12) crashed at startup with `streamline_upgrade=true`:** the game creates a test graphics device, releases it, sets an option of DirectX's debug layer, then creates its real device. With the upgrade active, CaptureEngine and the upgraded NVIDIA Streamline runtime still held that first device. Changing the debug-layer setting while a device exists makes DirectX reset that device, so every later attempt to create a device failed with `DXGI_ERROR_DEVICE_RESET` and the game exited (`0xE06D7363`). While the upgrade holds a device, CaptureEngine now answers the game's debug-layer request with "debug layer not installed". The game already handles that answer, and the setting could never have applied to a device that already exists. The log shows `Streamline bridge: refusing D3D12GetDebugInterface`.

- **Witcher 3 (DX12) with `streamline_upgrade=true`: DLSS looked aliased in motion and frame generation added no frames:** three errors in how CaptureEngine translates the game's older Streamline 1.x calls to the newer 2.x runtime: - **Motion vectors misread:** CaptureEngine read the game's camera settings with the wrong field sizes, so the "camera motion is already included" flag was taken from a different field. Streamline then added camera motion a second time. DLSS was sharp at rest but aliased in motion, and frame generation interpolated the wrong motion. - **Latency markers dropped:** the game sends its Reflex latency markers and Reflex sleep calls without a command list, and CaptureEngine discarded every one. DLSS frame generation never saw a present marker, refused every frame (`eDLSSGStatusFailReflexNotDetectedAtRuntime` in `sl.log`) and added no frames. - **Reflex reported unavailable:** CaptureEngine did not answer the game's Reflex availability query, so the game believed Reflex was missing and left it off. The markers now reach the 2.x runtime, the availability query is answered, and the camera settings are read correctly. Each frame also keeps a single Streamline frame ID instead of being reassigned between the game's threads. The log reports `first Reflex marker translated`, a periodic `Reflex markers so far` count, and `answered slGetFeatureSettings(Reflex)`. Turning frame generation off now restores the game's own Reflex setting instead of switching Reflex off until the game's next settings call.

- **DX12 games could crash on their first display creation when the Steam overlay loaded before CaptureEngine (seen in Witcher 3):** the Steam overlay had already hooked DirectX's swapchain creation, and CaptureEngine replaced Steam's hook with its own while Steam was still setting it up. Steam then gave up that hook, but CaptureEngine still passed the game's swapchain creation to Steam's code, which jumped to an empty address. When another overlay already owns that function, CaptureEngine now leaves its hook untouched and intercepts further down, where it handles the call the same way. The log shows `CreateSwapChainForHwnd entry ... is owned by a foreign patch`. A second crash showed a cause underneath: CaptureEngine's own startup check created DirectX factories through the Steam overlay's hook, while the game was creating its own. Steam set up its swapchain hook twice, and the game's first swapchain creation then looped inside Steam until it crashed. CaptureEngine's check now goes around the Steam overlay's hook (`factory vtable discovery enters no overlay handler`).

- **The Steam overlay could stay invisible in DX12 games when it loaded before CaptureEngine (seen in Witcher 3):** whether it worked depended on which program set up its swapchain-creation hooks first. The Steam overlay decides what to hook by reading the DirectX factory's method table (vtable), and skips any entry that already points into another program. When CaptureEngine had redirected those entries, or patched the function itself, first, the Steam overlay never saw the game's display and drew nothing for the whole session (Steam's own log: `points to another module, skipping hooks`). With an overlay loaded, CaptureEngine now leaves both the table entries and the functions to it, even before the overlay has hooked them, and intercepts swapchain creation further down instead. The log shows `Factory vtable slots left to the loaded overlay` and `below-chain swapchain create originated in <module>`.

- **Direct3D 10 games could break as soon as their display was created:** CaptureEngine's texture-filtering hook for Direct3D 10 patched the wrong function. It replaced the game's draw call instead of sampler creation, so every draw call went through code meant for something else. The hook now patches sampler creation.

- **Release changelog promotion:** Windows paths and backslash sequences in release notes now remain literal, preventing promotion from failing or changing the notes after publication.

## v0.1.6868

Changes since [v0.1.6772](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6772).

### New

- **Overlay on 8-bit and 24-bit DirectDraw fullscreen (classic games):** the CPU overlay composite only wrote 32/16-bit surfaces, so classic 8bpp fullscreen (the dominant late-90s DirectDraw mode) and 24-bit primaries got zero overlay pixels and a "cannot write a %u-bit presented surface" log every frame. Both are composited now (8-bit through the game's live palette); YUV formats stay rejected.


### Improved

- **The Vulkan log now says why a game rebuilds its display (e.g. DOOM Eternal staying black while changing resolution):** after switching from 4K to a 1440p mode, DOOM Eternal rebuilt its display 18 times in 11 seconds, alternating between the two sizes while the screen stayed black. The log could not say what asked for each rebuild. CaptureEngine's Vulkan layer now logs when the driver tells the game its display is out of date, suboptimal, lost, or has lost exclusive fullscreen, together with the display's size (`returned VK_ERROR_OUT_OF_DATE_KHR ... told to recreate the swapchain`). These lines are rate-limited, and the result is passed to the game unchanged.

- **The first recording after starting CaptureEngine goes live about 2 seconds sooner:** before its first recording, CaptureEngine measures how late the output device's audio arrives so it can keep audio and video in sync. It plays a near-inaudible tone five times and used to record about 0.6 s of audio after each one, although the tone shows up within about 0.1 s. That took over 3 seconds of the start delay (3.2 s in a 5.5 s start on a 192 kHz device). Each measurement now stops as soon as its tone has been heard completely, which should bring this to under a second. The measured delay is the same, and an unusually slow device still gets the full listening time. The log now shows each measurement's stop reason and duration (`stop=`, `shotMs=`) and the total (`probeMs=`).

- **The log reports the real recording start time:** `[Controller] Recording is live` gave the time until CaptureEngine's once-per-second check noticed the recording, up to a second later than the actual start (6375 ms logged for a recording live after 5453 ms). It now reports the moment the recording went live, plus when it was noticed.

- **The log now warns when the recording output (disk or network share) cannot keep up:** encoded video waits in a queue before it is written to the file, and that queue could fill to over 80% of its 512 MB limit (seen when a network share stalled for about 45 seconds) with only a periodic info line recording it. A `Mux write queue reached N% of its limit` warning now appears at 25%, 50% and 75% of the limit, and a `Mux write queue recovered` line once it drains. Each line compares the file writer's speed with the encoder's, shows how busy the writer was and its slowest write, and says whether the output target or the writer thread is at fault. A single file write that blocks for 250 ms or more is logged too (at most every 5 seconds). Nothing is dropped: at 100% the encoder is held back, which shows up as capture stutter, and the log now points to the output target before that happens.

- **No more false "Texture slot reuse anomaly" and "Inject lineage regression" warnings after a game rebuilds its display (injected capture, seen in The Talos Principle Reawakened when toggling FSR frame generation or changing resolution):** the new display images restart their frame count at 1, and CaptureEngine compared them with the old ones, so each rebuild logged about 17 warnings although nothing was wrong. These checks now start over with the new images, and a single `Inject lineage restarted` info line records the switch. Real out-of-order frames within the same set of images are still reported.

- **The log now names what holds up the recording when it falls behind:** a recording-loop pass that takes longer than four frame intervals logs an `[EncoderThread] Slow loop iteration` line: which step held the thread, the time of every step, and how much CPU time the thread actually got. CPU time close to the stall means CaptureEngine was busy. CPU time near zero means the thread was blocked or preempted. Lines are rate-limited, and a summary counts the ones left out. Slow passes before the recording goes live (one-time encoder startup) are logged as information, not as warnings.

- **The log now breaks down what CaptureEngine's present hook costs each frame, stage by stage:** every 10 seconds a `[PRESENT STAGE COST]` line per presenting thread (the game's own, or a frame-generation runtime's presenter such as AMD FSR's) lists the mean, 95th-percentile and worst time CaptureEngine spent in each part of its DirectX present hook (routing, overlay drawing, frame limiter, bookkeeping and more), with the game's actual present call kept out of the total. This shows where CaptureEngine's ~80 us per frame on FSR frame generation's presenter thread (GTA V Enhanced) goes; the measurement itself costs well under 1 us per frame.

- **Overlay PC latency read 110-150 ms with FSR frame generation (e.g. GTA V Enhanced):** right after FSR frame generation switched on, AMD's runtime showed nothing for about half a second while the game kept rendering. CaptureEngine counted all of those frames as still waiting in the frame-generation queue, hit its limit of 8, and added about 7 frames to the latency for the whole session. Such an impossible count is now discarded, and the latency uses the normal one-frame estimate until the queue can be measured again. `[Overlay] PC latency chain` reports each discarded count as `queueCountRejects=`.

- **No more per-frame module lookups when another overlay patches DirectX's present function:** with another tool's jump on the present function (for example an overlay loaded by the Epic launcher), CaptureEngine checked on every frame whether that jump belonged to DLSS frame generation, asking Windows' module loader each time, on the game's and on AMD's presenter thread. The answer is now remembered until a module loads or unloads. The once-a-second DLSS module scan, which also went through the loader, likewise runs only after a module loads or unloads.

- **Less background CPU and cache load in games with AMD FSR (e.g. GTA V Enhanced):** once a second CaptureEngine re-checked every loaded module for FSR function pointers to redirect. In GTA that meant reading and rewriting all 39 MB of the game's global data every second (~115 ms of CPU, flushing the processor cache under the running game) for the whole FSR session, although nothing new turned up after the first check. The check now runs only when a module loads or unloads, or when the game reaches FSR through a route CaptureEngine has not redirected yet. It also only reads the game's data and writes only the pointers it actually redirects.

- **Present and command submission no longer wait for DLL loads on other threads:** CaptureEngine identified the caller of every present and GPU submission by asking Windows' module loader several times per call. That query takes the loader lock, so whenever another thread was loading a DLL (DLSS, Streamline and FSR all do this mid-game), the game's present thread or AMD's frame-generation presenter thread stalled until the load finished. Module identity is now remembered and refreshed only when a module unloads.

- **Shorter hitch when frame generation turns on (DirectX 12):** creating an overlay renderer built its text font from scratch (~4 ms) and allocated 32 separate GPU buffers (~6 ms). GTA V Enhanced did this twice on AMD's frame-generation presenter thread when FSR frame generation started, then re-created 16 more buffers because the frame-time graph did not fit. Later renderers now reuse the finished font, the buffers come from one allocation, and they are large enough for the graph.

- **Stop summary shows whether an audio device's clock drifts:** each source now reports how often, and by how much, CaptureEngine had to insert or remove a millisecond of audio to keep it in sync after startup (`[STOP AUDIO PLACEMENT] ... driftPpm=`), so a USB microphone or audio interface that runs slower or faster than the system clock is visible in the log.

- **Frame-time graph no longer looks uneven with FSR frame generation at the vsync limit:** there Windows reports every other frame about 3.5 ms early, so the graph alternated between roughly 3.5 and 10.4 ms on a 144 Hz display while the screen showed every frame for exactly one refresh. With vsync on, the graph now never draws a frame sooner than the display could show it. Real stutter, repeated frames, variable-refresh pacing below the limit and presents without vsync are drawn exactly as before, and the debug log reports how often and how far frames were moved.

- **The debug log now names every frame generation switch where the DirectX 12 overlay briefly disappeared:** for each swapchain replacement it records whether the last image of the old swapchain and the first image of the new one carried the overlay, and how long nothing was presented in between. Overlay coverage is also counted once per presented frame; a frame reported by two CaptureEngine hooks was previously counted twice and could be listed as missing the overlay when it was not.

- **Vulkan sharpening on every window of a game:** only the first window (swapchain) of a game was sharpened; a second one, such as a launcher or editor viewport, was left unfiltered. Each window now gets its own sharpen pass. Rebuilding a pass (for example when a game moves its present to another GPU queue) or switching sharpening off no longer waits on the game's present for CaptureEngine's own GPU work; the old pass is released once that work has finished.

- **CaptureEngine's Vulkan layer stays out of non-target applications:** the layer is registered for all Vulkan applications, and until now it stayed in every one of them in pass-through mode, including games whose profile says `dll_injection=never` and anti-cheat-protected titles. It now declines at load time in processes that are not injection targets, so the Vulkan runtime builds their instances without it and unloads the layer again. While CaptureEngine is closed, it still enters games from your injection-enabled profiles, so a Vulkan game started before CaptureEngine still gets the overlay once CaptureEngine starts. That list is kept in the registry (`HKCU\Software\CaptureEngine`), not as a file in the program folder, and `layer_register.exe --unregister` removes it.

- **Other Vulkan applications no longer load CaptureEngine's full layer at all:** before deciding, every Vulkan program on the PC (browsers, Discord, Explorer, games) briefly loaded the 1.5 MB layer and the graphics DLLs it depends on, and kept its file locked. Those programs now load only a small gate (`VK_LAYER_CE_gate.dll`) that makes the decision and depends on nothing but core Windows DLLs. The full layer is loaded only into games that CaptureEngine injects into.

- **Up to several GB less disk use for session logs:** every CaptureEngine start copied about 180 MB of debug symbols into the new session folder (twice), and 20 sessions are kept. Sessions now share one stored copy through hard links (volumes without hard links still get copies), and stored symbols no retained session uses are removed. The injected game DLL no longer copies symbols at all.

- **Freeze dumps no longer extend the freeze:** when a game stops presenting for the watchdog timeout, the diagnostic dump is now written by the external helper whenever it is available, so the game's threads are not suspended while it is written.

- **External crash dumps name the faulting instruction:** dumps written by the external helper now carry the crashing thread's exception context, so the debugger opens at the fault instead of at the helper launch.

- **CaptureEngine keeps the game's own error-reporting settings:** the injected crash handler used to replace the game's Windows error mode and error-reporting flags; it now only adds its own.

- **Slow log writes now explain themselves:** a single log call that blocks longer than 50 ms writes a rate-limited warning into the log it slowed, so a stall episode is recorded in the very output it affected.

- **Settings-state mixups between different CaptureEngine builds can no longer hide in struct padding:** the shared-memory compatibility fingerprint now also covers the nested graphics/overlay struct sizes and the sharpen/DLSS frame generation fields, so a layout change that consumes padding fails closed between builds instead of silently misreading state.

- **Forced AF / sampler override visibility on DX9:** "CaptureEngine reached no sampler at all" is now logged at a settled render loop (2000 presents) instead of only at process shutdown, and another overlay re-patching the sampler hooks is detected per Present and reported as "sampler hook drift" instead of silently disabling forced AF and the sampler state shadow.

- **Bridged DLSS-G cadence writes are now attributable in logs:** when a forced frame-generation cadence is written on a Streamline 1.x bridged title, a rate-limited warning names the write and the unconfirmed 1.x field mapping behind it, so a wrongly-written cadence can be traced in session logs.

- **A `config.ini` change saved right after it was read is no longer missed (UTF-8 config files):** CaptureEngine keeps the parsed config and reused it while the file's write time and size matched, so a rewrite that kept both inside one file-time tick (a setting flipped `1` to `0`) kept its old value. A file written within the last 2.5 seconds is now confirmed byte for byte before the cached parse is used.

- **No wrong frame right after a DirectX 12 game rebuilds its display (injected capture):** a frame from the old display could still pass the recorder's handle check against a texture handle the new display reused, because the new generation began only after the old textures were closed. It now begins before, so such a frame is dropped instead.

- **A hidden helper window no longer keeps the game's graphics queue alive (DirectX 12 games that create a swapchain on a hidden window):** CaptureEngine held a reference on the queue of every hidden-window swapchain until that swapchain showed a frame, which a helper never does. References for windows that were destroyed are now released.

- **Caller identification cannot touch a DLL that is unloading:** the first lookup of a DLL's address range read its image headers without holding the DLL loaded, so a game unloading that exact DLL in that instant could crash it. The DLL is now held for the read.

- **A refused swapchain resize can no longer hold a game for minutes:** when a DirectX 12 game's resize is refused because something still holds its back buffers, CaptureEngine scans the process's memory once to name the holder. That scan now stops after 3 seconds and says so in the log, instead of running as long as the process has memory.

- **Another component's crash-time release is no longer removed when the keyboard hook stops:** CaptureEngine releases its low-level keyboard hook before writing a crash dump so that keyboard input on the whole desktop does not freeze during the dump; stopping the hook now removes only its own release.


### Fixed

- **Release log privacy:** fixed automatic cleanup and its verification for GitHub's empty log-deletion response, so self-hosted build logs are removed and checked unavailable after each release attempt.

- **Stable release builds:** isolated unused FPS-limiter test helpers and made the GPU pacing regression independent of host scheduling, so analyzer warnings and a busy PC no longer block verified releases containing the FSR pacing fixes.

- **FSR frame generation could exceed the FPS limit with uneven frame pacing:** when FSR presented distinct frames close together, CaptureEngine mistook them for duplicates and let them skip the general FPS limit, including when a lower general limit took precedence over capture sync. Every confirmed FSR output now takes its own pacing slot while the presenter keeps its nonblocking lock.

- **FPS limits cut the frame rate in half with FSR frame generation (DirectX 12 games where FSR presents the frames itself, seen in The Talos Principle Reawakened):** the general FPS limit (`FpsLimiter.general_fps`) held every frame FSR showed to half the configured rate, so a 120 fps limit showed 60 fps. With capture sync the same happened for about the first second of every recording, and the game then took a few seconds to become smooth again. In Reflex limiter mode, capture sync let the game render twice the recording rate. Capture sync was also compared against the general limit at double its real rate, so a higher general limit (for example 144) replaced it. CaptureEngine now knows that each of these frames is one shown frame and paces it at the configured rate. The limiter log shows `captureSource=final site=3` for this case.

- **Recordings with FSR frame generation moved at half the on-screen frame rate (injected DirectX 12 capture, seen in The Talos Principle Reawakened):** the game showed a smooth 120 fps, but the recording only took the frames the game rendered itself (60 per second) and skipped every frame FSR generated in between. Each missing frame was filled by repeating the previous one, so the 120 fps video looked like 60 fps. When FSR's own presentation step reports a frame just before showing it, CaptureEngine now records it whether FSR generated it or the game rendered it, so the recording matches what is on screen. The hook log now counts both kinds (`generatedOutputs=` / `applicationOutputs=`).

- **Recording stop could still hang on a stalled disk or network share:** a full packet queue stopped checking the write timeout, leaving the encoder blocked and shutdown waiting forever. The queue wait now keeps cancelling overdue writes so recording finalization can continue.

- **Game crash when FSR runtime unload overlaps CaptureEngine shutdown or runtime replacement:** restoring the FFX create-entry breakpoint now holds the exact DLL loaded through the byte restoration. A disappearing runtime is rejected without reading its old address, and DLL references are acquired and released outside the breakpoint mutex.

- **Recording never started after switching from DLSS to FSR frame generation in-game (DirectX 12 injected capture, seen in The Talos Principle Reawakened):** the recording stayed in its preparation phase and was discarded when stopped. With FSR's own presentation step, FSR draws CaptureEngine's overlay, so CaptureEngine's regular overlay stays switched off, and the switch from DLSS had just torn it down. CaptureEngine's frame processing stopped at "overlay not ready" on every frame, before it reached the capture step, so no frame was ever recorded. Now only the overlay setup waits and every frame still reaches the capture step. Recordings set to leave the overlay out now also record frames the overlay step never reached, instead of dropping them (`Overlay-free capture published after the overlay draw chain did not reach it` in the log).

- **Game closed when switching from DLSS to FSR frame generation after a recording (DirectX 12 injected capture, seen in The Talos Principle Reawakened):** once a recording had run, CaptureEngine's capture kept a hold on the game's display even after the recording stopped. When the game later switched frame generation, it released its old display, but CaptureEngine's hold kept it alive. Windows then refused FSR's new display for the same window, and the game quit. The capture now only remembers which display it copies from and takes no hold on it, so a released display really goes away. The "access denied" diagnostics in the log now also name the display the capture is bound to (`captureKey=`).

- **Capture sync FPS cap stopped working after switching from FSR to DLSS frame generation in-game (DirectX 12 games with the Steam overlay, seen in The Talos Principle Reawakened):** with `capture_sync_enabled=true` the game kept running above the capture rate (about 140 fps instead of 120) in every recording until the game was restarted. After the switch, DLSS frame generation's final frames take a separate route past the Steam overlay's outdated hook, and that route never ran CaptureEngine's FPS limiter, so neither capture sync nor the general FPS cap engaged. The route now paces frames exactly like the normal one, and the log records when the limiter starts or stops pacing there.

- **Profile DLSS/Streamline overrides were ignored in Portal RTX and other RTX Remix games:** `dlss_sr_dll_path`, `dlss_rr_dll_path`, `dlss_fg_dll_path`, `streamline_dll_path`, `dlss_sr_preset`, `dlss_rr_preset`, `dlss_fg_preset` and `dlss_debug_overlay` never took effect. These games draw with DLSS in a separate helper process (`NvRemixBridge.exe`), so CaptureEngine's Vulkan layer loads its hook there to apply the profile. The layer looked for that hook next to its own copy under `%LOCALAPPDATA%\CaptureEngine\vulkan_layers\`, which only holds the layer files, so the load failed (`error=126`) every time, while the game process had already stepped aside for the helper. Nothing applied the overrides, with no message outside the layer log. CaptureEngine now records its install folder beside the staged layer at every start and the layer loads the hook from there. If the hook still cannot be loaded, the layer says which overrides will not apply and hands them back to the game process instead of leaving them unowned.

- **Recording never started in Portal RTX and other RTX Remix games (injected Vulkan capture):** the recording stayed in its preparation phase and was discarded when stopped. In these games the game process only hands its frames to a separate helper process (`NvRemixBridge.exe`), which draws them with Vulkan, and CaptureEngine's Vulkan layer captures them there. The recorder only accepted frames from the game process itself, so it threw away every frame the helper sent. It now also accepts frames from that helper, but only when the helper is the game's own child process and CaptureEngine's Vulkan layer registered it as the game's renderer. The mouse cursor in the recording is placed using the game's window, which the helper does not have. The media log now reports `Admitting split-renderer frames` (or `Refusing split-renderer frames` with the reason) when the helper's frames arrive.

- **Overlay showed a far too low, lagging FPS and frame-time graph, then no latency at all, after the game switched to composed presentation (seen in DOOM Eternal on Vulkan, going from native 4K to a 1440p mode):** at that resolution NVIDIA's Vulkan driver stops flipping the game's frames onto the screen itself and hands them to Windows to compose instead. From then on CaptureEngine's display timing saw no flips of the game's own. It kept every unflipped frame for 10 to 15 seconds and later matched unrelated screen updates whose internal number happened to be the same to those old frames. In the reported session every frame the overlay was given had supposedly taken 9 to 12 seconds to reach the screen. The overlay drew about 29 fps for a game running at 140, and its graph kept flipping between display and presentation timing. A frame now only counts as displayed if the screen update comes within one second of it, and frames nobody displayed are dropped after that second. Composed frames are now timed by Windows' compositor instead: a frame counts as displayed with the first compositor screen update made after the game finished it. The frame-time graph therefore keeps measuring what reaches the screen, and games without Reflex show "Latency est." again instead of "PC Latency -". The sensor log reports the switch and names the compositor process (`now expire without a flip completion` / `Flip completions resumed`). The health line counts `expired=` and `staleRejected=` frames and has a new `composed(...)` section.

- **No video after restarting CaptureEngine while a D3D11 game on DXVK kept running (injected Vulkan capture):** once the game had switched to the recorder's own textures, a restarted CaptureEngine was handed those old textures, which only the previous recorder could open, plus a sync object that was no longer signaled. The game now rebuilds its own capture textures for the new CaptureEngine instead. The same stale textures were also reused after the game recreated its display at the same size between recordings, and a busy moment during the handover could skip it entirely. Both are fixed.

- **Recording could show one still image for its whole length in D3D11 games running on DXVK (injected Vulkan capture):** when the game was already running before recording started, CaptureEngine switched to its faster direct-texture path shortly after the start, but kept waiting on a sync object that nothing ever signals. Every frame was then held back as not finished yet. The Vulkan layer now hands over its own sync object together with the textures.

- **Recording froze a few seconds after changing the resolution (injected Vulkan capture, seen in DOOM Eternal switching 4K to 1440p):** the rebuilt display got a new sync object for its frames, but CaptureEngine's Vulkan layer announced it in a place the recorder only reads for a different capture mode. The recorder kept checking the old, retired sync object, so the new frames looked finished until their count passed where the old one stopped; from then on every frame was held back and the video showed one still image until the recording ended. The layer now announces the new sync object exactly where the recorder reads it.

- **A crashing game froze for about a minute before it closed (games with the Steam overlay, seen in The Talos Principle Reawakened):** CaptureEngine re-reads the version information of certain loaded DLLs whenever a DLL loads, and did so by reopening the file through a system call the Steam overlay intercepts and locks. When the game's own crash reporter started writing its crash file (which loads a DLL and pauses every other thread of the game), CaptureEngine could be paused while holding Steam's lock, and the crash reporter then waited on that lock for every module it listed: 50 seconds of a frozen game in the reported session. CaptureEngine now reads that information from the DLL already in memory, without touching the file or the intercepted call.

- **CaptureEngine's dump of an Unreal Engine fatal error was either huge or too thin (Unreal Engine titles with an overlay loaded):** Unreal raises one specific error just before it shuts down on a fatal error, and CaptureEngine treated it as a harmless warning. Its dump first pulled in the whole game's global data (158 MB, an 18-second pause in The Talos Principle Reawakened), and then only thread call stacks (533 KB), which could say neither who held a lock the game hung on nor what the stacks pointed to. The dump now records call stacks, open handles with lock owners, the memory the stacks refer to and the memory layout, without the global data, and the crash log names the error as Unreal's fatal-error path.

- **DirectX 12 games closed at startup when their window was shown only after the display was set up (seen in The Talos Principle Reawakened in its "windowed" native-resolution mode, with or without the Steam overlay):** CaptureEngine deliberately ignores displays created for hidden windows, because programs create such helper displays that never appear. It therefore also forgot which GPU queue owned the game's real display when the game created it before showing its window. The first overlay frame was then sent on a different queue of the game's, which DirectX does not allow, so it shut down the graphics device and the game quit without a CaptureEngine crash dump. CaptureEngine now remembers what it skipped and catches up on the display's first frame once the window is visible, so the overlay goes out on the display's own queue. The log reports `Parked create-time queue ownership` at creation and `First visible Present of hidden-window swapchain` when it catches up. A new `No swapchain queue captured` line flags any other route that leaves the display's queue unknown.

- **Changing the resolution closed DirectX 12 games that have the Steam overlay (seen in The Talos Principle Reawakened, with or without frame generation):** the Steam overlay keeps a hold on each of the game's display images and lets go of them only in its own handling of the game's resize. It installs that handling by patching the start of DirectX's resize function, and it skips a function that another program has already patched there. CaptureEngine had put its own resize handling at exactly that spot (and in earlier builds, in the display's function table). Steam therefore never saw the resize and kept its holds, DirectX refused the new resolution, and the game quit. CaptureEngine now hooks further inside the resize function, as it already did for presenting frames, and leaves the start of the function to Steam. Steam releases its holds first, and CaptureEngine still runs right after it. A new diagnostic counts who takes and returns holds on the display images after every DirectX 12 resize (`BackBufferRefTrace:` lines). The log also names who owns each of the display's functions (`slot owners`) after the first resizes and at any refused one.

- **Recording never started in games with FSR frame generation drawn through FSR's own presentation step (injected DirectX 12 capture, seen in The Talos Principle Reawakened):** the recording stayed in its preparation phase and was discarded when stopped. To keep its cost off AMD's frame-generation threads, CaptureEngine stopped counting the game's GPU submissions on that route, but it still used that count to tell real frames from generated ones, so every frame looked generated and none was ever recorded. It now takes the real/generated decision from FSR itself, which reports it for every frame just before presenting it.

- **Recording could keep showing old frames after a game rebuilt its display (injected capture, mainly DirectX 12; also Vulkan resolution changes and DirectX 8-11/OpenGL display resets):** when a game recreated its swapchain (alt-tab in exclusive fullscreen, resolution change, switching frame generation), CaptureEngine replaced its shared capture textures, and Windows can give the new ones the same internal numbers the old ones had. The recorder identified textures only by those numbers, so it could keep reading the old, no longer updated textures. Each set of capture textures now carries its own generation number: the recorder reopens everything when it changes and skips the few frames that were captured just before the switch.

- **Recording froze on a still image after alt-tabbing out of a game and back (injected capture, seen in DOOM Eternal on Vulkan):** while the game was in the background it stopped drawing, and CaptureEngine kept its very last frame back as a safety reserve instead of recording it. When the game came back it rebuilt its display, which has to wait until CaptureEngine hands back every frame it still holds, so the game side waited for CaptureEngine and CaptureEngine waited for a newer frame that never came. The video then showed the same picture until the recording ended, while the audio kept going. A frame whose GPU copy is already finished is no longer held back when it is the only one left, so recording resumes on the game's first frame after the switch. This applies to every injected graphics API, not only Vulkan.

- **A Vulkan game started at the same moment as CaptureEngine could run without the overlay:** at every start CaptureEngine briefly removed its own Vulkan layer registration and added it back a few milliseconds later, because it mistook the 64-bit and 32-bit entries in the shared per-user registry key for leftovers of each other. A Vulkan game that initialized in that gap never loaded the layer and could not get the overlay or be recorded through it until it was restarted. The registration now stays in place the whole time.

- **"Recording saved - video degraded" shown when only audio was affected:** every degraded recording or stream was reported as video degraded, even when the only problem was audio (for example audio the recording could not keep up with, an unplugged microphone, or a crashed audio worker). The message now names what was affected: "audio degraded", "video degraded" or "audio and video degraded", in the in-game overlay and on the desktop alike. The recording's manifest file records it as `recording_degraded=`.

- **Recording reported as degraded when Windows repeated a piece of system audio:** when system sound resumed after a quiet period, the Windows audio engine sometimes handed over the same 10 ms of audio twice (flagged as a glitch). CaptureEngine correctly kept only one copy but counted the dropped repeat as audio lost to an overloaded pipeline, so the overlay showed "Recording saved - video degraded" for a recording with no missing audio or video. Only audio the recording really could not keep up with counts as lost now; dropped repeats are logged separately (`dedup=` in the stop summary).

- **10 ms of system audio lost when sound resumed after a quiet period:** when the output device had been silent for a while (for example between games), some audio drivers stamped the first packets of the returning sound with a time far in the past. CaptureEngine replaced each of those with the moment it read them, but it reads several packets at once, so they landed on top of each other and all but one were thrown away. The recording was then reported as degraded. Such packets now stay back to back, and nothing is discarded.

- **Game audio could waver in pitch on loading screens and static scenes (app audio capture, screen capture):** the catch-up that pulls game audio back toward the video briefly sped it up by up to 0.5%, several times a second, because its target followed the measured video delay, which jumps around while the screen barely changes. In one 4-minute recording it switched on and off 715 times. It now follows the delay's peak over the last several seconds, so it only runs for a real, lasting backlog.

- **Game audio ran ahead of the video when the game started after the recording (app audio capture):** a game whose audio appeared only after recording start (for example launched mid-recording, or restarted) joined its track without the delay that keeps audio in step with the delayed video. Its sound then played early by that delay for the rest of the recording (about 0.3 s in the reported desktop-duplication session). CaptureEngine then kept slowing that audio down by the maximum allowed amount (0.05%) trying to close the gap. A late game now joins at its correct position, with the same delay as every other track.

- **The recording thread ran without its high scheduling priority:** CaptureEngine registered its encoder thread for high-priority multimedia scheduling and undid the registration immediately, while the log still said it was enabled. Under heavy CPU load (for example a game loading or compiling shaders) the thread could fall behind, and the recording dropped frames to catch up, which is visible as a stutter. The registration now lasts the whole recording.

- **An audio speed correction stayed active after a game went quiet:** when an app's audio stopped (for example after the game closed), the last speed correction stayed set and applied to its first audio if it came back, and the stop summary reported it as maxed out. It is now cleared when the app goes silent.

- **Audio and video drifted apart in long CFR recordings (worse at high frame rates):** CaptureEngine picked each recorded frame on a real-time schedule that ran slightly faster than the file's frame timestamps, because the frame interval was rounded down to whole timer ticks. Video therefore fell behind the audio by about 14 ms per recorded hour at 60 or 120 fps, 23 ms at 144 fps, 58 ms at 240 fps and 100 ms at 360 fps. With capture sync active, the drift also caused an occasional repeated frame (about every 4 minutes at 240 fps). The schedule now places every frame at its exact time, for WGC, desktop duplication and injected capture alike.

- **A short click when a track's audio came back after silence (e.g. a game muting itself on alt-tab):** the fade-in at the resume point only lasted one video frame's worth of audio and then jumped to full volume, on top of the source's own fade-in. The fade now runs its full length across frames, and is skipped when the source already fades itself in. The recording-start fade could stop early the same way when the first audio chunk was short; it now runs its full length too.

- **The last fraction of a millisecond of a 44.1 kHz or 96 kHz audio track was silent:** the sample-rate converter's final samples were never written, so each such track ended on a tiny silent gap instead of the recorded audio. They are written now.

- **Audio crossfades after a trim in VFR recordings started too abruptly:** the crossfade began at 60% of the new audio instead of at the previous sample, and a backlog trim crossfaded the wrong spot while leaving the actual cut hard. Both now start exactly where the audio left off.

- **Possible crash in games that unload AMD's FSR library (e.g. GTA V Enhanced after every FSR session):** CaptureEngine could put its FSR breakpoint back at the address where the unloaded library used to be. Whatever the system loaded there next would then have been corrupted. It was never seen crashing, but GTA hit this situation at every start. CaptureEngine now forgets that address when the library unloads, and checks that the address still belongs to FSR before every change.

- **GTA V Enhanced: FSR frame generation cleanup never ran when the game closed FSR:** GTA loads AMD's FSR library fresh for every FSR session and creates frame generation right away, through function lookups CaptureEngine cannot redirect. CaptureEngine therefore never saw FSR frame generation being created and treated every FSR object the game closed as unrelated, so the cleanup that releases the overlay from FSR never ran; recent GTA fixes only worked around what it left behind. CaptureEngine now catches the creation as soon as the library loads, and recognizes any FSR object it still missed from the game's first FSR call on it.

- **Overlay disappeared for good in The Talos Principle Reawakened's menu after switching FSR frame generation on, off and then to DLSS frame generation:** while FSR frame generation was on, CaptureEngine drew its overlay through FSR's own presentation step, so at the switch to DLSS it wrongly concluded the overlay had not been visible and skipped preparing it for the new DLSS swapchain. Without that preparation nothing proved the new swapchain safe to draw on while the menu kept DLSS frame generation idle. CaptureEngine now counts the overlay as visible on any route it last drew on.

- **Overlay stayed hidden in GTA V Enhanced's menu after switching from FSR frame generation to all frame generation off:** in the menu GTA sets up FSR frame generation but never switches it on, so CaptureEngine kept holding its overlay back and waited for FSR to start. CaptureEngine normally stops waiting when the game closes FSR, but it had missed GTA creating FSR's frame-generation contexts, so it did not recognize them when they were closed. When the game then went back to its normal display, the overlay stayed off until the game was closed. CaptureEngine now stops waiting as soon as the game creates its normal display again in the same window.

- **Overlay stayed hidden in GTA V Enhanced's menu after toggling DLSS frame generation (after an earlier FSR frame generation session):** CaptureEngine kept a recovery guard from the old FSR-to-DLSS switch active after the game had already returned to its normal display, because that return happened shortly after frame generation was on. When DLSS frame generation was then switched on again in the menu, the game created a new DLSS display that stays idle while the menu is open, and the old guard kept the overlay switched off on it until the game was closed. The guard now ends as soon as the game's normal display or the new DLSS display is confirmed, so the overlay stays visible. The overlay on the normal display also no longer takes the slower copy path the guard forced.

- **GTA V Enhanced crashed with `ERR_GFX_STATE` when switching from FSR frame generation to DLSS frame generation:** with DLSS frame generation not yet running, CaptureEngine judged the new DLSS swapchain as left over because the game kept rendering on its own GPU queue. It then drew the overlay into that swapchain from the game's queue, which does not own those buffers, and Windows removed the GPU device. CaptureEngine now checks which queue the presented swapchain belongs to and leaves a live DLSS swapchain alone.

- **Overlay disappeared after switching from FSR frame generation to DLSS frame generation (DirectX 12):** on the new DLSS swapchain CaptureEngine never marked its own overlay GPU work as finished, so after 16 frames the overlay treated every buffer as busy and stopped drawing. It now marks that work as finished after every present, as it does on any other swapchain, and the overlay keeps drawing. Only AMD's own FSR presentation queue is still left untouched.

- **Overlay briefly missing after switching from FSR frame generation to DLSS frame generation:** during DLSS frame generation's first frames the overlay only reached its generated frames, not the real ones, until CaptureEngine's regular DLSS overlay route took over, and that takeover waited for the game to stop presenting through its old path. Switching again in that window left a real frame without the overlay on screen for up to a second. When the game explicitly enables DLSS frame generation after FSR on a proven safe route, the overlay is now drawn into every output from the first frame on.

- **DirectX 12 games dropped to 1 FPS for about 10 seconds after switching from FSR frame generation to DLSS frame generation (e.g. GTA V Enhanced):** CaptureEngine's overlay work on the new DLSS swapchain was never marked as finished until DLSS frame generation actually started (fixed separately above), and the overlay waited up to one second on every frame for that work before drawing again. The overlay never waits there anymore: while that work is still pending it skips drawing for that frame (as it already did after the wait), so the game keeps its full frame rate and the overlay comes back as soon as the queue catches up.

- **The DX12 frame generation switch test app could not switch from FSR FG back to DLSS FG after the first DLSS session:** it asked Streamline to accept its device a second time after rebuilding its renderer, which Streamline refuses by design, and then treated DLSS as unavailable for the rest of the run. It now keeps using the device Streamline already accepted. It also no longer tags the back buffers of a swapchain Streamline does not present: DLSS frame generation kept the tagged native or FSR swapchain alive, so switching DLSS FG -> OFF -> DLSS FG (or FSR FG -> DLSS FG after an earlier DLSS session) failed to create the new swapchain and the app stopped.

- **DirectDraw / DirectX 7 games closed with their own error box when rebuilding their display (e.g. Gothic II after loading a save):** CaptureEngine still held on to the game's old screen surfaces and 3D device, so the game could not create new ones. CaptureEngine now lets go of them before the game rebuilds its display.

- **No crash dump when a game shows its own error box and then quits:** a game that reports a fatal error in a message box and exits normally afterwards now gets a crash dump, and the log records the message box text.

- **DirectX 7 and DirectX 8 games lost texture settings whenever they restored a state block (even with no filtering override configured):** after a game applied a saved state block, CaptureEngine wrote its own older copy of the texture settings back over it. With no override configured that copy was stale (the startup defaults, or whatever an earlier state block had set), so textures could switch to the wrong wrapping or filtering in state-block-heavy games. With no override CaptureEngine now writes nothing after a state block; with forced filtering it remembers what each state block restores (including blocks the game records), keeps the game's own values, and still forces the configured filtering.

- **A selected microphone or output device that was unplugged was silently replaced by the Windows default:** the recording then captured a different device (for example a webcam microphone) and was reported as saved. The track now stays on the selected device: it holds silence while the device is missing, switches back as soon as the device is plugged in again, and the recording completes as saved (degraded).

- **Audio lost to an overloaded audio pipeline was reported as a clean save:** captured audio the recording could not keep up with (logged as `starve=` in the stop summary) never reached the file, and a crashed or failed-to-start audio worker left every track silent, yet the recording still said saved. Both now complete as saved (degraded). A source that was simply quiet is not affected.

- **Overlay missing in DirectX games with two overlays active (e.g. Steam plus RTSS) when only part of CaptureEngine's present hook could be installed:** if the hook for the game's `Present` call failed while the one for `Present1` succeeded, CaptureEngine treated presentation as covered, never retried, and saw none of the game's frames. Coverage is now judged per call: the missing hook is retried on the next swapchain event without touching the part that works, and CaptureEngine's own swapchain view stays on while `Present` is uncovered.

- **Removing a hotkey from `config.ini` did not disable it until restart:** a blanked or deleted overlay, screenshot, audio-only or benchmark hotkey kept its previous binding after a live settings reload. Every reload now applies the file as written.

- **A settings reload during a locked `config.ini` published default settings and never retried:** if the file could not be read at that moment (an editor, sync tool or virus scanner holding it), games and the recording process switched to defaults and CaptureEngine considered the change applied. A reload is now used only when the whole file was readable and did not change while it was read; otherwise the current settings stay and the reload is retried.

- **Vulkan games could freeze for good after one failed overlay submission:** a failed overlay GPU submission left one of the overlay's fences in a state nothing would ever signal, and a later present could wait on it forever. A failed submission now re-arms its fence (or retires that slot for good), and the wait for a busy slot is bounded; in that case the overlay skips one frame instead of holding the game.

- **The logging process could outlive CaptureEngine after a crash:** if CaptureEngine was ended hard, its logging process kept running and competed with the next CaptureEngine's logger for the games' log buffers. It now exits when CaptureEngine does.

- **DirectX 9 games lost texture settings whenever they restored a state block (with anisotropic filtering or mip-map overrides active):** after a game applied a saved state block, CaptureEngine re-applied the filter override from the texture settings the game had used *before* that block, undoing the block's address modes, filters and LOD bias on every apply (wrong texture wrapping or blurriness in state-block-heavy games). With no override configured, the first state block after startup could reset every texture sampler to the Direct3D defaults. CaptureEngine now remembers what each state block restores (including blocks the game records), keeps the game's own values, and still forces the configured filtering.

- **Forced anisotropic filtering stopped working in DirectX 9 games once another overlay re-hooked the device:** a second overlay that took over the texture-sampler functions left CaptureEngine's filter override and its view of the game's texture settings dead for the rest of the session. When such a takeover persists, CaptureEngine now hooks the Direct3D 9 implementation underneath the other overlay (once, without touching the other overlay's hook), so filtering keeps working and both tools stay active.

- **Settings with Japanese, Chinese or Korean characters were misread on Japanese, Chinese or Korean Windows:** on those systems Windows' own settings reader garbled UTF-8 text in `config.ini` before CaptureEngine saw it, so paths such as a Japanese recordings folder or a profile named after a Japanese game executable did not work. UTF-8 configs are now read directly from the file.

- **The first section of a `config.ini` saved as "UTF-8 with BOM" was ignored:** the byte-order mark hid the first section header from Windows' settings reader, so its settings silently stayed at their defaults. The first section is read now.

- **Crash dumps, fallback logs, debug switches and add-on detection failed below folders with non-Latin characters:** with CaptureEngine or the game in a folder the Windows language setting cannot express, crash dumps written before the session folder was known went to a mangled path, the external crash-dump helper was never found, and the DirectX 12 debug flag files, fallback log folders, DXVK/ReShade/Special K/OptiScaler detection, the Streamline version check and the check for a game's own Streamline all came up empty. These lookups now use Unicode paths (or the folder's short 8.3 name where an older interface needs one).

- **A recording lost everything before an HDR switch:** turning HDR on or off during a recording (the Windows HDR toggle, or a game's own HDR setting with injected capture) made the encoder re-open the recording file and overwrite what had already been recorded. The recording now ends at the switch instead, keeps everything recorded so far, and completes as saved (degraded).

- **Cropped, stretched or frozen video after a resolution change during a recording:** when the recorded window or game changed size mid-recording (window resized, resolution changed, fullscreen toggled), the new frames went through a converter built for the old size. They are now scaled into the recording's size with the aspect ratio preserved and black borders, and the recorded cursor stays on the right spot.

- **OpenGL games rendered incorrectly once the overlay was shown (missing depth, wrong transparency, wrong textures):** the overlay's OpenGL 3+ path switched off depth testing and face culling and changed the blend mode and active texture unit without restoring them, and left its own vertex buffer bound. Games that set this state once, or keep their own copy of it, rendered wrongly from the first overlay frame on. Every piece of state the overlay touches is restored now; the older OpenGL path also restores the game's vertex array pointers (they could point into freed overlay memory), and the overlay's font upload no longer reads from a pixel buffer the game left bound.

- **System audio went silent after switching the Windows output device:** with the default setting (record the default output), switching the default output during a recording (the volume flyout, or a headset that becomes the default while the speakers stay connected) kept recording the old, now silent device for the rest of the recording. Recording now moves to the new default output (or default microphone) right away.

- **Silent microphone or system-audio track without any warning:** a device that was missing, busy or blocked by the Windows microphone privacy setting when the recording started was dropped for the whole recording, and the recording was still reported as saved. The track now picks the device up as soon as it becomes available (for example when a headset is plugged in), and a recording with such a gap, or with audio lost to encoder faults, completes as saved (degraded).

- **A recording kept running without video after a GPU driver reset:** when the video encoder or its GPU device failed for good, every frame failed and the recording went on as audio over no video, reported as saved. After 5 seconds without a single encoded frame the recording now ends, keeps everything encoded before, and completes as saved (degraded).

- **Game exit stalled and wrote a 50 MB crash dump for a normal quit:** a game that quits with exit code -1 (seen with Strange Brigade quitting through Steam) was treated as a crash, holding the exit for about 2 seconds. Small negative exit codes are ordinary exits now; real crashes are still dumped.

- **DirectX 12 games could hang for good after alt-tabbing out (windowed):** a wait for CaptureEngine's own overlay GPU work before presenting fell back to waiting forever after 2 seconds, even when that work could only finish after the game's present. The wait is bounded now; the overlay just waits for that work before it draws again.

- **Short stall during frame generation swapchain changes:** swapchain teardown polled for up to 100 ms on a counter that could not change at that point. The wait is gone.

- **DirectX 11 overlay garbled or missing in some games:** a geometry or tessellation shader the game left bound ran on the overlay's triangles. The overlay now disables those shader stages for its own draw and restores them, and it restores every viewport the game had bound, not just the first.

- **Settings with non-English characters were misread:** a `config.ini` saved as UTF-8 (the Windows Notepad default) turned characters such as umlauts in paths, window titles or process names into garbage, so recordings could go to a differently named folder and profiles never matched. UTF-8 configs are read correctly now; characters the Windows language setting cannot represent are logged.

- **Settings briefly fell back to defaults while `config.ini` was being saved:** a reload could read the file while an editor was still writing it, and for about a second the games and the recording process ran on defaults (profiles, injection targets and overrides dropped and re-applied). A change is now applied only once the file has stopped changing, and an empty or missing file is never applied.

- **Installations in a folder with non-Latin characters ran on default settings:** `config.ini` and the logs were looked up through a path the Windows language setting could not express, both in CaptureEngine and in the game. The folder's short (8.3) name is used when needed, and a warning is logged when the volume has none. This also works with the Windows "UTF-8 for worldwide language support" option, where the lookup previously came back empty.

- **Rare audio crash at recording start:** the audio timeline was rebuilt at recording start while the audio worker could still be reading it.

- **Overlay disappeared after switching from FSR frame generation to DLSS frame generation (e.g. The Talos Principle Reawakened):** when the switch was made in a menu, DLSS frame generation created its new swapchain on its own GPU queue but did not start generating frames yet. CaptureEngine waited for the game's rendering to move onto that queue before setting the overlay up again, which never happens, so the overlay stayed gone until the game closed. The overlay is now set up on the new swapchain right away, and the second wait path for the same condition recognizes the handoff too, so the menu-switch case cannot recur through it.

- **Long recordings were deleted when finalizing failed:** if writing the file's final index failed (a full disk at the end, or any earlier transient write error, which FFmpeg reports again at the end), the whole recording was removed. A video recording with committed packets that fails to finalize is now kept; its index may be incomplete, so seeking can be slower until it is remuxed. Audio-only recordings follow the same rule.

- **Stutters and stalls in games that handle their own exceptions (Unity/Mono, Java, .NET, LuaJIT titles, emulators):** CaptureEngine's in-game crash handler treated every first-chance access violation or runtime exception as a crash and wrote a dump, stalling the game for up to 5 seconds, using up the one crash dump, and then appending to `crash.log` on every following exception. Such exceptions are now only recorded. A dump is written, with the recorded faulting context, only when the process actually dies of one.

- **Recording could stop when settings changed during a recording:** one config-reload acknowledgement that took longer than 1 s tore down the channel to the recording process, which then stopped the live recording. A first late reply to a reload or ping is now discarded without dropping the channel; the recording process also acknowledges reloads before doing the work.

- **Rare crash when settings changed while recording:** a config reload replaced settings that the recording threads were still reading. Reloads during a recording are now applied when the next recording starts.

- **Rare game crash with FidelityFX frame generation when two threads configured it at once:** a second thread hitting CaptureEngine's `ffxConfigure` breakpoint while the first had just disarmed it was left unhandled and could end the game; it now simply continues. A breakpoint that could not be removed now fails visibly instead of hanging the thread.

- **Injection failed for installations in folders with non-Latin characters:** the hook DLL path was passed through the system ANSI code page, which turns characters such as Cyrillic or Japanese into `?`, so no game could be injected. Paths are now passed as Unicode.

- **Steam overlay recovery could write into the wrong memory:** CaptureEngine's recovery for a Steam overlay callback that is not yet initialized could act on crashes of unrelated threads, and fell back to a fixed Steam memory offset that current Steam builds no longer use. It now acts only on the thread it guards and only on the proven callback slot, and is registered once instead of on every frame.

- **Black screen in Vulkan games with sharpening enabled (e.g. DOOM Eternal):** when a game destroyed its swapchain and created a new one, CaptureEngine's sharpen pass kept drawing through views of the destroyed images, and the GPU driver reported a lost device on the next frame. The sharpen pass is now released before the swapchain is destroyed, and the semaphores a pending present may still wait on are kept until afterwards.

- **Black screen in Vulkan games that present from a compute queue with sharpening enabled (e.g. DOOM Eternal with "present from compute"):** the sharpen pass kept submitting graphics work to the game's compute-only present queue, which the GPU driver answered with a lost device. On such queues sharpening now runs as a compute shader on the game's own present queue; where the game's swapchain or GPU does not allow that, sharpening is skipped with a logged reason instead of breaking the game.

- **Sharpening changes had no effect in running Vulkan games:** a `sharpen` setting changed while a Vulkan game was running was ignored until the game restarted. The setting now applies on the next frame, as it already did for D3D11 and D3D12.

- **Delayed keystrokes in other applications while a hotkey fired:** the global hotkey keyboard hook, which every keystroke on the desktop waits for, wrote a log line (with a disk flush under a process-wide lock) inside its callback. A slow flush, typically right when a recording starts, held keyboard input for every application. The hook thread now only counts. The controller does the logging.

- **Keyboard input could wait on busy game threads:** the hotkey hook thread now runs at time-critical priority, so a game's high-priority render threads can no longer keep it from answering while all cores are busy.

- **Silent loss of the hotkey keyboard hook:** Windows removes a keyboard hook without notice after repeated timeouts, which left hotkeys dead in games that suppress normal hotkeys (e.g. DOOM Eternal). A late answer is now detected, logged (`[Hotkey] Keyboard hook answered late`), and the hook is re-armed immediately; a removal Windows had already made is logged and repaired.

- **Keyboard input froze during a CaptureEngine crash dump:** writing the controller's crash dump suspends its threads, including the keyboard hook's, so every keystroke on the desktop waited on the hook timeout until the dump finished. The hook is now removed before the dump starts.

- **Recordings with hours of content could still be deleted at stop:** a cancellation arriving after video was already written (or a single HDR metadata packet failing to normalize) removed the whole file. A recording with written video frames is now always kept; only recordings without a single written video frame are removed.

- **A full disk produced a recording with missing frames that was still reported as saved:** when the disk filled up or writes failed mid-recording, frames were dropped for the rest of the session (logged at most ten times) and the completion message still said saved. The recording now stops at the first lost write, keeps everything written so far, and completes as saved (degraded). A memory shortage while queueing encoded frames is counted the same way instead of dropping frames silently.

- **Stopping a recording could hang forever on a stuck disk or dead network share:** writes to local recording files were unbounded, so a hung write wedged the stop path and the recording was never finalized. Local outputs now have a write timeout (30 s): a write still blocked past it is cancelled, which turns it into a reportable error and lets the stop finalize the file.

- **A recording could be reported as cleanly saved while its finalization was still unfinished:** when stopping gave up waiting for a slow finalize, the completion was decided before the final index write and frame-continuity check had run, so a failure there never reached it. Such a stop now completes as saved (degraded).

- **Game crash at startup when a launcher spawns the game as a child process:** the injected parent freed the DLL path buffer in the child while the child's loader could still be reading it (a cross-process crash), and every outcome was logged as "Injected" even when the load failed. The buffer is now retained until the child's load finishes, the path is Unicode end to end, and the real result is reported.

- **Early child injection silently skipped for games launched with a command line and arguments (the common launcher shape):** the whitelist check compared the tail of the whole command line (e.g. `game.exe" -dx12`) against the executable name and never matched. It now resolves the program a launch actually starts.

- **Wrapper DLL, crash dumps, hook byte recovery, and child injection failed for installations in non-Latin folders:** the hook derived its own directory with the system ANSI code page, so characters such as Cyrillic or Japanese became `?` after loading. These consumers now derive the directory as Unicode; the per-app `config.ini` reader is still ANSI at its file API and remains affected.

- **Rare startup crash in 32-bit games from an out-of-range hook jump:** jump displacement was computed by a truncating cast, so a hook DLL loaded more than 2 GB away from the hooked function could receive a wrapped-around jump target. Displacements are now computed and range-verified explicitly and the install fails cleanly instead of writing a bad jump.

- **Game freeze at startup while installing hooks next to other overlays (Steam, RTSS):** the hook installer wrote log lines while every other thread of the game was suspended and could deadlock against a suspended thread holding the log lock. Failure reports are now emitted after all threads resume.

- **Steam overlay recovery logged a stale fixed memory offset as its callback slot:** current Steam builds moved it, so the diagnostic line named a random address. The read is gone; the recovery itself already acted only on proven slots.

- **Small hitch when switching from DLSS frame generation to FSR frame generation:** every such switch could stall the game for up to 200 ms draining GPU work even though the overlay state was deliberately kept alive. The keep-alive exceptions now cover the drain, teardown, and cooldown steps uniformly instead of being listed per step.

- **Frame generation status could stay wrong for the rest of the session:** a game re-enabling DLSS frame generation through the status query alone (no explicit enable call) stayed suppressed, leaving the overlay without frame generation status and the frame limiter at base rate against generated frames. Sustained real generation now clears the suppression, and the log states when suppression is active.

- **Rare crash risk when switching frame generation modes:** a deferred overlay GPU-signal path touched the overlay's fence outside its lifetime lock and could race an overlay rebuild. It is now serialized with rebuilds and pins the fence while signaling, and logs the fence identity for future diagnosis.

- **Game crash with NVIDIA Smooth Motion on DX12 (device removed 0x887A002B):** when Smooth Motion's private output chain appeared without its recorded GPU queue, the overlay draw was routed to an arbitrary queue and could take the device and the game down. The draw is now skipped there with a logged reason.

- **Recording kept filming the wrong screen after the game exited (window capture):** when the captured window closed mid-recording, capture retargeted to the foreground monitor or primary and recorded whatever the user was doing next. The retarget now lands on the same display the window was on (pinned at start) or stops the recording and keeps what was captured.

- **Recording ran to its end as a frozen picture when the capture source died:** a failed capture replacement whose rollback also failed left a frozen frame plus live audio with no visible error, and when the captured window itself had closed, the rollback restarted capture of that dead window, which can start and then deliver no frames. The recording now never rolls back onto a closed window; it stops through the normal path and completes as saved (degraded).

- **Screenshots could use the wrong HDR brightness on multi-monitor systems:** tone-map calibration read the primary monitor's SDR white level instead of the display the captured content lives on, which differs per monitor ("SDR content brightness"). The captured display's own level is used now, with the same 203-nit fallback.

- **Up to 2 seconds of wrong-color frames after toggling HDR during capture:** the format recheck waited for its periodic 2-second timer even when delivered frames visibly contradicted the capture's HDR contract. The recheck now fires immediately on the contradicting frame.

- **Misspelled settings silently changed behavior:** an unknown `capture_method` became `auto` with no warning, and `auto` normally means injected capture — one wrong character in `capture_method=wgc` quietly enabled injection. Unknown capture methods, hotkeys, and limiter modes now log which key was invalid and what replaced it, once per typo.

- **`crash.log` leaked the Windows account name:** the crash trace log is now privacy-masked like every other log, and the dump paths it names are collapsed to drive + file name, so it is safe to share in support requests.

- **UE5 `ensure()` errors could freeze the game repeatedly and flood the session with dump files:** every ensure wrote its own dump in-process (a multi-second stall with Steam or Social Club overlays loaded) with no limit. Assert dumps now go through the external dump helper when another overlay is present and stop after three per run.

- **Foreign breakpoint traffic could stall the game and cost the real crash its dump:** an `int3` from anti-cheat or another hooking tool (handled by that tool) each cost a full in-game dump stall, and the first one also used up the process's single crash dump, so a real crash afterwards got none. Breakpoints are now recorded like every other first-chance fault and dumped only if the process actually dies of one (through the unhandled-exception path, the termination that follows it, or the Windows error-reporting dump CaptureEngine adopts).

- **Rare capture statistics corruption during a capture stats reset:** the reset updated pool timing state without the lock the capture producers use.

- **Stack overflow with a co-resident overlay in DX8/DX9/OpenGL games (e.g. Steam overlay):** a foreign injector hooking below CaptureEngine re-issued presents through CaptureEngine's hooked entry until the stack overflowed (32,768 recursion levels in 2 ms in Gothic II). Present detours now answer a nested presentation with the real implementation past the foreign entry patch (once per present) instead of calling back into the cycle, and return without presenting, with a caller-module log, when no bypass can be built.

- **Game hang after the D3D7 overlay font-atlas upload (e.g. Gothic II):** the atlas lock was the one DirectDraw lock CaptureEngine took without DDLOCK_NOSYSLOCK, taking the Win16 lock inside the game's Flip; it now behaves like every other CaptureEngine lock and falls back to the CPU composite if rejected.

- **Overlay and freeze watchdog dead after one failed DirectDraw Unlock:** a failed Unlock (or an unbalanced application Lock/Unlock pair) permanently deferred DirectScanout presentations and the freeze-watchdog heartbeat while the game ran on. Unlock attempts now always resolve the surface's lock tracking, with a failed Unlock treated as a conservative "surface changed". Games holding several rectangle locks at once are tracked per lock, so the overlay is not composited while another rectangle is still being written.

- **Vulkan capture survives single-queue and async-present GPUs:** the sharpen and compute-present overlay submits raced game submissions on the game's own queue without the serialization lock every other CaptureEngine submit takes — the prime suspect for the recurring QueueSubmit/device-lost black-window failures (DOOM Eternal "present from compute"). All queue submissions now hold that lock.

- **Vulkan overlay/capture/sharpen can no longer be locked out of a game for the whole session:** a running CaptureEngine host completely masked the persisted Vulkan layer target list at layer negotiation (decided once per process), so a profile edit mid-session, a CaptureEngine/game start-order race, or a non-ASCII exe name silently removed the layer for the rest of the run. Either signal now admits (worst case: a dormant stale entry), and executable names are matched in Unicode end to end against both whitelists.

- **Vulkan implicit layer no longer maps the full layer into every Vulkan process from the fallback install path:** when layer staging falls back to the install directory, the stale manifest naming the full layer could be registered, bringing back the 1.5 MB layer (and its imports) in Explorer, browsers and anti-cheat-protected games. The negotiation-gate manifest is now generated in that path too.

- **Sharpen no longer stalls the present thread when two swapchains are live (Vulkan):** the per-device sharpen state rebuilt its whole pipeline — including up to 3x1 s fence waits — on every present of a second swapchain (auxiliary windows/tool surfaces). The first live swapchain keeps the filter, other chains pass through untouched until it is destroyed, and swapchains under 320x180 never trigger the filter.

- **FPS limiter no longer flips to async-present routing for a whole session off one stale worker submit:** the submit-thread heuristic remembered a single background-thread submit forever and permanently moved the limiter boundary and overlay route. It now requires a mismatch within the same 2-second recency window the acquire route uses.

- **Audio stays in place when the encoder rejects a frame:** a mid-track `avcodec_send_frame` failure froze the audio PTS counter, placing every later frame one frame early (~21 ms AAC, up to ~86 ms PCM) for the rest of the track, and back-pressure (`EAGAIN`) dropped the frame entirely — sends are now retried after draining, and a truly failed frame becomes an explicit silence hole at its exact position, never a re-placed track.

- **No more compressed audio after encoder faults:** a consumed chunk the encoder could not take (resampler/FIFO failure) was re-requested and refilled with newer samples, silently compressing the track while every length check stayed green — consumed ranges are never re-filled now; lost tails are placed as explicit silence holes at their exact timeline positions. Audio trimmed on purpose at the recording end is never counted as lost, and a hole on the final chunk stops at the recording end.

- **Audio encoder FIFO never truncates recorded audio:** the old five-second overflow ceiling was dead code with a stale "drop newest" policy; a consumed batch now enters in full (the FIFO grows to fit) or is refused whole and placed as an explicit hole, with honest capacity diagnostics.

- **Race-free audio timeline cursors:** the audio worker read the pull thread's sample cursors (write-cursor pinning, gap suppression, diagnostics) as plain integers; they are now snapshotted under a dedicated leaf lock, preventing erroneous timeline trims under load.

- **Possible game crash when an exception hit a Windows loader worker thread while the Steam overlay hook was active (games with the Steam overlay):** the exception handler that recovers from Steam's null callbacks runs first for every exception on every thread and read per-thread state that loader worker threads do not have, so it faulted inside itself until the stack ran out. It now checks a thread-id list first and only touches per-thread state on the thread that armed it.


### Removed

- **Limiter helper process:** the separate limiter process had not paced anything since frame pacing moved into the game process, but it still ran a highest-priority thread pinned to CPU core 1 with a spin-wait, which could delay other threads on that core, including input processing of unrelated applications. Its request wait could also spin a core at full load. It is gone. Capture-synced recording no longer waits for it or fails with "limiter readiness failure". The FPS limiter itself is unchanged.

- **In-game window procedure hook:** the injected runtime no longer replaces the game window's message handler. It forwarded every window message (including every high-rate mouse input message) through a lock without using any of them, and made unloading riskier alongside other overlays.

## v0.1.6772

Changes since [v0.1.6652](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6652).

### New

- **AMD FidelityFX CAS and RCAS post-processing sharpening (D3D11, D3D12, Vulkan):** added Contrast Adaptive Sharpening (`sharpen=cas`) and Robust Contrast Adaptive Sharpening (`sharpen=rcas`). Configurable via `sharpen_contrast` (adaptation sensitivity) and `sharpen_amount` (blend weight). Executes as a full-screen GPU pass prior to overlay composition, preserving clean overlay text. Supports SDR and HDR (using ST 2084 PQ for scRGB to protect specular highlights), includes sharpened output in screenshots, and supports live runtime tuning via configuration reloading without restarting the game.

- **Driver-level DLSS Multi-Frame Generation controls:** added `[DLSS]` configuration keys (`dlss_fg_mode`, `dlss_fg_fixed_count`, `dlss_fg_dynamic_max`, `dlss_fg_target_fps`) to override the driver-settings channel in-process. Enables fixed generation up to 5x/6x or dynamic cadence targeting display refresh rate (`dlss_fg_target_fps=max_refresh`) without modifying global driver profiles.

- **DLSS Frame Generation V-Sync override:** extended `vsync_mode` to intercept and answer the driver-level V-Sync query read by the DLSS-G runtime, ensuring forced synchronization settings apply correctly above Streamline swapchain proxies.

- **NVIDIA NGX over-the-air update control (`ngx_ota`):** added `[DLSS] ngx_ota=off|on` to intercept and suppress background `nvngx_update.exe` launches and clear Streamline OTA preferences during `slInit`, preventing remote OTA updates from overriding locally configured DLL versions.

- **NVIDIA NGX diagnostic logging (`ngx_log`):** added `[DLSS] ngx_log=off|on|verbose` to route NGX runtime diagnostic logs directly into the session directory for diagnosing DLL override and Streamline plugin resolution.

- **Automated GitHub release notes generation & changelog tooling:** added `tools/manage_changelog.py` to validate formatting, prevent tag/release note drift, and automate synchronized GitHub release note publication during stable release workflows.


### Improved

- **Vulkan implicit layer registration decoupling:** the implicit layer manifest is now staged in CaptureEngine's runtime directory rather than build paths. Stale `baseDir` entries from prior builds are neutralized on startup, and orphaned machine-wide (HKLM) registrations are explicitly warned on when running unelevated.

- **Unelevated process monitoring CPU reduction:** replaced periodic WMI process table queries (previously twice per second) with direct native Windows process queries, eliminating sustained background WMI service CPU overhead.

- **Early recording cancellation handling:** stopping a recording during media pipeline startup now cleanly marks the session cancelled in the manifest and overlay rather than hanging indefinitely on "Finalizing recording...". Audio latency calibration is now cached per session to eliminate startup latency on subsequent recordings.

- **Recording startup telemetry:** the controller now logs the confirmed live timestamp and measured pipeline startup latency rather than assuming immediate recording start upon request dispatch.

- **Hook installation diagnostics under NVIDIA Smooth Motion:** logged hook installations and removals now report when fallback thread-suspension heuristics were used, clarifying hook status when driver background threads prevent strict thread quiescence.

- **Startup performance diagnostics accuracy:** unified startup timing accumulation across controller subsystems so `[StartupPerf]` logs accurate elapsed durations rather than uninitialized or system uptime values.

- **Developer LSP, code style, and editor tooling integration:** restored canonical root configuration files (`.clangd`, `.clang-format`, `.clang-tidy`, `.editorconfig`, `pyrightconfig.json`) to enable in-editor clangd diagnostics, automatic 4-space K&R code formatting, and EditorConfig support across all IDEs. Retained compilation database entries for unbuilt translation units across partial and test-only builds so `clangd` maintains full-codebase indexing and IntelliSense during routine test loops. Enabled C++ standard library indexing, all-scopes symbol completions, and block-end inlay hints, and configured safe header insertion policies to prevent Windows SDK include ordering issues. Configured `.vscode/settings.json` to use the project's bundled MSYS2 Clang 22 toolchain, added recommended workspace extensions, synchronized Python type-checking exclusions with internal facade units to eliminate false diagnostic squiggles, and added HLSL shader file associations.


### Fixed

- **Games failed to start with a driver error when `backbuffer_count` was set:** Strange Brigade (DX12) aborted during startup with "Can't recover from driver error. Error Code 80070057" and never rendered a frame. `backbuffer_count` adds a frame-latency waitable object to the swap chain, which DirectX requires the game to repeat on every buffer resize; the correction that hid it from the game was skipped whenever another overlay (for example Steam) owned the present entry, so the game's own resize was rejected. The flag is now hidden consistently on every path, and it is only requested when that correction is guaranteed to exist.

- **Anisotropic filtering and mip bias silently did nothing in DirectX 12 games:** forced AF and negative mip bias never reached titles that resolved Direct3D 12 before CaptureEngine attached, because the sampler and root-signature overrides were installed only when CaptureEngine observed device creation itself. They are now installed during injection setup, which covers devices the game had already created. A session that still reaches no sampler now says so explicitly in the log instead of failing silently.

- **Debug log could drop lines without saying so:** under heavy logging a few entries were discarded with no record anywhere, making any gap impossible to tell from "nothing happened". Dropped lines are now counted and reported, log buffer overflow is reported, and the buffer is drained again immediately instead of after a pause whenever it was found full.

- **Performance CSV was cut off mid-row in Unreal Engine titles:** games that exit by terminating themselves — routine in UE5 — skipped the only code that closed `perf_metrics_*.csv`, so the buffered tail was lost and the final row was truncated. The file is now finalized from the process-termination path.

- **Buffer count override could shrink a swap chain the game asked to leave alone:** a resize that passes no buffer count means "keep the current one" in DirectX; CaptureEngine substituted the configured depth there and reallocated the chain.

- **Strange Brigade / Steam module injection crash:** fixed startup access violation crash caused by concurrent CaptureEngine hook threads racing to modify memory page protection while patching adjacent Import Address Table (IAT) entries during Steam overlay library loading.

- **Live configuration reload freeze & overlay blackout:** fixed in-game overlay disappearing and potential game render thread deadlocks when saving `config.ini`. Configuration reload queries now respond immediately with asynchronous background cache warming, preventing false IPC timeout respawns.

- **Recording finalization freeze:** fixed a deadlock where stopping a recording while privacy blackout focus checks were executing could leave the media worker hung indefinitely waiting on a shared lock.

- **Windows Error Reporting dialog suppression:** properly configured WER error modes so game crashes write diagnostic minidumps directly to the session folder without hanging on a modal "program has stopped working" prompt.

- **Crash on virtual desktop focus transition:** fixed a crash caused by caching an unmarshaled COM interface pointer across thread apartment teardown during window focus changes.

- **DirectX 12 sharpen queue drain:** fixed fence drain and ComPtr lifecycle issues during overlay teardown when sharpening was active.

- **DirectX 12 sharpen crash during DLSS Frame Generation toggles:** properly synchronized command queue transitions when enabling/disabling DLSS Frame Generation mid-game, preventing GPU resource recycling hazards and frame tearing.

- **Sharpening mode dynamic switch crash:** fixed crash when switching between CAS and RCAS at runtime caused by releasing pipeline state objects while previous GPU frames were still executing.

- **Vulkan sharpening command buffer starvation:** fixed an issue where failed or aborted queue submissions left command buffers permanently marked in-flight, which eventually disabled the sharpening pass for the remainder of the session.

- **The Witcher 3 (DX11) with NVIDIA Smooth Motion:** fixed startup crash caused by compositing onto the interposer's transient output swapchain buffers. Also resolved severe overlay flickering and corrected FPS counter to reflect game render rate rather than interposer presentation rate.

- **`__fastfail` crash dump capture:** unhandled fatal crashes and security check terminations (`0xC0000409`) that bypass in-process SEH/VEH handlers are now captured from WER crash dumps into the session directory, and legacy invalid registry configuration paths are automatically cleaned up.

- **Early-startup Streamline DLL override race:** armed the DLL redirection loader hook directly in `DllMain` using pre-published injector configuration, preventing early-loaded modules like `sl.common.dll` from bypassing overrides before the background hook thread finishes reading configuration.

- **Steam overlay hook layering reliability:** added automatic retries when hooking present body calls during initial game thread creation, ensuring CaptureEngine layers correctly beneath the Steam overlay.

- **Render thread stutter during Reflex / PCL hook resolution:** bounded retry attempts for Streamline functions not exported by specific library builds, preventing periodic 2.5-second render thread hitches.

- **False frame generation health warnings in auto/dynamic modes:** suppressed false-positive activation warnings when DLSS Frame Generation dynamically chooses not to interpolate frames.

- **DLSS Dynamic MFG status detection:** fixed false "dynamic MFG not supported" overlay warnings by gating state field reads on the game's actual DLSS-G struct version.

## v0.1.6652

Changes since [v0.1.6261](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6261).

### New

- **DirectDraw / Direct3D 7 games are supported** (Gothic II and other legacy titles). Overlay, recording,
  screenshots and the `[Graphics]` overrides (V-Sync, anisotropic filtering, mip mapping) now work there. The
  overlay draws with the game's own Direct3D 7 device (`legacy_d3d_native_overlay=on`) and falls back to a CPU
  compositor for 2D frames, loading screens and unusual surface formats - neither route does a GPU readback on
  the game's render thread.
- **Screenshots can save HDR and SDR from one capture.** The new default `[Screenshot] color_space=both`
  publishes an HDR shot twice: as a native 10-bit BT.2020/PQ AVIF and as a tone-mapped SDR PNG, under one name
  that differs only by extension. Both encodes run at the same time, so the pair costs little more than the AVIF
  alone, and a capture that is not in HDR still saves exactly one PNG.
- **NVIDIA Smooth Motion is recognised.** CaptureEngine detects it, reports its status with the real base and
  output frame rates, draws the overlay on Smooth Motion's own output flip - topmost above Steam and RTSS, and
  not interpolated with the game frame - and applies `vsync_mode=fifo` there.
- **FFmpeg messages now reach the session log.** Encoder, muxer and RTMP diagnostics previously went to a
  discarded stderr. They are logged with stream keys redacted, so live streaming keeps full diagnostics instead
  of being silenced to protect the key.

### Improved

- **Lower input lag from the FPS limiter.** It used to spend its entire wait after the game had already finished
  the frame, so a finished frame aged in the present hook - 9.3 ms of it in Strange Brigade at a 90 fps cap. The
  game is now released ahead of the deadline, with a reservation learned from real overruns and from GPU
  completion times so smoothness is not traded away for latency. Recording with capture sync deliberately keeps
  the old placement, because a missed deadline there costs a repeated frame in the file.
- **GPU load is readable under frame generation.** The overlay summed the 3D, Compute and Copy engines and
  clamped the total to 100%, which parked the reading on the clamp whenever FG was running. It now reports the
  busiest engine, the same way Task Manager does.
- **The GPU and VRAM rows stop flashing `--`** while a game briefly has no GPU work. Windows removes the counter
  instance there, which means idle, not unreadable.
- **Faster, steadier game startup.** Four variable-cost stalls are gone: a machine-wide thread snapshot taken for
  every installed hook, a throwaway WARP Direct3D 12 device created in games that never use DX12, and two
  unbounded window-title queries on the render thread. Hook installation dropped from about 1.25 s to 0.65 s and
  the worst single hitch from 533 ms to 15 ms.
- **Crash dumps from 32-bit games now contain 32-bit stacks.** The dump helper is 64-bit and had been recording
  only the WoW64 side, which holds nothing but a syscall thunk - the same gap Task Manager's own dumps have.
- **A freeze the game explains itself is recorded with stacks instead of 30 MB.** When the stuck thread is simply
  running its own error dialog, the freeze is still detected and named in the log, just without a full memory dump.
- **Privacy blackout follows virtual desktops.** `black_when_no_fullscreen_focus` now rejects cloaked windows,
  Task View and shell windows, and keeps monitor capture tied to the process that established fullscreen focus.
- **Cheaper Vulkan capture:** per-frame allocations removed from the present path, and a swapchain that moves to
  another queue family mid-run re-learns its settings instead of keeping the ones chosen at startup.

### Fixed

- **Gothic II (DirectDraw/SystemPack):** fixed injection crashing at startup on this title's older import tables
  and on relocated bypass code; fixed the overlay flickering every frame, disappearing during loading screens and
  darkening itself on repeated draws; fixed a crash in the intro videos caused by the overlay re-binding a texture
  the game had already released; fixed a full game freeze caused by a hook cycle between CaptureEngine and the
  Steam overlay, and the frozen picture that the first fix for it produced; fixed V-Sync and anisotropic-filtering
  overrides doing nothing in this game's borderless mode; fixed screenshots never completing in-game; fixed the
  overlay compositor costing most of the frame rate; and fixed a VRAM reading of 8.7 exabytes.
- **Portal RTX (RTX Remix):** fixed heavy stutter with `vsync_mode=fifo` under DLSS multi-frame generation -
  CaptureEngine was taking the generator's own flip scheduling away. Fixed the same stutter at 4x MFG, where the
  generated batch no longer fits the display's refresh rate: the rendered rate is now bounded by the panel, so
  2x/3x/4x all pace cleanly on a 144 Hz display. Fixed clean exits writing a 183 MB dump.
- **DOOM Eternal:** fixed the window going black for the rest of the session after a swapchain change (overlay
  objects were released too late, and their present-wait semaphores too early). Fixed CaptureEngine pushing NVIDIA's
  Vulkan driver off its native present path - visible as the Windows volume popup drawing over the fullscreen game.
- **Strange Brigade:** fixed the frame rate running at ~130 fps against a 90 fps cap while the limiter reported a
  perfect 90. Fixed a first-frame crash when the driver's Smooth Motion was enabled.
- **Frame generation and the overlay:** fixed overlay text rendering as blank boxes under DLSS 4x MFG, and fixed
  screenshots and overlay-free recording picking the wrong frame while a game had DLSS FG temporarily suspended.
- **Forced V-Sync in Vulkan titles:** the layer's bridge functions were never actually exported, so the
  vertical-blank correction could never run. It is verified against the shipped DLL now.
- **32-bit games:** fixed every `[UE5]` console-variable override being silently inert in 32-bit Unreal titles,
  and a weakened sanity check on the legacy Direct3D 9 path.
- **Crash and freeze reporting:** dumps are written from a copy of the exception state instead of stack memory
  that may already have been reused; a second crash can no longer start a second dump worker on top of the first;
  the crash handler can no longer deadlock on itself; and the freeze watchdog no longer accuses Vulkan and
  DirectDraw games it never observed presenting.
- **Capture:** an injected frame that names a process other than the capture session's own is dropped instead of
  being opened.

## v0.1.6261

Changes since [v0.1.6143](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6143).

### New

- Added in-game benchmark recording, an overlay benchmark HUD, and interactive HTML reports. The `benchmark` hotkey
  (`CTRL+7` by default) starts, stops, or clears a run; `[Benchmark]` controls an optional start delay, a fixed run
  duration, and the output directory (empty means a `benchmarks` folder beside the executable). Reports use the same
  live frame metrics the overlay already collects.
- Added `[Overlay] frametime_source=display_change` (the new default): frame time, FPS, 1% lows, variance, graph
  samples, and stutter state can come from real screen-change timestamps instead of application presents, so
  generated frames and variable-refresh scanout are included. A dedicated timing service owns the tracing work and
  publishes a lock-free ring that each DXGI and Vulkan overlay consumes independently; the overlay falls back to
  presentation timing whenever the display stream is unavailable, denied, failed, or stale.
- Added NVIDIA scheduled-flip decoding for display-change timing. Deferred flip completions are corrected with the
  driver's scheduled screen-time announcement; the payload is decoded positionally and continuously revalidated so
  an unrecognised or moved field yields no correction instead of a wrong one. This removes the DLSS 4 MFG
  presentation sawtooth that made smooth generated output look stuttery, validated in Talos at MFG 1x/2x/3x/4x.
- Added injected-overlay PC latency. `PC Latency~` uses D3D/Vulkan Reflex/PCL frame markers plus measured display
  timing, while `Latency est.` provides a frame-cadence fallback without markers. Both account for dropped frames
  and frame-generation base cadence, fail closed below the heuristic's supported rate, and exclude
  peripherals/scanout.
- Added an opt-in USB/webcam face-camera overlay for WGC/DXGI and inject capture. Camera ingest is nonblocking and
  latest-frame-only; a one-draw D3D11 compositor provides configurable placement, size, crop, mirroring, opacity,
  rectangle/rounded/circle masks, borders, SDR/HDR mapping, and moving camera content on CFR repeated game frames.
- Added opt-in, stream-only YouTube, Twitch, and custom RTMP/RTMPS output. It reuses CaptureEngine's CFR/audio
  timing, selects a low-latency H.264/AAC compatibility profile on the configured hardware backend, redacts stream
  keys, and stops the session on bounded network/queue failure instead of sacrificing A/V synchronization.
- Added optional LibreHardwareMonitor polling for CPU/GPU temperature, package power, fan RPM, core clocks, and
  voltages in the existing overlay rows. The runtime files are now installed by the build from a pinned,
  digest-verified archive instead of being copied in by hand; the PowerShell bridge was replaced by a native CLR
  host, and an unresolvable hardware scope now fails loudly instead of publishing zeros.
- Added bundled PawnIO setup with integrity verification. When CPU sensors are requested and the kernel driver is
  missing, CaptureEngine offers a single elevated Windows Package Manager install prompt (or the project page);
  install and removal are elevated product commands (`--install-pawnio` / `--uninstall-pawnio`) rather than
  user-editable scripts beside the executable.
- Added log privacy filtering. Shared logs mask the Windows account name in user-profile paths and collapse
  user-configured capture/screenshot output paths to a root prefix plus leaf; game process names, PIDs, timestamps,
  and hardware model stay logged. CE's crash-dump content map and the deliberate no-redaction containment policy
  are documented.

### Improved

- Rebalanced `ray_reconstruction_optimal_settings` by cost into a strict `off|light|medium|high|full` ladder:
  `light` applies the reconstruction and pre-smoothing passes RR replaces, `medium` adds full-resolution reflection
  tracing plus every cost-free stabilizer and engine-default floor, `high` adds the paid sampling that is visibly
  worth it (virtual-shadow ray counts and local resolution, the screen-probe octahedron lattice, radiance-cache
  probe resolution), and `full` keeps the maximum screen-probe ray count, the radiance-cache probe budget, and
  full-resolution short-range AO on UE 5.6+ (`on` still aliases `full`).
  `r.Lumen.ScreenProbeGather.StochasticInterpolation` is now `1` at every level - the cheaper stochastic path and
  the signal a ray-reconstruction denoiser expects - and inserting the new level renumbered the shared-memory
  preset byte and moved `SHARED_MEMORY_VERSION` accordingly.
- Reduced CaptureEngine's cost and interference in frame-generation games. The command-queue detour no longer
  re-registers the FG runtime's own queues on every submission, overlay work stays off the present critical path,
  hidden-overlay GPU work is isolated, and CE now measures its own per-hook cost with forwarded runtime blocking
  subtracted. In the validated FG scene the overlay costs about 7 us of GPU time per output frame, and CE's own
  CPU in the hottest hooks is about 1.4% of one core.
- Made FSR frame-generation pacing less invasive and explainable: bounded pacing-episode traces capture
  automatically on health regressions or manually on demand, preserve context across trace boundaries, decompose
  displayed frames against the callback that produced them, and record the rate-loss table. CE no longer adopts
  the runtime's queue, keeps display telemetry alive while the overlay is hidden, and reduces contention in
  callback-owned pacing.
- Improved frame-generation capture and limiter robustness: DLSS frame-generated output captures smoothly, DLSS
  MFG capture clock drift and capture-phase liveness are fixed, inject CFR recovers after display phase shifts,
  Reflex limiter recovery and pacing are stable, and CFR encoder overload recovery is faster.
- Applied native Vulkan present timing and the FFX VSync intent before output scheduling. Forced FIFO now follows
  the swapchain's own presentation contract instead of CE adding a second rate limiter, `VK_NV_present_metering`
  is withheld where it would override FIFO, and CaptureEngine no longer force-overrides variable-refresh presents
  with fixed vertical-blank pacing.
- Made split-renderer setups work correctly: Vulkan profile inheritance and GPU telemetry attribution now follow
  the real renderer child instead of the host process.
- Made the tray and elevation flow more reliable: the context menu opens above the Windows taskbar, startup stays
  responsive under delayed shell startup, admin restart hands over cleanly, a second instance no longer collides
  with a running one, and PawnIO uninstallation tears down cleanly.
- Cleaned up diagnostics and background state: duplicate per-frame trace logging on the game render thread was
  removed (about 135 lines/s in Talos under FSR FG), and orphaned CE display-timing ETW sessions left by killed
  instances are reclaimed before they exhaust the machine-wide session budget and silently degrade display timing
  and PC latency to fallbacks.
- The overlay frame-time graph now scrolls by drawn frames instead of sample arrival, so it animates smoothly under
  frame generation instead of stepping like a lower frame rate; metric values themselves are unchanged.

### Fixed

- **Portal RTX (RTX Remix):** fixed `vsync_mode=fifo` under DLSS multi-frame generation. The layer withholds
  `VK_NV_present_metering`, propagates native FIFO before Streamline DLSS-G, corrects the final DXGI FIFO present,
  and fixes a FIFO present crash. A 143 Hz display now receives a paced FIFO stream rather than the metering-driven
  ~172 fps burst.
- **Portal RTX (RTX Remix):** fixed `general_limiter_mode=reflex` applying the frame-generation divisor twice: a
  130 fps cap with 3x DLSS MFG displayed about 43 fps. The driver-owned low-latency interval is now fed the final
  output rate.
- **Portal RTX (RTX Remix):** fixed an FPS-cap escape where generated callbacks were mistaken for new output groups,
  letting the game run at ~146-167 fps against a 130 fps cap. Output-group admission is now deterministic and
  ordinal instead of a time-window guess.
- **Portal RTX (RTX Remix):** fixed overlay flicker and a stale FG multiplier under 4x DLSS MFG by keeping the
  Vulkan overlay on one composite route, growing the submit ring when a group has no reusable slot, and mirroring
  the live DLSS-G state on every present; a stale semaphore/fence reuse bug that could re-signal a still-pending
  present was fixed as well.
- **Portal RTX (RTX Remix):** fixed clean exits writing a 191 MB pre-termination dump and spending 2.5 s in it; the
  fallback now recognises an application ending itself, while genuine crashes still dump.
- **RTX Remix:** fixed override and Vulkan pacing regressions, frame-generation scheduling, and late
  frame-generation control.
- **Talos:** fixed PC-latency estimation under FSR FG, which reported about 45 ms against 70 ms of real Reflex/PCL
  markers. CE now classifies the application's own Present inside AMD's proxy and measures the generator's real
  in-flight queue depth instead of assuming one frame; the anchored generator hold is reported as measured.
- **Talos:** fixed frame-time variance that flipped between runs of the same build. The overlay no longer treats a
  flip-latch timestamp as displayed frame time or mixes screen times with latch times: display timing is selected
  only for a stream that actually resolves screen times, and presentation timing is used otherwise.
- **Talos Reawakened:** fixed DLSS overrides being skipped after a stale `NvRemixBridge.exe` renderer claim from
  Portal RTX; renderer claims are now scoped to the client that published them.
- **Frame generation (all supported titles):** fixed blank gap lines across FG switching and keep-alive, FSR FG
  frame-pacing stutter on AMD's presentation queue, a swapchain COM reference leak and Streamline runtime state
  loss across FG mode switches, and DLSS MFG capture clock drift/liveness so recorded generated output stays
  smooth. DLSS final-output declarations are complete, and the overlay reports the live DLSS-G multiplier even
  when no override is configured.
- **PC-latency overlay:** fixed idle mislabeling and survival across FG switches, 4x-vs-2x and 2x-vs-3x/4x
  reporting inversions, transition outlier spikes, doubled latency after a DLSS FG -> FSR FG switch, fallback
  recovery over hitches and unconstrained present rates, and async frame-generation correlation. Streamline PCL
  reports are ignored while FSR FG is active.
- **Display timing / VRR:** fixed `msBetweenDisplayChange` and the overlay frame-time source on variable-refresh
  displays: unclocked flip-latch sawtooth is rejected, valid VRR completions are accepted, vsync-deferred
  completions are rounded onto the blank they reach the screen at, flat but partially unlabelled display streams
  are accepted, and an FSR-FG -> DLSS-FG regime change recovers within one measurement window instead of averaging
  the old stream in for several seconds.
- **Vulkan / DX12 integration:** fixed nested DXGI swapchain recovery and its timed retry amplification, DX12
  execution discovery replacing established same-device queues, late D3D12 bootstrap and injection startup
  perturbing FSR FG, a protected FFX startup latch escaping its swapchain, a proven-queue DLSS startup blank, and
  a recursion in the NGX proxy hook path.
- **Vulkan layer:** scoped Vulkan and DLSS state to the renderer process that owns it, fixed stale renderer claims
  silencing the next game's overrides, and fixed queue-loader data plus resume verification.
- **NVIDIA LOD-spread override:** fixed `nv_lod_spread_fix=on` becoming a silent no-op on 32-bit Vulkan/OpenGL
  titles under newer drivers. The branch is neutralized by zeroing its relative displacement instead of writing a
  two-byte NOP pair that can tear at that alignment; already-patched encodings are still recognised.
- **Sensors and shutdown:** fixed a face-camera teardown deadlock and stopped unreadable sensor values from being
  published as zeros.

## v0.1.6143

Changes since [v0.1.6142](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6142).

### New

- Added a Streamline 1.x-to-2.x upgrade bridge behind `streamline_upgrade=on`. This feature is still
  work-in-progress and currently non-functioning: it does not yet produce a working upgrade, so enabling it
  is not expected to restore Streamline features in a bridged game. The mechanism loads a complete
  user-supplied 2.x plugin set as a second, CE-owned runtime, repoints the game's `sl.interposer` import slots
  at it in memory (nothing on disk is renamed or patched), and translates the game's own 1.x Streamline calls
  onto the 2.x ABI - including measured 1.x feature-constant layouts, Reflex translation, and synthesized
  Reflex activation plus per-frame Reflex sleeps. Existing plumbing holds device continuity across adapter
  resets, reuses proven D3D12 devices, resolves adapters by fresh LUID, falls back safely for capability
  probes, serializes legacy teardown before upgrade, takes over NGX identity cleanly, and fixes tag lifetime
  and FG option deduplication.
- Expanded `[UE5]` overrides with `depth_of_field`, `dlss_super_resolution`, `dlss_super_resolution_quality`,
  `hdr_output`, `hdr_peak_luminance`, `hdr_paper_white`, `hdr_ui_luminance`, `hdr_min_luminance`, and
  `hdr_color_gamut`: `depth_of_field=off|on` writes `r.DepthOfFieldQuality`; `dlss_super_resolution=on` sets
  the NVIDIA plugin's `r.NGX.DLSS.Enable` plus the engine levers that route rendering through the third-party
  temporal upscaler (`r.NGX.Enable`, `r.TemporalAA.Upscaler`, `r.AntiAliasingMethod=2`), with quality selected
  through UE's screen percentage; the `hdr_*` settings drive `r.HDR.EnableHDROutput` and the
  `r.HDR.Display.*`/`r.HDR.UI.*` parameters in the nits and gamut the engine documents. None of them can add a
  missing plugin, invent HDR output on an SDR display, or create depth of field a game never configured.
- Expanded `ray_reconstruction_optimal_settings` into graduated `off|light|medium|full` presets: `light`
  applies four temporal/reconstruction settings (`r.SSR.Temporal=0`, `r.Lumen.Reflections.Temporal=0`,
  `r.Lumen.Reflections.BilateralFilter=0`, `r.Lumen.Reflections.ScreenSpaceReconstruction=0`), `medium` adds
  `r.Lumen.Reflections.DownsampleFactor=1`, and `full` adds the remaining former bundle values; the legacy
  `on` spelling remains an alias for `full`. Presets no longer enforce `r.NGX.DLSS.DenoiserMode=1` (select the
  RR denoiser explicitly via `force_ray_reconstruction=on`). Added `custom_cvar_overrides` /
  per-app `UE5.custom_cvar_overrides` for typed final-value overrides of individual UE5 CVars; valid entries
  take precedence over all presets and dedicated options.

### Improved

- Made overlay rendering cheap under DOOM Eternal's Vulkan "present from compute": overlay submits land on
  the game's own graphics queue instead of the compute present queue, the compute-present overlay hot path
  avoids redundant work, and CE diagnostics moved off the present critical path.
- Stopped CE from destabilizing Vulkan games structurally: no longer blocks the runtime's own presenter thread,
  no longer shrinks a swapchain below what a blocking acquire guarantees, and no longer takes stdout away
  from the game.
- Explained failing D3D12 device creation with its actual cause instead of repeating a bare HRESULT.

### Fixed

- Fixed UE5 CVar overrides and the Streamline DLSS-G override silently vanishing after a single overlay-toggle
  hotkey press: toggling republished the base config without the target profile's contributions.
- Fixed overzealous freeze detection: the watchdog armed on DX12-hook install but its heartbeat only moved on
  CE's D3D/DXGI present paths, so pure Vulkan titles were declared frozen exactly 30 s in and the in-process
  dump itself was the stall users saw; freeze claims now require live render-loop evidence across APIs, and
  dumps go through the external helper process.
- Fixed screenshots freezing games using Streamline (an infinite fence wait sat on the present path) and
  fixed overlay exclusion on screenshots not being applied there because the capture point ran after the
  PostSL overlay draw.
- Fixed Vulkan late injection producing no overlay: the implicit layer registration now stays resident so a
  title launched while CaptureEngine was closed still gets the layer, and layer discovery compatibility is
  judged by layout instead of an exact build-number match.
- Fixed hotkeys doing nothing while games like DOOM Eternal were foreground: such titles register their
  raw-input keyboard with `RIDEV_NOHOTKEYS`, suppressing `WM_HOTKEY` for everyone; CE now delivers recording,
  screenshot, and overlay hotkeys itself.
- Fixed the Vulkan layer failing device creation for applications it cannot attribute (Red Dead Redemption 2
  takes physical devices from `vkEnumeratePhysicalDeviceGroups`): device-group entry points feed the ownership
  map, resolution falls back through loader dispatch keys, and unresolvable instances forward the
  application's own `VkDeviceCreateInfo` instead of returning `VK_ERROR_INITIALIZATION_FAILED`, so dormant
  non-whitelisted installs no longer break device-group applications.
- Fixed Witcher 3 Remastered crashing under injection by adding Streamline 1.x hooking support: the API
  generation is established from generation-exclusive exports before any ABI-sensitive hook installs, so 1.x
  interposers get correct signatures (command-buffer-first `slEvaluateFeature`, enum-based `slSetTag`)
  instead of CE assuming the 2.x shapes.
- Fixed a potential game crash/freeze on close caused by the injected hook: process exit took the
  `DLL_PROCESS_DETACH` branch without ever requesting hook shutdown, so loader hooks kept resolving redirects
  into already-destroyed globals.
- Fixed a crash window where a transient d3d11.dll probe load committed CE to a full DX11 install and the
  module vanished mid-init: modules CE patches and calls are now pinned via `GetModuleHandleEx`.
- Fixed `STATUS_HEAP_CORRUPTION` on close with OptiScaler/Special K/ReShade/Steam overlay injected: the
  swapchain destructor's post-destruction refcount probe touched a chain whose last references it had just
  released.
- Fixed the inject overlay not using Windows' effective monitor DPI scale factor under some conditions:
  overlay geometry now scales from `GetDpiForMonitor(MDT_EFFECTIVE_DPI)` instead of a DPI-awareness-dependent
  window DPI.

## v0.1.6142

Changes since the last stable release [v0.1.5299](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.5299).

### New

- Added `[ThirdParty]` configuration to load ReShade, OptiScaler, and Special K from user-supplied DLL paths
  (`reshade_dll_path`, `optiscaler_dll_path`, `specialk_dll_path`) so the tools work without copying their DLLs into
  each game folder; all three can be active at once and load in the order Special K, ReShade, OptiScaler.
- Added persistent `[UE5]` overrides for injected x64 games: `force_ray_reconstruction`,
  `ray_reconstruction_optimal_settings`, `disable_post_processing_effects`, `tonemapper_sharpen`,
  `internal_fps_limit`, and `internal_anisotropic_filtering`. They redirect validated game-thread/render-thread
  CVar shadows in memory (never Engine.ini or game files), stay authoritative across map, scalability, and config
  reloads, and skip missing CVars from older UE/plugin builds safely. `internal_texture_mip_bias` shifts which
  mip level all 2D textures sample from via `r.MipMapLODBias`; `display_gamma` selects `r.TonemapperGamma`
  with sRGB/Rec709 or a pure power-curve exponent and is guarded so `r.HDR.Display.OutputDevice` is only
  written on SDR devices. Added "internal_texture_mip_bias" and "display_gamma" to the per-app profile example in the default config template.
- Added per-application `[ThirdParty]` override keys (`reshade_dll_path`, `optiscaler_dll_path`,
  `specialk_dll_path`) that take precedence over the global paths.

### Improved

- Made build and verification gates faster by isolating sanitizer Vulkan objects and overlapping packaging with lint.
- Packaged a default `testappconfig.ini` into the test-app archive folders.
- Reduced diagnostic log spam from the injected overlay and media pipeline with rate-limited, trace-level logging.
- Extended the UE5 console-registry scanner with data-pointer redirects so Lumen CVars resolve correctly in
  UE 5.6 and across UE versions; every proven element region is now covered, not just the first.
- Made the UE5 console-registry sweep resumable so large heap scans do not freeze partial results; absence
  conclusions now require a complete enumeration of all committed private RW regions.
- Prevented CE's DLL redirect from duplicating a loaded Streamline runtime instance: the hook-slot retarget
  now refuses to move a live forward pointer to a second mapped image, keeping each runtime's plugin set
  coherent.
- Decided Present-entry ownership from the loaded overlay module rather than a single byte sample, and applied
  the Streamline override all-or-nothing anchored on sl.common.
- Retried the deferred temp-swapchain Present-hook install via the guarded system-DXGI route on every service
  pass so late-injection installs do not stall when the device signal never arrives.
- Made the PostSL keep-alive submit attribute its draw to the enclosing present, preventing a false
  uncovered-present count during FG-toggle transitions.

### Fixed

- Fixed the injected overlay disappearing under FSR FG and DLSS FG, including during the DLSS toggle-ON startup
  window and after warm DLSS-FG resume; the overlay now stays visible across every FG-mode switch
  (off <-> FSR FG <-> DLSS FG) with stable topmost ownership through handoffs.
- Hardened the FG-switching matrix against blanks, lost overlay rendering, and crashes; fatal E_ACCESSDENIED switch
  failures now dump through the external helper process instead of freezing the game in-process for ~36 s.
- Fixed overlay coexistence with Steam, RTSS, and other overlays: CaptureEngine now intercepts Present below the
  foreign overlay chain, classifies the chain owner (Steam vs RTSS), validates foreign hook targets, and invokes
  RTSS's own Present handler directly instead of re-patching its callback slots.
- Fixed crashes and deadlocks around third-party tool loading: ReShade proxy-queue re-entry, the ReShade
  factory-proxy crash in the temp-swapchain install, swapchain wrapper base-reference over-release on game close, and
  the startup loader deadlock when Special K, ReShade, and OptiScaler load together (Special K now loads last, peer
  threads are suspended around tool loads, and loads run on the loader work queue).
- Fixed the pseudo-overlay font and circle scale to track the anchor monitor's DPI instead of the
  DPI-awareness-dependent window DPI, so text no longer resizes when the foreground app's DPI awareness changes.
- Fixed a DX12 startup crash in which the third-party-overlay Present-hook deferral was never made real: the
  deferred temp-swapchain Present hook now waits for the game's own D3D12 device and installs inside the same
  startup window, resolving the intermittent Steam-overlay recursion and the CaptureEngine access violation.
- Fixed FSR heuristic FG: an authoritative `ffxConfigure` OFF now vetoes the heuristic, preventing it from
  composing into AMD's UI resource after frame generation was explicitly disabled.
- Stopped re-attempting console-registry CVars whose layout CaptureEngine cannot drive, avoiding repeated failed
  writes and scan retries.
- Fixed frame timing under native FSR FG by ticking from the FFX present callback and submitting the overlay on the
  queue the FG runtime actually flushes.
- Fixed DLSS FG integration: the overlay no longer uses a dedicated overlay queue for NVIDIA DLSS FG, late-inject
  overlay submits route to the swapchain-owning queue, the FG multiplier is reported from MultiFrameCount, the
  Streamline multiplier stays latched across CreateFeature, and already-loaded DLSS-G/Reflex exports resolve at late
  injection.
- Fixed CodeQL-flagged format-argument and multiplication-overflow defects in overlay and device code.
- Fixed GCC compilation of the FFX hook header by ordering the `template` keyword before `inline`.

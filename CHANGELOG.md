# Changelog

## Unreleased

Changes since [v0.1.6772](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.6772).

### New

- **Overlay on 8-bit and 24-bit DirectDraw fullscreen (classic games):** the CPU overlay composite only wrote 32/16-bit surfaces, so classic 8bpp fullscreen (the dominant late-90s DirectDraw mode) and 24-bit primaries got zero overlay pixels and a "cannot write a %u-bit presented surface" log every frame. Both are composited now (8-bit through the game's live palette); YUV formats stay rejected.

### Fixed

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

### Improved

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

# llm-wiki Log Archive

### 2026-11-11 - First sealed display trace: GTA's cold DLSS-G black screen stops in DWM

- `20261111_021555` (0.1.7503) sealed `display_stall_7008_ep1_ended.etl` (1.42 s silence, 157 presents, nothing lost).
  The new DLSS-G chain presented 153 frames to DWM, which confirmed and discarded each without binding the chain's
  composition surface; the window's old surface stayed in independent flip on MPO plane 1 (destroyed with
  `KeepExistingContent=1`), no plane flipped at all. Another process's ~1 s screen capture forced the realization at
  1388 ms; DWM then left the old surface's independent flip and composed the new frame. Evidence and method:
  `display-stall-trace.md` ("First live seal"). CE's share is unproven until a passive or CE-off comparison.
- The ~1 s screen reader is most likely TrafficMonitor's per-second GDI taskbar color probe. That it ended the black
  screen was refuted by the second seal (entry above).

### 2026-11-11 - GTA 0.1.7501: ownership gate held, cold DLSS-G start still black for 1.7 s

- Session `20261111_013758` (DLSS FG on at load, then FSR <-> DLSS <-> off many times, 8 Streamline handoffs): no
  crash, no device removal, no `[PostSL OWNERSHIP]` refusal. `[FG OWNERSHIP] slDLSSGGetState(ON) starts no DLSS FG`
  fired on FidelityFX chains in protected disabled startup (`held by=ffx-swapchain lifetime=4/6/10/13`), first at
  01:40:41.192, 853 ms after the takeover, 32 calls - the 0.1.7500 crash precursor (that one published DLSS FG 1.1 s
  after the takeover), now refused.
- Black screen: only the first, cold DLSS-G handoff. FG was enabled through SetOptions 114 ms after the create, before
  the chain's first present (255 ms); first screen change after 1.69 s and 156 S_OK presents, recovered via composed
  frames (`[SCANOUT STALL] ep=2 ... firstChange=composed`). The 7 later handoffs, where FG came on 1.5-2 s after the
  first screen change, showed the screen after 0.20-0.25 s (11-16 presents). The user hears coil whine during the
  black phase: frames are rendered and presented, scanout never gets them. Same signature as the 37 s in
  `20261110_222057`; still unattributed (no CE-off comparison, no ETL).
- The recorder ran but sealed nothing: the expiry monitor started 1.5 s into the silence and counted 531 ms.
- `20261111_015744` (0.1.7502): the same cold start, black for 1.37 s (121 presents, ended by the game's own flip, the
  game again toggled Reflex off and DLSS-G off/on in the first frames). The expiry monitor never started, so even the
  corrected expiry trigger sealed nothing. The trigger now measures the silence directly from presents and published
  screen changes, as the hook's `[SCANOUT STALL]` does, with a 1 s threshold (`display-stall-trace.md`).

### 2026-11-11 - Raw display trace sealed on no-display episodes (pending hardware)

- For the GTA save-load stall (37 s of presents, nothing on screen) the user asked for evidence without external tools.
  The elevation service now keeps a circular raw ETL of DXGI/DxgKrnl/DWM/Win32k-token/NVIDIA events with trace logging
  on; the sensor seals it into the session folder when an expiry episode shows <= 2 composed frames/s (once at 10 s,
  at the end if >= 1.5 s). Read with `tools/tracing/etl_stall_digest.py`. Details and open questions:
  `display-stall-trace.md`. Unit tests (policy, sealer, protocol, path validation) pass; no live seal yet.

### 2026-11-11 - GTA late DLSS -> FSR device removal: stale GetState took FidelityFX's chain

- Session `20261110_222057` (0.1.7500): the unload-order fix worked (forwarded at proxy release, FFX create ok,
  same COM address). At 22:24:34.526 a `slDLSSGGetState(mode=on)` on the title's thread (its stale options) passed the
  lapsed 250 ms suppression and `safeBootstrap`, published DLSS FG, retired the protected FFX startup and armed PostSL;
  the first PostSL submit (`falling back to scQueue` = the DLSS-G-era queue) on FFX's chain removed the device
  (`0x887A002B`) right after logging `PostSL CONFIRMED`. Dumps: `ERR_GFX_STATE` dialog, then GTA's `int 3`.
- Fix: presenter ownership in `ce::fg_presenter` gates every DLSS-G signal, the protected-FFX retirement and PostSL
  admission; device-removed submits never confirm (see `frame-generation/guardrails.md`, "Presenter ownership").
  WARP repro failed before and passes after; all 236 flow scenarios and the unit suite pass. Hardware retest pending.

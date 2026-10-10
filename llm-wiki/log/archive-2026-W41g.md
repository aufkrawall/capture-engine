# llm-wiki Log Archive - 2026 W41g

### 2026-10-09 - Automatic dump missed a CE fault consumed by game handling

- The same Witcher 3 startup session retained one unresolved CE access violation in its first-chance slot, but
  had no CE crash dump or crash log and ultimately exited with code zero. The manual dump's render stack had
  already returned to game code. Record-only first-chance handling relied on an unhandled filter or crash exit;
  those never arrived. Background freeze suppression also cannot substitute for preserving the original fault.
- The classifier now captures undebugged hardware faults whose instruction address is inside the module hosting
  CE's crash handler. `crash_first_chance::Install` caches that immutable image range before VEH registration;
  classification performs only atomic reads and range arithmetic. Foreign faults, breakpoints, managed and C++
  exceptions retain their existing behavior. The fault is also recorded for retry if immediate capture fails.
- The external-helper routing regression fails before the fix (zero calls), then passes with exactly one call and
  the original thread/address/registers. Module-identity and policy tests cover foreign/null/noncanonical
  addresses, debugger ownership and ignored software exceptions. Full build 0.1.7056 passes native tests,
  Python self-tests and all 34 FG flows and produces a fresh 39057612-byte installer. Real-game retesting remains pending.

### 2026-10-09 - Witcher 3 DX12/ReShade startup renderer type confusion

- Session 20261009_093652 (0.1.7053): second Present switches between a ReShade device view and the native view,
  causing descriptor-free backend replacement. The manual dump has no exception stream, but CE's first-chance
  slot retains the render thread's access violation in `OverlayAdapter::DestroyResourcesLocked`, inlined
  `DX12Backend::HasInlineUploadsInFlight`: it reads a noncanonical glyph-data pointer as a completion buffer.
  x64 CDB verified the archived CE PDB GUID/age and loaded matching Microsoft ntdll symbols.
- `DX12DescFreeBackend` derives from `RendererBackend`, not `DX12Backend`. The shared DX12 label was used as
  proof for five unsafe casts, including retirement. Adapter binding now caches an explicitly advertised texture
  interface; descriptor-free resources and generic virtual upload-slot dispatch remain intact.
- A native custom-renderer regression fails before the fix because texture helper calls overwrite unrelated
  storage. Added poisoned-storage shutdown/rebind checks and real WARP texture/custom lifecycle coverage to the
  descriptor-free format probe. Closing build 0.1.7056 passes the full native suite, Python tool self-tests and all
  34 FG scenarios; fresh installer is 39057612 bytes. The real-game cold-start check after installation remains pending.

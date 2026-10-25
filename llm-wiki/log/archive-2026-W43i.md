# llm-wiki Log Archive

### 2026-10-24 - OptiScaler FFX inputs bypass export observation; front input calls retire the live FSR overlay

- Sessions `20261023_235619` (protected startup never observes enabled FFX configure) and `20261023_235838`
  (`EnableFfxInputs=false`, native calls observed but repeated DLSS-G inputs retire/recreate the FSR overlay).
- Root cause and contracts: `frame-generation/xefg.md`, section "OptiScaler EnableFfxInputs=true". Native body
  observers preserve the foreign input detours and saved originals; front signals no longer request FSR teardown
  or DLSS UI activation. Hardware retest pending; flow and prologue regressions accompany the change.

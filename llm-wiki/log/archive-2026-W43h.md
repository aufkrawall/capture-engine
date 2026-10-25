# llm-wiki Log Archive

### 2026-10-23 - OptiScaler FSRFG: no overlay, then device removed; DLSS-G wrapper lost its overlay (session 20261023_221541)

- Run 1 (OptiScaler over real DLSS-G 3x): today's provider-only front rule kept DLSS FG off although `sl.dlss_g`
  originated every present; fixed by requiring that Streamline presents nothing. Run 2 (`FGOutput=FSRFG`): CE hooked
  the engine's FidelityFX image, OptiScaler ran its own same-named copy unseen (no overlay, protected startup 4 s);
  the game's DLSS-G enable that OptiScaler answered retired the FFX startup and CE drew FFX's chain from the game
  queue: `0x887A002B`, Unreal exit 3, no dump.
- Fixes and evidence: `frame-generation/xefg.md` ("OptiScaler FGOutput=FSRFG and two FidelityFX images"). Flow:
  `FlowFSRToolRuntime`, `FlowDLSSGApiTool`; unit: `RuntimePresentedSwapchainPolicyTest`, `FGPresenterPolicyTest`.
  Hardware rerun of both OptiScaler configurations pending.

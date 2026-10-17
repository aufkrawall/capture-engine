#pragma once

// ID3D11DeviceContext(1) vtable indices CE's raw context hooks patch. One table, so
// the install code and the layout test (tests/test_d3d11_context_slots.cpp, which
// checks each against the SDK's C vtable) cannot disagree.

namespace ce::d3d11_slots {

inline constexpr unsigned kPSSetShaderResources = 8;
inline constexpr unsigned kPSSetShader = 9;
inline constexpr unsigned kPSSetSamplers = 10;
inline constexpr unsigned kDrawIndexed = 12;
inline constexpr unsigned kDraw = 13;
inline constexpr unsigned kDrawIndexedInstanced = 20;
inline constexpr unsigned kDrawInstanced = 21;
inline constexpr unsigned kDrawAuto = 38;
inline constexpr unsigned kDrawIndexedInstancedIndirect = 39;
inline constexpr unsigned kDrawInstancedIndirect = 40;
inline constexpr unsigned kExecuteCommandList = 58;
inline constexpr unsigned kPSGetSamplers = 75;
inline constexpr unsigned kClearState = 110;
inline constexpr unsigned kFinishCommandList = 114;
// ID3D11DeviceContext1 only: install after the context answers that QueryInterface.
inline constexpr unsigned kSwapDeviceContextState = 131;

}  // namespace ce::d3d11_slots

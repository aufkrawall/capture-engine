// A stand-in for a translation runtime's frontend, built as d3d9.dll under dxup/ and reshade/ with
// only its version resource differing. CE knows DXUP by that resource, never by the file name, which
// ReShade, dgVoodoo and Special K share (FlowDX11OverlayApiLabel).

extern "C" __declspec(dllexport) int CEFake_TranslatorFrontendStandIn() {
    return 1;
}

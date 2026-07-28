    // Stub implementation when WGC headers not available
    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dContext_ = nullptr;

    bool CreateWinRTDevice() {
        return false;
    }
    bool CreateForMonitor(void*) {
        return false;
    }
    bool CreateForWindow(void*) {
        return false;
    }
    bool StartCapture(uint32_t&, uint32_t&, bool) {
        return false;
    }
    void StopCapture() {}
    bool GetNextFrame(WGCCapturedFrame&) {
        return false;
    }
    bool GetCaptureOrigin(int32_t&, int32_t&) const {
        return false;
    }

        }
    }

    // VRAM Total still via DXGI
    // If VRAM Total was set explicitly (e.g. by hook on main thread), SKIP this
    // risky background creation!
    UpdateVRAMTotal();

    // Apply the computed GPU/VRAM values (short lock, no PDH/DXGI inside the
    // lock)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (newGpuUsageValid) {
            current.gpuUsage = newGpuUsage;
            current.gpuUsageValid = true;
        }
        if (haveVramUsed) {
            current.vramUsed = newVramUsed;
            current.vramUsageValid = true;
        }
    }
}

void SystemMetricsCollector::UpdateVRAMTotal() {
    static bool loggedEntry = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!loggedEntry) {
            EarlyLog("UpdateVRAMTotal: Entry. current.vramTotal=%llu, LUID=%08x:%08x", current.vramTotal,
                     adapterLuid.HighPart, adapterLuid.LowPart);
            loggedEntry = true;
        }

        if (current.vramTotal > 0) {
            return;  // Already have it.
        }
    }

    if (adapterLuid.LowPart == 0 && adapterLuid.HighPart == 0)
        return;
    if (!cachedFactory)
        CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&cachedFactory);
    if (cachedFactory && !cachedAdapter) {
        IDXGIFactory4* pFactory = (IDXGIFactory4*)cachedFactory;
        IDXGIAdapter1* tempAdapter = nullptr;
        static bool s_LoggedEnum = false;

        // Log once per detection attempt
        if (adapterLuid.LowPart != 0 || adapterLuid.HighPart != 0) {
            if (!s_LoggedEnum)
                EarlyLog("UpdateGPU: Searching for LUID %08x:%08x", adapterLuid.HighPart, adapterLuid.LowPart);
        }

        for (UINT i = 0; pFactory->EnumAdapters1(i, &tempAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            tempAdapter->GetDesc1(&desc);

            if (!s_LoggedEnum) {
                EarlyLog("UpdateGPU: Checking Adapter %d: LUID %08x:%08x", i, desc.AdapterLuid.HighPart,
                         desc.AdapterLuid.LowPart);
            }

            if (desc.AdapterLuid.LowPart == adapterLuid.LowPart && desc.AdapterLuid.HighPart == adapterLuid.HighPart) {
                tempAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&cachedAdapter);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    current.vramTotal = desc.DedicatedVideoMemory;
                    EarlyLog("UpdateGPU: Found match! VRAM Total: %llu bytes", desc.DedicatedVideoMemory);
                }
                s_LoggedEnum = true;
                tempAdapter->Release();
                break;
            }
            tempAdapter->Release();
        }
    }
}

SystemMetrics SystemMetricsCollector::GetMetrics() {
    std::lock_guard<std::mutex> lock(mutex);
    return current;
}

// Helper to shorten names
static std::string CleanHardwareName(std::string raw, bool isGpu) {
    // 1. Common cleanup
    std::string s = raw;
    // Replace (R), (TM), @ with spaces
    for (char& c : s) {
        if (c == '(' || c == ')' || c == '@' || c == ',')
            c = ' ';
    }

    // 2. Tokenize and Filter
    std::vector<std::string> tokens;
    std::string token;
    for (char c : s) {
        if (isspace((unsigned char)c)) {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }
    if (!token.empty())
        tokens.push_back(token);

    std::string result = "";

    if (isGpu) {
        // GPU Strategy: Look for specific start anchors or valid tokens
        // Check for specific series markers first
        bool foundAnchor = false;
        const char* anchors[] = {"RTX", "GTX", "RX", "Arc", "Titan", "Quadro"};

        for (const auto& t : tokens) {
            std::string upperT = t;
            // ToUpper
            for (auto& c : upperT)
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                c = toupper((unsigned char)c);

            // Check anchors
            for (const char* anchor : anchors) {
                if (upperT.find(anchor) == 0)
                    foundAnchor = true;
            }

            // Filter out brands/garbage
            bool skip = false;
            const char* blocklist[] = {"NVIDIA",      "AMD", "INTEL", "GEFORCE", "RADEON", "GRAPHICS",
                                       "CORPORATION", "CO.", "LTD",   "TM",      "R",      "ADAPTER"};
            for (const char* block : blocklist) {
                if (upperT == block)
                    skip = true;
            }

            // Heuristic usage
            if (foundAnchor) {
                // Once anchor found, keep everything (e.g. RTX 4090 Ti)
                if (!skip) {
                    if (!result.empty())
                        result += " ";
                    result += t;
                }
            } else {
                // While searching for anchor, verify if this token IS the anchor logic?
                // If the token IS "RTX" or similar, we start keeping.
                // Wait, logic above sets foundAnchor if token matches.
                // So if this token IS the anchor, foundAnchor becomes true NOW.
                // But we need to add THIS token too.
                // Re-check anchor logic
                for (const char* anchor : anchors) {
                    if (upperT.find(anchor) == 0) {
                        foundAnchor = true;
                    }
                }

                if (foundAnchor) {
                    if (!skip) {
                        if (!result.empty())
                            result += " ";
                        result += t;
                    }
                }
            }
        }
        // If no anchor found (e.g. "Intel Graphics"), just show filtered tokens
        if (result.empty()) {
            for (const auto& t : tokens) {
                std::string upperT = t;
                for (auto& c : upperT)
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    c = toupper((unsigned char)c);

                bool skip = false;
                const char* blocklist[] = {"NVIDIA",      "AMD", "INTEL", "GEFORCE", "RADEON", "GRAPHICS",
                                           "CORPORATION", "CO.", "LTD",   "TM",      "R"};
                for (const char* block : blocklist) {
                    if (upperT == block)
                        skip = true;
                }

                if (!skip) {
                    if (!result.empty())
                        result += " ";
                    result += t;
                }
            }
        }

    } else {
        // CPU Strategy: Filter noise, keep model
        for (const auto& t : tokens) {
            std::string upperT = t;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            for (auto& c : upperT)
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                c = toupper((unsigned char)c);

            bool skip = false;

            // Blocklist
            const char* blocklist[] = {"AMD",  "INTEL", "RYZEN",   "CORE",      "PROCESSOR", "CPU",     "APU",
                                       "SOC",  "GEN",   "COMPUTE", "MICROSOFT", "DUAL",      "QUAD",    "HEXA",
                                       "OCTA", "EIGHT", "SIX",     "TEN",       "TWELVE",    "SIXTEEN", "TWENTY",
                                       "R",    "TM",    "CO.",     "LTD."};
            for (const char* block : blocklist) {
                if (upperT == block)
                    skip = true;
            }

            // Suffix check
            if (upperT.size() > 5 && upperT.substr(upperT.size() - 5) == "-CORE")
                skip = true;
            if (upperT.size() > 7 && upperT.substr(upperT.size() - 7) == "-THREAD")
                skip = true;
            if (upperT.find("GHZ") != std::string::npos)
                skip = true;
            if (upperT.find("MHZ") != std::string::npos)
                skip = true;

            // Length check (Skip "7", "5", "i9" if split?)
            // User wants "5700X". "i9-13900K".
            // "i9" is 2 chars. "7" is 1 char.
            if (t.size() < 2)
                skip = true;
            // Also skip "i3", "i5", "i7", "i9" if they are standalone?
            // "i9-13900K" is kept (>=2).
            // "Core i9 13900K" -> "i9 13900K".
            // If I skip "i9", I get "13900K". Cleaner.
            if (upperT == "I3" || upperT == "I5" || upperT == "I7" || upperT == "I9")
                skip = true;

            if (!skip) {
                if (!result.empty())
                    result += " ";
                result += t;
            }
        }
    }

    // Fallback if empty (e.g. logic stripped everything)
    if (result.empty())
        return raw;
    return result;
}

void SystemMetricsCollector::DetectHardwareNames() {
    // 1. CPU Name via Registry
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) ==
        ERROR_SUCCESS) {
        char buf[256] = {0};
        DWORD dataSize = sizeof(buf);
        if (RegQueryValueExA(hKey, "ProcessorNameString", 0, 0, (LPBYTE)buf, &dataSize) == ERROR_SUCCESS) {
            cachedCpuName = CleanHardwareName(buf, false);
        }
        RegCloseKey(hKey);
    }

    // 2. GPU Name via DXGI
    // We reuse cachedFactory if available or create new one
    if (adapterLuid.LowPart != 0 || adapterLuid.HighPart != 0) {
        IDXGIFactory4* pFactory = nullptr;
        bool localFactory = false;

        if (cachedFactory) {
            pFactory = (IDXGIFactory4*)cachedFactory;
        } else {
            CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&pFactory);
            localFactory = true;
        }

        if (pFactory) {
            IDXGIAdapter1* tempAdapter = nullptr;
            for (UINT i = 0; pFactory->EnumAdapters1(i, &tempAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc;
                tempAdapter->GetDesc1(&desc);
                if (desc.AdapterLuid.LowPart == adapterLuid.LowPart &&
                    desc.AdapterLuid.HighPart == adapterLuid.HighPart) {
                    char mbBuf[128];
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, mbBuf, sizeof(mbBuf), NULL, NULL);
                    cachedGpuName = CleanHardwareName(mbBuf, true);
                    tempAdapter->Release();
                    break;
                }
                tempAdapter->Release();
            }
            if (localFactory)
                pFactory->Release();
        }
    }
    EarlyLog("Hardware Detected: CPU='%s', GPU='%s'", cachedCpuName.c_str(), cachedGpuName.c_str());
}

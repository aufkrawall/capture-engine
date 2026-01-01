#include <windows.h>
#include <stdio.h>
#include <stdint.h>

typedef LONG NTSTATUS;

struct D3DKMT_OPENADAPTERFROMLUID {
    LUID AdapterLuid;
    uint32_t hAdapter;
};

struct D3DKMT_QUERYSTATISTICS {
    uint32_t Type;
    LUID AdapterLuid;
    uint32_t hAdapter;
    uint32_t QueryType;
    char Reserved[256];
    union {
        struct {
            uint64_t RunningTime;
            char Padding[248];
        } AdapterInfo;
    } Result;
};

typedef NTSTATUS (WINAPI *PFN_D3DKMTOpenAdapterFromLuid)(D3DKMT_OPENADAPTERFROMLUID*);
typedef NTSTATUS (WINAPI *PFN_D3DKMTQueryStatistics)(D3DKMT_QUERYSTATISTICS*);

int main() {
    // Get primary adapter LUID from DXGI
    printf("Testing D3DKMT GPU usage query...\n");
    
    HMODULE gdi32 = LoadLibraryA("gdi32.dll");
    if (!gdi32) {
        printf("Failed to load gdi32.dll\n");
        return 1;
    }
    
    auto D3DKMTOpenAdapterFromLuid = (PFN_D3DKMTOpenAdapterFromLuid)GetProcAddress(gdi32, "D3DKMTOpenAdapterFromLuid");
    auto D3DKMTQueryStatistics = (PFN_D3DKMTQueryStatistics)GetProcAddress(gdi32, "D3DKMTQueryStatistics");
    
    if (!D3DKMTOpenAdapterFromLuid || !D3DKMTQueryStatistics) {
        printf("Failed to get D3DKMT functions\n");
        return 1;
    }
    
    // For testing, use LUID {0, 0} or get from DXGI
    LUID testLuid = {0, 0};
    
    D3DKMT_OPENADAPTERFROMLUID openArgs = {};
    openArgs.AdapterLuid = testLuid;
    
    NTSTATUS status = D3DKMTOpenAdapterFromLuid(&openArgs);
    printf("D3DKMTOpenAdapterFromLuid status: 0x%08X, hAdapter: %u\n", status, openArgs.hAdapter);
    
    if (status != 0) {
        printf("Failed to open adapter\n");
        return 1;
    }
    
    // Query statistics
    D3DKMT_QUERYSTATISTICS queryStats = {};
    queryStats.Type = 1; // Adapter
    queryStats.AdapterLuid = testLuid;
    queryStats.hAdapter = openArgs.hAdapter;
    
    status = D3DKMTQueryStatistics(&queryStats);
    printf("D3DKMTQueryStatistics status: 0x%08X\n", status);
    printf("RunningTime: %llu\n", queryStats.Result.AdapterInfo.RunningTime);
    
    return 0;
}

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstdio>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

#pragma comment(lib, "pdh.lib")

void VerifyPdH(const char* targetLuid) {
    PDH_HQUERY gpuQuery, vramQuery;
    PdhOpenQueryA(NULL, 0, &gpuQuery);
    PdhOpenQueryA(NULL, 0, &vramQuery);

    PDH_HCOUNTER gpuCounter, vramCounter;
    PdhAddEnglishCounterA(gpuQuery, "\\GPU Engine(*)\\Utilization Percentage", 0, &gpuCounter);
    PdhAddEnglishCounterA(vramQuery, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &vramCounter);

    printf("Verifying with LUID filter: %s\n", targetLuid);
    printf("Collecting 5 samples...\n");

    for (int s = 0; s < 5; s++) {
        PdhCollectQueryData(gpuQuery);
        PdhCollectQueryData(vramQuery);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        
        PdhCollectQueryData(gpuQuery);
        PdhCollectQueryData(vramQuery);

        // GPU Load
        DWORD gpuBufSize = 0, gpuItemCount = 0;
        PdhGetFormattedCounterArrayA(gpuCounter, PDH_FMT_DOUBLE, &gpuBufSize, &gpuItemCount, NULL);
        std::vector<char> gpuBuffer(gpuBufSize);
        PDH_FMT_COUNTERVALUE_ITEM_A* gpuItems = (PDH_FMT_COUNTERVALUE_ITEM_A*)gpuBuffer.data();
        
        double sum3D = 0, sumCompute = 0, sumVideo = 0, sumVR = 0;
        if (PdhGetFormattedCounterArrayA(gpuCounter, PDH_FMT_DOUBLE, &gpuBufSize, &gpuItemCount, gpuItems) == ERROR_SUCCESS) {
            for (DWORD i = 0; i < gpuItemCount; i++) {
                const char* instance = gpuItems[i].szName;
                if (instance && (strstr(instance, targetLuid) || strstr(instance, "luid_") == nullptr)) {
                    double val = gpuItems[i].FmtValue.doubleValue;
                    if (strstr(instance, "engtype_3D")) sum3D += val;
                    else if (strstr(instance, "engtype_Compute")) sumCompute += val;
                    else if (strstr(instance, "VideoDecode") || strstr(instance, "VideoEncode")) sumVideo += val;
                    else if (strstr(instance, "engtype_VR")) sumVR += val;
                }
            }
        }
        double totalGpuLoad = std::max({sum3D, sumCompute, sumVideo, sumVR});
        if (totalGpuLoad > 100.0) totalGpuLoad = 100.0;

        // VRAM
        DWORD vramBufSize = 0, vramItemCount = 0;
        PdhGetFormattedCounterArrayA(vramCounter, PDH_FMT_DOUBLE, &vramBufSize, &vramItemCount, NULL);
        std::vector<char> vramBuffer(vramBufSize);
        PDH_FMT_COUNTERVALUE_ITEM_A* vramItems = (PDH_FMT_COUNTERVALUE_ITEM_A*)vramBuffer.data();
        
        double vramUsed = 0;
        if (PdhGetFormattedCounterArrayA(vramCounter, PDH_FMT_DOUBLE, &vramBufSize, &vramItemCount, vramItems) == ERROR_SUCCESS) {
            for (DWORD i = 0; i < vramItemCount; i++) {
                if (vramItems[i].szName && strstr(vramItems[i].szName, targetLuid)) {
                    vramUsed = vramItems[i].FmtValue.doubleValue;
                    break;
                }
            }
        }

        static double smoothedGpu = 0, smoothedVram = 0;
        smoothedGpu = smoothedGpu * 0.35 + totalGpuLoad * 0.65;
        smoothedVram = smoothedVram * 0.6 + vramUsed * 0.4;

        printf("Sample %d: GPU Load = %.2f%% (Smoothed: %.2f%%), VRAM Used = %.2f MiB (Smoothed: %.2f MiB)\n", 
               s+1, totalGpuLoad, smoothedGpu, vramUsed / (1024*1024), smoothedVram / (1024*1024));
    }

    PdhCloseQuery(gpuQuery);
    PdhCloseQuery(vramQuery);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: verify_pdh_v2.exe <LUID_PART>\n");
        printf("Example: verify_pdh_v2.exe luid_0x00000000_0x0000C73A\n");
        return 1;
    }
    VerifyPdH(argv[1]);
    return 0;
}

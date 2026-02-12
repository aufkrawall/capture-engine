#include <chrono>
#include <cstdio>
#include <pdh.h>
#include <pdhmsg.h>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#pragma comment(lib, "pdh.lib")

void MonitorGpu(const char *pattern) {
  PDH_HQUERY query;
  if (PdhOpenQueryA(NULL, 0, &query) != ERROR_SUCCESS)
    return;

  PDH_HCOUNTER gpuCounter, vramCounter;
  char gpuPath[256], vramPath[256];
  snprintf(gpuPath, sizeof(gpuPath), "\\GPU Engine(%s)\\Utilization Percentage",
           pattern);
  snprintf(vramPath, sizeof(vramPath),
           "\\GPU Adapter Memory(%s)\\Dedicated Usage", pattern);

  PdhAddEnglishCounterA(query, gpuPath, 0, &gpuCounter);
  PdhAddEnglishCounterA(query, vramPath, 0, &vramCounter);

  printf("Monitoring... Pattern: %s\n", pattern);
  printf("%-5s | %-10s | %-10s | %-10s\n", "Sec", "Sum GPU %", "Max GPU %",
         "VRAM MiB");

  std::vector<char> gpuBuf, vramBuf;

  for (int s = 0; s < 20; s++) {
    PdhCollectQueryData(query);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    PdhCollectQueryData(query);

    double sumGpu = 0;
    double maxGpu = 0;
    double totalVram = 0;

    DWORD bufSize = 0, itemCount = 0;
    PdhGetFormattedCounterArrayA(gpuCounter, PDH_FMT_DOUBLE, &bufSize,
                                 &itemCount, NULL);
    if (bufSize > 0) {
      gpuBuf.resize(bufSize);
      PDH_FMT_COUNTERVALUE_ITEM_A *items =
          (PDH_FMT_COUNTERVALUE_ITEM_A *)gpuBuf.data();
      if (PdhGetFormattedCounterArrayA(gpuCounter, PDH_FMT_DOUBLE, &bufSize,
                                       &itemCount, items) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < itemCount; i++) {
          if (items[i].FmtValue.CStatus == ERROR_SUCCESS) {
            const char *name = items[i].szName;
            if (strstr(name, "engtype_3D") || strstr(name, "engtype_Compute") ||
                strstr(name, "VideoEncode") || strstr(name, "VideoDecode")) {
              double val = items[i].FmtValue.doubleValue;
              sumGpu += val;
              if (val > maxGpu)
                maxGpu = val;
            }
          }
        }
      }
    }

    bufSize = 0;
    itemCount = 0;
    PdhGetFormattedCounterArrayA(vramCounter, PDH_FMT_DOUBLE, &bufSize,
                                 &itemCount, NULL);
    if (bufSize > 0) {
      vramBuf.resize(bufSize);
      PDH_FMT_COUNTERVALUE_ITEM_A *items =
          (PDH_FMT_COUNTERVALUE_ITEM_A *)vramBuf.data();
      if (PdhGetFormattedCounterArrayA(vramCounter, PDH_FMT_DOUBLE, &bufSize,
                                       &itemCount, items) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < itemCount; i++) {
          if (items[i].FmtValue.CStatus == ERROR_SUCCESS) {
            totalVram += items[i].FmtValue.doubleValue;
          }
        }
      }
    }

    printf("%-5d | %-10.2f | %-10.2f | %-10.2f\n", s, sumGpu, maxGpu,
           totalVram / (1024 * 1024));
  }
  PdhCloseQuery(query);
}

int main(int argc, char **argv) {
  const char *pattern = (argc > 1) ? argv[1] : "*";
  MonitorGpu(pattern);
  return 0;
}

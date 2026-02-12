#include <map>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <windows.h>

#pragma comment(lib, "pdh.lib")

void PrintBruteForce(const char *filter) {
  PDH_HQUERY query;
  if (PdhOpenQueryA(NULL, 0, &query) != ERROR_SUCCESS)
    return;

  PDH_HCOUNTER counter;
  if (PdhAddEnglishCounterA(query, "\\GPU Engine(*)\\Utilization Percentage", 0,
                            &counter) != ERROR_SUCCESS) {
    PdhCloseQuery(query);
    return;
  }

  PdhCollectQueryData(query);
  Sleep(500); // Longer interval for more stable average
  PdhCollectQueryData(query);

  DWORD bufSize = 0, itemCount = 0;
  PdhGetFormattedCounterArrayA(counter, PDH_FMT_DOUBLE, &bufSize, &itemCount,
                               NULL);

  if (bufSize > 0) {
    std::vector<char> buffer(bufSize);
    PDH_FMT_COUNTERVALUE_ITEM_A *items =
        (PDH_FMT_COUNTERVALUE_ITEM_A *)buffer.data();
    if (PdhGetFormattedCounterArrayA(counter, PDH_FMT_DOUBLE, &bufSize,
                                     &itemCount, items) == ERROR_SUCCESS) {

      std::map<std::string, double> physEngSum;
      double totalLuid = 0;

      printf("--- Full Engine Report ---\n");
      for (DWORD i = 0; i < itemCount; i++) {
        const char *name = items[i].szName;
        double val = items[i].FmtValue.doubleValue;

        if (filter && strstr(name, filter)) {
          if (items[i].FmtValue.CStatus == ERROR_SUCCESS && val > 0.0001) {
            printf("  [%s]: %.2f%%\n", name, val);
            totalLuid += val;

            // Try to find the physical engine part (e.g. phys_0_eng_0)
            const char *phys = strstr(name, "phys_");
            if (phys) {
              char physPart[64];
              strncpy(physPart, phys, 63);
              char *endIdx = strstr(physPart, "_engtype");
              if (endIdx)
                *endIdx = 0;
              physEngSum[physPart] += val;
            }
          }
        }
      }

      printf("\n--- Summary per Physical Engine ---\n");
      double maxPhys = 0;
      for (auto const &[phys, val] : physEngSum) {
        printf("  %s: %.2f%%\n", phys.c_str(), val);
        if (val > maxPhys && strstr(phys.c_str(), "engtype_Video") == NULL) {
          maxPhys = val;
        }
      }

      printf("\nSystem SUM: %.2f%%\n", totalLuid);
      printf("Max Phys Core: %.2f%%\n", maxPhys);
    }
  }

  PdhCloseQuery(query);
}

int main(int argc, char **argv) {
  const char *filter = (argc > 1) ? argv[1] : NULL;
  printf("Probing GPU Engines...\n");
  if (filter)
    printf("Filtering for: %s\n", filter);

  while (true) {
    PrintBruteForce(filter);
    printf("--------------------------\n");
    Sleep(500);
  }
  return 0;
}

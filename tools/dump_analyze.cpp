#include <windows.h>
#include <dbghelp.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "dbghelp.lib")

std::string GetBasename(const std::string& path) {
    size_t lastSlash = path.find_last_of("\\/");
    if (lastSlash != std::string::npos) return path.substr(lastSlash + 1);
    return path;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: dump_analyze <path_to_dmp>" << std::endl;
        return 1;
    }

    HANDLE hFile = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open dump file: " << GetLastError() << std::endl;
        return 1;
    }

    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) {
        std::cerr << "Failed to create file mapping." << std::endl;
        CloseHandle(hFile);
        return 1;
    }

    void* pBase = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!pBase) {
        std::cerr << "Failed to map view of file." << std::endl;
        CloseHandle(hMap);
        CloseHandle(hFile);
        return 1;
    }

    std::cout << " Analyzing Crash Dump: " << argv[1] << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    // 1. Read Exception Stream
    MINIDUMP_DIRECTORY* dir = nullptr;
    void* stream = nullptr;
    ULONG streamSize = 0;

    if (MiniDumpReadDumpStream(pBase, ExceptionStream, &dir, &stream, &streamSize)) {
        MINIDUMP_EXCEPTION_STREAM* exceptionStream = (MINIDUMP_EXCEPTION_STREAM*)stream;
        std::cout << "Exception Code:    0x" << std::hex << exceptionStream->ExceptionRecord.ExceptionCode << std::endl;
        std::cout << "Exception Flags:   0x" << std::hex << exceptionStream->ExceptionRecord.ExceptionFlags << std::endl;
        std::cout << "Exception Address: 0x" << std::hex << exceptionStream->ExceptionRecord.ExceptionAddress << std::endl;
        
        DWORD64 crashAddr = exceptionStream->ExceptionRecord.ExceptionAddress;

        // 2. Read Module List
        if (MiniDumpReadDumpStream(pBase, ModuleListStream, &dir, &stream, &streamSize)) {
            MINIDUMP_MODULE_LIST* moduleList = (MINIDUMP_MODULE_LIST*)stream;
            std::cout << "Module Count:      " << std::dec << moduleList->NumberOfModules << std::endl;
            std::cout << "----------------------------------------" << std::endl;

            for (ULONG i = 0; i < moduleList->NumberOfModules; i++) {
                MINIDUMP_MODULE& mod = moduleList->Modules[i];
                
                // Get Module Name
                std::string modName = "Unknown";
                if (mod.ModuleNameRva) {
                    // RVA is offset from start of dump file
                    MINIDUMP_STRING* pStr = (MINIDUMP_STRING*)((BYTE*)pBase + mod.ModuleNameRva);
                    if (pStr->Length > 0) {
                        // Convert WCHAR to MultiByte
                        int len = WideCharToMultiByte(CP_UTF8, 0, pStr->Buffer, pStr->Length / 2, NULL, 0, NULL, NULL);
                        if (len > 0) {
                            std::vector<char> buf(len + 1);
                            WideCharToMultiByte(CP_UTF8, 0, pStr->Buffer, pStr->Length / 2, buf.data(), len, NULL, NULL);
                            modName = std::string(buf.data());
                        }
                    }
                }

                if (crashAddr >= mod.BaseOfImage && crashAddr < (mod.BaseOfImage + mod.SizeOfImage)) {
                    std::cout << ">>> CRASH MODULE FOUND <<<" << std::endl;
                    std::cout << "Module: " << modName << std::endl;
                    std::cout << "Base:   0x" << std::hex << mod.BaseOfImage << std::endl;
                    std::cout << "Size:   0x" << std::hex << mod.SizeOfImage << std::endl;
                    std::cout << "Offset: 0x" << std::hex << (crashAddr - mod.BaseOfImage) << std::endl;
                    std::cout << "----------------------------------------" << std::endl;
                }
            }
        }
    } else {
        std::cerr << "Failed to find Exception Stream in dump." << std::endl;
    }

    UnmapViewOfFile(pBase);
    CloseHandle(hMap);
    CloseHandle(hFile);
    return 0;
}

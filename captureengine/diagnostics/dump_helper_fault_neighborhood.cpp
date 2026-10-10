#include "dump_helper_fault_neighborhood.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace {

namespace policy = ce::fault_neighborhood;

policy::Region QueryRegion(HANDLE process, uint64_t address) {
    policy::Region region;
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), &info, sizeof(info)) !=
        sizeof(info)) {
        return region;
    }
    const DWORD protect = info.Protect & 0xFFu;
    region.valid = true;
    region.base = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info.BaseAddress));
    region.size = static_cast<uint64_t>(info.RegionSize);
    region.allocationBase = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info.AllocationBase));
    region.committed = info.State == MEM_COMMIT && (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
    region.readable =
        region.committed &&
        (protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
         protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY);
    region.executable = protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
        protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
    return region;
}

std::wstring HelperDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) {
        return std::wstring();
    }
    path.resize(length);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// Module paths from Toolhelp and GetModuleFileNameW both come from the same
// loader, so a case-insensitive prefix comparison answers "same install tree".
bool SameDirectory(const wchar_t* path, const std::wstring& directory) {
    if (!path || directory.empty()) {
        return false;
    }
    const std::wstring file(path);
    const size_t slash = file.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash != directory.size()) {
        return false;
    }
    return CompareStringOrdinal(file.c_str(), static_cast<int>(slash), directory.c_str(),
                                static_cast<int>(directory.size()), TRUE) == CSTR_EQUAL;
}

}  // namespace

FaultNeighborhoodCollector::FaultNeighborhoodCollector(HANDLE targetProcess,
                                                       unsigned long long exceptionPointersAddress)
    : process_(targetProcess), exceptionPointersAddress_(exceptionPointersAddress) {
    BOOL wow64 = FALSE;
    // The pointer-sized structure layouts and the CONTEXT the exception stream
    // points at differ for WoW64 targets, which keep the 32-bit stack fix and
    // nothing else.
    active_ = process_ != nullptr && exceptionPointersAddress_ != 0 &&
        IsWow64Process(process_, &wow64) != FALSE && wow64 == FALSE;
    if (active_) {
        ranges_.reserve(64);
        references_.reserve(32);
    }
}

bool FaultNeighborhoodCollector::ReadTarget(uint64_t address, void* buffer, size_t bytes) const {
    if (!process_ || !buffer || bytes == 0 || address == 0) {
        return false;
    }
    SIZE_T read = 0;
    return ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), buffer, bytes,
                             &read) != FALSE &&
        read == bytes;
}

bool FaultNeighborhoodCollector::AddWindow(policy::Range range, bool harvestCode) {
    if (range.start == 0 || range.size == 0) {
        return false;
    }
    const policy::Region region = QueryRegion(process_, range.start);
    if (!region.valid || !region.committed || !region.readable) {
        return false;
    }
    const uint64_t regionEnd = region.base + region.size;
    if (range.start + range.size > regionEnd) {
        range.size = regionEnd - range.start;
    }
    for (const policy::Range& existing : ranges_) {
        if (policy::RangesOverlap(existing, range)) {
            return false;
        }
    }
    if (!budget_.Admit(range)) {
        return false;
    }
    if (harvestCode) {
        std::vector<uint8_t> bytes(static_cast<size_t>(range.size));
        if (ReadTarget(range.start, bytes.data(), bytes.size())) {
            policy::HarvestCodeReferences(bytes.data(), bytes.size(), range.start,
                                          policy::kMaxHarvestedRefsPerWindow, references_);
        }
    }
    ranges_.push_back(range);
    return true;
}

void FaultNeighborhoodCollector::AddCodePointer(uint64_t address) {
    if (codeWindows_ >= policy::kMaxCodeWindows) {
        return;
    }
    if (AddWindow(policy::CodeWindow(address), true)) {
        ++codeWindows_;
    }
}

void FaultNeighborhoodCollector::AddReference(uint64_t address) {
    if (referenceWindows_ >= policy::kMaxReferenceWindows) {
        return;
    }
    if (AddWindow(policy::ReferenceWindow(address), false)) {
        ++referenceWindows_;
    }
}

void FaultNeighborhoodCollector::AddRegisterValue(uint64_t value) {
    if (registerWindows_ >= policy::kMaxRegisterWindows || value < 0x10000) {
        return;
    }
    if (AddWindow(policy::ReferenceWindow(value, policy::kRegisterWindowBytes), false)) {
        ++registerWindows_;
    }
}

void FaultNeighborhoodCollector::AddOwnModuleData() {
    // The helper runs from CaptureEngine's own install tree. Its modules' data
    // segments hold the hook's state - what a debugger needs for a defect in
    // our own code - and are the only module data worth the dump size here.
    const std::wstring directory = HelperDirectory();
    if (directory.empty()) {
        return;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetProcessId(process_));
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (!SameDirectory(entry.szExePath, directory)) {
                continue;
            }
            uint8_t headers[4096] = {};
            if (!ReadTarget(reinterpret_cast<uintptr_t>(entry.modBaseAddr), headers, sizeof(headers))) {
                continue;
            }
            std::vector<policy::SectionRange> sections;
            if (!policy::HarvestWritableDataSections(headers, sizeof(headers), sections)) {
                continue;
            }
            for (const policy::SectionRange& section : sections) {
                AddWindow({reinterpret_cast<uintptr_t>(entry.modBaseAddr) + section.rva, section.size}, false);
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void FaultNeighborhoodCollector::Collect() {
    collected_ = true;

    EXCEPTION_POINTERS pointers = {};
    EXCEPTION_RECORD record = {};
    CONTEXT context = {};
    if (!ReadTarget(exceptionPointersAddress_, &pointers, sizeof(pointers)) ||
        pointers.ExceptionRecord == nullptr || pointers.ContextRecord == nullptr ||
        !ReadTarget(reinterpret_cast<uintptr_t>(pointers.ExceptionRecord), &record, sizeof(record)) ||
        !ReadTarget(reinterpret_cast<uintptr_t>(pointers.ContextRecord), &context, sizeof(context))) {
        return;
    }

    // The faulting instruction window first: it always carries the trap itself.
    if (context.Rip != 0) {
        AddCodePointer(context.Rip);
    }

    // The faulting thread's stack: the complete call chain, and the code
    // pointers on it are the call sites worth reading offline.
    policy::Range stack;
    if (policy::ResolveCommittedSpanAbove(
            context.Rsp, policy::kStackBytes,
            [this](uint64_t address) { return QueryRegion(process_, address); }, stack)) {
        AddWindow(stack, false);
        std::vector<uint8_t> stackBytes(static_cast<size_t>(stack.size));
        if (ReadTarget(stack.start, stackBytes.data(), stackBytes.size())) {
            std::vector<uint64_t> candidates;
            for (size_t offset = 0; offset + sizeof(uint64_t) <= stackBytes.size();
                 offset += sizeof(uint64_t)) {
                uint64_t value = 0;
                std::memcpy(&value, stackBytes.data() + offset, sizeof(value));
                if (value >= 0x10000) {
                    candidates.push_back(value);
                }
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            for (uint64_t value : candidates) {
                const policy::Region region = QueryRegion(process_, value);
                if (region.valid && region.executable) {
                    AddCodePointer(value);
                }
            }
        }
    }

    // Registers hold the objects the faulting code was working with.
    const uint64_t registers[] = {context.Rax, context.Rcx, context.Rdx, context.Rbx, context.Rbp,
                                  context.Rsi, context.Rdi, context.R8,  context.R9,  context.R10,
                                  context.R11, context.R12, context.R13, context.R14, context.R15};
    for (uint64_t value : registers) {
        AddRegisterValue(value);
    }

    // What the captured code references: callees and const data such as the
    // message strings a report/assert thunk passes along. Windows added while
    // harvesting append here in turn, so this walks transitively until the
    // caps stop it.
    for (size_t i = 0; i < references_.size(); ++i) {
        if (references_[i].code) {
            AddCodePointer(references_[i].address);
        } else {
            AddReference(references_[i].address);
        }
    }

    AddOwnModuleData();
}

bool FaultNeighborhoodCollector::ServeMemoryRange(PMINIDUMP_CALLBACK_OUTPUT output) {
    if (!output) {
        return false;
    }
    if (!collected_) {
        Collect();
    }
    if (emitted_ >= ranges_.size()) {
        // WriteSupplementalCrashDump falls back to smaller dump types when an
        // attempt fails, and each attempt asks for ranges again. The set does
        // not change between attempts, so the cursor rewinds for the next one.
        emitted_ = 0;
        return false;
    }
    const policy::Range range = ranges_[emitted_++];
    output->MemoryBase = range.start;
    output->MemorySize = static_cast<ULONG>(range.size);
    return true;
}

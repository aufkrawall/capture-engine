// Win32 mapping for the media finalization result slot. The cross-process layout and the pure
// logic live in media_finalization.h.

#include "media_finalization.h"

#include <windows.h>

#include "common/logging/logging.h"

namespace ce::media_result {

std::unique_ptr<ResultSlot> ResultSlot::Create() {
    // Anonymous (unnamed) so nothing outside the spawned child can open it. Not inheritable here:
    // SpawnChildProcess grants inheritance for the duration of one launch, like the IPC endpoint.
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(sizeof(Slot)), nullptr);
    if (!mapping) {
        LogWarn("[MediaResult] Result slot unavailable (CreateFileMapping error=%lu); the media child reports its "
                "outcome through the exit code alone",
                static_cast<unsigned long>(GetLastError()));
        return nullptr;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Slot));
    if (!view) {
        LogWarn("[MediaResult] Result slot unavailable (MapViewOfFile error=%lu); the media child reports its "
                "outcome through the exit code alone",
                static_cast<unsigned long>(GetLastError()));
        CloseHandle(mapping);
        return nullptr;
    }
    std::unique_ptr<ResultSlot> slot(new ResultSlot());
    slot->mapping_ = mapping;
    slot->slot_ = static_cast<Slot*>(view);
    InitSlot(*slot->slot_);
    return slot;
}

ResultSlot::~ResultSlot() {
    if (slot_)
        UnmapViewOfFile(slot_);
    if (mapping_)
        CloseHandle(static_cast<HANDLE>(mapping_));
}

void* ResultSlot::ChildHandle() const {
    return mapping_;
}

bool ResultSlot::Read(Result& out) const {
    return slot_ && ReadResult(*slot_, out);
}

Slot* MapInheritedSlot(void* inheritedHandle) {
    if (!inheritedHandle || inheritedHandle == INVALID_HANDLE_VALUE)
        return nullptr;
    // The child owns its inherited copy of the handle; the view outlives it, so close it as soon as
    // the mapping exists rather than leaking it for the life of the process.
    void* view = MapViewOfFile(static_cast<HANDLE>(inheritedHandle), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Slot));
    const DWORD mapError = view ? ERROR_SUCCESS : GetLastError();
    CloseHandle(static_cast<HANDLE>(inheritedHandle));
    if (!view) {
        LogWarn("[MediaResult] Inherited result slot could not be mapped (error=%lu); reporting through the exit "
                "code alone",
                static_cast<unsigned long>(mapError));
        return nullptr;
    }
    auto* slot = static_cast<Slot*>(view);
    if (!IsSlotCompatible(*slot)) {
        LogWarn("[MediaResult] Inherited result slot rejected: magic=0x%08X version=%u size=%u expected=(0x%08X,%u,%zu)",
                slot->magic, slot->version, slot->structSize, kSlotMagic, kSlotVersion, sizeof(Slot));
        UnmapViewOfFile(view);
        return nullptr;
    }
    return slot;
}

void UnmapInheritedSlot(Slot* slot) {
    if (slot)
        UnmapViewOfFile(slot);
}

}  // namespace ce::media_result

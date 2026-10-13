#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dxgi1_6.h>

// An independent overlay's save/restore/call/re-patch entry chain, loaded after CE.
// The test host supplies an ownership-checked, thread-quiesced byte writer.
namespace {
using EntryWriter = bool (*)(void*, const void*, const void*, size_t);
struct LatePresentEntry {
    void* target = nullptr;
    EntryWriter write = nullptr;
    unsigned char saved[14]{};
    unsigned char patch[14]{};
    uint32_t calls = 0;

    bool Restore() { return target && write(target, patch, saved, sizeof(saved)); }
    bool Repatch() { return write(target, saved, patch, sizeof(saved)); }
};
LatePresentEntry g_latePresent[2];

HRESULT STDMETHODCALLTYPE LatePresent(IDXGISwapChain* chain, UINT interval, UINT flags) {
    auto& entry = g_latePresent[0];
    if (!entry.Restore()) return E_FAIL;
    const auto original = reinterpret_cast<HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT)>(entry.target);
    const HRESULT result = original(chain, interval, flags);
    ++entry.calls;
    return entry.Repatch() ? result : E_FAIL;
}
HRESULT STDMETHODCALLTYPE LatePresent1(IDXGISwapChain* chain, UINT interval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* parameters) {
    auto& entry = g_latePresent[1];
    if (!entry.Restore()) return E_FAIL;
    const auto original = reinterpret_cast<HRESULT (STDMETHODCALLTYPE*)(
        IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*)>(entry.target);
    const HRESULT result = original(chain, interval, flags, parameters);
    ++entry.calls;
    return entry.Repatch() ? result : E_FAIL;
}
}  // namespace

extern "C" __declspec(dllexport) bool CEFake_InstallPresentEntry(void* target, bool present1, EntryWriter writer) {
    auto& entry = g_latePresent[present1 ? 1 : 0];
    if (!target || !writer || entry.target) return false;
    std::memcpy(entry.saved, target, sizeof(entry.saved));
    const unsigned char header[] = {0xFF, 0x25, 0, 0, 0, 0};
    std::memcpy(entry.patch, header, sizeof(header));
    void* handler = present1 ? reinterpret_cast<void*>(&LatePresent1) : reinterpret_cast<void*>(&LatePresent);
    std::memcpy(entry.patch + sizeof(header), static_cast<const void*>(&handler), sizeof(handler));
    entry.write = writer;
    entry.target = target;
    entry.calls = 0;
    if (entry.Repatch()) return true;
    entry.target = nullptr;
    return false;
}
extern "C" __declspec(dllexport) bool CEFake_RemovePresentEntry(bool present1) {
    auto& entry = g_latePresent[present1 ? 1 : 0];
    if (!entry.Restore()) return false;
    entry.target = nullptr;
    return true;
}
extern "C" __declspec(dllexport) uint32_t CEFake_PresentEntryCalls(bool present1) {
    return g_latePresent[present1 ? 1 : 0].calls;
}

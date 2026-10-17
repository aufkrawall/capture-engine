#pragma once

// CE-owned dispatch tables for D3D11 contexts whose function table belongs to the object.
//
// d3d11.dll keeps each context's function table inside the context (the vtable pointer points
// 8 bytes past itself, private heap memory) and rewrites entries of it while it runs, copying
// from static defaults or `*_Amortized` variants (CContext::SetAllAmortized11 and friends):
// - Flush, ExecuteCommandList and a Present point the draw and dispatch entries (12, 13, 20, 21,
//   38-42) at `*_Amortized<1>`, which do the runtime's pending work and restore the defaults at
//   the next draw;
// - SetMultithreadProtected and SwapDeviceContextState rewrite about 100 more, among them the
//   pixel-stage binds (8-10), ExecuteCommandList, PSGetSamplers, ClearState, FinishCommandList.
// A hook written into that table lasts until the runtime's next rewrite of its slot (WARP and
// d3d11.dll 10.0.26100.9711, 2026-10-17; the device's own table is static image memory).
//
// Adopt() leaves the runtime's table alone and points the context at a CE table instead: hooked
// slots hold CE's detours, every other slot a forwarder that jumps through whatever the runtime's
// table holds at that moment. The runtime keeps rewriting its own table, CE reads it at every
// call and never writes it, so no write of the runtime's can race or undo CE's. Detours reach the
// runtime's current entry through RuntimeTable(). Tables live as long as their context: a
// private-data object of the context's returns the table when the runtime destroys it.
//
// Static (image-backed) tables are not adopted: nothing rewrites them, and the slot hooks on them
// (dx11_hook_install.cpp) keep working.

#include <d3d11.h>

#include <cstddef>
#include <cstdint>

namespace ce::d3d11_dispatch {

// Entries per CE table. ID3D11DeviceContext4 has 149 methods; the rest forward harmlessly.
inline constexpr unsigned kSlots = 256;
// Tables CE can hand out at once (recycled when contexts die).
inline constexpr unsigned kMaxTables = 128;

// The context's vtable pointer points at `entries`; the runtime's table sits right before it.
struct Table {
    void** runtime;
    void* entries[kSlots];
};
static_assert(offsetof(Table, entries) == sizeof(void*), "entries[-1] must be the runtime table");

extern Table g_tables[kMaxTables];

// CE's table when `vtable` is one (a context's vtable pointer always points at its `entries`).
inline Table* TableOfVTable(void* const* vtable) {
    const uintptr_t offset =
        reinterpret_cast<uintptr_t>(vtable) - reinterpret_cast<uintptr_t>(&g_tables[0].entries[0]);
    return offset < sizeof(g_tables) ? reinterpret_cast<Table*>(reinterpret_cast<uintptr_t>(&g_tables[0]) + offset)
                                     : nullptr;
}

// The runtime's table behind a context CE dispatches, nullptr for any other object. The per-call
// path of every raw detour: one subtraction, one compare and one load.
inline void** RuntimeTable(const void* object) {
    Table* const table = TableOfVTable(*static_cast<void* const* const*>(object));
    return table ? table->runtime : nullptr;
}

// The same for a context CE adopted whose vtable pointer someone else has since replaced (a tool
// copying CE's table into its own): a locked lookup, for the callers' rare miss path only.
void** RuntimeTableOfAdopted(const void* object);

// The table CE's own vtable hooks belong in for `object`: the runtime's behind a CE table.
inline void** HookTable(const void* object) {
    void** const runtime = RuntimeTable(object);
    return runtime ? runtime : *static_cast<void** const*>(object);
}

struct Detour {
    unsigned slot;
    void* function;
};

enum class AdoptResult {
    kAdopted,         // the context dispatches through a new CE table
    kAlreadyAdopted,  // it already did (detours refreshed), or another thread is adopting it
    kStaticTable,     // image-backed table: nothing rewrites it, hook its slots
    kUnavailable,     // no table, no forwarder code, private data refused, or the table holds CE's own
                      // slot detours: hook its slots as before
};

// Gives `context` a CE table whose `detours` replace those slots. The caller holds a reference.
AdoptResult Adopt(ID3D11DeviceContext* context, const Detour* detours, size_t count, const char* source);

const char* ToString(AdoptResult result);

struct Stats {
    uint64_t adopted = 0;
    uint64_t retired = 0;
    uint64_t staticTables = 0;
    uint64_t unavailable = 0;
    uint32_t live = 0;
};
Stats SnapshotStats();

}  // namespace ce::d3d11_dispatch

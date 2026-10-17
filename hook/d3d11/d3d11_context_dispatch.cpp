// CE-owned dispatch tables for D3D11 contexts (d3d11_context_dispatch.h).
//
// Forwarder code is generated per table index, never per context: a table's forwarders read
// that table's `runtime` field, so recycling a table only rewrites data. Code comes in chunks
// of 16 tables (one 64 KB allocation, 4096 forwarders registered with Control Flow Guard),
// built when the first table of the chunk is handed out and never freed.
//
// Recycling: a retired table keeps its contents until it is handed out again, and the oldest
// retired table goes first, so a runtime that still calls through a dying context's vtable
// pointer after releasing its private data finds valid forwarders.

#include "hook/d3d11/d3d11_context_dispatch.h"

#include <atomic>
#include <mutex>
#include <new>
#include <vector>

#include "hook/d3d11/d3d11_dispatch_forwarders.h"
#include "hook/hooking/inline_hook.h"
#include "hook/runtime/hook_common.h"

namespace ce::d3d11_dispatch {

Table g_tables[kMaxTables];

namespace {

constexpr unsigned kTablesPerChunk = 16;
constexpr size_t kCodeBytesPerTable = kSlots * kForwarderStride;
constexpr size_t kChunkBytes = kTablesPerChunk * kCodeBytesPerTable;
constexpr unsigned kChunks = kMaxTables / kTablesPerChunk;
static_assert(kMaxTables % kTablesPerChunk == 0, "whole chunks");

// {7F0C2A1E-3D11-4CE0-9A57-D15A7C4B0E11}: the context's private data that returns its table.
const GUID kTableLeaseGuid = {0x7f0c2a1e, 0x3d11, 0x4ce0, {0x9a, 0x57, 0xd1, 0x5a, 0x7c, 0x4b, 0x0e, 0x11}};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex constructors are noexcept on this toolchain
std::mutex g_mutex;
uint8_t* g_chunks[kChunks] = {};                // g_mutex
unsigned g_builtTables = 0;                     // g_mutex: tables whose forwarders exist
unsigned g_freshTables = 0;                     // g_mutex: tables never handed out
uint16_t g_retired[kMaxTables] = {};            // g_mutex: FIFO of retired tables
unsigned g_retiredHead = 0;                     // g_mutex
unsigned g_retiredCount = 0;                    // g_mutex
const void* g_owner[kMaxTables] = {};           // g_mutex: the context each live table serves
std::vector<const void*> g_adopting;            // g_mutex: contexts some thread is adopting

std::atomic<uint64_t> g_adopted{0};
std::atomic<uint64_t> g_retiredTotal{0};
std::atomic<uint64_t> g_staticTables{0};
std::atomic<uint64_t> g_unavailable{0};
std::atomic<uint32_t> g_live{0};

void* ForwarderOf(unsigned table, unsigned slot) {
    return g_chunks[table / kTablesPerChunk] + (table % kTablesPerChunk) * kCodeBytesPerTable +
           slot * kForwarderStride;
}

bool BuildChunkLocked(unsigned chunk) {
    uint8_t* const code = InlineHook::AllocateWritableCodePage(kChunkBytes);
    if (!code) {
        HookLogImportant("[D3D11 DISPATCH] forwarder code allocation failed chunk=%u error=%lu", chunk,
                         GetLastError());
        return false;
    }
    for (unsigned i = 0; i < kTablesPerChunk; ++i) {
        const unsigned table = chunk * kTablesPerChunk + i;
        for (unsigned slot = 0; slot < kSlots; ++slot) {
            EncodeForwarder(code + i * kCodeBytesPerTable + slot * kForwarderStride, &g_tables[table].runtime,
                            slot);
        }
    }
    if (!InlineHook::SealCodePage(code, kChunkBytes, kForwarderStride, kTablesPerChunk * kSlots)) {
        HookLogImportant("[D3D11 DISPATCH] forwarder code could not be sealed chunk=%u", chunk);
        return false;
    }
    g_chunks[chunk] = code;
    g_builtTables += kTablesPerChunk;
    HookLog("[D3D11 DISPATCH] forwarder chunk=%u code=%p tables=%u", chunk, static_cast<void*>(code),
            g_builtTables);
    return true;
}

// Fresh tables with forwarders first, then the longest-retired one, then a new chunk.
bool TakeTableLocked(unsigned* table) {
    if (g_freshTables < g_builtTables) {
        *table = g_freshTables++;
        return true;
    }
    if (g_retiredCount != 0) {
        *table = g_retired[g_retiredHead];
        g_retiredHead = (g_retiredHead + 1) % kMaxTables;
        --g_retiredCount;
        return true;
    }
    if (g_builtTables >= kMaxTables || !BuildChunkLocked(g_builtTables / kTablesPerChunk)) {
        return false;
    }
    *table = g_freshTables++;
    return true;
}

void Retire(unsigned table) {
    const void* owner = nullptr;
    unsigned live = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        owner = g_owner[table];
        g_owner[table] = nullptr;
        g_retired[(g_retiredHead + g_retiredCount) % kMaxTables] = static_cast<uint16_t>(table);
        ++g_retiredCount;
        if (owner) {
            live = g_live.fetch_sub(1, std::memory_order_relaxed) - 1;
        } else {
            live = g_live.load(std::memory_order_relaxed);
        }
    }
    if (!owner) {
        return;  // never published (adoption declined after the lease existed)
    }
    const uint64_t retired = g_retiredTotal.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!HookIsShuttingDown() && (retired <= 16 || (retired % 256) == 0)) {
        HookLog("[D3D11 DISPATCH] retired ctx=%p table=%u live=%u retired=%llu adopted=%llu", owner, table, live,
                static_cast<unsigned long long>(retired),
                static_cast<unsigned long long>(g_adopted.load(std::memory_order_relaxed)));
    }
}

// The context's private data: the runtime releases it when it destroys the context.
class TableLease final : public IUnknown {
public:
    explicit TableLease(unsigned table) : table_(table) {}
    TableLease(const TableLease&) = delete;
    TableLease& operator=(const TableLease&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        if (iid == __uuidof(IUnknown)) {
            *object = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            Retire(table_);
            delete this;
        }
        return remaining;
    }

private:
    ~TableLease() = default;
    const unsigned table_;
    std::atomic<ULONG> refs_{1};
};

bool IsStaticTable(void* const* vtable) {
    MEMORY_BASIC_INFORMATION info = {};
    return VirtualQuery(vtable, &info, sizeof(info)) == 0 || info.Type == MEM_IMAGE || info.State != MEM_COMMIT;
}

void WriteDetours(Table& table, const Detour* detours, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (detours[i].slot < kSlots && table.entries[detours[i].slot] != detours[i].function) {
            InterlockedExchangePointer(&table.entries[detours[i].slot], detours[i].function);
        }
    }
}

AdoptResult Decline(AdoptResult result, ID3D11DeviceContext* context, void* const* vtable, const char* reason,
                    const char* source) {
    std::atomic<uint64_t>& counter = result == AdoptResult::kStaticTable ? g_staticTables : g_unavailable;
    const uint64_t count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 8 || (count % 256) == 0) {
        HookLogImportant("[D3D11 DISPATCH] ctx=%p table=%p kept, slot hooks instead: %s (source=%s #%llu)",
                         static_cast<void*>(context), static_cast<const void*>(vtable), reason,
                         source ? source : "unknown", static_cast<unsigned long long>(count));
    }
    return result;
}

void EndAdopting(const void* context, unsigned table, bool published) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_adopting.begin(); it != g_adopting.end(); ++it) {
        if (*it == context) {
            g_adopting.erase(it);
            break;
        }
    }
    if (published) {
        g_owner[table] = context;
        g_live.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

void** RuntimeTableOfAdopted(const void* object) {
    if (!object) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (unsigned table = 0; table < g_builtTables; ++table) {
        if (g_owner[table] == object) {
            return g_tables[table].runtime;
        }
    }
    return nullptr;
}

AdoptResult Adopt(ID3D11DeviceContext* context, const Detour* detours, size_t count, const char* source) {
    if (!context) {
        return AdoptResult::kUnavailable;
    }
    void** const current = *reinterpret_cast<void***>(context);
    if (Table* const adopted = TableOfVTable(current)) {
        WriteDetours(*adopted, detours, count);
        return AdoptResult::kAlreadyAdopted;
    }
    if (IsStaticTable(current)) {
        return Decline(AdoptResult::kStaticTable, context, current, "static image memory", source);
    }
    for (size_t i = 0; i < count; ++i) {
        // CE hooked this table's slots earlier; forwarding to them would call the detour forever.
        if (current[detours[i].slot] == detours[i].function) {
            return Decline(AdoptResult::kUnavailable, context, current, "holds CE slot hooks", source);
        }
    }
    unsigned index = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const void* adopting : g_adopting) {
            if (adopting == context) {
                return AdoptResult::kAlreadyAdopted;
            }
        }
        for (unsigned table = 0; table < g_builtTables; ++table) {
            if (g_owner[table] == context) {
                return AdoptResult::kAlreadyAdopted;  // adopted; someone replaced CE's table since
            }
        }
        if (!TakeTableLocked(&index)) {
            return Decline(AdoptResult::kUnavailable, context, current, "no dispatch table", source);
        }
        g_adopting.push_back(context);
    }
    Table& table = g_tables[index];
    table.runtime = current;
    for (unsigned slot = 0; slot < kSlots; ++slot) {
        table.entries[slot] = ForwarderOf(index, slot);
    }
    WriteDetours(table, detours, count);

    // No CE lock is held across calls into the runtime: a multithread-protected device can hold
    // its own lock while another thread waits for one of CE's.
    TableLease* const lease = new (std::nothrow) TableLease(index);
    const bool leased = lease && SUCCEEDED(context->SetPrivateDataInterface(kTableLeaseGuid, lease));
    bool published = false;
    if (leased) {
        published = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(context), &table.entries[0],
                                                      current) == current;
    }
    EndAdopting(context, index, published);
    if (lease) {
        lease->Release();  // the context's reference keeps it now; without one this retires the table
    } else {
        Retire(index);
    }
    if (leased && !published) {
        context->SetPrivateDataInterface(kTableLeaseGuid, nullptr);  // retires the table
    }
    if (!published) {
        return Decline(AdoptResult::kUnavailable, context, current,
                       leased ? "vtable pointer changed meanwhile" : "private data refused", source);
    }
    const uint64_t adopted = g_adopted.fetch_add(1, std::memory_order_relaxed) + 1;
    if (adopted <= 16 || (adopted % 256) == 0) {
        HookLog("[D3D11 DISPATCH] adopted ctx=%p runtimeTable=%p table=%u detours=%zu live=%u adopted=%llu "
                "source=%s",
                static_cast<void*>(context), static_cast<void*>(current), index, count,
                g_live.load(std::memory_order_relaxed), static_cast<unsigned long long>(adopted),
                source ? source : "unknown");
    }
    return AdoptResult::kAdopted;
}

const char* ToString(AdoptResult result) {
    switch (result) {
        case AdoptResult::kAdopted:
            return "adopted";
        case AdoptResult::kAlreadyAdopted:
            return "already-adopted";
        case AdoptResult::kStaticTable:
            return "static-table";
        case AdoptResult::kUnavailable:
            return "unavailable";
    }
    return "unknown";
}

Stats SnapshotStats() {
    Stats stats;
    stats.adopted = g_adopted.load(std::memory_order_relaxed);
    stats.retired = g_retiredTotal.load(std::memory_order_relaxed);
    stats.staticTables = g_staticTables.load(std::memory_order_relaxed);
    stats.unavailable = g_unavailable.load(std::memory_order_relaxed);
    stats.live = g_live.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace ce::d3d11_dispatch

#ifdef CE_FLOW_TEST
// Values in CEFlowD3D11DispatchStats order (tests/flow/flow_api.h); returns how many were written.
extern "C" __declspec(dllexport) uint32_t CEFlow_GetD3D11DispatchStats(uint64_t* values, uint32_t count) {
    const ce::d3d11_dispatch::Stats stats = ce::d3d11_dispatch::SnapshotStats();
    const uint64_t ordered[] = {stats.adopted, stats.retired, stats.staticTables, stats.unavailable, stats.live};
    constexpr uint32_t kValues = static_cast<uint32_t>(sizeof(ordered) / sizeof(ordered[0]));
    const uint32_t written = values ? (count < kValues ? count : kValues) : 0u;
    for (uint32_t i = 0; i < written; ++i) {
        values[i] = ordered[i];
    }
    return written;
}
#endif

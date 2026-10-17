// Per-thread blocks of CE's D3D11 forced AF (d3d11_af_thread.h): claiming, reclaiming from
// exited threads, and the reference-free shader memo.

#include "d3d11_af_thread.h"

#include <new>

#include "hook/runtime/hook_common.h"

namespace ce::d3d11_af {

namespace {

// A thread that found every block owned by a live thread tries again after this many calls.
constexpr uint32_t kClaimRetryInterval = 4096;

std::atomic<ThreadBlock*> g_blocks[kMaxThreadBlocks] = {};
SRWLOCK g_claimLock = SRWLOCK_INIT;
std::atomic<uint64_t> g_blocksClaimed{0};

thread_local ThreadBlock* t_block = nullptr;
thread_local uint32_t t_unblockedCalls = 0;

// A reclaimed block keeps its counts (the hook thread sums them) and starts with empty memos.
void ResetMemos(ThreadBlock& block) {
    for (CreationMemoEntry& entry : block.creation) {
        entry.tagged.store(nullptr, std::memory_order_seq_cst);
        entry.used = false;
        entry.stamp = 0;
    }
    for (ShaderMemoEntry& entry : block.shaders) {
        entry.shader.store(nullptr, std::memory_order_seq_cst);
        entry.planOwner = nullptr;
        entry.stamp = 0;
    }
    block.creationClock = 0;
    block.shaderClock = 0;
}

// A block for the calling thread: a new one, or one whose thread has exited. Null when every
// block belongs to a live thread.
ThreadBlock* ClaimBlock() {
    HANDLE self = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, SYNCHRONIZE, FALSE, 0)) {
        self = nullptr;
    }
    ThreadBlock* claimed = nullptr;
    bool reclaimed = false;
    size_t index = 0;
    AcquireSRWLockExclusive(&g_claimLock);
    for (; index < kMaxThreadBlocks; ++index) {
        ThreadBlock* block = g_blocks[index].load(std::memory_order_relaxed);
        if (!block) {
            block = new (std::nothrow) ThreadBlock();
            if (block) {
                g_blocks[index].store(block, std::memory_order_release);
            }
            claimed = block;
            break;
        }
        if (block->owner && WaitForSingleObject(block->owner, 0) == WAIT_OBJECT_0) {
            CloseHandle(block->owner);
            ResetMemos(*block);
            claimed = block;
            reclaimed = true;
            break;
        }
    }
    if (claimed) {
        claimed->owner = self;
    }
    ReleaseSRWLockExclusive(&g_claimLock);
    if (!claimed) {
        if (self) {
            CloseHandle(self);
        }
        static std::atomic<int> s_unblockedLogs{0};
        const int logIndex = s_unblockedLogs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 8) {
            HookLog(
                "DX11 AF: no thread block for thread %lu (all %zu owned by live threads); its sampler "
                "creations and shader switches take the slow path, retried every %u calls (#%d)",
                GetCurrentThreadId(), kMaxThreadBlocks, kClaimRetryInterval, logIndex + 1);
        }
        return nullptr;
    }
    const uint64_t claims = g_blocksClaimed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (claims <= 32 || reclaimed) {
        HookLog("DX11 AF: thread block %zu %s by thread %lu (claims=%llu)", index,
                reclaimed ? "reclaimed from an exited thread" : "created", GetCurrentThreadId(),
                static_cast<unsigned long long>(claims));
    }
    return claimed;
}

// 32-bit arithmetic only: the x86 hook has no fast 64-bit multiply.
size_t ShaderHome(const ID3D11PixelShader* shader) {
    const auto address = reinterpret_cast<uintptr_t>(shader);
    uint32_t key = static_cast<uint32_t>(address >> 4);
    if constexpr (sizeof(uintptr_t) > sizeof(uint32_t)) {
        key ^= static_cast<uint32_t>(static_cast<uint64_t>(address) >> 32);
    }
    static_assert((ThreadBlock::kShaderEntries & (ThreadBlock::kShaderEntries - 1)) == 0);
    return static_cast<size_t>((key * 0x9E3779B1u) >> 22) & (ThreadBlock::kShaderEntries - 1);
}
static_assert(ThreadBlock::kShaderEntries == 1024, "ShaderHome takes the top 10 bits");

}  // namespace

ThreadBlock* CurrentThreadBlock() {
    if (ThreadBlock* block = t_block) {
        return block;
    }
    if (t_unblockedCalls++ % kClaimRetryInterval != 0) {
        return nullptr;
    }
    t_block = ClaimBlock();
    return t_block;
}

ThreadBlock* ThreadBlockAt(size_t index) {
    return index < kMaxThreadBlocks ? g_blocks[index].load(std::memory_order_acquire) : nullptr;
}

size_t ClaimedThreadBlocks() {
    size_t blocks = 0;
    for (std::atomic<ThreadBlock*>& slot : g_blocks) {
        blocks += slot.load(std::memory_order_acquire) ? 1 : 0;
    }
    return blocks;
}

IUnknown* FindShaderPlan(ThreadBlock& block, const ID3D11PixelShader* shader) {
    const size_t home = ShaderHome(shader);
    for (size_t probe = 0; probe < ThreadBlock::kShaderProbe; ++probe) {
        ShaderMemoEntry& entry = block.shaders[(home + probe) & (ThreadBlock::kShaderEntries - 1)];
        if (entry.shader.load(std::memory_order_acquire) == shader) {
            entry.stamp = ++block.shaderClock;
            return entry.planOwner;
        }
    }
    return nullptr;
}

bool RememberShaderPlan(ThreadBlock& block, const ID3D11PixelShader* shader, IUnknown* planOwner) {
    const size_t home = ShaderHome(shader);
    ShaderMemoEntry* victim = nullptr;
    bool victimUsed = false;
    for (size_t probe = 0; probe < ThreadBlock::kShaderProbe; ++probe) {
        ShaderMemoEntry& entry = block.shaders[(home + probe) & (ThreadBlock::kShaderEntries - 1)];
        const ID3D11PixelShader* held = entry.shader.load(std::memory_order_acquire);
        if (held == shader) {
            entry.planOwner = planOwner;
            entry.stamp = ++block.shaderClock;
            return false;
        }
        // An empty entry wins; otherwise the least recently used one in the window.
        const bool used = held != nullptr;
        if (!victim || (victimUsed && (!used || entry.stamp < victim->stamp))) {
            victim = &entry;
            victimUsed = used;
        }
    }
    // The key goes last: the entry names the shader only once its handle is in place.
    victim->shader.store(nullptr, std::memory_order_seq_cst);
    victim->planOwner = planOwner;
    victim->stamp = ++block.shaderClock;
    victim->shader.store(shader, std::memory_order_seq_cst);
    return victimUsed;
}

void ForgetShader(const ID3D11PixelShader* shader) {
    if (!shader) {
        return;
    }
    const size_t home = ShaderHome(shader);
    for (std::atomic<ThreadBlock*>& slot : g_blocks) {
        ThreadBlock* block = slot.load(std::memory_order_acquire);
        if (!block) {
            continue;
        }
        for (size_t probe = 0; probe < ThreadBlock::kShaderProbe; ++probe) {
            ShaderMemoEntry& entry = block->shaders[(home + probe) & (ThreadBlock::kShaderEntries - 1)];
            const ID3D11PixelShader* expected = shader;
            if (entry.shader.load(std::memory_order_seq_cst) == shader) {
                entry.shader.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst);
            }
        }
    }
}

#ifdef CE_FLOW_TEST
// Whether any thread's shader memo names `shader` (an address comparison only).
extern "C" __declspec(dllexport) bool CEFlow_D3D11ShaderMemoHolds(const ID3D11PixelShader* shader) {
    for (std::atomic<ThreadBlock*>& slot : g_blocks) {
        if (ThreadBlock* block = slot.load(std::memory_order_acquire)) {
            for (const ShaderMemoEntry& entry : block->shaders) {
                if (entry.shader.load(std::memory_order_seq_cst) == shader) {
                    return true;
                }
            }
        }
    }
    return false;
}
#endif

}  // namespace ce::d3d11_af

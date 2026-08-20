#include "streamline_hook_internal.h"

#include "streamline_hook_v1.h"

#include "../common/module_pin.h"
#include "dx12_streamline_ui_overlay.h"

namespace ce::streamline_v1 {
namespace {

namespace api = ce::streamline_api;

// One remembered UI colour tag, waiting for a command list to record into.
//
// 1.x `slSetTag` carries no command buffer, so CE cannot append the overlay where the 2.x
// path does. The next `slEvaluateFeature` supplies one, and it runs before the game
// presents - which is when DLSS-G consumes the UI layer - so the record still lands in time.
// At most one tag is ever held, and the reference is dropped by the very next evaluate
// whether or not it was used, so this can never become a lifetime extension of its own.
struct PendingUiTag {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
};

std::mutex g_pendingMutex;
PendingUiTag g_pending;
// Lets the evaluate hook skip every probe and QueryInterface when there is nothing waiting.
std::atomic<bool> g_hasPending{false};
// Latched once an evaluate has proven Streamline is driving D3D12 in this process.
std::atomic<bool> g_d3d12Published{false};

// Adopts `next`'s reference rather than taking its own: the reference comes straight from
// the QueryInterface that proved the resource, so it is never dropped and re-taken. A gap
// between those two would be a window for the game to free the texture underneath CE.
void ReplacePendingLocked(const PendingUiTag& next) {
    if (g_pending.resource) {
        g_pending.resource->Release();
    }
    g_pending = next;
    g_hasPending.store(g_pending.resource != nullptr, std::memory_order_release);
}

// True when every byte of [address, address+bytes) is committed and readable. The 1.x
// structure layout is a claim about somebody else's ABI; nothing is dereferenced until this
// says the memory exists.
bool IsReadableData(const void* address, size_t bytes) {
    if (address == nullptr || bytes == 0) {
        return false;
    }
    auto cursor = reinterpret_cast<const uint8_t*>(address);
    const uint8_t* end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info = {};
        if (VirtualQuery(cursor, &info, sizeof(info)) != sizeof(info)) {
            return false;
        }
        if (info.State != MEM_COMMIT) {
            return false;
        }
        constexpr DWORD kNoReadAccess = PAGE_NOACCESS | PAGE_EXECUTE | PAGE_GUARD;
        if ((info.Protect & kNoReadAccess) != 0 || info.Protect == 0) {
            return false;
        }
        const auto* regionEnd = reinterpret_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
        if (regionEnd <= cursor) {
            return false;
        }
        cursor = regionEnd;
    }
    return true;
}

// A COM object CE is willing to call: the object and its vtable pointer are readable, and
// the first slot holds executable code. Only then is QueryInterface reachable.
bool LooksCallableAsCom(const void* candidate) {
    if (!IsReadableData(candidate, sizeof(void*))) {
        return false;
    }
    const void* const* vtable = *reinterpret_cast<const void* const* const*>(candidate);
    if (!IsReadableData(static_cast<const void*>(vtable), sizeof(void*) * 3)) {
        return false;
    }
    return ce::module_pin::IsReadableCode(vtable[0], 1);
}

void LogOnce(std::atomic<uint32_t>& counter, uint32_t budget, const char* format, ...) {
    if (counter.fetch_add(1, std::memory_order_relaxed) >= budget) {
        return;
    }
    va_list args;
    va_start(args, format);
    char message[512];
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    HookLogImportant("%s", message);
}

// Read the 1.x `sl::Resource` a UI tag points at, proving the layout as it goes. Returns
// false - silently, on the hot path - for anything that does not verify.
bool TryReadV1UiResource(const void* resource, PendingUiTag* out) {
    if (!IsReadableData(resource, api::kV1ResourceProbeBytes)) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(resource);
    uint32_t type = 0;
    uint32_t state = 0;
    void* native = nullptr;
    std::memcpy(&type, bytes + api::kV1ResourceOffsetType, sizeof(type));
    std::memcpy(static_cast<void*>(&native), bytes + api::kV1ResourceOffsetNative, sizeof(native));
    std::memcpy(&state, bytes + api::kV1ResourceOffsetState, sizeof(state));

    if (!api::LooksLikeV1UiResource(type, native, state)) {
        return false;
    }
    if (!LooksCallableAsCom(native)) {
        return false;
    }

    // The layout hypothesis is only accepted once the pointer it produced answers as a real
    // D3D12 texture. A mislaid offset cannot survive this.
    ID3D12Resource* texture = nullptr;
    if (FAILED(static_cast<IUnknown*>(native)->QueryInterface(IID_PPV_ARGS(&texture))) || !texture) {
        return false;
    }
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0) {
        texture->Release();
        return false;
    }

    // Hands the QueryInterface reference to the caller; ReplacePendingLocked adopts it.
    out->resource = texture;
    out->state = static_cast<D3D12_RESOURCE_STATES>(state);
    out->format = desc.Format;
    out->width = static_cast<uint32_t>(desc.Width);
    out->height = desc.Height;
    return true;
}

void RememberUiTag(const void* resource, uint32_t bufferType) {
    if (bufferType != api::kV1BufferTypeUIColorAndAlpha) {
        return;
    }
    // Hot-path gate, same as the 2.x route's: while the bootstrap is dormant nothing here
    // is worth a probe, a QueryInterface or a held reference.
    if (!ce::dx12_streamline_ui_overlay::IsFrameTagTrackingActive()) {
        return;
    }
    if (ShouldKeepPureObserverOnlyStreamlineBehavior() ||
        !streamline_hook_g_StreamlineUsesD3D12.load(std::memory_order_acquire)) {
        return;
    }

    PendingUiTag tag;
    if (!TryReadV1UiResource(resource, &tag)) {
        static std::atomic<uint32_t> s_rejectLogs{0};
        LogOnce(s_rejectLogs, 4,
                "Streamline Hook: a 1.x UIColorAndAlpha tag did not verify as an ID3D12Resource "
                "(resource=%p) - the official-UI overlay route stays off for it",
                resource);
        return;
    }

    std::lock_guard<std::mutex> lock(g_pendingMutex);
    ReplacePendingLocked(tag);

    static std::atomic<uint32_t> s_acceptLogs{0};
    LogOnce(s_acceptLogs, 4,
            "Streamline Hook: remembered a 1.x UIColorAndAlpha tag (resource=%p native=%p %ux%u format=%u "
            "state=0x%X) for the next slEvaluateFeature command list",
            resource, static_cast<void*>(tag.resource), tag.width, tag.height, static_cast<unsigned>(tag.format),
            static_cast<unsigned>(tag.state));
}

// Streamline 1.x has no slSetD3DDevice, so the 2.x route's "Streamline accepted a D3D12
// device" signal never fires here. The first evaluate whose command buffer answers as an
// ID3D12GraphicsCommandList proves the same thing from evidence, and arms the same
// preactivation standby the 2.x path arms - without it the official-UI overlay route stays
// dormant for the whole process and DLSS-G's first generated frames lose the overlay.
void PublishD3D12StreamlineOnce(void* commandBuffer) {
    if (g_d3d12Published.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    streamline_hook_g_StreamlineUsesD3D12.store(true, std::memory_order_release);
    if (!ShouldKeepPureObserverOnlyStreamlineBehavior()) {
        ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(2);
        HookLogImportant(
            "Streamline Hook: 1.x evaluate presented a D3D12 command list (%p) - official UI preactivation "
            "standby ready before tags",
            commandBuffer);
    }
}

// Consume the remembered tag against this evaluate's command list. The tag is taken
// unconditionally so the held reference never outlives one evaluate, whether or not the
// overlay wanted it.
void RecordUiOverlayIfArmed(ID3D12GraphicsCommandList* commandList, uint32_t frameIndex) {
    if (!g_hasPending.load(std::memory_order_acquire)) {
        return;
    }
    PendingUiTag tag;
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        tag = g_pending;
        g_pending = PendingUiTag{};
        g_hasPending.store(false, std::memory_order_release);
    }
    if (!tag.resource) {
        return;
    }

    // Same arming gate as the 2.x route: only record while the bootstrap actually needs a
    // standby/activation record. `frameIndex` is Streamline's own per-frame counter, which
    // is exactly the distinct-per-frame identity OnFrameTag wants; the low bit is set so a
    // frame index of zero is still a non-null token.
    const auto token = reinterpret_cast<const void*>((static_cast<uintptr_t>(frameIndex) << 1u) | 1u);
    if (commandList && ce::dx12_streamline_ui_overlay::OnFrameTag(token)) {
        ID3D12CommandQueue* initializationQueue = DX12_AcquireOriginalGameQueueForOverlay();
        if (initializationQueue) {
            ce::dx12_streamline_ui_overlay::RecordRequest request;
            request.commandList = commandList;
            request.uiResource = tag.resource;
            request.initializationQueue = initializationQueue;
            request.resourceState = tag.state;
            request.format = tag.format;
            request.width = tag.width;
            request.height = tag.height;
            request.hdr = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(tag.format);
            request.frameToken = token;
            const bool recorded = ce::dx12_streamline_ui_overlay::TryRecordBootstrap(request);
            initializationQueue->Release();
            static std::atomic<uint32_t> s_recordLogs{0};
            LogOnce(s_recordLogs, 4,
                    "Streamline Hook: 1.x official-UI overlay record on the evaluate command list "
                    "(frame=%u recorded=%d)",
                    frameIndex, recorded ? 1 : 0);
        }
    }

    tag.resource->Release();
}

}  // namespace

void ForgetPendingUiTag(const char* reason) {
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    if (!g_pending.resource) {
        return;
    }
    HookLogImportant("Streamline Hook: dropped the pending 1.x UI tag (%s)", reason ? reason : "unspecified");
    ReplacePendingLocked(PendingUiTag{});
}

bool Hooked_slSetTagV1(const void* resource, uint32_t bufferType, uint32_t id, const void* extent) {
    // The saved original is one process-global slot shared by both generations; only the
    // generation-matched hook is ever installed, so this cast restores the shape the
    // interposer really has.
    auto original = reinterpret_cast<PFN_slSetTagV1>(GetCallableOriginalSetTag());
    if (!original) {
        return false;
    }
    if (HookIsShuttingDown()) {
        // Teardown runs on the game's own thread, not the loader lock, so releasing the
        // remembered texture here is safe and keeps CE from outliving its own reference.
        ForgetPendingUiTag("hook shutting down");
        return original(resource, bufferType, id, extent);
    }

    RememberUiTag(resource, bufferType);
    return original(resource, bufferType, id, extent);
}

bool Hooked_slEvaluateFeatureV1(void* commandBuffer, uint32_t feature, uint32_t frameIndex, uint32_t id) {
    auto original = reinterpret_cast<PFN_slEvaluateFeatureV1>(GetCallableOriginalEvaluateFeature());
    if (!original) {
        return false;
    }
    if (HookIsShuttingDown()) {
        ForgetPendingUiTag("hook shutting down");
        return original(commandBuffer, feature, frameIndex, id);
    }

    // The command list is resolved once and reused: it both proves the D3D12 route and
    // carries the overlay draw. Neither is needed once the route is established and no tag
    // is waiting, so the steady state costs two atomic loads and nothing else.
    ID3D12GraphicsCommandList* commandList = nullptr;
    const bool needsCommandList =
        !g_d3d12Published.load(std::memory_order_acquire) || g_hasPending.load(std::memory_order_acquire);
    if (needsCommandList && commandBuffer && LooksCallableAsCom(commandBuffer)) {
        if (FAILED(static_cast<IUnknown*>(commandBuffer)->QueryInterface(IID_PPV_ARGS(&commandList)))) {
            commandList = nullptr;
        }
    }
    if (commandList) {
        PublishD3D12StreamlineOnce(commandBuffer);
    }
    // Append CE before Streamline observes the UI layer, exactly as the 2.x route does.
    RecordUiOverlayIfArmed(commandList, frameIndex);
    if (commandList) {
        commandList->Release();
    }

    const bool result = original(commandBuffer, feature, frameIndex, id);
    if (result && feature == kV1FeatureDLSS) {
        const uint32_t previous =
            streamline_hook_g_LastUpscalerEvaluation.exchange(feature, std::memory_order_acq_rel);
        if (g_IPC && g_IPC->GetSharedMem()) {
            auto& state = g_IPC->GetSharedMem()->dlssState;
            // 1.x has no ray-reconstruction feature at all, so RR can only be off here.
            state.rrActive.store(false, std::memory_order_release);
            state.srActive.store(true, std::memory_order_release);
        }
        if (feature != previous) {
            static std::atomic<uint32_t> s_transitionLogs{0};
            LogOnce(s_transitionLogs, 4, "Streamline: 1.x DLSS evaluation confirmed (frame=%u)", frameIndex);
        }
    }
    return result;
}

}  // namespace ce::streamline_v1

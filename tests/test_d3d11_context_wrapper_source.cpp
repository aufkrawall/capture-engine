#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "source_fragment_reader.h"

namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    return ce::test_source::ReadLogicalSource(path);
}

// The device-context wrapper is implemented across one internal header and three
// translation units. Assertions below describe the wrapper as a whole, so they
// run over the concatenation in declaration-then-definition order.
std::string ReadContextWrapperSources() {
    const auto dir = std::filesystem::current_path() / "hook" / "wrappers";
    std::string combined;
    for (const char* part : {"d3d11_devicecontext_wrap_internal.h", "d3d11_devicecontext_wrap.cpp",
                             "d3d11_devicecontext_forward.cpp", "d3d11_devicecontext_get.cpp"}) {
        const std::filesystem::path path = dir / part;
        EXPECT_TRUE(std::filesystem::exists(path)) << part;
        combined += ReadTextFile(path);
        combined += '\n';
    }
    return combined;
}

}  // namespace

TEST(D3D11ContextWrapperSourceTest, PromotesInheritedContextInterfacesIndependently) {
    const std::string text = ReadContextWrapperSources();
    ASSERT_FALSE(text.empty());

    EXPECT_NE(text.find("m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1))"), std::string::npos);
    EXPECT_NE(text.find("m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2))"), std::string::npos);
    EXPECT_NE(text.find("m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3))"), std::string::npos);
    EXPECT_NE(text.find("m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4))"), std::string::npos);
    EXPECT_EQ(text.find("else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1))))"), std::string::npos);
    EXPECT_EQ(text.find("IID_ID3D11DeviceContext1 && m_Version >= 1"), std::string::npos);
    EXPECT_NE(text.find("IID_ID3D11DeviceContext1 && m_pReal1"), std::string::npos);
    EXPECT_NE(text.find("ClearView requested without real Context1"), std::string::npos);
}

// Both D3D11 paths run forced AF through one engine (hook/d3d11/d3d11_af_engine.h). The
// wrapper and the raw hooks used to carry separate state machines and only one of them got
// each optimization (BioShock Infinite, 20261017_003122: the wrapper's per-slot variant
// lookups cost ~0.8 ms/frame while the raw path had the sampler-keyed memo).
TEST(D3D11ContextWrapperSourceTest, BothPathsDelegateForcedAFToTheSharedEngine) {
    const auto root = std::filesystem::current_path();
    const std::string context = ReadContextWrapperSources();
    for (const char* call : {"ce::d3d11_af::OnViews(access", "ce::d3d11_af::OnShader(access",
                             "ce::d3d11_af::OnSamplers(access, host", "ce::d3d11_af::BeforeDraw(access, host",
                             "ce::d3d11_af::TranslateToLogical(m_AF", "ce::d3d11_af::ClearState(m_AF)"}) {
        EXPECT_NE(context.find(call), std::string::npos) << call;
    }
    // No private copy of the state machine survives in the wrapper.
    for (const char* gone : {"m_TrackedSamplers", "m_RealSamplers", "m_ForcedSamplerVariants", "ResolveForcedAFSampler",
                             "AcquireWrapperReplacementSampler"}) {
        EXPECT_EQ(context.find(gone), std::string::npos) << gone;
    }
    // The wrapper still resets on every boundary that empties the real context.
    EXPECT_NE(context.find("if (!RestoreContextState)"), std::string::npos);
    EXPECT_NE(context.find("if (SUCCEEDED(hr) && !RestoreDeferredContextState)"), std::string::npos);
    EXPECT_NE(context.find("ClearForcedAFTracking();"), std::string::npos);
    const std::string wrapperHeader = ReadTextFile(root / "hook/wrappers/d3d11_devicecontext_wrap.h");
    EXPECT_NE(wrapperHeader.find("ce::d3d11_af::PixelState m_AF;"), std::string::npos);

    const std::string detours = ReadTextFile(root / "hook/d3d11/dx11_hook_detours.cpp");
    for (const char* call : {"RawAF_OnShader(context, pixelShader);", "RawAF_OnViews(context, startSlot, numViews",
                             "RawAF_OnSamplers(original, context, startSlot, numSamplers", "RawAF_BeforeDraw(context);",
                             "RawAF_TranslateToLogical(context, startSlot, numSamplers, samplers);"}) {
        EXPECT_NE(detours.find(call), std::string::npos) << call;
    }
    // The raw hooks now see what only the wrapper saw: state resets and read-back.
    const std::string install = ReadTextFile(root / "hook/d3d11/dx11_hook_install.cpp");
    for (const char* slot : {"kPSGetSamplers, (LPVOID)&DetourPSGetSamplers11",
                             "kClearState, (LPVOID)&DetourClearState11",
                             "(LPVOID)&DetourFinishCommandList11",
                             "(LPVOID)&DetourSwapDeviceContextState11"}) {
        EXPECT_NE(install.find(slot), std::string::npos) << slot;
    }
    // SwapDeviceContextState exists only on a Context1 vtable.
    EXPECT_LT(install.find("context->QueryInterface(IID_PPV_ARGS(&context1))"),
              install.find("(LPVOID)&DetourSwapDeviceContextState11"));
    // A deferred context at a reused address must not inherit a destroyed one's state.
    const size_t deferredClear = detours.find("ClearTrackedContextState11(*deferredContext);");
    ASSERT_NE(deferredClear, std::string::npos);
    EXPECT_LT(deferredClear, detours.find("InstallContextVTableHooks11(*deferredContext, \"CreateDeferredContext\");"));

    // The raw state step holds only its context's slot: no process-wide mutex and no map lookup
    // in the steady state, and a slot dropped behind the thread's cached lookup is detected.
    const std::string raw = ReadTextFile(root / "hook/d3d11/dx11_hook_af_raw.cpp");
    const size_t run = raw.find("void Run(Fn&& fn) {");
    const size_t runEnd = raw.find("bool IsClean(uint64_t epoch) {", run);
    ASSERT_NE(run, std::string::npos);
    ASSERT_NE(runEnd, std::string::npos);
    const std::string runBody = raw.substr(run, runEnd - run);
    EXPECT_EQ(runBody.find("g_slotsMutex"), std::string::npos) << "the state step must not take the map mutex";
    EXPECT_NE(runBody.find("if (slot->incarnation != incarnation) {"), std::string::npos);
    EXPECT_LT(runBody.find("ce::d3d11_af::DeferredReleaseScope releases;"), runBody.find("AcquireSlot(slot);"))
        << "COM releases of a step run after the slot is free";
    // A dropped state is moved out and released with no lock held.
    const size_t moved = raw.find("std::unique_ptr<D3D11PerContextState> retired = std::move(slot->state);");
    const size_t released = raw.find("retired.reset();");
    ASSERT_NE(moved, std::string::npos);
    ASSERT_NE(released, std::string::npos);
    EXPECT_LT(moved, released);
    // The cross-thread clear keeps a used context's sampler tracking (views only).
    const size_t clear = raw.find("void ReleaseTrackedShaderResources11() {");
    ASSERT_NE(clear, std::string::npos);
    EXPECT_NE(raw.find("ce::d3d11_af::DropViews(slot->state->af);", clear), std::string::npos);
}

// Object-owned facts: the shader plan and the view class live on the objects as private data,
// so no process-global map or lock sits on the per-bind path.
TEST(D3D11ContextWrapperSourceTest, ForcedAFFactsAreObjectOwned) {
    const auto root = std::filesystem::current_path();
    const std::string metadata = ReadTextFile(root / "hook/d3d11/d3d11_af_metadata.cpp");
    EXPECT_NE(metadata.find("SetPrivateDataInterface(kShaderMetadataGuid, handle)"), std::string::npos);
    EXPECT_NE(metadata.find("view->SetPrivateData(kViewCacheGuid"), std::string::npos);
    const std::string device = ReadTextFile(root / "hook/wrappers/d3d11_device_wrap.cpp");
    EXPECT_NE(device.find("ce::d3d11_af::RegisterViewMetadata(*ppSRView)"), std::string::npos);
    // Both CreateSamplerState paths apply the one creation policy.
    EXPECT_NE(device.find("ce::d3d11_af::CreateSamplerWithCreationPolicy(pSamplerDesc"), std::string::npos);
    const std::string rawCreate = ReadTextFile(root / "hook/d3d11/dx11_hook_present.cpp");
    EXPECT_NE(rawCreate.find("ce::d3d11_af::CreateSamplerWithCreationPolicy(pSamplerDesc"), std::string::npos);
    EXPECT_NE(device.find("ce::d3d11_af::RegisterShaderMetadata(*ppPixelShader"), std::string::npos);
    const std::string detours = ReadTextFile(root / "hook/d3d11/dx11_hook_detours.cpp");
    EXPECT_NE(detours.find("ce::d3d11_af::RegisterShaderMetadata(*pixelShader"), std::string::npos);

    // One replacement policy for both paths: creation-tagged samplers change only AF,
    // older ones get the whole policy at bind time.
    const std::string slow = ReadTextFile(root / "hook/d3d11/d3d11_af_engine_slow.cpp");
    EXPECT_NE(slow.find("HasCurrentD3D11SamplerCreationPolicy(logical, gfx)"), std::string::npos);
    EXPECT_NE(slow.find("ApplyD3D11BindTimeAF(desc, gfx, resolved.allowAF)"), std::string::npos);
    EXPECT_NE(slow.find("DX11Hook_ApplySamplerOverrides(desc, gfx, resolved.allowAF)"), std::string::npos);
    EXPECT_NE(slow.find("FindReplacementSampler11(device, desc)"), std::string::npos);
}

TEST(D3D11ContextWrapperSourceTest, TranslatedSamplersKeepTheFrontendPolicyAtEveryCreationBoundary) {
    const auto root = std::filesystem::current_path();
    // Game sampler creation (both paths share CreateSamplerWithCreationPolicy) and each path's
    // replacement creation.
    for (const char* path : {"hook/d3d11/d3d11_af_engine_slow.cpp", "hook/d3d11/dx11_hook_af_raw.cpp",
                             "hook/wrappers/d3d11_devicecontext_wrap.cpp"}) {
        const std::string source = ReadTextFile(root / path);
        EXPECT_NE(source.find("D3D11SamplerCreationScope translationScope;"), std::string::npos) << path;
    }
    const std::string native = ReadTextFile(root / "hook/d3d12/dx12_sampler_hooks.cpp");
    const size_t begin = native.find("Result ApplyDynamicSampler(");
    const size_t end = native.find("Result ApplyStaticSampler(", begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const std::string apply = native.substr(begin, end - begin);
    const size_t ownership = apply.find("D3D11OwnsTranslatedSampler()");
    ASSERT_NE(ownership, std::string::npos);
    EXPECT_LT(ownership, apply.find("dx12_sampler_policy::Apply(desc, gfx)"));
    EXPECT_NE(apply.find("return {};", ownership), std::string::npos);
}

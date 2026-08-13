#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/dx12_overlay_policy/ecl_recursion_break.h"

#include "source_fragment_reader.h"

namespace {

using ce::dx12_overlay_policy::ClassifyEclBreakTargetCandidate;
using ce::dx12_overlay_policy::EclBreakSelection;
using ce::dx12_overlay_policy::EclBreakTargetClass;
using ce::dx12_overlay_policy::SelectEclRecursionBreakTarget;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(Dx12EclRecursionBreakPolicyTest, NativeD3D12ModulesAreTheOnlyProvenNativeTargets) {
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, R"(C:\Windows\System32\d3d12.dll)"),
              EclBreakTargetClass::kNativeD3D12);
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, R"(C:\Windows\System32\D3D12Core.dll)"),
              EclBreakTargetClass::kNativeD3D12);
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, R"(C:\Windows\System32\dxgi.dll)"),
              EclBreakTargetClass::kOtherModule);
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, R"(C:\Windows\System32\game.exe)"),
              EclBreakTargetClass::kOtherModule);
}

TEST(Dx12EclRecursionBreakPolicyTest, ThirdPartyOverlayHooksAreClassifiedForeign) {
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, R"(C:\tools\reshade.dll)"),
              EclBreakTargetClass::kForeignOverlayHook);
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, R"(C:\tools\ReShade64.dll)"),
              EclBreakTargetClass::kForeignOverlayHook);
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, R"(C:\tools\SpecialK64.dll)"),
              EclBreakTargetClass::kForeignOverlayHook);
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, R"(C:\tools\OptiScaler.dll)"),
              EclBreakTargetClass::kForeignOverlayHook);
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, R"(C:\tools\gameoverlayrenderer64.dll)"),
              EclBreakTargetClass::kForeignOverlayHook);
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, R"(C:\tools\RTSSHooks64.dll)"),
              EclBreakTargetClass::kForeignOverlayHook);
}

TEST(Dx12EclRecursionBreakPolicyTest, OurOwnDetourIsClassifiedSelf) {
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, R"(C:\Program Files\CE\capture_hook_x64.dll)"),
              EclBreakTargetClass::kSelfHook);
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, R"(C:\Program Files\CE\capture_hook_x86.dll)"),
              EclBreakTargetClass::kSelfHook);
}

TEST(Dx12EclRecursionBreakPolicyTest, UnresolvedOrMissingPathsStayUnresolved) {
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(false, R"(C:\tools\reshade.dll)"),
              EclBreakTargetClass::kUnresolved);
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, ""), EclBreakTargetClass::kUnresolved);
    EXPECT_EQ(ClassifyEclBreakTargetCandidate(true, nullptr), EclBreakTargetClass::kUnresolved);
}

TEST(Dx12EclRecursionBreakPolicyTest, SelectionPrefersNativeTargetsInOrder) {
    EXPECT_EQ(SelectEclRecursionBreakTarget(EclBreakTargetClass::kNativeD3D12, EclBreakTargetClass::kNativeD3D12,
                                            EclBreakTargetClass::kNativeD3D12, EclBreakTargetClass::kNativeD3D12),
              EclBreakSelection::kPerQueueOriginal);
    EXPECT_EQ(SelectEclRecursionBreakTarget(EclBreakTargetClass::kNativeD3D12,
                                            EclBreakTargetClass::kForeignOverlayHook,
                                            EclBreakTargetClass::kNativeD3D12, EclBreakTargetClass::kNativeD3D12),
              EclBreakSelection::kRealD3D12Ecl);
    EXPECT_EQ(SelectEclRecursionBreakTarget(EclBreakTargetClass::kNativeD3D12,
                                            EclBreakTargetClass::kForeignOverlayHook,
                                            EclBreakTargetClass::kForeignOverlayHook,
                                            EclBreakTargetClass::kNativeD3D12),
              EclBreakSelection::kGlobalOriginal);
}

TEST(Dx12EclRecursionBreakPolicyTest, SelectionFallsOpenToUnknownModuleButNeverForeignOrSelf) {
    EXPECT_EQ(SelectEclRecursionBreakTarget(EclBreakTargetClass::kNativeD3D12, EclBreakTargetClass::kOtherModule,
                                            EclBreakTargetClass::kUnresolved, EclBreakTargetClass::kUnresolved),
              EclBreakSelection::kPerQueueOriginal);
    EXPECT_EQ(SelectEclRecursionBreakTarget(EclBreakTargetClass::kNativeD3D12,
                                            EclBreakTargetClass::kForeignOverlayHook,
                                            EclBreakTargetClass::kUnresolved, EclBreakTargetClass::kOtherModule),
              EclBreakSelection::kGlobalOriginal);
    EXPECT_EQ(SelectEclRecursionBreakTarget(EclBreakTargetClass::kNativeD3D12,
                                            EclBreakTargetClass::kForeignOverlayHook,
                                            EclBreakTargetClass::kForeignOverlayHook,
                                            EclBreakTargetClass::kForeignOverlayHook),
              EclBreakSelection::kNone);
    EXPECT_EQ(SelectEclRecursionBreakTarget(EclBreakTargetClass::kNativeD3D12, EclBreakTargetClass::kSelfHook,
                                            EclBreakTargetClass::kForeignOverlayHook, EclBreakTargetClass::kSelfHook),
              EclBreakSelection::kNone);
    EXPECT_EQ(SelectEclRecursionBreakTarget(EclBreakTargetClass::kUnresolved, EclBreakTargetClass::kUnresolved,
                                            EclBreakTargetClass::kUnresolved, EclBreakTargetClass::kUnresolved),
              EclBreakSelection::kNone);
}

TEST(Dx12EclRecursionBreakPolicyTest, ProxyQueuesOnlyForwardThroughTheirOwnOriginal) {
    // A native runtime ECL is type-unsafe for a ReShade proxy object; only the
    // original taken from the proxy's own vtable matches its layout.
    EXPECT_EQ(SelectEclRecursionBreakTarget(EclBreakTargetClass::kForeignOverlayHook,
                                            EclBreakTargetClass::kForeignOverlayHook,
                                            EclBreakTargetClass::kNativeD3D12, EclBreakTargetClass::kNativeD3D12),
              EclBreakSelection::kPerQueueOriginal);
    EXPECT_EQ(SelectEclRecursionBreakTarget(EclBreakTargetClass::kForeignOverlayHook,
                                            EclBreakTargetClass::kUnresolved,
                                            EclBreakTargetClass::kNativeD3D12, EclBreakTargetClass::kNativeD3D12),
              EclBreakSelection::kNone);
    EXPECT_EQ(SelectEclRecursionBreakTarget(EclBreakTargetClass::kOtherModule, EclBreakTargetClass::kOtherModule,
                                            EclBreakTargetClass::kNativeD3D12, EclBreakTargetClass::kNativeD3D12),
              EclBreakSelection::kPerQueueOriginal);
}

TEST(Dx12EclRecursionBreakPolicyTest, RecursionBreakUsesResolvedTargetAndNeverBlindGlobal) {
    const std::string source = ReadSource("hook/apis/dx12_hook_ecl.cpp");
    ASSERT_FALSE(source.empty());

    const size_t recursionGuard = source.find("if (s_eclRecursionDepth > 0)");
    ASSERT_NE(recursionGuard, std::string::npos);

    const size_t resolveCall = source.find("ResolveECLRecursionBreakTarget(pThis)", recursionGuard);
    const size_t breakForward = source.find("breakTarget(pThis, NumCommandLists, ppCommandLists)", recursionGuard);
    const size_t outerDepthIncrement =
        source.find("++s_eclRecursionDepth;\n    auto depthGuard = ce::make_scope_guard");
    ASSERT_NE(resolveCall, std::string::npos);
    ASSERT_NE(breakForward, std::string::npos);
    ASSERT_NE(outerDepthIncrement, std::string::npos);
    EXPECT_LT(recursionGuard, resolveCall);
    EXPECT_LT(resolveCall, breakForward);
    EXPECT_LT(breakForward, outerDepthIncrement);

    // The break path must not call the global original directly: that is what
    // re-entered ReShade's proxy hook in Talos (session 20260813_041416).
    const std::string breakBlock = source.substr(recursionGuard, outerDepthIncrement - recursionGuard);
    EXPECT_EQ(breakBlock.find("oExecuteCommandLists(pThis"), std::string::npos);
}

TEST(Dx12EclRecursionBreakPolicyTest, QueueVTableHookPublishesNativeOriginalEagerly) {
    const std::string source = ReadSource("hook/apis/dx12_hook_ecl_install.cpp");
    ASSERT_FALSE(source.empty());

    const size_t originalSave = source.find("dx12_hook_g_ExecuteCommandListsOriginalByVTable[vtbl] = original;");
    const size_t eagerPublish = source.find("TryPublishRealD3D12ECLCandidate(original, \"fresh queue vtable hook\");");
    ASSERT_NE(originalSave, std::string::npos);
    ASSERT_NE(eagerPublish, std::string::npos);
    EXPECT_LT(originalSave, eagerPublish);
}

TEST(Dx12EclRecursionBreakPolicyTest, NullVtableForwardPrefersResolvedNativeEcl) {
    const std::string source = ReadSource("hook/apis/dx12_hook_ecl.cpp");
    ASSERT_FALSE(source.empty());

    const size_t nullVtableForward = source.find("real = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);");
    const size_t globalFallback = source.find("real = oExecuteCommandLists;");
    ASSERT_NE(nullVtableForward, std::string::npos);
    ASSERT_NE(globalFallback, std::string::npos);
    EXPECT_LT(nullVtableForward, globalFallback);
}

TEST(Dx12EclRecursionBreakPolicyTest, SignalTraceDetourForwardsPerVtableOriginalNotBlindGlobal) {
    const std::string source = ReadSource("hook/apis/dx12_hook_ecl_install.cpp");
    ASSERT_FALSE(source.empty());

    const size_t detourBegin =
        source.find("HRESULT STDMETHODCALLTYPE DetourTraceCommandQueueSignal(ID3D12CommandQueue* queue,");
    ASSERT_NE(detourBegin, std::string::npos);

    const size_t perVtableLookup =
        source.find("dx12_hook_g_CommandQueueSignalOriginalByVTable.find(vtbl)", detourBegin);
    const size_t nativeFallback = source.find("dx12_hook_g_RealD3D12Signal.load(std::memory_order_acquire)", detourBegin);
    const size_t globalFallback = source.find("original = oTraceCommandQueueSignal;", detourBegin);
    const size_t forward = source.find("original(queue, fence, value)", detourBegin);
    ASSERT_NE(perVtableLookup, std::string::npos);
    ASSERT_NE(nativeFallback, std::string::npos);
    ASSERT_NE(globalFallback, std::string::npos);
    ASSERT_NE(forward, std::string::npos);
    EXPECT_LT(perVtableLookup, nativeFallback);
    EXPECT_LT(nativeFallback, globalFallback);
    EXPECT_LT(globalFallback, forward);

    const size_t detourEnd = source.find("Dx12TraceLog(\"Signal\"", detourBegin);
    ASSERT_NE(detourEnd, std::string::npos);
    const std::string detourBody = source.substr(detourBegin, detourEnd - detourBegin);
    EXPECT_EQ(detourBody.find("oTraceCommandQueueSignal(queue, fence, value)"), std::string::npos);
}

TEST(Dx12EclRecursionBreakPolicyTest, SignalVTableHookStoresPerVtableOriginalAndPublishesNativeSignal) {
    const std::string source = ReadSource("hook/apis/dx12_hook_ecl_install.cpp");
    ASSERT_FALSE(source.empty());

    const size_t originalSave = source.find("dx12_hook_g_CommandQueueSignalOriginalByVTable[vtbl] = origSignal;");
    const size_t eagerPublish = source.find("TryPublishRealD3D12SignalCandidate(origSignal, \"fresh queue vtable hook\");");
    ASSERT_NE(originalSave, std::string::npos);
    ASSERT_NE(eagerPublish, std::string::npos);
    EXPECT_LT(originalSave, eagerPublish);
}

}  // namespace

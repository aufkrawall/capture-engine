#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../hook/common/d3d12_device_creation_policy.h"

#include "source_fragment_reader.h"

namespace {

namespace policy = ce::d3d12_device_creation;

using policy::AccumulateAdapter;
using policy::AdapterProbe;
using policy::Classify;
using policy::ClassifyEntryPatch;
using policy::EntryPatchKind;
using policy::Evidence;
using policy::IsTerminalCreationFailure;
using policy::ShouldEmitReport;
using policy::ShouldRetryTempDeviceCreation;
using policy::Verdict;

constexpr uint32_t kUnsupported = 0x887A0004u;
constexpr uint32_t kDeviceRemoved = 0x887A0005u;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

std::vector<uint8_t> Bytes(std::initializer_list<uint8_t> values) {
    std::vector<uint8_t> out(values);
    out.resize(16, 0xCC);
    return out;
}

TEST(D3D12DeviceCreationPolicyTest, RecognizesTheFourForeignEntryShapes) {
    const auto clean = Bytes({0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83});
    EXPECT_EQ(ClassifyEntryPatch(clean.data(), clean.size()), EntryPatchKind::None);

    const auto rel = Bytes({0xE9, 0x00, 0x10, 0x00, 0x00});
    EXPECT_EQ(ClassifyEntryPatch(rel.data(), rel.size()), EntryPatchKind::RelativeJump);

    const auto indirect = Bytes({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ(ClassifyEntryPatch(indirect.data(), indirect.size()), EntryPatchKind::IndirectJump);

    const auto movabs =
        Bytes({0x48, 0xB8, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0xFF, 0xE0});
    EXPECT_EQ(ClassifyEntryPatch(movabs.data(), movabs.size()), EntryPatchKind::AbsoluteMovJump);

    const auto pushRet = Bytes({0x68, 0x44, 0x33, 0x22, 0x11, 0xC3});
    EXPECT_EQ(ClassifyEntryPatch(pushRet.data(), pushRet.size()), EntryPatchKind::PushRet);

    EXPECT_EQ(ClassifyEntryPatch(nullptr, 16), EntryPatchKind::None);
    EXPECT_EQ(ClassifyEntryPatch(rel.data(), 3), EntryPatchKind::None);
}

TEST(D3D12DeviceCreationPolicyTest, ResolvesEntryPatchTargetsWithoutTouchingProcessMemory) {
    const auto rel = Bytes({0xE9, 0x10, 0x00, 0x00, 0x00});
    uint64_t target = 0;
    ASSERT_TRUE(policy::TryComputeRelativeJumpTarget(rel.data(), rel.size(), 0x7FF000001000ull, &target));
    EXPECT_EQ(target, 0x7FF000001015ull);

    // A negative displacement must sign-extend, not wrap to a 4 GB-away address.
    const auto back = Bytes({0xE9, 0xF0, 0xFF, 0xFF, 0xFF});
    ASSERT_TRUE(policy::TryComputeRelativeJumpTarget(back.data(), back.size(), 0x7FF000001000ull, &target));
    EXPECT_EQ(target, 0x7FF000000FF5ull);

    const auto indirect = Bytes({0xFF, 0x25, 0x20, 0x00, 0x00, 0x00});
    uint64_t slot = 0;
    ASSERT_TRUE(policy::TryComputeIndirectJumpSlot(indirect.data(), indirect.size(), 0x7FF000002000ull, &slot));
    EXPECT_EQ(slot, 0x7FF000002026ull);

    const auto movabs =
        Bytes({0x48, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE0});
    ASSERT_TRUE(policy::TryComputeAbsoluteMovTarget(movabs.data(), movabs.size(), &target));
    EXPECT_EQ(target, 0xDEADBEEFull);

    EXPECT_FALSE(policy::TryComputeRelativeJumpTarget(indirect.data(), indirect.size(), 0, &target));
    EXPECT_FALSE(policy::TryComputeIndirectJumpSlot(rel.data(), rel.size(), 0, &slot));
    EXPECT_FALSE(policy::TryComputeAbsoluteMovTarget(rel.data(), rel.size(), &target));
}

TEST(D3D12DeviceCreationPolicyTest, BlamesTheForeignPatchOnlyWhenTheUnpatchedBodySucceeds) {
    Evidence evidence;
    AccumulateAdapter(&evidence, AdapterProbe{/*software=*/false, /*baseline=*/true, /*top=*/true});
    evidence.entryForeignPatched = true;
    evidence.bypassAttempted = true;
    evidence.bypassCreatedDevice = true;
    evidence.hookedCreatedDevice = false;
    EXPECT_EQ(Classify(evidence), Verdict::ForeignEntryPatchRejectsCreation);

    // Both paths failing is proof the patch is innocent, not proof it is guilty.
    evidence.bypassCreatedDevice = false;
    EXPECT_NE(Classify(evidence), Verdict::ForeignEntryPatchRejectsCreation);

    // A patch nobody compared against cannot be blamed either.
    evidence.bypassAttempted = false;
    evidence.bypassCreatedDevice = true;
    EXPECT_NE(Classify(evidence), Verdict::ForeignEntryPatchRejectsCreation);
}

TEST(D3D12DeviceCreationPolicyTest, SeparatesARefusedRuntimeFromALimitedAdapter) {
    Evidence noAdapters;
    EXPECT_EQ(Classify(noAdapters), Verdict::NoAdapterEnumerated);

    Evidence enumerationFailed;
    AccumulateAdapter(&enumerationFailed, AdapterProbe{false, true, true});
    enumerationFailed.adapterEnumerationFailed = true;
    EXPECT_EQ(Classify(enumerationFailed), Verdict::NoAdapterEnumerated);

    // Every hardware adapter refuses even the baseline, but D3D11 still gets a device on
    // the same GPU: the problem is D3D12's, in this process.
    Evidence refused;
    AccumulateAdapter(&refused, AdapterProbe{/*software=*/false, /*baseline=*/false, /*top=*/false});
    AccumulateAdapter(&refused, AdapterProbe{/*software=*/true, /*baseline=*/false, /*top=*/false});
    refused.d3d11Attempted = true;
    refused.d3d11CreatedDevice = true;
    EXPECT_EQ(Classify(refused), Verdict::RuntimeRejectsEveryAdapter);

    // Never asked D3D11 - the report cannot claim a driver-level refusal it did not test for.
    Evidence untested = refused;
    untested.d3d11Attempted = false;
    untested.d3d11CreatedDevice = false;
    EXPECT_EQ(Classify(untested), Verdict::RuntimeRejectsEveryAdapter);

    // D3D11 refused on the same enumerable hardware adapter: the driver is refusing the
    // process itself. This is the Witcher 3 shape - on the reporting machine a process
    // named witcher3.exe is refused DXGI_ERROR_UNSUPPORTED by both D3D11 and D3D12 while
    // the identical binary under any other name gets a device.
    Evidence driverRefusal = refused;
    driverRefusal.d3d11CreatedDevice = false;
    EXPECT_EQ(Classify(driverRefusal), Verdict::DisplayDriverRefusesThisProcess);

    // A software-only enumeration cannot support that claim: there is no hardware adapter
    // for the driver to have refused.
    Evidence softwareOnly;
    AccumulateAdapter(&softwareOnly, AdapterProbe{/*software=*/true, /*baseline=*/false, /*top=*/false});
    softwareOnly.d3d11Attempted = true;
    EXPECT_EQ(Classify(softwareOnly), Verdict::RuntimeRejectsEveryAdapter);

    // Hardware is fine and only the software adapter falls short of the top level - the
    // shape that aborts an application which probes every adapter and treats a failure as
    // fatal (Witcher 3 20260820_211008 threw "HRESULT of 0x887A0004" with the Microsoft
    // Basic Render Driver description live in the throwing frame).
    Evidence softwareLimited;
    AccumulateAdapter(&softwareLimited, AdapterProbe{/*software=*/false, /*baseline=*/true, /*top=*/true});
    AccumulateAdapter(&softwareLimited, AdapterProbe{/*software=*/true, /*baseline=*/true, /*top=*/false});
    EXPECT_EQ(Classify(softwareLimited), Verdict::HardwareOkSomeAdapterBelowTopFeatureLevel);

    Evidence healthy;
    AccumulateAdapter(&healthy, AdapterProbe{false, true, true});
    healthy.hookedCreatedDevice = true;
    EXPECT_EQ(Classify(healthy), Verdict::HardwareCreatesDevices);
}

TEST(D3D12DeviceCreationPolicyTest, EveryVerdictHasItsOwnExplanation) {
    const Verdict all[] = {Verdict::Inconclusive,
                           Verdict::NoAdapterEnumerated,
                           Verdict::ForeignEntryPatchRejectsCreation,
                           Verdict::DisplayDriverRefusesThisProcess,
                           Verdict::RuntimeRejectsEveryAdapter,
                           Verdict::HardwareOkSomeAdapterBelowTopFeatureLevel,
                           Verdict::HardwareCreatesDevices};
    for (const Verdict verdict : all) {
        const std::string text = policy::Describe(verdict);
        EXPECT_FALSE(text.empty());
        EXPECT_NE(text, "unclassified");
        for (const Verdict other : all) {
            if (other != verdict) {
                EXPECT_NE(text, std::string(policy::Describe(other)));
            }
        }
    }
}

TEST(D3D12DeviceCreationPolicyTest, TerminalFailuresStopBeingRetriedButTransientOnesDoNot) {
    EXPECT_TRUE(IsTerminalCreationFailure(static_cast<int32_t>(kUnsupported)));
    EXPECT_FALSE(IsTerminalCreationFailure(static_cast<int32_t>(kDeviceRemoved)));

    // A transient failure is always worth another attempt, however often it has happened.
    EXPECT_TRUE(ShouldRetryTempDeviceCreation(0, static_cast<int32_t>(kDeviceRemoved)));
    EXPECT_TRUE(ShouldRetryTempDeviceCreation(1000, static_cast<int32_t>(kDeviceRemoved)));

    // A terminal one gets a small budget and then stops, so the retry storm the Witcher 3
    // session logged (thirty D3D12CreateDevice calls in five seconds, one vendor UMD
    // map/unmap each) cannot happen again.
    for (int i = 0; i < policy::kMaxTerminalDeviceCreationAttempts; ++i) {
        EXPECT_TRUE(ShouldRetryTempDeviceCreation(i, static_cast<int32_t>(kUnsupported))) << "attempt " << i;
    }
    EXPECT_FALSE(ShouldRetryTempDeviceCreation(policy::kMaxTerminalDeviceCreationAttempts,
                                               static_cast<int32_t>(kUnsupported)));
    EXPECT_FALSE(ShouldRetryTempDeviceCreation(99, static_cast<int32_t>(kUnsupported)));

    // A fresh process has no recorded HRESULT yet and must not be talked out of its first try.
    EXPECT_TRUE(ShouldRetryTempDeviceCreation(0, 0));
}

TEST(D3D12DeviceCreationPolicyTest, ReportsOncePerDistinctHresultUnderAHardCap) {
    EXPECT_TRUE(ShouldEmitReport(0, 0, static_cast<int32_t>(kUnsupported)));
    EXPECT_FALSE(ShouldEmitReport(1, static_cast<int32_t>(kUnsupported), static_cast<int32_t>(kUnsupported)));
    EXPECT_TRUE(ShouldEmitReport(1, static_cast<int32_t>(kUnsupported), static_cast<int32_t>(kDeviceRemoved)));
    EXPECT_FALSE(ShouldEmitReport(policy::kMaxDeviceCreationReports, static_cast<int32_t>(kUnsupported),
                                  static_cast<int32_t>(kDeviceRemoved)));
}

TEST(D3D12DeviceCreationPolicyTest, BootstrapConsultsTheBudgetBeforePayingForDeviceCreation) {
    const std::string source = ReadSource("hook/apis/dx12_hook_hook_install.cpp");
    ASSERT_FALSE(source.empty());

    const size_t budget = source.find("ShouldAttemptTempDeviceCreation()");
    const size_t create = source.find("pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0", budget);
    const size_t note = source.find("NoteTempDeviceCreationResult(deviceHr)", create);
    const size_t report = source.find("ReportDeviceCreationFailure(", note);
    ASSERT_NE(budget, std::string::npos);
    ASSERT_NE(create, std::string::npos);
    ASSERT_NE(note, std::string::npos);
    ASSERT_NE(report, std::string::npos);
    EXPECT_LT(budget, create);
    EXPECT_LT(create, note);
    EXPECT_LT(note, report);
}

TEST(D3D12DeviceCreationPolicyTest, ReportProbesCapabilityWithoutCreatingADevice) {
    const std::string source = ReadSource("hook/apis/dx12_device_creation_report.cpp");
    ASSERT_FALSE(source.empty());

    // The whole point of the matrix is that it costs nothing: a null ppDevice makes D3D12
    // answer S_FALSE instead of building (and then tearing down) a real device per cell.
    EXPECT_NE(source.find("create(adapter, level, __uuidof(ID3D12Device), nullptr)"), std::string::npos);
    // The comparison that identifies a foreign patch must run both sides.
    const size_t live = source.find("via the live entry");
    const size_t bypassed = source.find("past the foreign entry patch");
    ASSERT_NE(live, std::string::npos);
    ASSERT_NE(bypassed, std::string::npos);
    EXPECT_LT(live, bypassed);
}

TEST(D3D12DeviceCreationPolicyTest, ReportAsksD3D11OnlyWhenTheAnswerChangesTheVerdict) {
    const std::string source = ReadSource("hook/apis/dx12_device_creation_report.cpp");
    ASSERT_FALSE(source.empty());

    // A healthy process must not pay for a D3D11 device it does not need, and a report that
    // never asked must not be able to claim a driver-level refusal.
    const size_t gate = source.find("evidence.hardwareAdapters > 0 && evidence.hardwareAdaptersAtBaseline == 0");
    const size_t probe = source.find("ProbeD3D11(&evidence)", gate);
    const size_t classify = source.find("policy::Classify(evidence)", probe);
    ASSERT_NE(gate, std::string::npos);
    ASSERT_NE(probe, std::string::npos);
    ASSERT_NE(classify, std::string::npos);
    EXPECT_LT(gate, probe);
    EXPECT_LT(probe, classify);
}

}  // namespace

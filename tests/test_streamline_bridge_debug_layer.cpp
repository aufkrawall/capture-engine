#include <gtest/gtest.h>

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "hook/streamline/streamline_bridge_policy.h"

// Session 20261001_034038: with streamline_upgrade=true The Witcher 3 configured the D3D12
// debug layer (ID3D12Debug -> ID3D12Debug5::SetEnableAutoName) after releasing its probe
// device. CE, Streamline 2.x and NGX still held that device, the configuration reset it, and
// the title's next adapter probe got DXGI_ERROR_DEVICE_RESET and threw 0xE06D7363.
namespace {

namespace bridge = ce::streamline_bridge;

bridge::InterfaceId FromGuid(const GUID& guid) {
    bridge::InterfaceId id{};
    std::memcpy(&id, &guid, sizeof(id));
    return id;
}

std::string ReadProjectSource(const std::filesystem::path& relativePath) {
    std::ifstream stream(std::filesystem::current_path() / relativePath);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

TEST(StreamlineBridgeDebugLayerTest, RefusesTheWitcherDebugConfigurationWhileADeviceIsRetained) {
    // The two ids witcher3+0x7f7a70 actually passes, decoded from the shipped executable.
    const bridge::InterfaceId debug = {
        0x344488b7, 0x6846, 0x474b, {0xb9, 0x89, 0xf0, 0x27, 0x44, 0x82, 0x45, 0xe0}};
    const bridge::InterfaceId debug5 = {
        0x548d6b12, 0x09fa, 0x40e0, {0x90, 0x69, 0x5d, 0xcd, 0x58, 0x9a, 0x52, 0xc9}};

    EXPECT_TRUE(bridge::ShouldRefuseDebugLayerConfiguration(debug, /*bridgeKeepsDeviceAlive=*/true));
    EXPECT_TRUE(bridge::ShouldRefuseDebugLayerConfiguration(debug5, /*bridgeKeepsDeviceAlive=*/true));
}

TEST(StreamlineBridgeDebugLayerTest, ForwardsDebugConfigurationBeforeAnyDeviceExists) {
    // Without a retained device the layer still applies to the title's first device, which is
    // exactly what the title asked for.
    EXPECT_FALSE(bridge::ShouldRefuseDebugLayerConfiguration(FromGuid(__uuidof(ID3D12Debug)),
                                                             /*bridgeKeepsDeviceAlive=*/false));
    EXPECT_FALSE(bridge::ShouldRefuseDebugLayerConfiguration(FromGuid(__uuidof(ID3D12Debug5)),
                                                             /*bridgeKeepsDeviceAlive=*/false));
}

TEST(StreamlineBridgeDebugLayerTest, ClassifiesEveryDebugLayerInterfaceTheSdkDeclares) {
    // The table must agree with the SDK headers byte for byte; a transposed byte would let the
    // reset through silently.
    EXPECT_TRUE(bridge::IsD3D12DebugLayerConfigurationIid(FromGuid(__uuidof(ID3D12Debug))));
    EXPECT_TRUE(bridge::IsD3D12DebugLayerConfigurationIid(FromGuid(__uuidof(ID3D12Debug1))));
    EXPECT_TRUE(bridge::IsD3D12DebugLayerConfigurationIid(FromGuid(__uuidof(ID3D12Debug2))));
    EXPECT_TRUE(bridge::IsD3D12DebugLayerConfigurationIid(FromGuid(__uuidof(ID3D12Debug3))));
    EXPECT_TRUE(bridge::IsD3D12DebugLayerConfigurationIid(FromGuid(__uuidof(ID3D12Debug4))));
    EXPECT_TRUE(bridge::IsD3D12DebugLayerConfigurationIid(FromGuid(__uuidof(ID3D12Debug5))));
    // ID3D12Debug6 postdates the toolchain headers; Windows SDK 10.0.26100 declares it as
    // 82a816d6-5d01-4157-97d0-4975463fd1ed.
    const bridge::InterfaceId debug6 = {
        0x82a816d6, 0x5d01, 0x4157, {0x97, 0xd0, 0x49, 0x75, 0x46, 0x3f, 0xd1, 0xed}};
    EXPECT_TRUE(bridge::IsD3D12DebugLayerConfigurationIid(debug6));
    EXPECT_EQ(sizeof(bridge::kD3D12DebugLayerConfigurationIids) / sizeof(bridge::InterfaceId), 7u);
}

TEST(StreamlineBridgeDebugLayerTest, LeavesOtherDebugInterfacesAlone) {
    // DRED settings and the device-level debug interfaces are not layer configuration.
    const bridge::InterfaceId dredSettings = {
        0x82bc481c, 0x6b9b, 0x4030, {0xae, 0xdb, 0x7e, 0xe3, 0xd1, 0xdf, 0x1e, 0x63}};
    EXPECT_FALSE(bridge::ShouldRefuseDebugLayerConfiguration(dredSettings, /*bridgeKeepsDeviceAlive=*/true));
    EXPECT_FALSE(bridge::ShouldRefuseDebugLayerConfiguration(FromGuid(__uuidof(ID3D12DebugDevice)),
                                                             /*bridgeKeepsDeviceAlive=*/true));
    EXPECT_FALSE(bridge::ShouldRefuseDebugLayerConfiguration(FromGuid(__uuidof(IUnknown)),
                                                             /*bridgeKeepsDeviceAlive=*/true));
    // One differing byte is a different interface.
    bridge::InterfaceId nearMiss = FromGuid(__uuidof(ID3D12Debug5));
    nearMiss.data4[7] ^= 0x01;
    EXPECT_FALSE(bridge::IsD3D12DebugLayerConfigurationIid(nearMiss));
}

TEST(StreamlineBridgeDebugLayerTest, BridgedEntryPointRefusesBeforeReachingD3D12) {
    // A refusal after forwarding would be too late: the reset may happen inside the call.
    const std::string source = ReadProjectSource("hook/streamline/streamline_bridge.cpp");
    ASSERT_FALSE(source.empty());
    const size_t entry = source.find("HRESULT WINAPI Bridged_D3D12GetDebugInterface(");
    const size_t refuse = source.find("ShouldRefuseDebugLayerConfiguration(requested, HasRetainedDevice())", entry);
    const size_t answer = source.find("return DXGI_ERROR_SDK_COMPONENT_MISSING;", refuse);
    const size_t forward = source.find("V2Target(\"D3D12GetDebugInterface\")", entry);
    ASSERT_NE(entry, std::string::npos);
    ASSERT_NE(refuse, std::string::npos);
    ASSERT_NE(answer, std::string::npos);
    ASSERT_NE(forward, std::string::npos);
    EXPECT_LT(refuse, forward);
    EXPECT_LT(answer, forward);
    EXPECT_NE(source.find("Streamline bridge: refusing D3D12GetDebugInterface(iid=%s)"), std::string::npos);
}

}  // namespace

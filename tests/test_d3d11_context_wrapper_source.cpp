#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

}  // namespace

TEST(D3D11ContextWrapperSourceTest, PromotesInheritedContextInterfacesIndependently) {
    const std::filesystem::path source =
        std::filesystem::current_path() / "hook" / "wrappers" / "d3d11_devicecontext_wrap.cpp";
    ASSERT_TRUE(std::filesystem::exists(source));

    const std::string text = ReadTextFile(source);
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

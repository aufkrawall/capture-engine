#include <vulkan/vulkan.h>
#include <iostream>

int main() {
    std::cout << "VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT = " << VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT << std::endl;
    std::cout << "VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT = " << VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT << std::endl;
    std::cout << "VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT = " << VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT << std::endl;
    return 0;
}

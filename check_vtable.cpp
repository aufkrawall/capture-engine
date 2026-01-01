#include <iostream>
#include <d3d12.h>
#include <dxgi1_4.h>

// Helper to get vtable index
template <typename T, typename F>
int GetVTableIndex(F T::*func) {
    union {
        F T::*func;
        void* ptr;
        struct {
            void* ptr;
            size_t adj;
        } s;
    } u;
    u.func = func;
    // It's a bit of a hack and compiler dependent (MSVC vs GCC/Clang).
    // For MinGW/Clang on Windows, the address is typically thunk or offset.
    // A more reliable way for runtime check:
    return -1; 
}

int main() {
    // We can't easily get compile-time index portably without extensive template magic.
    // However, we can use a dummy interface and print indices if we know the order.
    // Actually, simply printing the sizeof/inheritance might not be enough.
    
    // Alternative: Create a dummy class implementing the interface and see where it calls? No.
    
    // Best way: Just count the methods.
    // IUnknown: 3
    // ID3D12Object: +4 = 7
    // ID3D12DeviceChild: +1 = 8
    // ID3D12Pageable: +0 = 8
    // ID3D12CommandQueue: 
    //   8: UpdateTileMappings
    //   9: CopyTileMappings
    //   10: ExecuteCommandLists ?
    
    std::cout << "Checking d3d12.h..." << std::endl;
    // We will just compile this and if it compiles, we are good.
    // But we want to know the index.
    
    // Let's rely on the textual include.
    
    return 0;
}

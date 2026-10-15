#pragma once

#include "hook/d3d9/d3d9_ex_flag_layout.h"

#include <cstddef>
#include <cstdint>
#include <map>

// Sparse byte map standing in for process memory in the D3D9 layout tests: every
// read outside the bytes a test put fails, like an unmapped page.
class D3D9FakeMemory final : public ce::d3d9_ex_flag::MemoryReader {
public:
    void PutPointer(uintptr_t address, uint64_t value, size_t size = sizeof(uintptr_t)) { Put(address, &value, size); }
    void PutDword(uintptr_t address, uint32_t value) { Put(address, &value, sizeof(value)); }
    void PutWord(uintptr_t address, uint16_t value) { Put(address, &value, sizeof(value)); }

    bool Read(uintptr_t address, void* out, size_t size) const override {
        auto* bytes = static_cast<uint8_t*>(out);
        for (size_t i = 0; i < size; ++i) {
            const auto it = bytes_.find(address + i);
            if (it == bytes_.end())
                return false;
            bytes[i] = it->second;
        }
        return true;
    }

private:
    void Put(uintptr_t address, const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i)
            bytes_[address + i] = static_cast<const uint8_t*>(data)[i];
    }
    std::map<uintptr_t, uint8_t> bytes_;
};

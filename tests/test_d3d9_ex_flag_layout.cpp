#include <gtest/gtest.h>

#include "hook/d3d9/d3d9_ex_flag_layout.h"

#include <cstring>
#include <map>
#include <vector>

using namespace ce::d3d9_ex_flag;

namespace {

struct Expected {
    std::vector<uint8_t> code;
    uint32_t factoryOffset;
    uint32_t flagOffset;
};

std::vector<Candidate> Find(const std::vector<uint8_t>& code, CodeArch arch) {
    Candidate out[kMaxCandidates] = {};
    const size_t count = FindCandidates(code.data(), code.size(), arch, out, kMaxCandidates);
    return {out, out + count};
}

// Ex-gate sequences from shipped d3d9.dll builds (Windows 7 through 11 22H2), padded with
// unrelated prologue/epilogue bytes so the scan has to find them mid-function.
std::vector<uint8_t> Embed(std::vector<uint8_t> gate) {
    std::vector<uint8_t> code = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20};
    code.insert(code.end(), gate.begin(), gate.end());
    code.insert(code.end(), {0xC3, 0xCC, 0xCC, 0xCC});
    return code;
}

class FakeMemory final : public MemoryReader {
public:
    void PutPointer(uintptr_t address, uintptr_t value) { Put(address, &value, sizeof(value)); }
    void PutDword(uintptr_t address, uint32_t value) { Put(address, &value, sizeof(value)); }

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

constexpr uintptr_t kDevice = 0x10000000;
constexpr uintptr_t kFactory = 0x20000000;

}  // namespace

TEST(D3D9ExFlagLayoutTest, DecodesEveryKnownX64GateShape) {
    const Expected cases[] = {
        // Windows 7: mov rax,[rbx+3DB8h]; cmp [rax+5068h],r15d; jnz
        {{0x48, 0x8B, 0x83, 0xB8, 0x3D, 0x00, 0x00, 0x44, 0x39, 0xB8, 0x68, 0x50, 0x00, 0x00, 0x75, 0x12}, 0x3DB8,
         0x5068},
        // Windows 10 1709: mov rax,[r15+4178h]; cmp [rax+51E0h],ebx; jnz
        {{0x49, 0x8B, 0x87, 0x78, 0x41, 0x00, 0x00, 0x39, 0x98, 0xE0, 0x51, 0x00, 0x00, 0x75, 0x12}, 0x4178, 0x51E0},
        // Windows 10 1803: mov rax,[r15+4058h]; cmp [rax+53C0h],ebx; jnz
        {{0x49, 0x8B, 0x87, 0x58, 0x40, 0x00, 0x00, 0x39, 0x98, 0xC0, 0x53, 0x00, 0x00, 0x75, 0x12}, 0x4058, 0x53C0},
        // Windows 11 22H2: mov rax,[r14+4030h]; cmp dword [rax+55A0h],0; jnz
        {{0x49, 0x8B, 0x86, 0x30, 0x40, 0x00, 0x00, 0x83, 0xB8, 0xA0, 0x55, 0x00, 0x00, 0x00, 0x75, 0x12}, 0x4030,
         0x55A0},
    };
    for (const auto& expected : cases) {
        const auto found = Find(Embed(expected.code), CodeArch::X64);
        ASSERT_EQ(found.size(), 1u);
        EXPECT_EQ(found[0].layout.factoryFieldOffset, expected.factoryOffset);
        EXPECT_EQ(found[0].layout.exFlagOffset, expected.flagOffset);
        EXPECT_EQ(found[0].codeOffset, 10u);
    }
}

TEST(D3D9ExFlagLayoutTest, DecodesEveryKnownX86GateShape) {
    const Expected cases[] = {
        // Windows 7: mov eax,[ebx+29E8h]; cmp [eax+4B80h],esi; jnz
        {{0x8B, 0x83, 0xE8, 0x29, 0x00, 0x00, 0x39, 0xB0, 0x80, 0x4B, 0x00, 0x00, 0x75, 0x14}, 0x29E8, 0x4B80},
        // Windows 10 1703: mov eax,[esi+2BF8h]; cmp dword [eax+4D00h],0; jnz
        {{0x8B, 0x86, 0xF8, 0x2B, 0x00, 0x00, 0x83, 0xB8, 0x00, 0x4D, 0x00, 0x00, 0x00, 0x75, 0x0F}, 0x2BF8, 0x4D00},
        // Windows 10 1803: mov eax,[esi+2B68h]; cmp dword [eax+4DF4h],0; jnz
        {{0x8B, 0x86, 0x68, 0x2B, 0x00, 0x00, 0x83, 0xB8, 0xF4, 0x4D, 0x00, 0x00, 0x00, 0x75, 0x0F}, 0x2B68, 0x4DF4},
        // Windows 11 22H2: mov eax,[ebx+2B3Ch]; cmp [eax+4F44h],edi; jnz
        {{0x8B, 0x83, 0x3C, 0x2B, 0x00, 0x00, 0x39, 0xB8, 0x44, 0x4F, 0x00, 0x00, 0x75, 0x0F}, 0x2B3C, 0x4F44},
    };
    for (const auto& expected : cases) {
        const auto found = Find(expected.code, CodeArch::X86);
        ASSERT_EQ(found.size(), 1u);
        EXPECT_EQ(found[0].layout.factoryFieldOffset, expected.factoryOffset);
        EXPECT_EQ(found[0].layout.exFlagOffset, expected.flagOffset);
    }
}

TEST(D3D9ExFlagLayoutTest, AcceptsNearJumpAndReversedCompare) {
    // mov rax,[rcx+100h]; cmp ebx,[rax+200h]; jne rel32
    const std::vector<uint8_t> code = {0x48, 0x8B, 0x81, 0x00, 0x01, 0x00, 0x00, 0x3B, 0x98, 0x00,
                                       0x02, 0x00, 0x00, 0x0F, 0x85, 0x10, 0x00, 0x00, 0x00};
    const auto found = Find(code, CodeArch::X64);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].layout, (Layout{0x100, 0x200}));
}

TEST(D3D9ExFlagLayoutTest, RejectsShapesThatAreNotTheGate) {
    const std::vector<std::vector<uint8_t>> rejected = {
        // compare base differs from the loaded register (rcx instead of rax)
        {0x49, 0x8B, 0x87, 0x58, 0x40, 0x00, 0x00, 0x39, 0x99, 0xC0, 0x53, 0x00, 0x00, 0x75, 0x12},
        // compare against a non-zero immediate
        {0x49, 0x8B, 0x86, 0x30, 0x40, 0x00, 0x00, 0x83, 0xB8, 0xA0, 0x55, 0x00, 0x00, 0x01, 0x75, 0x12},
        // qword compare (REX.W) is not the BOOL
        {0x49, 0x8B, 0x87, 0x58, 0x40, 0x00, 0x00, 0x48, 0x39, 0x98, 0xC0, 0x53, 0x00, 0x00, 0x75, 0x12},
        // no conditional branch after the compare
        {0x49, 0x8B, 0x87, 0x58, 0x40, 0x00, 0x00, 0x39, 0x98, 0xC0, 0x53, 0x00, 0x00, 0x90, 0x90},
        // 32-bit load cannot hold an x64 factory pointer
        {0x8B, 0x83, 0x3C, 0x2B, 0x00, 0x00, 0x39, 0xB8, 0x44, 0x4F, 0x00, 0x00, 0x75, 0x0F},
        // SIB addressing
        {0x48, 0x8B, 0x84, 0x24, 0x00, 0x01, 0x00, 0x00, 0x39, 0x98, 0xC0, 0x53, 0x00, 0x00, 0x75, 0x12},
        // misaligned factory field
        {0x49, 0x8B, 0x87, 0x5C, 0x40, 0x00, 0x00, 0x39, 0x98, 0xC0, 0x53, 0x00, 0x00, 0x75, 0x12},
        // implausibly large flag offset
        {0x49, 0x8B, 0x87, 0x58, 0x40, 0x00, 0x00, 0x39, 0x98, 0xC0, 0x53, 0x10, 0x00, 0x75, 0x12},
        // truncated: the branch is cut off
        {0x49, 0x8B, 0x87, 0x58, 0x40, 0x00, 0x00, 0x39, 0x98, 0xC0, 0x53, 0x00, 0x00, 0x0F, 0x85},
    };
    for (const auto& code : rejected)
        EXPECT_TRUE(Find(code, CodeArch::X64).empty());
}

TEST(D3D9ExFlagLayoutTest, DeduplicatesRepeatedGates) {
    std::vector<uint8_t> gate = {0x8B, 0x83, 0x3C, 0x2B, 0x00, 0x00, 0x39, 0xB8, 0x44, 0x4F, 0x00, 0x00, 0x75, 0x0F};
    std::vector<uint8_t> code = gate;
    code.insert(code.end(), gate.begin(), gate.end());
    EXPECT_EQ(Find(code, CodeArch::X86).size(), 1u);
}

TEST(D3D9ExFlagLayoutTest, SelectionRequiresExReferenceObjects) {
    FakeMemory memory;
    memory.PutPointer(kDevice + 0x100, 0x30000000);  // first candidate: unrelated object
    memory.PutPointer(kDevice + 0x4030, kFactory);   // second: the factory, flag FALSE
    memory.PutDword(kFactory + 0x55A0, 0);
    memory.PutDword(kFactory + 0x55A4, 1);  // third: the factory, flag TRUE
    const Candidate candidates[] = {{{0x100, 0x200}, 0}, {{0x4030, 0x55A0}, 8}, {{0x4030, 0x55A4}, 16}};

    const Selection selection = SelectLayout(memory, candidates, 3, kDevice, kFactory);
    ASSERT_TRUE(selection.found);
    EXPECT_EQ(selection.layout, (Layout{0x4030, 0x55A4}));
    EXPECT_EQ(selection.rejected, 2u);
    EXPECT_EQ(selection.lastReject, ObjectCheck::FlagMismatch);
}

TEST(D3D9ExFlagLayoutTest, SelectionFailsClosed) {
    FakeMemory memory;
    const Candidate candidates[] = {{{0x4030, 0x55A0}, 0}};
    const Selection unreadable = SelectLayout(memory, candidates, 1, kDevice, kFactory);
    EXPECT_FALSE(unreadable.found);
    EXPECT_EQ(unreadable.lastReject, ObjectCheck::ReadFailed);
    EXPECT_FALSE(SelectLayout(memory, nullptr, 0, kDevice, kFactory).found);
}

TEST(D3D9ExFlagLayoutTest, ClassicTargetMustHoldItsFactoryWithFlagCleared) {
    FakeMemory memory;
    const Layout layout{0x4030, 0x55A0};
    memory.PutPointer(kDevice + 0x4030, kFactory);
    memory.PutDword(kFactory + 0x55A0, 0);
    EXPECT_EQ(CheckObjects(memory, layout, kDevice, kFactory, 0), ObjectCheck::Match);
    // A wrapper device or a foreign factory never matches.
    EXPECT_EQ(CheckObjects(memory, layout, kDevice, kFactory + 0x1000, 0), ObjectCheck::FactoryMismatch);
    // Already set (another writer, or not a classic factory): never touched.
    memory.PutDword(kFactory + 0x55A0, 1);
    EXPECT_EQ(CheckObjects(memory, layout, kDevice, kFactory, 0), ObjectCheck::FlagMismatch);
    // Any value other than exactly 0/1 is not the BOOL.
    memory.PutDword(kFactory + 0x55A0, 0x7F);
    EXPECT_EQ(CheckObjects(memory, layout, kDevice, kFactory, 1), ObjectCheck::FlagMismatch);
    EXPECT_EQ(CheckObjects(memory, layout, 0, kFactory, 0), ObjectCheck::ReadFailed);
}

TEST(D3D9ExFlagLayoutTest, UnlockPolicy) {
    EXPECT_EQ(DecideUnlock(true, true, false, false), UnlockDecision::NotNeeded);
    EXPECT_EQ(DecideUnlock(false, false, false, false), UnlockDecision::NoLayout);
    EXPECT_EQ(DecideUnlock(false, true, false, false), UnlockDecision::Unlock);
    // The factory-wide flag cannot be serialized against other application threads.
    EXPECT_EQ(DecideUnlock(false, true, true, false), UnlockDecision::MultithreadedGameFactory);
    EXPECT_EQ(DecideUnlock(false, true, true, true), UnlockDecision::Unlock);
}

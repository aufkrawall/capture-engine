#include <gtest/gtest.h>

#include <d3d11_1.h>

#include <array>
#include <utility>

#include "hook/d3d11/d3d11_context_slots.h"

// CE's raw context hooks patch ID3D11DeviceContext(1) vtable slots by index. Each index is
// checked here against the slot the compiler itself calls for that method: a fake object
// whose vtable entries only record their own index stands in for the context. (x64: the
// probes ignore their arguments, which the caller cleans up.)
namespace {

int g_calledSlot = -1;

template <int N>
void STDMETHODCALLTYPE RecordSlot() {
    g_calledSlot = N;
}

template <int... N>
std::array<void*, sizeof...(N)> MakeProbeTable(std::integer_sequence<int, N...>) {
    return {reinterpret_cast<void*>(&RecordSlot<N>)...};
}

struct ProbeObject {
    void** vtable;
};

class D3D11ContextSlotsTest : public ::testing::Test {
protected:
    D3D11ContextSlotsTest() : table_(MakeProbeTable(std::make_integer_sequence<int, 140>{})), object_{table_.data()} {}

    ID3D11DeviceContext1* Context() { return reinterpret_cast<ID3D11DeviceContext1*>(&object_); }

    template <class Call>
    int SlotOf(Call&& call) {
        g_calledSlot = -1;
        call(Context());
        return g_calledSlot;
    }

private:
    std::array<void*, 140> table_;
    ProbeObject object_;
};

TEST_F(D3D11ContextSlotsTest, EveryHookedSlotIsTheMethodsVtableIndex) {
    namespace slots = ce::d3d11_slots;
    EXPECT_EQ(SlotOf([](auto* c) { c->PSSetShaderResources(0, 0, nullptr); }), slots::kPSSetShaderResources);
    EXPECT_EQ(SlotOf([](auto* c) { c->PSSetShader(nullptr, nullptr, 0); }), slots::kPSSetShader);
    EXPECT_EQ(SlotOf([](auto* c) { c->PSSetSamplers(0, 0, nullptr); }), slots::kPSSetSamplers);
    EXPECT_EQ(SlotOf([](auto* c) { c->DrawIndexed(0, 0, 0); }), slots::kDrawIndexed);
    EXPECT_EQ(SlotOf([](auto* c) { c->Draw(0, 0); }), slots::kDraw);
    EXPECT_EQ(SlotOf([](auto* c) { c->DrawIndexedInstanced(0, 0, 0, 0, 0); }), slots::kDrawIndexedInstanced);
    EXPECT_EQ(SlotOf([](auto* c) { c->DrawInstanced(0, 0, 0, 0); }), slots::kDrawInstanced);
    EXPECT_EQ(SlotOf([](auto* c) { c->DrawAuto(); }), slots::kDrawAuto);
    EXPECT_EQ(SlotOf([](auto* c) { c->DrawIndexedInstancedIndirect(nullptr, 0); }),
              slots::kDrawIndexedInstancedIndirect);
    EXPECT_EQ(SlotOf([](auto* c) { c->DrawInstancedIndirect(nullptr, 0); }), slots::kDrawInstancedIndirect);
    EXPECT_EQ(SlotOf([](auto* c) { c->ExecuteCommandList(nullptr, FALSE); }), slots::kExecuteCommandList);
    EXPECT_EQ(SlotOf([](auto* c) { c->PSGetSamplers(0, 0, nullptr); }), slots::kPSGetSamplers);
    EXPECT_EQ(SlotOf([](auto* c) { c->ClearState(); }), slots::kClearState);
    EXPECT_EQ(SlotOf([](auto* c) { (void)c->FinishCommandList(FALSE, nullptr); }), slots::kFinishCommandList);
    EXPECT_EQ(SlotOf([](auto* c) { c->SwapDeviceContextState(nullptr, nullptr); }), slots::kSwapDeviceContextState);
}

}  // namespace

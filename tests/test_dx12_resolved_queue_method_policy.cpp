#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "hook/d3d12/dx12_overlay_policy/resolved_queue_method.h"

#include "source_fragment_reader.h"

// CE resolves the real ExecuteCommandLists and Signal from a D3D12Core queue vtable and calls them to bypass
// hooks. With the D3D12 debug layer on, the game's queues are d3d12SDKLayers.dll objects: D3D12Core's ECL
// called with one read the wrapper as its own object - an access violation on CE's first overlay submit in
// every FG flow scenario (2026-10-03). RenderDoc, PIX and Streamline's proxies wrap queues the same way.

namespace {

using ce::dx12_overlay_policy::ClassifyResolvedQueueMethodFit;
using ce::dx12_overlay_policy::MayCallResolvedQueueMethod;
using ce::dx12_overlay_policy::ResolvedQueueMethodFit;

// Stand-ins for image bases: only their identity matters.
const char kD3D12Core = 0;
const char kSdkLayers = 0;

TEST(Dx12ResolvedQueueMethodPolicyTest, OnlyAQueueOfTheMethodsOwnImageIsCallable) {
    EXPECT_EQ(ClassifyResolvedQueueMethodFit(true, &kD3D12Core, &kD3D12Core),
              ResolvedQueueMethodFit::kSameImplementation);
    EXPECT_TRUE(MayCallResolvedQueueMethod(ResolvedQueueMethodFit::kSameImplementation));

    // The debug layer's (or a capture tool's, or Streamline's) queue object.
    EXPECT_EQ(ClassifyResolvedQueueMethodFit(true, &kD3D12Core, &kSdkLayers),
              ResolvedQueueMethodFit::kForeignQueueObject);
    EXPECT_FALSE(MayCallResolvedQueueMethod(ResolvedQueueMethodFit::kForeignQueueObject));
}

TEST(Dx12ResolvedQueueMethodPolicyTest, UnprovableQueuesAreNeverCalledDirectly) {
    // A vtable copied to the heap, a missing queue or vtable, a method outside any image.
    EXPECT_EQ(ClassifyResolvedQueueMethodFit(true, &kD3D12Core, nullptr), ResolvedQueueMethodFit::kUnprovable);
    EXPECT_EQ(ClassifyResolvedQueueMethodFit(true, nullptr, &kD3D12Core), ResolvedQueueMethodFit::kUnprovable);
    EXPECT_EQ(ClassifyResolvedQueueMethodFit(true, nullptr, nullptr), ResolvedQueueMethodFit::kUnprovable);
    EXPECT_FALSE(MayCallResolvedQueueMethod(ResolvedQueueMethodFit::kUnprovable));
}

TEST(Dx12ResolvedQueueMethodPolicyTest, NothingResolvedTakesTheCallersUnresolvedPath) {
    EXPECT_EQ(ClassifyResolvedQueueMethodFit(false, &kD3D12Core, &kD3D12Core), ResolvedQueueMethodFit::kNoMethod);
    EXPECT_FALSE(MayCallResolvedQueueMethod(ResolvedQueueMethodFit::kNoMethod));
}

// A resolved method loaded from its global and then called on a queue must be checked against that queue
// first: in every hook/d3d12 unit, a call of a variable loaded from dx12_hook_g_RealD3D12ECL/Signal is
// preceded by DX12_MayCallResolvedQueueMethod on that variable (DX12_RealD3D12ECLForQueue returns a checked
// one). The one exception forwards a queue whose vtable is already gone: there is nothing to judge.
bool IsIdentifierChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The first call `name(<identifier>,` in `text`, or npos.
size_t FindCallWithQueueArgument(const std::string& text, const std::string& name) {
    for (size_t at = text.find(name + "("); at != std::string::npos; at = text.find(name + "(", at + 1)) {
        if (at > 0 && IsIdentifierChar(text[at - 1]))
            continue;
        size_t cursor = at + name.size() + 1;
        while (cursor < text.size() && text[cursor] == ' ')
            ++cursor;
        const size_t argument = cursor;
        while (cursor < text.size() && IsIdentifierChar(text[cursor]))
            ++cursor;
        if (cursor > argument && cursor < text.size() && text[cursor] == ',')
            return at;
    }
    return std::string::npos;
}

TEST(Dx12ResolvedQueueMethodPolicyTest, RawLoadedResolvedMethodsAreCheckedBeforeTheyAreCalled) {
    const std::filesystem::path root = std::filesystem::current_path() / "hook" / "d3d12";
    int units = 0;
    int checkedCalls = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp")
            continue;
        ++units;
        const std::string source = ce::test_source::ReadFile(entry.path());
        for (const char* global : {"dx12_hook_g_RealD3D12ECL.load(", "dx12_hook_g_RealD3D12Signal.load("}) {
            for (size_t load = source.find(global); load != std::string::npos; load = source.find(global, load + 1)) {
                // `<name> = <global>.load(...);`, possibly with the assignment on the line before.
                size_t cursor = load;
                while (cursor > 0 && std::isspace(static_cast<unsigned char>(source[cursor - 1])) != 0)
                    --cursor;
                if (cursor == 0 || source[cursor - 1] != '=')
                    continue;
                --cursor;
                while (cursor > 0 && source[cursor - 1] == ' ')
                    --cursor;
                const size_t nameEnd = cursor;
                while (cursor > 0 && IsIdentifierChar(source[cursor - 1]))
                    --cursor;
                const std::string name = source.substr(cursor, nameEnd - cursor);
                const size_t loadEnd = source.find(';', load);
                if (name.empty() || loadEnd == std::string::npos)
                    continue;
                const std::string window = source.substr(loadEnd, 3000);
                const size_t call = FindCallWithQueueArgument(window, name);
                if (call == std::string::npos)
                    continue;
                const std::string preamble = source.substr(load > 600 ? load - 600 : 0, 600);
                if (preamble.find("Freed COM objects have null vtable") != std::string::npos)
                    continue;
                ++checkedCalls;
                EXPECT_NE(window.substr(0, call).find("reinterpret_cast<const void*>(" + name + ")"), std::string::npos)
                    << entry.path().filename().string() << ": `" << name
                    << "` is called without DX12_MayCallResolvedQueueMethod on its queue";
            }
        }
    }
    EXPECT_GT(units, 50);
    // The PostSL wrapper-dispatch fallback and the overlay completion fence are raw loads checked at the call.
    EXPECT_GE(checkedCalls, 2);
}

// The submit sites CE's overlay uses, each with its own decision log site.
TEST(Dx12ResolvedQueueMethodPolicyTest, OverlaySubmitSitesCheckTheirQueue) {
    struct Site {
        const char* unit;
        const char* check;
    };
    const std::vector<Site> sites = {
        {"dx12_hook_process_session_draw_submission.cpp", R"(DX12_RealD3D12ECLForQueue(eclQueue, "overlay submit"))"},
        {"dx12_hook_process_session_draw_submission.cpp", R"("overlay completion fence")"},
        {"dx12_hook_overlay_render.cpp", R"(DX12_RealD3D12ECLForQueue(submitQueue, "overlay command list"))"},
        {"dx12_hook_ffx_ui_composite.cpp", R"(DX12_RealD3D12ECLForQueue(submitQueue, "ffx-ui-composite"))"},
        {"dx12_hook_postsl_render_route.cpp", R"(DX12_RealD3D12ECLForQueue(queue, "PostSL transition probe"))"},
        {"dx12_hook_postsl_render_submit.cpp", R"("PostSL selected scQueue")"},
        {"dx12_hook_postsl_render_submit.cpp", R"("PostSL real queue behind wrapper")"},
        {"dx12_hook_postsl_render_submit.cpp", R"("PostSL submit")"},
        {"dx12_hook_ecl.cpp", R"(DX12_RealD3D12ECLForQueue(pThis, "CE overlay queue"))"},
        {"dx12_hook_ecl_forward.cpp", R"(DX12_RealD3D12ECLForQueue(queue, "ECL recursion break"))"},
    };
    for (const Site& site : sites) {
        const std::string source = ce::test_source::ReadFile(ce::test_source::FindSource("hook", site.unit));
        ASSERT_FALSE(source.empty()) << site.unit;
        EXPECT_NE(source.find(site.check), std::string::npos) << site.unit << " lost " << site.check;
    }
}

}  // namespace

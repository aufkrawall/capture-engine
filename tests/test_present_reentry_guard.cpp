#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include "../hook/common/present_reentry_guard.h"
#include "source_fragment_reader.h"

// The shared re-entry guard for the DX8/DX9/OpenGL present detours. The proven
// failure it exists for (ddraw_hook_present_reentry.cpp, Gothic II +
// gameoverlayrenderer) has two halves, and both are pinned here: a nested
// presentation may run the real implementation at most once per outermost
// presentation - through a bypass past the foreign entry patch, never through
// CE's saved original - and every other nested call returns without invoking
// anything, so no chain of injectors can make CE recurse.
namespace {

using TestPresentFn = long (*)(void* self, int marker);

struct RealImplCounters {
    std::atomic<int> calls{0};
    long returnResult = 0;
    void* self = nullptr;
    int marker = 0;
    // Simulates a re-entry arriving from inside the bypass run.
    ce::present_reentry::PresentReentryFamily* reenterFamily = nullptr;
    void* reenterSavedOriginal = nullptr;
    long reenterDropResult = 0;
};

long FakeRealPresent(void* self, int marker) {
    auto* counters = static_cast<RealImplCounters*>(self);
    counters->calls.fetch_add(1);
    counters->self = self;
    counters->marker = marker;
    if (counters->reenterFamily) {
        ce::present_reentry::PresentReentryScope nested(*counters->reenterFamily);
        counters->reenterDropResult = nested.AnswerNestedPresentation(
            counters->reenterSavedOriginal, counters->reenterSavedOriginal, nullptr,
            static_cast<TestPresentFn>(&FakeRealPresent), counters, marker);
    }
    return counters->returnResult;
}

void* CountingBuilderTarget() {
    return reinterpret_cast<void*>(&FakeRealPresent);
}

struct BuilderCounters {
    std::atomic<int> calls{0};
    void* result = nullptr;
};

// The builder is handed the saved original it should build a bypass for - that
// is its whole contract with production - so the tests stand their answers in a
// lookup keyed by that target. Each test allocates its own executable entry
// page and registers the answer that page's bypass must produce.
std::mutex g_builderMutex;
std::unordered_map<void*, BuilderCounters*> g_builderAnswers;

void RegisterBuilderAnswer(void* entry, BuilderCounters& counters) {
    std::lock_guard<std::mutex> lock(g_builderMutex);
    g_builderAnswers[entry] = &counters;
}

void* CountingBuilder(void* target) {
    BuilderCounters* counters = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_builderMutex);
        const auto answer = g_builderAnswers.find(target);
        if (answer == g_builderAnswers.end())
            return nullptr;
        counters = answer->second;
    }
    counters->calls.fetch_add(1);
    return counters->result;
}

// Executable stand-ins for "a saved original whose entry another injector has
// patched" (a five-byte `E9` jump) and for an unpatched implementation
// (an ordinary prologue). The pages are deliberately never freed: the guard's
// bypass cache is process-lifetime and keyed on the target address, so an
// allocator reusing a freed page for the next test would silently alias it.
unsigned char* MakeExecutableEntry(bool inlinePatched) {
    auto* memory = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!memory)
        return nullptr;
    memory[0] = inlinePatched ? 0xE9u : 0x55u;  // jmp rel32 / push ebp
    memory[1] = 0x00u;
    memory[2] = inlinePatched ? 0xC3u : 0x8Bu;  // ret / mov ebp,esp
    return memory;
}

TEST(PresentReentryGuard, OnlyASecondEntryOfTheSameEntryPointIsReentrant) {
    // Each test builds its own families: the drop answer echoes the family's
    // last recorded result, and that record lives as long as the thread.
    ce::present_reentry::PresentReentryFamily family{"Test Present", 77};
    ce::present_reentry::PresentReentryFamily otherFamily{"Test Other Present", 88};

    EXPECT_FALSE(ce::present_reentry::PresentReentryScope(family).IsReentrant());
    {
        ce::present_reentry::PresentReentryScope outer(family);
        EXPECT_FALSE(outer.IsReentrant());
        EXPECT_EQ(outer.nestedLevel(), 0);
        ce::present_reentry::PresentReentryScope inner(family);
        EXPECT_TRUE(inner.IsReentrant());
        EXPECT_EQ(inner.nestedLevel(), 1);
    }
    // A different entry point on the same thread is a benign same-API nested
    // present (the D3D9 vtable/inline double route, the runtime's device ->
    // swapchain dispatch), not a re-entry.
    {
        ce::present_reentry::PresentReentryScope outer(family);
        ce::present_reentry::PresentReentryScope other(otherFamily);
        EXPECT_FALSE(other.IsReentrant());
    }
}

TEST(PresentReentryGuard, TheFirstNestedPresentationRunsTheRealImplementation) {
    ce::present_reentry::PresentReentryFamily family{"Test Present", 77};
    RealImplCounters counters;
    counters.returnResult = 4242;
    unsigned char* entry = MakeExecutableEntry(true);
    ASSERT_NE(entry, nullptr);
    BuilderCounters builder;
    builder.result = CountingBuilderTarget();
    RegisterBuilderAnswer(entry, builder);

    long result = 0;
    {
        ce::present_reentry::PresentReentryScope outer(family);
        ce::present_reentry::PresentReentryScope inner(family);
        result = inner.AnswerNestedPresentation(entry, entry, &CountingBuilder, static_cast<TestPresentFn>(nullptr),
                                                &counters, 7);
    }
    EXPECT_EQ(result, 4242);
    EXPECT_EQ(counters.calls.load(), 1);
    EXPECT_EQ(counters.marker, 7);
    EXPECT_EQ(builder.calls.load(), 1);
}

TEST(PresentReentryGuard, TheSecondNestedPresentationIsDroppedWithinOneOutermost) {
    ce::present_reentry::PresentReentryFamily family{"Test Present", 77};
    RealImplCounters counters;
    counters.returnResult = 4242;
    unsigned char* entry = MakeExecutableEntry(true);
    ASSERT_NE(entry, nullptr);
    BuilderCounters builder;
    builder.result = CountingBuilderTarget();
    RegisterBuilderAnswer(entry, builder);

    long first = 0;
    long second = 0;
    {
        ce::present_reentry::PresentReentryScope outer(family);
        outer.RecordResult(1234);
        ce::present_reentry::PresentReentryScope inner(family);
        first = inner.AnswerNestedPresentation(entry, entry, &CountingBuilder, static_cast<TestPresentFn>(nullptr),
                                               &counters, 1);
        second = inner.AnswerNestedPresentation(entry, entry, &CountingBuilder, static_cast<TestPresentFn>(nullptr),
                                                &counters, 2);
    }
    EXPECT_EQ(first, 4242);
    // The once-per-outermost budget is spent: nothing is called, and the drop
    // echoes the last recorded result rather than inventing a status.
    EXPECT_EQ(second, 1234);
    EXPECT_EQ(counters.calls.load(), 1);

    // A new outermost presentation earns the budget back.
    {
        ce::present_reentry::PresentReentryScope outer(family);
        ce::present_reentry::PresentReentryScope inner(family);
        inner.AnswerNestedPresentation(entry, entry, &CountingBuilder, static_cast<TestPresentFn>(nullptr), &counters, 3);
    }
    EXPECT_EQ(counters.calls.load(), 2);
}

TEST(PresentReentryGuard, AReEntryFromInsideTheBypassReturnsWithoutCallingAnything) {
    ce::present_reentry::PresentReentryFamily family{"Test Present", 77};
    RealImplCounters counters;
    counters.returnResult = 4242;
    unsigned char* entry = MakeExecutableEntry(true);
    ASSERT_NE(entry, nullptr);
    BuilderCounters builder;
    builder.result = CountingBuilderTarget();
    RegisterBuilderAnswer(entry, builder);
    counters.reenterFamily = &family;
    counters.reenterSavedOriginal = entry;

    {
        ce::present_reentry::PresentReentryScope outer(family);
        ce::present_reentry::PresentReentryScope inner(family);
        inner.AnswerNestedPresentation(entry, entry, &CountingBuilder, static_cast<TestPresentFn>(nullptr), &counters,
                                       1);
    }
    // One call reached the real implementation; the re-entry it made from
    // inside the bypass was refused before invoking anything. The refused
    // answer echoes the API-appropriate success when nothing was recorded.
    EXPECT_EQ(counters.calls.load(), 1);
    EXPECT_EQ(counters.reenterDropResult, 77);
}

TEST(PresentReentryGuard, ANestedPresentationWithoutABypassCallsNothing) {
    ce::present_reentry::PresentReentryFamily family{"Test Present", 77};
    RealImplCounters counters;
    unsigned char* entry = MakeExecutableEntry(true);
    ASSERT_NE(entry, nullptr);
    BuilderCounters builder;
    builder.result = nullptr;  // no trampoline could be built
    RegisterBuilderAnswer(entry, builder);

    long result = 0;
    {
        ce::present_reentry::PresentReentryScope outer(family);
        ce::present_reentry::PresentReentryScope inner(family);
        result = inner.AnswerNestedPresentation(entry, entry, &CountingBuilder, static_cast<TestPresentFn>(nullptr),
                                                &counters, 1);
    }
    EXPECT_EQ(result, 77);
    EXPECT_EQ(counters.calls.load(), 0);
}

TEST(PresentReentryGuard, AnUnpatchedSavedOriginalNeverGetsABypass) {
    unsigned char* entry = MakeExecutableEntry(false);
    ASSERT_NE(entry, nullptr);
    BuilderCounters builder;
    builder.result = CountingBuilderTarget();
    RegisterBuilderAnswer(entry, builder);

    EXPECT_EQ(ce::present_reentry::AcquirePresentEntryBypass(entry, "Test Present", &CountingBuilder), nullptr);
    EXPECT_EQ(builder.calls.load(), 0);
}

TEST(PresentReentryGuard, ABuiltBypassIsCachedButANegativeAnswerIsReprobed) {
    unsigned char* entry = MakeExecutableEntry(true);
    ASSERT_NE(entry, nullptr);
    BuilderCounters builder;
    builder.result = CountingBuilderTarget();
    RegisterBuilderAnswer(entry, builder);

    EXPECT_EQ(ce::present_reentry::AcquirePresentEntryBypass(entry, "Test Present", &CountingBuilder),
              CountingBuilderTarget());
    EXPECT_EQ(ce::present_reentry::AcquirePresentEntryBypass(entry, "Test Present", &CountingBuilder),
              CountingBuilderTarget());
    EXPECT_EQ(builder.calls.load(), 1);

    // A patched entry whose trampoline could not be built is not cached: the
    // injector that owns the entry can appear at any time, and a durable
    // negative would keep CE dropping nested presentations for the run.
    unsigned char* hopeless = MakeExecutableEntry(true);
    ASSERT_NE(hopeless, nullptr);
    BuilderCounters failingBuilder;
    failingBuilder.result = nullptr;
    RegisterBuilderAnswer(hopeless, failingBuilder);
    EXPECT_EQ(ce::present_reentry::AcquirePresentEntryBypass(hopeless, "Test Present", &CountingBuilder), nullptr);
    EXPECT_EQ(ce::present_reentry::AcquirePresentEntryBypass(hopeless, "Test Present", &CountingBuilder), nullptr);
    // The failed build was retried on the second probe rather than trusted.
    EXPECT_EQ(failingBuilder.calls.load(), 2);
}

// The wiring is only a fix while every present detour keeps its guard: the
// DirectDraw test of record (test_ddraw_lock_flags.cpp) is the template.
TEST(PresentReentryGuard, EveryNamedPresentDetourKeepsItsReentryGuard) {
    const std::filesystem::path root = std::filesystem::current_path();
    const struct {
        const char* path;
        size_t scopes;
        size_t answers;
    } expectations[] = {
        // dx9_hook.cpp's logical source is the whole dx9_hook_* family
        // (dx9_hook_internal.h plus every dx9_hook_*.cpp sibling), so it spans
        // both DX9 present routes: three vtable detours and three inline ones.
        {"hook/apis/dx9_hook.cpp", 6, 6},
        {"hook/apis/dx9_hook_present_detours.cpp", 3, 3},
        {"hook/apis/dx8_hook_detours.cpp", 1, 1},
        {"hook/apis/opengl_hook_capture.cpp", 3, 3},
    };
    for (const auto& expectation : expectations) {
        const std::string contents = ce::test_source::ReadLogicalSource(root / expectation.path);
        ASSERT_FALSE(contents.empty()) << expectation.path;
        size_t scopes = 0;
        for (size_t at = contents.find("PresentReentryScope reentryScope"); at != std::string::npos;
             at = contents.find("PresentReentryScope reentryScope", at + 1)) {
            ++scopes;
        }
        size_t answers = 0;
        for (size_t at = contents.find("AnswerNestedPresentation"); at != std::string::npos;
             at = contents.find("AnswerNestedPresentation", at + 1)) {
            ++answers;
        }
        EXPECT_EQ(scopes, expectation.scopes) << expectation.path << " lost a re-entry guard";
        EXPECT_EQ(answers, expectation.answers) << expectation.path << " stopped answering nested presentations";
    }
}

}  // namespace

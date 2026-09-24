#pragma once

// Refusing to re-enter CE's own present detours (DX8, DX9, OpenGL).
//
// CE hooks presentation vtable slots / entries and keeps the pointer it
// replaced as "the original". A second overlay that hooks the same entry does
// the same thing: with the entries owned below CE, CE's saved original leads
// into the injector that re-issues the presentation through the vtable -
// which is CE's detour again. ddraw_hook_present_reentry.cpp documents that
// cycle in full (Gothic II + Steam's gameoverlayrenderer, 32,768 recursion
// levels in two milliseconds); the DirectDraw detours got a bounded answer and
// the DX8/DX9/OpenGL ones did not.
//
// The mechanics are the proven ones: a nested presentation is answered by
// running the real implementation past the foreign entry patch, through a
// bypass trampoline built from the module's own on-disk bytes, at most once per
// outermost presentation. A re-entry from inside the bypass, or one with no
// bypass to use, returns without calling anything - never CE's saved original.
//
// Depth is counted per present entry point, not globally. D3D9 has legitimate
// same-API nested presents: both the vtable and the inline route can fire for
// one call (dx9_hook_present.cpp), and the runtime can dispatch a device
// Present into the swapchain's Present through its vtable. Those chains visit
// each entry point once and must keep reaching the real implementation. An
// unbounded mutual cycle cannot: with finitely many entry points it must re-enter
// one already on the stack, and that second visit is where it is answered or
// dropped.

namespace ce::present_reentry {

// Builds a trampoline that runs `target`'s real implementation past an inline
// entry patch. A parameter so the unit tests can stand in a fixed answer; null
// selects the production builder.
using BypassBuilder = void* (*)(void* target);

// The production builder: InlineHook::CreateBypassTrampoline, the trampoline
// over the target module's own on-disk bytes that DXGIShared and the DirectDraw
// detours already use to step around a foreign entry patch.
void* BuildInlineEntryBypass(void* target);

// The real implementation behind a saved original whose entry another injector
// has patched, reached past that patch. Null when the entry carries no foreign
// patch, when no bypass could be built, or when CE is shutting down; the
// caller then drops the nested presentation rather than calling anything.
// Cached per target, so the trampoline is built at most once; a negative
// answer is re-probed because the injector that patches an entry can appear at
// any time after CE installed its own hook.
void* AcquirePresentEntryBypass(void* savedOriginal, const char* operation, BypassBuilder builder);

// Reports one nested presentation, naming the module that re-entered CE and
// how CE answered it ("bypass" ran the real implementation, "dropped" did not
// present). Naming the module is the whole diagnostic: it separates a
// co-resident overlay from the runtime itself from CE calling back into its own
// detour, and none of the three can be told apart from the recursion alone.
// Declared before the scope classes: the in-class answer below calls it.
void NoteNestedPresentation(void* savedOriginal, void* returnAddress, const char* answer);

// One present entry-point family. Each hooked present entry point declares its
// own: the scope counts re-entry of that entry point, which is the shape a
// mutual-hook cycle takes (the injector re-issues the very call it
// intercepted).
class PresentReentryFamily {
public:
    PresentReentryFamily(const char* operation, long apiSuccessResult) noexcept;

    const char* operation() const {
        return operation_;
    }
    long apiSuccessResult() const {
        return apiSuccessResult_;
    }
    unsigned slot() const {
        return slot_;
    }

    PresentReentryFamily(const PresentReentryFamily&) = delete;
    PresentReentryFamily& operator=(const PresentReentryFamily&) = delete;

private:
    const char* operation_;
    long apiSuccessResult_;
    unsigned slot_;
};

// Depth of this family's present detours on the calling thread. A second entry
// is a foreign overlay calling back into the entry CE owns, never the
// application presenting twice at once.
class PresentReentryScope {
public:
    explicit PresentReentryScope(const PresentReentryFamily& family);
    ~PresentReentryScope();

    PresentReentryScope(const PresentReentryScope&) = delete;
    PresentReentryScope& operator=(const PresentReentryScope&) = delete;

    // Nesting below the outermost presentation: 0 is the outermost call, 1 is
    // the first nested one, 2 is a re-entry from inside the bypass.
    int nestedLevel() const {
        return nestedLevel_;
    }
    bool IsReentrant() const {
        return nestedLevel_ > 0;
    }

    // The outermost presentation's result once it returns. A later dropped
    // nested presentation echoes it instead of inventing a status.
    void RecordResult(long result);

    // The answer to a nested presentation. The bypass runs the real
    // implementation exactly once per outermost presentation; every other
    // nested call returns without invoking anything CE saved. `savedOriginal`
    // is the entry that must never be called back and the target the bypass
    // steps past; `fn` carries the detour's own signature so the bypass runs
    // with the arguments the nested call was made with.
    template <typename PresentFn, typename... Args>
    long AnswerNestedPresentation(void* savedOriginal, void* returnAddress, BypassBuilder builder, PresentFn,
                                  Args... args) {
        void* bypass = AcquirePresentEntryBypass(savedOriginal, family().operation(), builder);
        if (!MayRunRealImplementation(bypass != nullptr)) {
            NoteNestedPresentation(savedOriginal, returnAddress, "dropped");
            return DropResult();
        }
        MarkBypassUsed();
        NoteNestedPresentation(savedOriginal, returnAddress, "bypass");
        return static_cast<long>(reinterpret_cast<PresentFn>(bypass)(args...));
    }

    const PresentReentryFamily& family() const {
        return *family_;
    }

private:
    // Whether this nested presentation may run the real implementation: the
    // first nested level of one outermost presentation, with a bypass that
    // exists, and never twice on one thread.
    bool MayRunRealImplementation(bool bypassAvailable) const;
    void MarkBypassUsed();
    long DropResult() const;

    const PresentReentryFamily* family_;
    int nestedLevel_ = 0;
};

}  // namespace ce::present_reentry

// The caller of the detour, which is that one fact. Same capture the DirectDraw
// detours use (ddraw_hook_blit_classification.h).
#if !defined(CE_PRESENT_RETURN_ADDRESS)
#if defined(__clang__) || defined(__GNUC__)
#define CE_PRESENT_RETURN_ADDRESS() __builtin_extract_return_addr(__builtin_return_address(0))
#else
#define CE_PRESENT_RETURN_ADDRESS() _ReturnAddress()
#endif
#endif

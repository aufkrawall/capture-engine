#pragma once

// RAII Resource Wrappers for CaptureEngine
// Provides automatic cleanup for Windows handles, COM objects, Vulkan
// resources, etc. All wrappers are move-only to prevent double-free issues.

#include <windows.h>
#include <functional>
#include <utility>

// Forward declarations for Vulkan types (avoid including vulkan.h here)
#ifndef VK_NULL_HANDLE
#define VK_NULL_HANDLE nullptr
typedef struct VkDevice_T* VkDevice;
typedef struct VkInstance_T* VkInstance;
#endif

namespace ce {

// HandleGuard - RAII wrapper for Windows HANDLE
// Auto-closes handle on destruction
class HandleGuard {
    HANDLE h_ = INVALID_HANDLE_VALUE;

public:
    HandleGuard() = default;
    explicit HandleGuard(HANDLE h) : h_(h) {}

    ~HandleGuard() {
        reset();
    }

    // Move-only
    HandleGuard(HandleGuard&& o) noexcept : h_(o.h_) {
        o.h_ = INVALID_HANDLE_VALUE;
    }

    HandleGuard& operator=(HandleGuard&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = o.h_;
            o.h_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;

    void reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (h_ && h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
        }
        h_ = h;
    }

    HANDLE get() const {
        return h_;
    }

    HANDLE release() {
        HANDLE t = h_;
        h_ = INVALID_HANDLE_VALUE;
        return t;
    }

    HANDLE* addressof() {
        reset();
        return &h_;
    }

    explicit operator bool() const {
        return h_ && h_ != INVALID_HANDLE_VALUE;
    }
};

// EventGuard - RAII wrapper for Windows Event handles
// Same as HandleGuard but semantically clearer for events
using EventGuard = HandleGuard;

// ModuleGuard - RAII wrapper for LoadLibrary results
// Keeps code-backed objects alive until they have been released.
class ModuleGuard {
    HMODULE module_ = nullptr;

public:
    ModuleGuard() = default;
    explicit ModuleGuard(HMODULE module) : module_(module) {}

    ~ModuleGuard() {
        reset();
    }

    ModuleGuard(ModuleGuard&& o) noexcept : module_(o.module_) {
        o.module_ = nullptr;
    }

    ModuleGuard& operator=(ModuleGuard&& o) noexcept {
        if (this != &o) {
            reset();
            module_ = o.module_;
            o.module_ = nullptr;
        }
        return *this;
    }

    ModuleGuard(const ModuleGuard&) = delete;
    ModuleGuard& operator=(const ModuleGuard&) = delete;

    void reset(HMODULE module = nullptr) {
        if (module_) {
            FreeLibrary(module_);
        }
        module_ = module;
    }

    HMODULE get() const {
        return module_;
    }

    HMODULE release() {
        HMODULE module = module_;
        module_ = nullptr;
        return module;
    }

    explicit operator bool() const {
        return module_ != nullptr;
    }
};

// MappingGuard - RAII wrapper for MapViewOfFile results
class MappingGuard {
    void* ptr_ = nullptr;

public:
    MappingGuard() = default;
    explicit MappingGuard(void* p) : ptr_(p) {}

    ~MappingGuard() {
        reset();
    }

    MappingGuard(MappingGuard&& o) noexcept : ptr_(o.ptr_) {
        o.ptr_ = nullptr;
    }

    MappingGuard& operator=(MappingGuard&& o) noexcept {
        if (this != &o) {
            reset();
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }

    MappingGuard(const MappingGuard&) = delete;
    MappingGuard& operator=(const MappingGuard&) = delete;

    void reset(void* p = nullptr) {
        if (ptr_) {
            UnmapViewOfFile(ptr_);
        }
        ptr_ = p;
    }

    void* get() const {
        return ptr_;
    }

    template <typename T>
    T* as() const {
        return static_cast<T*>(ptr_);
    }

    void* release() {
        void* t = ptr_;
        ptr_ = nullptr;
        return t;
    }

    explicit operator bool() const {
        return ptr_ != nullptr;
    }
};

// VirtualAllocGuard - RAII wrapper for VirtualAllocEx in remote process
class VirtualAllocGuard {
    HANDLE process_ = nullptr;
    void* ptr_ = nullptr;

public:
    VirtualAllocGuard() = default;
    VirtualAllocGuard(HANDLE proc, void* p) : process_(proc), ptr_(p) {}

    ~VirtualAllocGuard() {
        reset();
    }

    VirtualAllocGuard(VirtualAllocGuard&& o) noexcept : process_(o.process_), ptr_(o.ptr_) {
        o.process_ = nullptr;
        o.ptr_ = nullptr;
    }

    VirtualAllocGuard& operator=(VirtualAllocGuard&& o) noexcept {
        if (this != &o) {
            reset();
            process_ = o.process_;
            ptr_ = o.ptr_;
            o.process_ = nullptr;
            o.ptr_ = nullptr;
        }
        return *this;
    }

    VirtualAllocGuard(const VirtualAllocGuard&) = delete;
    VirtualAllocGuard& operator=(const VirtualAllocGuard&) = delete;

    void reset() {
        if (ptr_ && process_) {
            VirtualFreeEx(process_, ptr_, 0, MEM_RELEASE);
        }
        ptr_ = nullptr;
        process_ = nullptr;
    }

    void* get() const {
        return ptr_;
    }

    void* release() {
        void* t = ptr_;
        ptr_ = nullptr;
        process_ = nullptr;
        return t;
    }

    explicit operator bool() const {
        return ptr_ != nullptr;
    }
};

// ComGuard<T> - RAII wrapper for COM interface pointers
// Auto-releases reference on destruction
template <typename T>
class ComGuard {
    T* ptr_ = nullptr;

public:
    ComGuard() = default;
    explicit ComGuard(T* p) : ptr_(p) {}

    ~ComGuard() {
        reset();
    }

    // Move-only
    ComGuard(ComGuard&& o) noexcept : ptr_(o.ptr_) {
        o.ptr_ = nullptr;
    }

    ComGuard& operator=(ComGuard&& o) noexcept {
        if (this != &o) {
            reset();
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }

    ComGuard(const ComGuard&) = delete;
    ComGuard& operator=(const ComGuard&) = delete;

    void reset(T* p = nullptr) {
        if (ptr_) {
            ptr_->Release();
        }
        ptr_ = p;
    }

    T* get() const {
        return ptr_;
    }
    T* operator->() const {
        return ptr_;
    }
    T& operator*() const {
        return *ptr_;
    }

    // Safe addressof - releases existing before returning address
    // Use this with QueryInterface and similar COM functions
    T** addressof() {
        reset();
        return &ptr_;
    }

    // Get address without releasing (for out params where we know ptr is null)
    T** put() {
        return &ptr_;
    }

    T* release() {
        T* t = ptr_;
        ptr_ = nullptr;
        return t;
    }

    // Attach without AddRef (takes ownership of existing ref)
    void attach(T* p) {
        reset();
        ptr_ = p;
    }

    // Detach and AddRef (caller responsible for release)
    T* detach() {
        T* t = ptr_;
        ptr_ = nullptr;
        return t;
    }

    explicit operator bool() const {
        return ptr_ != nullptr;
    }
};

// VkHandleGuard<T> - RAII wrapper for Vulkan handles
// Takes a destroy function to call on cleanup
template <typename T>
class VkHandleGuard {
public:
    using DestroyFunc = std::function<void(T)>;

private:
    T handle_{};
    DestroyFunc destroy_;
    bool valid_ = false;

public:
    VkHandleGuard() = default;

    VkHandleGuard(T handle, DestroyFunc destroy) : handle_(handle), destroy_(std::move(destroy)), valid_(true) {}

    ~VkHandleGuard() {
        reset();
    }

    VkHandleGuard(VkHandleGuard&& o) noexcept : handle_(o.handle_), destroy_(std::move(o.destroy_)), valid_(o.valid_) {
        o.valid_ = false;
    }

    VkHandleGuard& operator=(VkHandleGuard&& o) noexcept {
        if (this != &o) {
            reset();
            handle_ = o.handle_;
            destroy_ = std::move(o.destroy_);
            valid_ = o.valid_;
            o.valid_ = false;
        }
        return *this;
    }

    VkHandleGuard(const VkHandleGuard&) = delete;
    VkHandleGuard& operator=(const VkHandleGuard&) = delete;

    void reset() {
        if (valid_ && destroy_) {
            destroy_(handle_);
        }
        valid_ = false;
    }

    T get() const {
        return handle_;
    }

    T release() {
        valid_ = false;
        return handle_;
    }

    explicit operator bool() const {
        return valid_;
    }
};

// ScopedThreadPriority - temporarily changes thread priority, restores on
// destruction
class ScopedThreadPriority {
    HANDLE thread_;
    int oldPriority_;
    bool valid_ = false;

public:
    explicit ScopedThreadPriority(int newPriority, HANDLE thread = GetCurrentThread()) : thread_(thread) {
        oldPriority_ = GetThreadPriority(thread_);
        if (oldPriority_ != THREAD_PRIORITY_ERROR_RETURN) {
            valid_ = SetThreadPriority(thread_, newPriority) != 0;
        }
    }

    ~ScopedThreadPriority() {
        if (valid_) {
            SetThreadPriority(thread_, oldPriority_);
        }
    }

    ScopedThreadPriority(const ScopedThreadPriority&) = delete;
    ScopedThreadPriority& operator=(const ScopedThreadPriority&) = delete;
    ScopedThreadPriority(ScopedThreadPriority&&) = delete;
    ScopedThreadPriority& operator=(ScopedThreadPriority&&) = delete;

    bool valid() const {
        return valid_;
    }
};

// ScopedTimerResolution - calls timeBeginPeriod/timeEndPeriod
class ScopedTimerResolution {
    UINT period_;
    bool valid_ = false;

public:
    explicit ScopedTimerResolution(UINT periodMs = 1) : period_(periodMs) {
        valid_ = (timeBeginPeriod(period_) == TIMERR_NOERROR);
    }

    ~ScopedTimerResolution() {
        if (valid_) {
            timeEndPeriod(period_);
        }
    }

    ScopedTimerResolution(const ScopedTimerResolution&) = delete;
    ScopedTimerResolution& operator=(const ScopedTimerResolution&) = delete;
    ScopedTimerResolution(ScopedTimerResolution&&) = delete;
    ScopedTimerResolution& operator=(ScopedTimerResolution&&) = delete;

    bool valid() const {
        return valid_;
    }
};

// ScopeGuard - generic scope exit handler (like Go's defer)
template <typename F>
class ScopeGuard {
    F func_;
    bool active_ = true;

public:
    explicit ScopeGuard(F&& f) : func_(std::forward<F>(f)) {}

    ~ScopeGuard() {
        if (active_) {
            func_();
        }
    }

    ScopeGuard(ScopeGuard&& o) noexcept : func_(std::move(o.func_)), active_(o.active_) {
        o.active_ = false;
    }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;

    void dismiss() {
        active_ = false;
    }
};

// Helper to create ScopeGuard with type deduction
template <typename F>
ScopeGuard<F> make_scope_guard(F&& f) {
    return ScopeGuard<F>(std::forward<F>(f));
}

// Macro for convenient scope guard creation
#define CE_SCOPE_EXIT(code) auto CE_CONCAT(_scope_guard_, __LINE__) = ::ce::make_scope_guard([&]() { code; })

#define CE_CONCAT_IMPL(a, b) a##b
#define CE_CONCAT(a, b) CE_CONCAT_IMPL(a, b)

}  // namespace ce

#pragma once

// Threading-model independent access to a std::thread's Win32 thread handle.
//
// std::thread::native_handle_type is only a waitable Win32 HANDLE when the
// standard library uses the Win32 threading model, which is what MSYS2's
// clang64 libc++ does. MinGW toolchains built against winpthreads - the default
// for the system mingw-w64 GCC on several Linux distributions, and therefore
// for cross builds - return a pthread_t instead. That is an opaque winpthreads
// object, not a thread handle: passing its bit pattern to WaitForSingleObject
// fails with ERROR_INVALID_HANDLE, so every bounded join silently degrades into
// its timeout/failure path. Ask winpthreads for the real handle instead.
//
// Overloads, not #ifdef on a guessed model macro: the exact native_handle_type
// selects the right conversion, and a toolchain that matches neither fails to
// compile rather than producing a bogus handle.

#include <windows.h>
#include <thread>

#if defined(__has_include)
#if __has_include(<pthread.h>)
#include <pthread.h>
#endif
#endif

namespace ce {
namespace detail {

// Win32 threading model: native_handle() already is the thread handle.
inline HANDLE ToWin32ThreadHandle(HANDLE nativeHandle) {
    return nativeHandle;
}

// winpthreads: unwrap the pthread_t to the Win32 handle the thread was created
// with. pthread_gethandle() is winpthreads' own accessor and returns exactly
// that handle, so no ownership is transferred and it must not be closed here.
//
// A template so this body is instantiated only where it is actually needed. The
// exact overload above wins wherever native_handle_type already is a Win32
// handle, and pthread_gethandle then never has to exist at all - which keeps
// toolchains whose winpthreads headers lack it building unchanged.
template <typename NativeHandle>
inline HANDLE ToWin32ThreadHandle(NativeHandle nativeHandle) {
    return static_cast<HANDLE>(pthread_gethandle(nativeHandle));
}

}  // namespace detail

// Waitable Win32 handle for a running std::thread. Only valid while the thread
// is joinable; the caller must still join() after a successful wait.
inline HANDLE Win32ThreadHandle(std::thread& thread) {
    return detail::ToWin32ThreadHandle(thread.native_handle());
}

}  // namespace ce

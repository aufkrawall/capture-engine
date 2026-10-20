# CaptureEngine SDK: find_package(cengine) package. Installed at <CaptureEngine>/sdk/lib/cmake/cengine/.
# Provides the imported target cengine::cengine (cengine.dll with its import library and headers) and
# cengine_PACKAGE_DIR, the CaptureEngine folder a client executable has to run from.
include_guard(GLOBAL)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(cengine_FOUND FALSE)
    set(cengine_NOT_FOUND_MESSAGE "cengine is x64 only")
    return()
endif()

get_filename_component(_cengine_sdk_dir "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
get_filename_component(cengine_PACKAGE_DIR "${_cengine_sdk_dir}/.." ABSOLUTE)

if(MSVC)
    set(_cengine_import_lib "${_cengine_sdk_dir}/lib/cengine.lib")
else()
    set(_cengine_import_lib "${_cengine_sdk_dir}/lib/libcengine.dll.a")
endif()

foreach(_cengine_required IN ITEMS "${_cengine_import_lib}" "${_cengine_sdk_dir}/include/cengine/cengine.h"
                                   "${cengine_PACKAGE_DIR}/cengine.dll")
    if(NOT EXISTS "${_cengine_required}")
        set(cengine_FOUND FALSE)
        set(cengine_NOT_FOUND_MESSAGE "cengine package is incomplete: ${_cengine_required} is missing")
        return()
    endif()
endforeach()

if(NOT TARGET cengine::cengine)
    add_library(cengine::cengine SHARED IMPORTED)
    set_target_properties(cengine::cengine PROPERTIES
        IMPORTED_LOCATION "${cengine_PACKAGE_DIR}/cengine.dll"
        IMPORTED_IMPLIB "${_cengine_import_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_cengine_sdk_dir}/include")
endif()

unset(_cengine_required)
unset(_cengine_import_lib)
unset(_cengine_sdk_dir)

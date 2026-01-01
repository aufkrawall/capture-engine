# Test App Build Script for DX12 and Vulkan test apps
# These are minimal apps for testing capture + limiter without user games

cmake_minimum_required(VERSION 3.15)
project(CaptureTestApps)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output to build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../build/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/../build/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/../build/bin)

# DX12 Test App
add_executable(dx12_test WIN32 dx12_test.cpp)
target_link_libraries(dx12_test d3d12 dxgi d3dcompiler)
set_target_properties(dx12_test PROPERTIES 
    OUTPUT_NAME "dx12_test"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

# Make it a console app for printf output
set_target_properties(dx12_test PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")

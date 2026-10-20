cmake_minimum_required(VERSION 3.16)
project(cengine_headless_sample C)

# Point CMAKE_PREFIX_PATH at <CaptureEngine>/sdk. The executable has to run from the CaptureEngine folder
# (cengine_PACKAGE_DIR); see sdk/README.md "Deployment".
find_package(cengine 2.0 REQUIRED)

add_executable(headless main.c)
set_target_properties(headless PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_link_libraries(headless PRIVATE cengine::cengine)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(headless PRIVATE -Wall -Wextra)
endif()

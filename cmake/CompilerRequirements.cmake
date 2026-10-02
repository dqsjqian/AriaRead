# Stable compiler baseline, reviewed on 2026-10-02. Keep this separate from
# C++23 language mode: accepting -std=c++23 alone is not a toolchain policy.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_ariaread_compiler_minimum 16.2.0)
    set(_ariaread_compiler_name "GCC 16.2")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(_ariaread_compiler_minimum 21.0.0)
    set(_ariaread_compiler_name "AppleClang 21 from stable Xcode 27.0")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(_ariaread_compiler_minimum 23.1.2)
    set(_ariaread_compiler_name "LLVM Clang 23.1.2")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # CMake derives 19.51 from _MSC_VER=1951; cl.exe's toolset banner calls
    # this 14.51. Do not confuse the Visual Studio IDE and compiler versions.
    set(_ariaread_compiler_minimum 19.51.36247)
    set(_ariaread_compiler_name "MSVC Build Tools 14.51.36247 (Visual Studio 2026 stable)")
else()
    message(FATAL_ERROR "Unsupported AriaRead compiler: ${CMAKE_CXX_COMPILER_ID}. Use a supported stable GCC, Clang, AppleClang or MSVC toolchain.")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS _ariaread_compiler_minimum)
    message(FATAL_ERROR
        "AriaRead requires ${_ariaread_compiler_name} or newer; detected "
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. Upgrade the stable toolchain.")
endif()
message(STATUS "AriaRead compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}; C++23")
unset(_ariaread_compiler_minimum)
unset(_ariaread_compiler_name)

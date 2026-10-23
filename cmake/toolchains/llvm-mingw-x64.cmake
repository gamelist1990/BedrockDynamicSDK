# Cross-compile Windows x64 DLLs from Linux with llvm-mingw.
#
# Usage:
#   cmake -S exsample/HitBox -B build-hitbox-linux \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw-x64.cmake \
#     -DLLVM_MINGW_ROOT=/path/to/llvm-mingw
#
# llvm-mingw's GNU-target Clang needs -fms-extensions for the SDK's guarded
# Windows SEH reads (__try/__except). Keep those flags in the toolchain so
# consumers do not need to know about compiler-specific source details.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT LLVM_MINGW_ROOT)
    if(DEFINED ENV{LLVM_MINGW_ROOT})
        set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
    else()
        message(FATAL_ERROR
            "Set LLVM_MINGW_ROOT to the llvm-mingw installation directory")
    endif()
endif()
set(LLVM_MINGW_ROOT "${LLVM_MINGW_ROOT}" CACHE PATH "llvm-mingw installation root")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW_ROOT)

set(_llvm_mingw_bin "${LLVM_MINGW_ROOT}/bin")
set(CMAKE_C_COMPILER "${_llvm_mingw_bin}/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${_llvm_mingw_bin}/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${_llvm_mingw_bin}/x86_64-w64-mingw32-windres")
set(CMAKE_C_FLAGS_INIT "-fms-extensions")
set(CMAKE_CXX_FLAGS_INIT "-fms-extensions")
# Embed llvm-mingw's C++ runtime into each mod DLL; only Windows system DLLs
# should remain as runtime dependencies.
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

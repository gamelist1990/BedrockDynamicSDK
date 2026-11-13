# Cross-compile toolchain for building the BedrockDynamicSDK Mods on Linux.
# Uses clang-cl + lld-link against an xwin-splatted MSVC CRT and Windows SDK,
# producing MSVC-ABI Windows x64 binaries identical in kind to the MSVC CI build.
#
# Prepare once:
#   xwin --accept-license --arch x86_64 --variant desktop \
#        --sdk-version 10.0.22621 splat --output ~/tools/xwin-splat
# (SDK 10.0.26100 currently fails in xwin 0.6.7 with
#  "unable to find x86_64 headers for Win11SDK_10.0.26100", hence the pin.)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(XWIN_ROOT "$ENV{HOME}/tools/xwin-splat" CACHE PATH "xwin splat output")

find_program(CLANG_CL clang-cl REQUIRED)
find_program(LLD_LINK lld-link REQUIRED)
set(CMAKE_C_COMPILER "${CLANG_CL}")
set(CMAKE_CXX_COMPILER "${CLANG_CL}")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_LINKER "${LLD_LINK}" CACHE FILEPATH "lld-link")

# MSVC CRT + Windows SDK headers as implicit system includes so SDK header
# warnings are silenced the same way they are under a real MSVC install.
add_compile_options(
    "/imsvc${XWIN_ROOT}/crt/include"
    "/imsvc${XWIN_ROOT}/sdk/include/ucrt"
    "/imsvc${XWIN_ROOT}/sdk/include/um"
    "/imsvc${XWIN_ROOT}/sdk/include/shared"
    "/imsvc${XWIN_ROOT}/sdk/include/winrt"
    "/imsvc${XWIN_ROOT}/sdk/include/cppwinrt"
)
add_link_options(
    "-libpath:${XWIN_ROOT}/crt/lib/x86_64"
    "-libpath:${XWIN_ROOT}/sdk/lib/ucrt/x86_64"
    "-libpath:${XWIN_ROOT}/sdk/lib/um/x86_64"
)

set(CMAKE_FIND_ROOT_PATH "${XWIN_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Only used if a project embeds .rc resources.
find_program(CMAKE_RC_COMPILER NAMES llvm-rc-21 llvm-rc
    HINTS /usr/lib/llvm-21/bin /usr/bin)

# Everything in this repo builds Release with the static CRT; defaulting the
# runtime here also keeps CMake compiler detection away from the debug CRT
# (msvcrtd.lib), which xwin does not splat unless asked.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# /usr/bin/mt on Ubuntu is the magnetic-tape tool, not the Windows manifest
# tool CMake expects on MSVC-like targets; point it at LLVMs instead.

# /usr/bin/mt on Ubuntu is the magnetic-tape tool, not the Windows manifest
# tool CMake expects on MSVC-like targets; point CMAKE_MT at the LLVM one.
find_program(CMAKE_MT NAMES llvm-mt-21 llvm-mt HINTS /usr/lib/llvm-21/bin /usr/bin)

# Archiver for static libraries: llvm-lib is the lib.exe equivalent.
find_program(CMAKE_AR NAMES llvm-lib-21 llvm-lib HINTS /usr/lib/llvm-21/bin /usr/bin)
set(CMAKE_RANLIB "true" CACHE FILEPATH "no ranlib for MSVC archives")

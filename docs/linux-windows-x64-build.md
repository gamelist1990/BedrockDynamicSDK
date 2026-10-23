# Build Windows x64 DLLs from Linux

This repository can cross-compile its Windows DLL examples from Linux with
`llvm-mingw` and CMake/Ninja. Use the LLVM-MinGW release for Linux x86-64; the
toolchain includes Windows headers, import libraries, and an x64 SEH-capable
Clang. The SDK's guarded memory reads use MSVC-style `__try/__except`, so use
the supplied `cmake/toolchains/llvm-mingw-x64.cmake` rather than plain MinGW
GCC.

## HitBox example

Install/extract llvm-mingw to a user-owned directory, then configure and build:

```sh
cmake -S exsample/HitBox -B build-hitbox-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/llvm-mingw-x64.cmake" \
  -DLLVM_MINGW_ROOT=/path/to/llvm-mingw \
  -DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK="$PWD"
cmake --build build-hitbox-linux --config Release --parallel
```

The `FETCHCONTENT_SOURCE_DIR_BEDROCK_SDK` option uses this checkout as the SDK
source. Omit it to fetch the configured SDK ref from GitHub instead. CMake
downloads the pinned Dear ImGui and EnTT dependencies on the first configure.

The resulting artifact is `build-hitbox-linux/BedrockHitBox.dll`. Check its
architecture with:

```sh
file build-hitbox-linux/BedrockHitBox.dll
```

It should report a PE32+ x86-64 DLL. This is a cross-compiled Windows DLL, not a
native Linux library. Wine/Proton can load PE DLLs, but the SDK's Minecraft
injection/runtime behavior still needs testing inside the target Bedrock
process; a successful compile alone does not prove that runtime compatibility.

cmake_minimum_required(VERSION 3.24)
project(BedrockCheckSig LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BEDROCK_SDK_SOURCE_DIR "" CACHE PATH "Use this local BedrockDynamicSDK checkout")
set(BEDROCK_SDK_BUILD_TOOLS OFF CACHE BOOL "Do not build SDK tools" FORCE)
set(BEDROCK_SDK_ENABLE_GUI OFF CACHE BOOL "Do not build the SDK GUI" FORCE)

if(BEDROCK_SDK_SOURCE_DIR)
    set(CHECKSIG_SDK_ROOT "${BEDROCK_SDK_SOURCE_DIR}")
    add_subdirectory("${BEDROCK_SDK_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/bedrock-sdk")
else()
    include(FetchContent)
    set(BEDROCK_SDK_REF "main" CACHE STRING "BedrockDynamicSDK branch/tag/commit")
    FetchContent_Declare(bedrock_sdk
        GIT_REPOSITORY https://github.com/gamelist1990/BedrockDynamicSDK.git
        GIT_TAG ${BEDROCK_SDK_REF}
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(bedrock_sdk)
    set(CHECKSIG_SDK_ROOT "${bedrock_sdk_SOURCE_DIR}")
endif()

if(WIN32)
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "CheckSig requires MSVC on Windows x64.")
    endif()

    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/investigation/requirements-1.26.60.5.json" CHECKSIG_REQUIREMENTS)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    configure_file(src/checksig_catalog.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/checksig_catalog.hpp" @ONLY)
    add_library(CheckSig SHARED src/check_sig.cpp)
    target_include_directories(CheckSig PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${CHECKSIG_SDK_ROOT}/third_party/minhook-vendor/src/hde")
    target_link_libraries(CheckSig PRIVATE
        bedrock::symbols bedrock::hook_runtime bedrock_hook_minhook shell32 ole32)
    bedrock_enable_hook_runtime(CheckSig)
    target_compile_definitions(CheckSig PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(CheckSig PRIVATE /W4 /permissive- /utf-8)
    set_target_properties(CheckSig PROPERTIES
        PREFIX ""
        OUTPUT_NAME "CheckSig"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    message(STATUS "CheckSig is an in-process Windows x64 diagnostic mod; no target on this platform")
endif()

option(CHECKSIG_BUILD_TESTS "Build CheckSig input and DLL smoke tests" OFF)
if(CHECKSIG_BUILD_TESTS)
    enable_testing()
    add_executable(CheckSigCoreTest tests/core_test.cpp)
    target_include_directories(CheckSigCoreTest PRIVATE src "${CHECKSIG_SDK_ROOT}/third_party" "${CHECKSIG_SDK_ROOT}/include")
    add_test(NAME CheckSigCore COMMAND CheckSigCoreTest "${CHECKSIG_SDK_ROOT}")
    if(MSVC)
        target_compile_options(CheckSigCoreTest PRIVATE /W4 /utf-8)
        set_property(TARGET CheckSigCoreTest PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    if(WIN32)
        add_executable(CheckSigDllSmokeTest tests/dll_smoke_test.cpp)
        target_include_directories(CheckSigDllSmokeTest PRIVATE src)
        target_link_libraries(CheckSigDllSmokeTest PRIVATE bedrock::symbols shell32 ole32)
        target_compile_definitions(CheckSigDllSmokeTest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(CheckSigDllSmokeTest PRIVATE /W4 /utf-8)
        set_property(TARGET CheckSigDllSmokeTest PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        add_dependencies(CheckSigDllSmokeTest CheckSig)
        add_test(NAME CheckSigDllSmoke COMMAND CheckSigDllSmokeTest "$<TARGET_FILE:CheckSig>")
        set_tests_properties(CheckSigDllSmoke PROPERTIES TIMEOUT 90)
    endif()
endif()

cmake_minimum_required(VERSION 3.24)
project(BedrockScanClient LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

set(BEDROCK_SDK_ENABLE_GUI OFF CACHE BOOL "" FORCE)
set(BEDROCK_SDK_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BEDROCK_SDK_REF "main" CACHE STRING "BedrockDynamicSDK ref")
FetchContent_Declare(bedrock_sdk
    GIT_REPOSITORY https://github.com/gamelist1990/BedrockDynamicSDK.git
    GIT_TAG ${BEDROCK_SDK_REF}
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(bedrock_sdk)

if(WIN32)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tools/ScanClientImport.py" SCANCLIENT_GHIDRA_SCRIPT)
    configure_file(src/ghidra_import_script.hpp.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/ghidra_import_script.hpp" @ONLY)
    add_library(ScanClient SHARED src/scan_client.cpp)
    target_include_directories(ScanClient PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set_target_properties(ScanClient PROPERTIES PREFIX "" OUTPUT_NAME "ScanClient")
    target_link_libraries(ScanClient PRIVATE bedrock::sdk version bcrypt)
    target_compile_definitions(ScanClient PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        set_property(TARGET ScanClient PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(ScanClient PRIVATE /W4 /permissive-)
    else()
        target_link_options(ScanClient PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    message(STATUS "ScanClient is a Windows x64 in-process DLL; no target on this platform")
endif()

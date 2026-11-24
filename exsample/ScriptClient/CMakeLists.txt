cmake_minimum_required(VERSION 3.24)
project(ScriptClient LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BEDROCK_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "SDK checkout")
set(BEDROCK_GAME_VERSION "1.26.65.1" CACHE STRING "Minecraft exact build")
set(STAGEB_GOLDEN_DIR "${BEDROCK_SDK_ROOT}/tests/golden/EffectFake" CACHE PATH
    "EffectFake fixture for ScriptClient host tests")

# quickjs-ng is vendored at a pinned commit (§2.2); build it as a static lib.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${BEDROCK_SDK_ROOT}/third_party/quickjs-ng" quickjs-ng EXCLUDE_FROM_ALL)

set(BEDROCK_SDK_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
if(WIN32)
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "ScriptClient requires Windows x64 MSVC ABI")
    endif()
    set(BEDROCK_SDK_ENABLE_GUI ON CACHE BOOL "" FORCE)
    set(BEDROCK_SDK_ENABLE_NATIVE_CLIENT ON CACHE BOOL "" FORCE)
    set(BEDROCK_SDK_ENABLE_NATIVE_WORLD ON CACHE BOOL "" FORCE)
    set(BEDROCK_SDK_ENABLE_MOD_SETTINGS ON CACHE BOOL "" FORCE)
endif()
add_subdirectory("${BEDROCK_SDK_ROOT}" bedrock-sdk EXCLUDE_FROM_ALL)

foreach(builtin path events files network)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/core/builtins/${builtin}.js")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    file(READ "${source}" source_text)
    # MSVC limits the size of an individual source literal. The builtin
    # sources are ASCII; adjacent raw literals preserve the exact JS bytes.
    string(LENGTH "${source_text}" source_length)
    set(offset 0)
    set(BUILTIN_${builtin} "")
    while(offset LESS source_length)
        string(SUBSTRING "${source_text}" ${offset} 8192 chunk)
        string(APPEND BUILTIN_${builtin} "\nR\"scriptclient(${chunk})scriptclient\"")
        math(EXPR offset "${offset} + 8192")
    endwhile()
endforeach()
configure_file(core/builtin_sources.hpp.in builtin_sources.hpp @ONLY)

add_library(script_client_core STATIC
    core/manifest.cpp
    core/mod_scanner.cpp
    core/js_runtime.cpp
    core/transpiler.cpp
    core/module_loader.cpp
    core/command_model.cpp
    core/sdk_bindings.cpp
    core/plugin_host.cpp
    core/http_client.cpp
    core/http_transport.cpp
)
target_include_directories(script_client_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${BEDROCK_SDK_ROOT}/sdk/include"
    "${BEDROCK_SDK_ROOT}/third_party"
    "${BEDROCK_SDK_ROOT}/third_party/quickjs-ng"
)
target_include_directories(script_client_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(script_client_core PUBLIC qjs bedrock::sdk bedrock::internal_headers)
find_package(Threads REQUIRED)
target_link_libraries(script_client_core PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(script_client_core PRIVATE winhttp)
else()
    # The shipped DLL uses WinHTTP. libcurl enables real HTTP(S) on native test hosts.
    find_package(CURL 7.85 QUIET)
    if(CURL_FOUND)
        target_link_libraries(script_client_core PRIVATE CURL::libcurl)
        target_compile_definitions(script_client_core PUBLIC SCRIPTCLIENT_HAS_CURL)
    else()
        message(STATUS "libcurl >= 7.85 not found; native ScriptClient HTTP unavailable")
    endif()
endif()

if(WIN32)
    add_library(ScriptClient SHARED
        src/main.cpp
        src/command_service.cpp
    )
    target_include_directories(ScriptClient PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(ScriptClient PRIVATE
        script_client_core bedrock::sdk)
    target_compile_options(ScriptClient PRIVATE /utf-8)
    set_target_properties(ScriptClient PROPERTIES PREFIX ""
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET script_client_core PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(stage_b_test tests/stage_b_test.cpp)
    target_link_libraries(stage_b_test PRIVATE script_client_core)
    target_include_directories(stage_b_test PRIVATE
        "${BEDROCK_SDK_ROOT}/internal/include")
    target_compile_definitions(stage_b_test PRIVATE
        STAGEB_GOLDEN_DIR="${STAGEB_GOLDEN_DIR}"
        STAGEB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(stage_b_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(stage_b_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME stage_b_phase1 COMMAND stage_b_test)
    if(NOT CMAKE_CROSSCOMPILING)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME stage_c_ts_sdk
                COMMAND "${Python3_EXECUTABLE}" "${BEDROCK_SDK_ROOT}/tests/test_stage_c_ts_sdk.py")
            if(WIN32 OR CURL_FOUND)
                add_test(NAME stage_b_http COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/http_fixture.py" --
                    "$<TARGET_FILE:stage_b_test>" --http-only)
            endif()
        endif()
    endif()
endif()

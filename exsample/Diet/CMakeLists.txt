cmake_minimum_required(VERSION 3.24)
project(BedrockDiet LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "Diet reads native EnTT/BlockSource objects and requires the Microsoft C++ ABI. Use MSVC or clang-cl in Release; MinGW DLLs are not supported.")
endif()

set(BEDROCK_DIET_BUILD_ID "local" CACHE STRING "Source revision for diagnostic logs")

# Host-testable decision core: no Windows, no SDK, no native pointers.
add_library(diet_features STATIC src/occlusion.cpp src/traffic.cpp src/debug_layout.cpp)
target_include_directories(diet_features PUBLIC include)
target_compile_features(diet_features PUBLIC cxx_std_20)
# The native Mod must share the game's static CRT (/MT); a static library built
# with the default /MD makes the final link fail with LNK2038 on MSVC CI.
function(diet_static_runtime target)
    if(MSVC)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()
diet_static_runtime(diet_features)
if(MSVC)
    target_compile_options(diet_features PRIVATE /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(diet_occlusion_test tests/occlusion_test.cpp)
    target_link_libraries(diet_occlusion_test PRIVATE diet_features)
    diet_static_runtime(diet_occlusion_test)
    add_executable(diet_traffic_test tests/traffic_test.cpp)
    target_link_libraries(diet_traffic_test PRIVATE diet_features)
    diet_static_runtime(diet_traffic_test)
    add_executable(diet_debug_layout_test tests/debug_layout_test.cpp)
    target_link_libraries(diet_debug_layout_test PRIVATE diet_features)
    diet_static_runtime(diet_debug_layout_test)
    if(MSVC)
        target_compile_options(diet_occlusion_test PRIVATE /UNDEBUG /utf-8)
        target_compile_options(diet_traffic_test PRIVATE /UNDEBUG /utf-8)
        target_compile_options(diet_debug_layout_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(diet_occlusion_test PRIVATE -UNDEBUG)
        target_compile_options(diet_traffic_test PRIVATE -UNDEBUG)
        target_compile_options(diet_debug_layout_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME diet_occlusion_test COMMAND diet_occlusion_test)
    add_test(NAME diet_traffic_test COMMAND diet_traffic_test)
    add_test(NAME diet_debug_layout_test COMMAND diet_debug_layout_test)
endif()

if(WIN32)
    include(FetchContent)
    set(BEDROCK_SDK_REF "main" CACHE STRING "BedrockDynamicSDK branch/tag/commit")
    set(BEDROCK_SDK_BUILD_TOOLS OFF CACHE BOOL "Do not build SDK tools" FORCE)
    set(BEDROCK_SDK_ENABLE_NATIVE_ACTOR_REGISTRY ON CACHE BOOL "SDK Actor registry capture" FORCE)
    set(BEDROCK_SDK_ENABLE_NATIVE_WORLD ON CACHE BOOL "SDK native world access (block reads)" FORCE)
    set(BEDROCK_SDK_ENABLE_NATIVE_CLIENT ON CACHE BOOL "SDK native client adapter" FORCE)
    set(BEDROCK_SDK_ENABLE_GUI ON CACHE BOOL "Dear ImGui: the F3 screen draws through sdk::Overlay" FORCE)
    FetchContent_Declare(bedrock_sdk
        GIT_REPOSITORY https://github.com/gamelist1990/BedrockDynamicSDK.git
        GIT_TAG ${BEDROCK_SDK_REF}
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE)
    FetchContent_MakeAvailable(bedrock_sdk)

    add_library(Diet SHARED src/diet.cpp src/debug_screen.cpp)
    set_target_properties(Diet PROPERTIES PREFIX "" OUTPUT_NAME "Diet")
    target_link_libraries(Diet PRIVATE diet_features bedrock::sdk bedrock::gui_overlay
        shell32 ole32 psapi dxgi)
    target_compile_definitions(Diet PRIVATE BEDROCK_DIET_BUILD_ID="${BEDROCK_DIET_BUILD_ID}")
    target_compile_features(Diet PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(Diet PRIVATE /utf-8)
        set_property(TARGET Diet PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    bedrock_enable_hook_runtime(Diet)
endif()

cmake_minimum_required(VERSION 3.24)
project(BedrockHitBox LANGUAGES C CXX)

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "HitBox reads native EnTT/C++ objects and requires the Microsoft C++ ABI. Use MSVC or clang-cl in Release; MinGW DLLs are not supported.")
endif()
set(BEDROCK_HITBOX_BUILD_ID "local" CACHE STRING "Source revision for diagnostic logs")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(BEDROCK_SDK_REF "main" CACHE STRING "BedrockDynamicSDK branch/tag/commit")
set(BEDROCK_SDK_ENABLE_GUI OFF CACHE BOOL "Native world rendering needs no GUI overlay" FORCE)
set(BEDROCK_SDK_BUILD_TOOLS OFF CACHE BOOL "Do not build SDK tools" FORCE)
FetchContent_Declare(bedrock_sdk
    GIT_REPOSITORY https://github.com/gamelist1990/BedrockDynamicSDK.git
    GIT_TAG ${BEDROCK_SDK_REF}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(bedrock_sdk)

if(WIN32)
    FetchContent_Declare(entt
        GIT_REPOSITORY https://github.com/skypjack/entt.git
        GIT_TAG fe8d7d78c4823e8a66a050bf86f5c6318cf76ce7
        GIT_SHALLOW FALSE
        GIT_PROGRESS TRUE)
    FetchContent_MakeAvailable(entt)
    add_library(BedrockHitBox SHARED src/hitbox.cpp)
    # Keep the artifact name identical under MSVC and MinGW/LLVM toolchains.
    set_target_properties(BedrockHitBox PROPERTIES PREFIX "")
    target_include_directories(BedrockHitBox PRIVATE include)
    target_compile_definitions(BedrockHitBox PRIVATE BEDROCK_HITBOX_BUILD_ID="${BEDROCK_HITBOX_BUILD_ID}")
    target_link_libraries(BedrockHitBox PRIVATE bedrock::symbols bedrock::logging
        bedrock::hook_runtime EnTT::EnTT shell32 ole32)
    target_compile_features(BedrockHitBox PRIVATE cxx_std_20)
    bedrock_enable_hook_runtime(BedrockHitBox)
    if(MSVC)
        set_property(TARGET BedrockHitBox PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # Small, target-restricted local test loader. It refuses every process
    # except Minecraft.Windows.exe and validates the DLL's PE architecture.
    add_executable(BedrockHitBoxLoader src/loader.cpp)
    target_compile_features(BedrockHitBoxLoader PRIVATE cxx_std_20)
    target_link_libraries(BedrockHitBoxLoader PRIVATE kernel32)
    if(MSVC)
        set_property(TARGET BedrockHitBoxLoader PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(hitbox_box_cache_test tests/box_cache_test.cpp)
    target_include_directories(hitbox_box_cache_test PRIVATE include)
    target_link_libraries(hitbox_box_cache_test PRIVATE bedrock::game_api)
    target_include_directories(hitbox_box_cache_test PRIVATE ${bedrock_sdk_SOURCE_DIR}/third_party)
    if(MSVC)
        target_compile_options(hitbox_box_cache_test PRIVATE /UNDEBUG)
        set_property(TARGET hitbox_box_cache_test PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(hitbox_box_cache_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME hitbox_box_cache_test COMMAND hitbox_box_cache_test)
endif()

cmake_minimum_required(VERSION 3.24)
project(BedrockHitBox LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(BEDROCK_SDK_REF "main" CACHE STRING "BedrockDynamicSDK branch/tag/commit")
set(BEDROCK_SDK_ENABLE_GUI ON CACHE BOOL "Enable the in-game overlay" FORCE)
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
    target_include_directories(BedrockHitBox PRIVATE include)
    target_link_libraries(BedrockHitBox PRIVATE bedrock::symbols bedrock::logging
        bedrock::gui_overlay bedrock::hook_runtime EnTT::EnTT shell32 ole32)
    target_compile_features(BedrockHitBox PRIVATE cxx_std_20)
    bedrock_enable_hook_runtime(BedrockHitBox)
    if(MSVC)
        set_property(TARGET BedrockHitBox PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

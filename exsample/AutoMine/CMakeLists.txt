cmake_minimum_required(VERSION 3.24)
project(BedrockAutoMine LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "AutoMine requires MSVC Release x64 native ABI.")
endif()
include(FetchContent)
set(BEDROCK_SDK_REF "main" CACHE STRING "SDK branch/tag/commit")
set(BEDROCK_SDK_BUILD_TOOLS OFF CACHE BOOL "No SDK tools" FORCE)
FetchContent_Declare(bedrock_sdk GIT_REPOSITORY https://github.com/gamelist1990/BedrockDynamicSDK.git
    GIT_TAG ${BEDROCK_SDK_REF} GIT_SHALLOW TRUE)
if(WIN32)
    set(BEDROCK_SDK_ENABLE_NATIVE_WORLD ON CACHE BOOL "Use SDK native world adapter" FORCE)
    set(BEDROCK_SDK_ENABLE_NATIVE_CLIENT ON CACHE BOOL "Use SDK native client adapter" FORCE)
    set(BEDROCK_SDK_ENABLE_NATIVE_ACTOR_REGISTRY ON CACHE BOOL "Use SDK native actor registry" FORCE)
endif()
FetchContent_MakeAvailable(bedrock_sdk)
if(WIN32)
    add_library(BedrockAutoMine SHARED
        src/auto_mine.cpp
        src/block_knowledge.cpp
        src/boat.cpp
        src/collection.cpp
        src/combat.cpp
        src/construction.cpp
        src/controller.cpp
        src/controls.cpp
        src/crafting.cpp
        src/crafting_table.cpp
        src/diagnostics.cpp
        src/excavation.cpp
        src/expedition.cpp
        src/forage.cpp
        src/inventory.cpp
        src/mining.cpp
        src/navigation.cpp
        src/path_render.cpp
        src/pathfinding.cpp
        src/recovery.cpp
        src/runtime.cpp
        src/screens.cpp
        src/smelting.cpp
        src/target_selection.cpp
        src/tools.cpp)
    target_precompile_headers(BedrockAutoMine PRIVATE src/auto_mine.hpp)
    set_source_files_properties(src/collection.cpp PROPERTIES
        COMPILE_DEFINITIONS AUTOMINE_COLLECTION_PICKUP_WIRING)
    target_include_directories(BedrockAutoMine PRIVATE include)
    # Public SDK only (plugin-system completion program): every game
    # capability goes through bedrock::sdk facades.
    target_link_libraries(BedrockAutoMine PRIVATE bedrock::sdk bedrock_nlohmann_headers shell32 ole32)
    target_compile_options(BedrockAutoMine PRIVATE /utf-8)
    set_property(TARGET BedrockAutoMine PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

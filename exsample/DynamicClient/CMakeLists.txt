cmake_minimum_required(VERSION 3.24)
project(DynamicClient LANGUAGES C CXX)
set(BEDROCK_GAME_VERSION "1.26.65.1" CACHE STRING "Minecraft exact build")
set(FETCHCONTENT_SOURCE_DIR_BEDROCK_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "SDK checkout")
set(BEDROCK_SDK_BUILD_TOOLS OFF)
if(WIN32)
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT BEDROCK_GAME_VERSION STREQUAL "1.26.65.1")
        message(FATAL_ERROR "DynamicClient requires Windows x64 MSVC ABI and Minecraft 1.26.65.1")
    endif()
    set(BEDROCK_SDK_ENABLE_GUI ON)
    set(BEDROCK_SDK_ENABLE_NATIVE_CLIENT ON)
    set(BEDROCK_SDK_ENABLE_NATIVE_WORLD ON)
    set(BEDROCK_SDK_ENABLE_MOD_SETTINGS ON)
endif()
add_subdirectory("${FETCHCONTENT_SOURCE_DIR_BEDROCK_SDK}" bedrock-sdk EXCLUDE_FROM_ALL)

add_library(dynamic_client_features STATIC
    src/zoom/controller.cpp src/zoom/config.cpp
    src/sprint/auto_sprint.cpp
    src/tablist/model.cpp src/tablist/config.cpp)
target_include_directories(dynamic_client_features PUBLIC include)
target_link_libraries(dynamic_client_features PUBLIC bedrock::symbols)
target_compile_features(dynamic_client_features PUBLIC cxx_std_20)
set_property(TARGET dynamic_client_features PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(WIN32)
    add_library(DynamicClient SHARED
        src/client.cpp src/settings.cpp src/keybinds.cpp src/brightness.cpp
        src/zoom/zoom.cpp src/sprint/sprint.cpp
        src/tablist/roster.cpp src/tablist/overlay.cpp
        src/freecam/freecam.cpp src/freecam/worker.cpp)
    target_include_directories(DynamicClient PRIVATE src)
    # Use the public SDK alongside the first-party native adapters.
    target_link_libraries(DynamicClient PRIVATE bedrock::sdk dynamic_client_features
        bedrock::client bedrock::game_hooks bedrock::gui_overlay bedrock::logging EnTT::EnTT)
    target_compile_options(DynamicClient PRIVATE /utf-8)
    set_target_properties(DynamicClient PROPERTIES PREFIX ""
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)
if(BUILD_TESTING)
    if(WIN32)
        # An independent probe: stopping it must not stop DynamicClient.
        add_library(StageALifecycleProbe SHARED EXCLUDE_FROM_ALL tests/lifecycle_probe.cpp)
        target_link_libraries(StageALifecycleProbe PRIVATE bedrock::sdk)
        set_target_properties(StageALifecycleProbe PROPERTIES PREFIX ""
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    foreach(feature IN ITEMS zoom auto_sprint tablist)
        add_executable(${feature}_test tests/${feature}_test.cpp)
        target_link_libraries(${feature}_test PRIVATE dynamic_client_features)
        set_property(TARGET ${feature}_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        if(MSVC)
            target_compile_options(${feature}_test PRIVATE /UNDEBUG)
        else()
            target_compile_options(${feature}_test PRIVATE -UNDEBUG)
        endif()
        add_test(NAME dynamic_client_${feature} COMMAND ${feature}_test)
    endforeach()
endif()

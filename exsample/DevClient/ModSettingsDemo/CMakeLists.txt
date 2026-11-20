cmake_minimum_required(VERSION 3.24)
project(ModSettingsDemo LANGUAGES CXX)
set(BEDROCK_GAME_VERSION "1.26.65.1" CACHE STRING "Minecraft exact build")
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
    NOT BEDROCK_GAME_VERSION STREQUAL "1.26.65.1")
    message(FATAL_ERROR "ModSettingsDemo requires Windows x64 and Minecraft 1.26.65.1")
endif()
set(FETCHCONTENT_SOURCE_DIR_BEDROCK_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../../.." CACHE PATH "SDK checkout")
set(BEDROCK_SDK_ENABLE_MOD_SETTINGS ON)
add_subdirectory("${FETCHCONTENT_SOURCE_DIR_BEDROCK_SDK}" bedrock-sdk EXCLUDE_FROM_ALL)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_library(ModSettingsDemo SHARED src/mod.cpp)
target_compile_features(ModSettingsDemo PRIVATE cxx_std_20)
target_link_libraries(ModSettingsDemo PRIVATE bedrock::oreui_settings_backend bedrock::logging)
target_compile_options(ModSettingsDemo PRIVATE /W4 /permissive- /utf-8)
set_target_properties(ModSettingsDemo PROPERTIES PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mod_settings_demo_preview.hpp"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_preview.py"
        "${CMAKE_CURRENT_BINARY_DIR}/mod_settings_demo_preview.hpp"
    DEPENDS make_preview.py
    VERBATIM)
target_sources(ModSettingsDemo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/mod_settings_demo_preview.hpp")
target_include_directories(ModSettingsDemo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

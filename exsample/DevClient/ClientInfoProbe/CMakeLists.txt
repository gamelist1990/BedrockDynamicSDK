cmake_minimum_required(VERSION 3.24)
project(ClientInfoProbe LANGUAGES C CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ClientInfoProbe requires the Windows x64 MSVC ABI")
endif()

set(FETCHCONTENT_SOURCE_DIR_BEDROCK_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../../.." CACHE PATH "SDK checkout")
set(BEDROCK_SDK_BUILD_TOOLS OFF)
set(BEDROCK_SDK_ENABLE_NATIVE_CLIENT ON)
set(BEDROCK_SDK_ENABLE_NATIVE_WORLD ON)
add_subdirectory("${FETCHCONTENT_SOURCE_DIR_BEDROCK_SDK}" bedrock-sdk EXCLUDE_FROM_ALL)

add_library(ClientInfoProbe SHARED src/client_info_probe.cpp)
target_link_libraries(ClientInfoProbe PRIVATE bedrock::sdk)
set_target_properties(ClientInfoProbe PROPERTIES PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

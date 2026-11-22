add_library(bedrock_stage_a STATIC
    src/settings.cpp src/system.cpp src/mod.cpp src/keybinds.cpp
    src/world.cpp src/entity.cpp src/display.cpp src/item_packet_decode.cpp
    src/logging.cpp src/profiles.cpp src/resolver.cpp src/actors.cpp src/world_render.cpp
    src/roster.cpp src/packets.cpp src/overlay.cpp)
if(WIN32)
    target_sources(bedrock_stage_a PRIVATE
        src/commands.cpp src/title_packets.cpp src/game_hooks.cpp)
    if(BEDROCK_SDK_ENABLE_NATIVE_WORLD)
        target_sources(bedrock_stage_a PRIVATE src/world_access.cpp)
    endif()
else()
    target_sources(bedrock_stage_a PRIVATE src/commands.cpp src/world_access.cpp src/title_packets.cpp src/game_hooks.cpp)
endif()
if(TARGET bedrock::world AND TARGET bedrock::client AND TARGET bedrock::game_hooks)
    target_sources(bedrock_stage_a PRIVATE src/client_host.cpp src/native_game_frame.cpp)
    target_compile_definitions(bedrock_stage_a PRIVATE BEDROCK_STAGE_A_NATIVE_DISPLAY)
    target_link_libraries(bedrock_stage_a PRIVATE bedrock::world bedrock::client bedrock::game_hooks)
else()
    target_sources(bedrock_stage_a PRIVATE src/client_host_stub.cpp)
endif()
set(vanilla_generator "${CMAKE_CURRENT_SOURCE_DIR}/../tools/vanilla_data.py")
set(ts_sdk_generator "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_ts_sdk.py")
set(binding_registry "${CMAKE_CURRENT_SOURCE_DIR}/../exsample/ScriptClient/data/binding-registry.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${BEDROCK_DATA_INPUTS} "${vanilla_generator}" "${ts_sdk_generator}" "${binding_registry}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${vanilla_generator}" generate
        --root "${CMAKE_CURRENT_SOURCE_DIR}/.." "--version=${BEDROCK_GAME_VERSION}"
        --cpp "${BEDROCK_GENERATED_INCLUDE}/bedrock/sdk/vanilla.hpp"
        --typescript "${CMAKE_CURRENT_BINARY_DIR}/generated/vanilla-data.ts"
    COMMAND_ERROR_IS_FATAL ANY)
if(EXISTS "${binding_registry}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${ts_sdk_generator}"
            --root "${CMAKE_CURRENT_SOURCE_DIR}/.."
            --registry "${binding_registry}"
            --out-dir "${CMAKE_CURRENT_SOURCE_DIR}/ts"
            --cpp-bootstrap "${CMAKE_CURRENT_SOURCE_DIR}/../exsample/ScriptClient/core/generated_sdk_bootstrap.hpp"
            "--version=${BEDROCK_GAME_VERSION}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()
if(TARGET bedrock::oreui_settings_backend)
    target_link_libraries(bedrock_stage_a PRIVATE bedrock::oreui_settings_backend)
else()
    target_sources(bedrock_stage_a PRIVATE src/settings_stub.cpp)
endif()
target_include_directories(bedrock_stage_a PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include" "${BEDROCK_GENERATED_INCLUDE}")
target_link_libraries(bedrock_stage_a PRIVATE
    bedrock_mod_settings_store bedrock_nlohmann_headers bedrock_resolver)
if(WIN32)
    target_link_libraries(bedrock_stage_a PRIVATE
        EnTT::EnTT bedrock::hook_runtime bedrock::game_hooks)
    if(BEDROCK_SDK_ENABLE_NATIVE_WORLD)
        target_link_libraries(bedrock_stage_a PRIVATE bedrock::world)
    endif()
    if(BEDROCK_SDK_ENABLE_NATIVE_CLIENT)
        target_link_libraries(bedrock_stage_a PRIVATE bedrock::client)
    endif()
endif()
target_compile_features(bedrock_stage_a PUBLIC cxx_std_20)
add_library(bedrock::stage_a ALIAS bedrock_stage_a)
# Public SDK entry point: aggregates the static core with the native adapters
# (resolver/world/client/hooks) that the public headers may require, so mods
# link only bedrock::sdk.
add_library(bedrock_sdk_public INTERFACE)
target_link_libraries(bedrock_sdk_public INTERFACE bedrock_stage_a)
add_library(bedrock::sdk ALIAS bedrock_sdk_public)

if(BEDROCK_SDK_BUILD_TOOLS AND BUILD_TESTING)
    foreach(test IN ITEMS core world)
        set(target "stage_a_${test}_test")
        add_executable(${target} "tests/${test}_test.cpp")
        target_link_libraries(${target} PRIVATE bedrock::stage_a)
        if(test STREQUAL "world")
            target_link_libraries(${target} PRIVATE bedrock::internal_headers)
        endif()
        target_compile_features(${target} PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(${target} PRIVATE /UNDEBUG /utf-8)
        else()
            target_compile_options(${target} PRIVATE -UNDEBUG)
        endif()
        add_test(NAME ${target} COMMAND ${target})
    endforeach()

    add_executable(stage_a_facades_test tests/facades_test.cpp)
    target_link_libraries(stage_a_facades_test PRIVATE bedrock::stage_a)
    target_compile_features(stage_a_facades_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(stage_a_facades_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(stage_a_facades_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME stage_a_facades_test COMMAND stage_a_facades_test)

    add_executable(stage_a_world_render_test tests/world_render_test.cpp)
    target_link_libraries(stage_a_world_render_test PRIVATE bedrock::stage_a)
    target_compile_features(stage_a_world_render_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(stage_a_world_render_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(stage_a_world_render_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME stage_a_world_render_test COMMAND stage_a_world_render_test)
endif()

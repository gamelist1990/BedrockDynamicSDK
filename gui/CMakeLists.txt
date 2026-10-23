include(FetchContent)

# Pinned source: upgraded deliberately, never an unreviewed moving branch.
set(BEDROCK_IMGUI_TAG "v1.92.9b-docking" CACHE STRING "Dear ImGui version (fixed by default)")
FetchContent_Declare(bedrock_imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG ${BEDROCK_IMGUI_TAG}
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(bedrock_imgui)

add_library(bedrock_gui STATIC
    ${bedrock_imgui_SOURCE_DIR}/imgui.cpp
    ${bedrock_imgui_SOURCE_DIR}/imgui_draw.cpp
    ${bedrock_imgui_SOURCE_DIR}/imgui_tables.cpp
    ${bedrock_imgui_SOURCE_DIR}/imgui_widgets.cpp
)
add_library(bedrock::gui ALIAS bedrock_gui)
target_include_directories(bedrock_gui PUBLIC
    ${bedrock_imgui_SOURCE_DIR}
)
# When the SDK is fetched into a client project, PROJECT_SOURCE_DIR is the
# consumer, so use this CMakeLists' parent, NOT the top-level project's source.
get_filename_component(BEDROCK_GUI_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
target_include_directories(bedrock_gui PUBLIC "${BEDROCK_GUI_SDK_ROOT}/include")
target_compile_features(bedrock_gui PUBLIC cxx_std_20)
if(MSVC)
    set_property(TARGET bedrock_gui PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(WIN32)
    add_library(bedrock_gui_host STATIC
        ${BEDROCK_GUI_SDK_ROOT}/src/gui_host_win32.cpp
        ${bedrock_imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${bedrock_imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    )
    add_library(bedrock::gui_host ALIAS bedrock_gui_host)
    target_include_directories(bedrock_gui_host PUBLIC
        "${BEDROCK_GUI_SDK_ROOT}/include")
    target_include_directories(bedrock_gui_host PRIVATE
        "${bedrock_imgui_SOURCE_DIR}/backends")
    target_link_libraries(bedrock_gui_host PUBLIC
        bedrock_gui d3d11 dxgi dwmapi user32 gdi32 imm32)
    if(MSVC)
        set_property(TARGET bedrock_gui_host PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

if(WIN32)
    # In-game renderer: intentionally separate from the Win32 GuiHost.
    # MinHook is statically linked only when this opt-in target is requested.
    add_library(bedrock_overlay_hook STATIC
        ${BEDROCK_GUI_SDK_ROOT}/third_party/minhook-vendor/src/buffer.c
        ${BEDROCK_GUI_SDK_ROOT}/third_party/minhook-vendor/src/hook.c
        ${BEDROCK_GUI_SDK_ROOT}/third_party/minhook-vendor/src/trampoline.c
        ${BEDROCK_GUI_SDK_ROOT}/third_party/minhook-vendor/src/hde/hde64.c
    )
    target_include_directories(bedrock_overlay_hook PUBLIC
        "${BEDROCK_GUI_SDK_ROOT}/third_party/minhook-vendor/include")
    target_include_directories(bedrock_overlay_hook PRIVATE
        "${BEDROCK_GUI_SDK_ROOT}/third_party/minhook-vendor/src"
        "${BEDROCK_GUI_SDK_ROOT}/third_party/minhook-vendor/src/hde")
    if(MSVC)
        set_property(TARGET bedrock_overlay_hook PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_library(bedrock_gui_overlay STATIC
        ${BEDROCK_GUI_SDK_ROOT}/src/gui_overlay_win32.cpp
        ${bedrock_imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${bedrock_imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    )
    add_library(bedrock::gui_overlay ALIAS bedrock_gui_overlay)
    target_link_libraries(bedrock_gui_overlay PUBLIC
        bedrock_gui bedrock_overlay_hook bedrock::symbols
        d3d11 d3d12 d3dcompiler dxgi dwmapi user32)
    target_include_directories(bedrock_gui_overlay PUBLIC
        "${BEDROCK_GUI_SDK_ROOT}/include")
    target_include_directories(bedrock_gui_overlay PRIVATE
        "${bedrock_imgui_SOURCE_DIR}/backends")
    if(MSVC)
        set_property(TARGET bedrock_gui_overlay PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

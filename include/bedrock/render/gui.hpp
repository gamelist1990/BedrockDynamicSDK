#pragma once
// Optional high-quality GUI facade built on Dear ImGui.
// Nothing in events/logger/symbols depends on this header.
// A GuiHost or your own renderer must create an ImGui context and call NewFrame.
// For fully custom widgets/layouts you can always use ImGui:: directly.
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <string_view>

namespace bedrock::gui {

using Color = ImVec4;
using Vec2 = ImVec2;
using Texture = ImTextureID;

struct Theme {
    static void dark(Color accent = Color{0.45f, 0.59f, 1.0f, 1.0f}) {
        ImGui::StyleColorsDark();
        auto& s = ImGui::GetStyle();
        s.WindowPadding = {18, 18};
        s.FramePadding = {12, 8};
        s.ItemSpacing = {11, 11};
        s.ItemInnerSpacing = {9, 7};
        s.WindowRounding = 16.0f;
        s.ChildRounding = 12.0f;
        s.FrameRounding = 9.0f;
        s.PopupRounding = 12.0f;
        s.ScrollbarRounding = 10.0f;
        s.GrabRounding = 9.0f;
        s.TabRounding = 9.0f;
        s.WindowBorderSize = 1.0f;
        s.FrameBorderSize = 0.0f;
        s.Colors[ImGuiCol_WindowBg] = {0.055f, 0.067f, 0.11f, 0.96f};
        s.Colors[ImGuiCol_ChildBg] = {0.088f, 0.105f, 0.16f, 0.98f};
        s.Colors[ImGuiCol_PopupBg] = {0.095f, 0.11f, 0.16f, 1.0f};
        s.Colors[ImGuiCol_Border] = {0.21f, 0.24f, 0.33f, 0.8f};
        s.Colors[ImGuiCol_FrameBg] = {0.13f, 0.155f, 0.22f, 1.0f};
        s.Colors[ImGuiCol_FrameBgHovered] = {0.2f, 0.25f, 0.36f, 1.0f};
        s.Colors[ImGuiCol_FrameBgActive] = {0.22f, 0.3f, 0.43f, 1.0f};
        s.Colors[ImGuiCol_Header] = {accent.x, accent.y, accent.z, 0.28f};
        s.Colors[ImGuiCol_HeaderHovered] = {accent.x, accent.y, accent.z, 0.46f};
        s.Colors[ImGuiCol_HeaderActive] = {accent.x, accent.y, accent.z, 0.65f};
        s.Colors[ImGuiCol_Button] = {accent.x * 0.7f, accent.y * 0.7f, accent.z * 0.7f, 0.8f};
        s.Colors[ImGuiCol_ButtonHovered] = {accent.x, accent.y, accent.z, 1.0f};
        s.Colors[ImGuiCol_ButtonActive] = {accent.x * 0.86f, accent.y * 0.86f, accent.z * 0.86f, 1.0f};
        s.Colors[ImGuiCol_CheckMark] = accent;
        s.Colors[ImGuiCol_SliderGrab] = accent;
        s.Colors[ImGuiCol_SliderGrabActive] = {accent.x, accent.y, accent.z, 1.0f};
        s.Colors[ImGuiCol_Separator] = {0.21f, 0.24f, 0.33f, 0.7f};
        s.Colors[ImGuiCol_Text] = {0.94f, 0.95f, 0.99f, 1.0f};
        s.Colors[ImGuiCol_TextDisabled] = {0.55f, 0.60f, 0.68f, 1.0f};
        s.Colors[ImGuiCol_Tab] = {0.10f, 0.12f, 0.18f, 1.0f};
        s.Colors[ImGuiCol_TabHovered] = {accent.x, accent.y, accent.z, 0.62f};
    }

    static void scale(float factor) {
        if (factor > 0.25f && factor <= 4.0f)
            ImGui::GetStyle().ScaleAllSizes(factor);
    }
};

class Window {
public:
    Window(const char* title, bool* open = nullptr,
           ImGuiWindowFlags flags = ImGuiWindowFlags_None)
        : visible_(ImGui::Begin(title, open, flags)) {}
    ~Window() { ImGui::End(); }
    explicit operator bool() const noexcept { return visible_; }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
private:
    bool visible_{};
};

class Panel {
public:
    Panel(const char* title, Vec2 size = {0, 0},
          ImGuiChildFlags flags = ImGuiChildFlags_Borders)
        : visible_(ImGui::BeginChild(title, size, flags)) {}
    ~Panel() { ImGui::EndChild(); }
    explicit operator bool() const noexcept { return visible_; }
    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;
private:
    bool visible_{};
};

class Tabs {
public:
    explicit Tabs(const char* label, ImGuiTabBarFlags flags = 0)
        : visible_(ImGui::BeginTabBar(label, flags)) {}
    ~Tabs() { if (visible_) ImGui::EndTabBar(); }
    explicit operator bool() const noexcept { return visible_; }
private:
    bool visible_{};
};

class Tab {
public:
    explicit Tab(const char* label) : visible_(ImGui::BeginTabItem(label)) {}
    ~Tab() { if (visible_) ImGui::EndTabItem(); }
    explicit operator bool() const noexcept { return visible_; }
private:
    bool visible_{};
};

inline void text(std::string_view label) {
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
}
inline void muted(std::string_view label) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    text(label);
    ImGui::PopStyleColor();
}
inline void heading(std::string_view label, float fontScale = 1.3f) {
    ImGui::SetWindowFontScale(fontScale);
    text(label);
    ImGui::SetWindowFontScale(1.0f);
}
inline void separator() { ImGui::Separator(); }
inline void sameLine(float offset = 0, float spacing = -1) { ImGui::SameLine(offset, spacing); }
inline void space(float height = 8.0f) { ImGui::Dummy({0, height}); }
inline bool button(const char* label, Vec2 size = {0, 0}) {
    return ImGui::Button(label, size);
}
inline bool toggle(const char* label, bool& value) {
    return ImGui::Checkbox(label, &value);
}
inline bool slider(const char* label, float& value, float minimum, float maximum) {
    return ImGui::SliderFloat(label, &value, minimum, maximum);
}
inline bool colorPicker(const char* label, Color& value) {
    return ImGui::ColorEdit4(label, &value.x);
}
inline bool combo(const char* label, int& current,
                  const char* const items[], int count) {
    return ImGui::Combo(label, &current, items, count);
}
inline bool input(const char* label, char* buffer, std::size_t capacity) {
    return ImGui::InputText(label, buffer, capacity);
}
inline void tooltip(std::string_view label) {
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        text(label);
        ImGui::EndTooltip();
    }
}
inline void progress(float fraction, Vec2 size = {-1, 0}) {
    ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), size);
}

namespace draw {
    inline ImU32 rgba(float r, float g, float b, float a = 1.f) {
        return ImGui::ColorConvertFloat4ToU32(Color{r, g, b, a});
    }
    inline void rect(Vec2 a, Vec2 b, ImU32 color,
                     float rounding = 0, float thickness = 1.0f) {
        ImGui::GetWindowDrawList()->AddRect(a, b, color, rounding, 0, thickness);
    }
    inline void filledRect(Vec2 a, Vec2 b, ImU32 color, float rounding = 0) {
        ImGui::GetWindowDrawList()->AddRectFilled(a, b, color, rounding);
    }
    inline void gradient(Vec2 a, Vec2 b,
                         ImU32 topLeft, ImU32 topRight,
                         ImU32 bottomRight, ImU32 bottomLeft) {
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
            a, b, topLeft, topRight, bottomRight, bottomLeft);
    }
    inline void line(Vec2 a, Vec2 b, ImU32 color, float thickness = 1) {
        ImGui::GetWindowDrawList()->AddLine(a, b, color, thickness);
    }
    inline void circle(Vec2 position, float radius, ImU32 color, int segments = 0) {
        ImGui::GetWindowDrawList()->AddCircleFilled(position, radius, color, segments);
    }
    inline void text(Vec2 position, ImU32 color, const char* value) {
        ImGui::GetWindowDrawList()->AddText(position, color, value);
    }
}
inline float smooth(float from, float to, float speed, float deltaSeconds) noexcept {
    const float alpha = 1.0f - std::exp(-std::max(0.f, speed) * std::max(0.f, deltaSeconds));
    return from + (to - from) * alpha;
}

} // namespace bedrock::gui

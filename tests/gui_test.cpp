#include <bedrock/gui.hpp>
#include <cassert>
#include <cmath>
int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    // Headless test has no GPU backend but must advertise the modern texture
    // path to let ImGui 1.92+ build fonts before NewFrame().
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    // Unit tests do not render on GPU; focus on UI tree and draw data generation.
    io.Fonts->AddFontDefault();
    bedrock::gui::Theme::dark();
    bool selected = false;
    float density = 0.7f;
    int mode = 0;
    char input[64] = "Demo";
    ImGui::NewFrame();
    {
        bedrock::gui::Window main("Client settings", nullptr,
                                 ImGuiWindowFlags_NoSavedSettings);
        if (main) {
            bedrock::gui::heading("Bedrock GUI");
            bedrock::gui::muted("Custom widgets / panels");
            bedrock::gui::toggle("AutoSprint", selected);
            bedrock::gui::slider("Opacity", density, 0.1f, 1.0f);
            static const char* modes[] = {"None", "Simple", "Full"};
            bedrock::gui::combo("Mode", mode, modes, 3);
            bedrock::gui::input("Title", input, sizeof(input));
            bedrock::gui::progress(0.65f);
            {
                bedrock::gui::Panel panel("Main", {350, 180});
                if (panel) {
                    bedrock::gui::text("Custom canvas");
                    const auto p = ImGui::GetCursorScreenPos();
                    using namespace bedrock::gui::draw;
                    filledRect(p, {p.x + 100.f, p.y + 50.f},
                               rgba(.2f, .4f, .95f, 1.f), 9.f);
                    gradient({p.x + 112.f,p.y}, {p.x + 240.f,p.y + 44.f},
                             rgba(1.f,0.f,0.f), rgba(0.f,1.f,0.f),
                             rgba(0.f,0.f,1.f), rgba(1.f,1.f,0.f));
                    circle({p.x + 250.f,p.y + 22.f}, 7, rgba(1.f,1.f,1.f));
                }
            }
            {
                bedrock::gui::Tabs tabs("tabs");
                if (tabs) {
                    bedrock::gui::Tab tab("Info");
                    if (tab) bedrock::gui::text("Tab content");
                }
            }
        }
    }
    ImGui::Render();
    assert(ImGui::GetDrawData() != nullptr);
    assert(bedrock::gui::smooth(0.f, 1.f, 10.f, 0.016f) > 0.f);
    assert(bedrock::gui::smooth(0.f, 1.f, 10.f, 0.f) == 0.f);
    ImGui::DestroyContext();
}

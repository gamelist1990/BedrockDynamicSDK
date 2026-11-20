#include "client.hpp"

#include <bedrock/sdk.hpp>
#include <stdexcept>

namespace dynamic_client {

void registerKeybinds(bedrock::sdk::Mod& mod) {
    using bedrock::sdk::Key;
    auto& bindings = mod.keybinds();
    const auto add = [&](const char* action, const char* labelKey, Key defaultKey) {
        const auto added = bindings.add(action, labelKey, defaultKey);
        if (!added) throw std::runtime_error(added.error().message);
    };
    add("zoom", "key.zoom", Key::C);
    add("tablist", "key.tablist", Key::Tab);
    add("freecam", "key.freecam", Key::M);
    bindings.locale("en_US", {{"key.zoom", "Zoom"}, {"key.tablist", "Show player list"},
                              {"key.freecam", "Toggle free camera"}});
    bindings.locale("ja_JP", {{"key.zoom", "ズーム"}, {"key.tablist", "プレイヤー一覧を表示"},
                              {"key.freecam", "フリーカメラを切り替える"}});
}

} // namespace dynamic_client

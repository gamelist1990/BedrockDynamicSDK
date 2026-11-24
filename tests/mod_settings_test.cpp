#include "bedrock/detail/mod_settings_oreui.hpp"

#include <cassert>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <fstream>
#include <limits>

using namespace bedrock::detail::oreui_settings;
using namespace bedrock::detail::oreui_settings::impl;

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path() / ("bedrock-mod-settings-" + std::to_string(nonce));
    assert(std::filesystem::create_directory(dir));
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(dir, ignored); }
    } cleanup{dir};

    // A complete 1x1 PNG. The renderer, not the store, decodes IDAT pixels.
    constexpr unsigned char png[] = {
        137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,4,0,0,0,
        181,28,12,2,0,0,0,11,73,68,65,84,120,218,99,100,248,15,0,1,5,1,1,39,24,227,102,
        0,0,0,0,73,69,78,68,174,66,96,130
    };
    const auto image = dir / "pixel.png";
    std::ofstream(image, std::ios::binary).write(reinterpret_cast<const char*>(png), sizeof png);
    const auto utf8 = image.u8string();
    const std::string path(utf8.begin(), utf8.end());
    const Choice choices[] = {{"a", tr("choice.alpha")}, {"b", "Beta"}};
    const ControlDescriptor rows[] = {
        toggle("toggle", tr("toggle.label"), false, tr("toggle.help")),
        slider("number", "Number", 4, 0, 10, 1),
        {.id="option", .label="Option", .kind=Kind::Option, .initial={Kind::Option, 0, 0, "a"},
         .choices=choices, .choice_count=2},
        {.id="string", .label="String", .kind=Kind::String, .initial={Kind::String, 0, 0, "test"}},
        {.id="action", .label="Action", .kind=Kind::Action},
        text("text", "Text", tr("missing.description")),
        {.id="image", .label="Image", .kind=Kind::Image, .image_path=path.c_str()},
        {.id="embedded-image", .label="Embedded", .kind=Kind::Image, .image_png=png, .image_png_size=sizeof png},
        {.id="drop-zone", .label="Drop here", .kind=Kind::DropZone},
    };
    int calls = 0;
    char ja_title[] = "設定ページ";
    const Translation en[] = {{"page.title", "Example"}, {"mod.name", "Example Mod"},
        {"toggle.label", "Toggle"}, {"toggle.help", "English-only help"}, {"choice.alpha", "Alpha"}};
    const Translation ja[] = {{"page.title", ja_title}, {"mod.name", "設定モッド"},
        {"toggle.label", "切り替え"}, {"choice.alpha", "選択肢 A"}};
    const Locale dictionaries[] = {locale("en_US", en), locale("ja_JP", ja)};
    PageDescriptor page = makePage("example", tr("page.title"), rows,
        [](void* data, const char* id, const RawValue* value) noexcept {
            assert(*id && value);
            ++*static_cast<int*>(data);
        }, &calls);
    page.mod_name = tr("mod.name");
    page.translations(dictionaries);
    Store store(dir / "config");
    assert(store.add(page) == Result::Ok);
    assert(store.add(page) == Result::AlreadyRegistered);
    page.mod_id = "../example";
    assert(store.add(page) == Result::Invalid);
    page.mod_id = "other";
    assert(store.add(page) == Result::Ok);

    const auto id = [](const char* control) { return Store::wireId("example", control); };
    assert(store.dispatch("vanilla.setting", {Kind::Boolean, 1}) == Result::NotFound);
    assert(store.dispatch(id("nonexistent"), {Kind::Boolean, 1}) == Result::NotFound);
    assert(store.dispatch(id("text"), {Kind::Text}) == Result::WrongType);
    assert(store.dispatch(id("toggle"), {Kind::Number, 0, 1}) == Result::WrongType);
    assert(store.dispatch(id("toggle"), {Kind::Boolean, 2}) == Result::Invalid);
    assert(store.dispatch(id("number"), {Kind::Number, 0, std::numeric_limits<double>::quiet_NaN()}) == Result::Invalid);
    assert(store.dispatch(id("option"), {Kind::Option, 0, 0, "missing"}) == Result::Invalid);
    assert(store.dispatch(id("string"), {Kind::String, 0, 0, "\xff"}) == Result::Invalid);
    assert(store.dispatch(id("string"), {Kind::String, 0, 0, "preview"}, false) == Result::Ok);
    assert(!std::filesystem::exists(dir / "config"));
    assert(calls == 0);

    assert(store.dispatch(id("toggle"), {Kind::Boolean, 1}) == Result::Ok);
    assert(store.dispatch(id("toggle"), {Kind::Boolean, 1}) == Result::Ok);
    assert(store.dispatch(id("number"), {Kind::Number, 0, 10}) == Result::Ok);
    assert(store.dispatch(id("option"), {Kind::Option, 0, 0, "b"}) == Result::Ok);
    assert(store.dispatch(id("string"), {Kind::String, 0, 0, "stored"}) == Result::Ok);
    assert(store.dispatch(id("action"), {Kind::Action}) == Result::Ok);
    assert(calls == 5);
    assert(!std::filesystem::exists(dir / "config/mod-other.json"));
    assert(std::get<bool>(store.snapshot()[1].rows[0].value) == false);

    Store reopened(dir / "config");
    page.mod_id = "example";
    assert(reopened.add(page) == Result::Ok);
    const auto saved = reopened.snapshot().front().rows;
    assert(std::get<bool>(saved[0].value));
    assert(std::get<double>(saved[1].value) == 10);
    assert(std::get<std::string>(saved[2].value) == "b");
    assert(std::get<std::string>(saved[3].value) == "stored");
    assert(std::holds_alternative<std::monostate>(saved[4].value));
    assert(saved[6].image_url == "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=");
    assert(saved[7].image_url == saved[6].image_url);
    RawValue read_back{};
    char text[32]{};
    assert(reopened.read("example", "option", read_back, text, 1) == Result::BufferTooSmall);
    assert(reopened.read("example", "option", read_back, text, sizeof text) == Result::Ok);
    assert(read_back.kind == Kind::Option && read_back.text == text && std::string_view(text) == "b");
    assert(reopened.read("example", "text", read_back, nullptr, 0) == Result::WrongType);
    assert(reopened.remove(page.mod_id) == Result::Ok);
    assert(reopened.dispatch(id("action"), {Kind::Action}) == Result::NotFound);

    // A file in place of the storage directory must not change the live value.
    std::ofstream(dir / "blocked").put('x');
    Store blocked(dir / "blocked");
    assert(blocked.add(page) == Result::StorageError);
    Store delayed_failure(dir / "replace");
    assert(delayed_failure.add(page) == Result::Ok);
    std::ofstream(dir / "replace").put('x');
    assert(delayed_failure.dispatch(id("toggle"), {Kind::Boolean, 1}) == Result::StorageError);
    assert(!std::get<bool>(delayed_failure.snapshot().front().rows[0].value));
    assert(calls == 5);

    const auto snapshot = settingsResource(store, "menus/1.json");
    assert(snapshot && snapshot->find("bedrockdynamic-mod-example") != std::string::npos &&
        snapshot->find("data:image/png;base64,") != std::string::npos);
    ja_title[0] = '\0'; // Registered dictionaries own their strings.
    const auto japanese = settingsResource(store, "menus/2/ja_JP.json");
    assert(japanese && japanese->find(R"("title":"設定ページ")") != std::string::npos);
    assert(japanese->find(R"("modName":"設定モッド")") != std::string::npos);
    assert(japanese->find(R"("label":"切り替え")") != std::string::npos);
    assert(japanese->find(R"("description":"English-only help")") != std::string::npos);
    assert(japanese->find(R"("description":"missing.description")") != std::string::npos);
    assert(japanese->find(R"("id":"a","label":"選択肢 A")") != std::string::npos);
    assert(japanese->find(R"("id":"b","label":"Beta")") != std::string::npos);
    assert(japanese->find(R"("value":"b")") != std::string::npos);
    assert(settingsResource(store, "menus/3/fr_FR.json"));
    assert(settingsResource(store, "menus/4/en_US.json") == snapshot);
    assert(!settingsResource(store, "menus/5/ja_JP/foo.json"));
    assert(store.read("example", "$visible", read_back, nullptr, 0) == Result::Ok && !read_back.boolean);
    DropZoneRect zone{};
    auto readZone = [&] {
        return store.read("example", "$drop-zone", read_back, reinterpret_cast<char*>(&zone), sizeof zone);
    };
    assert(readZone() == Result::NotFound);
    assert(!settingsResource(store, "zone/1/example/1920/1080/700/200/1800.json"));
    assert(!settingsResource(store, "zone/1/example/1920/1080/700/200/1800/300/0.json"));
    assert(!settingsResource(store, "zone/2/example/1920/1080/700/2x/1800/300.json"));
    assert(settingsResource(store, "zone/3/example/1920/1080/700/200/1921/300.json") == R"({"ok":false})");
    assert(settingsResource(store, "zone/4/example/1920/1080/700/200/1800/300.json") == R"({"ok":true})");
    assert(readZone() == Result::Ok && read_back.kind == Kind::DropZone);
    assert(zone.width == 1920 && zone.height == 1080 && zone.left == 700 &&
           zone.top == 200 && zone.right == 1800 && zone.bottom == 300);
    assert(store.dispatch(id("drop-zone"), {Kind::DropZone}) == Result::WrongType);
    assert(settingsResource(store, "zone/5/example/0/0/0/0/0/0.json") == R"({"ok":true})");
    assert(readZone() == Result::NotFound);
    assert(settingsResource(store, "zone/6/example/1920/1080/700/200/1800/300.json") == R"({"ok":true})");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    assert(readZone() == Result::NotFound);
    assert(!settingsResource(store, "visibility/not-a-sequence/1/example.json"));
    assert(settingsResource(store, "visibility/6/1/example.json"));
    assert(store.read("example", "$visible", read_back, nullptr, 0) == Result::Ok && read_back.boolean);
    assert(settingsResource(store, "visibility/7/0/example.json"));
    assert(store.read("example", "$visible", read_back, nullptr, 0) == Result::Ok && !read_back.boolean);
    const auto write = [&](std::string_view payload) {
        constexpr char h[] = "0123456789abcdef";
        std::string path = "write/123/";
        for (unsigned char byte : payload) {
            path += h[byte >> 4];
            path += h[byte & 15];
        }
        return settingsResource(store, path + ".json");
    };
    assert(write(R"({"id":"bedrockdynamic.mod.example.string","kind":"string","value":"雪/雪"})")
        ->find(R"("ok":true)") != std::string::npos);
    assert(std::get<std::string>(store.snapshot().front().rows[3].value) == "雪/雪");
    assert(write(R"({"id":"vanilla.settings.toggle","kind":"boolean","value":true})")
        ->find(R"("ok":false)") != std::string::npos);
    assert(write(R"({"id":"bedrockdynamic.mod.example.toggle","kind":"boolean","value":"true"})")
        ->find(R"("ok":false)") != std::string::npos);
    assert(!settingsResource(store, "write/not-a-sequence/bad.json"));
    assert(!settingsResource(store, "menus/.json"));
    assert(!settingsResource(store, "menus/../2.json"));
    assert(calls == 6);

    const ControlDescriptor keys[] = {
        {.id="zoom", .label=tr("key.zoom"), .kind=Kind::Key, .initial={Kind::Key, 0, 'C'}},
    };
    const Translation keyTranslations[] = {{"key.zoom", "ズーム"}};
    const Locale keyLocales[] = {locale("ja_JP", keyTranslations)};
    auto keyPage = makePage("test-keys", "Test Keybinds", keys);
    keyPage.translations(keyLocales);
    assert(store.add(keyPage) == Result::Ok);
    const auto keySnapshot = settingsResource(store, "menus/6/ja_JP.json");
    assert(keySnapshot && keySnapshot->find(R"("defaultKey":67)") != std::string::npos);
    assert(keySnapshot->find(R"("label":"ズーム")") != std::string::npos);
    assert(keySnapshot->find(R"("keybind.title":"Mod キー設定")") != std::string::npos);
    const auto keyId = Store::wireId("test-keys", "zoom");
    assert(store.dispatch(keyId, {Kind::Key, 0, 88.5}) == Result::Invalid);
    assert(store.dispatch(keyId, {Kind::Key, 0, 27}) == Result::Invalid);
    assert(write(R"({"id":"bedrockdynamic.mod.test-keys.zoom","kind":"key","value":88})")
        ->find(R"("ok":true)") != std::string::npos);
    Store keyReopened(dir / "config");
    assert(keyReopened.add(keyPage) == Result::Ok);
    assert(keyReopened.read("test-keys", "zoom", read_back, nullptr, 0) == Result::Ok);
    assert(read_back.kind == Kind::Key && read_back.number == 'X');
    assert(keyReopened.dispatch(keyId, {Kind::Key, 0, 'C'}) == Result::Ok);
    Store keyReset(dir / "config");
    assert(keyReset.add(keyPage) == Result::Ok);
    assert(keyReset.read("test-keys", "zoom", read_back, nullptr, 0) == Result::Ok);
    assert(read_back.number == 'C');

    // Native language resolution path: an "auto" request resolves the game
    // language on the native side and reports it in the response.
#ifdef _WIN32
    _putenv_s("BEDROCK_GAME_LANGUAGE", "ja_JP");
#else
    setenv("BEDROCK_GAME_LANGUAGE", "ja_JP", 1);
#endif
    const auto autoSnapshot = settingsResource(store, "menus/8/auto.json");
    assert(autoSnapshot && autoSnapshot->find(R"("language":"ja_JP")") != std::string::npos);
    assert(autoSnapshot->find(R"("locale":"ja_JP")") != std::string::npos);
    assert(autoSnapshot->find(R"("menuLabel":"Mod 設定")") != std::string::npos);
#ifdef _WIN32
    _putenv_s("BEDROCK_GAME_LANGUAGE", "");
#else
    unsetenv("BEDROCK_GAME_LANGUAGE");
#endif
    const auto defaultSnapshot = settingsResource(store, "menus/9/auto.json");
    assert(defaultSnapshot && defaultSnapshot->find(R"("language":"en_US")") != std::string::npos);
}

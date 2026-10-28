#include "check_sig_core.hpp"
#include <iostream>

namespace {
using namespace checksig;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F fn, const char* message) {
    bool failed = false;
    try { fn(); } catch (const std::exception&) { failed = true; }
    require(failed, message);
}
void put(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    require(static_cast<bool>(out), "could not create fixture");
}
json profile() {
    return {{"schema", 1}, {"target", {
        {"product", "minecraft-bedrock"}, {"platform", "windows"}, {"arch", "x64"},
        {"version", "1.26.60.5"}, {"sha256", std::string(64, 'a')},
        {"size_of_image", 8192}, {"text_rva", "0x1000"}, {"text_size", 4096}}},
        {"symbols", {{"test", {{"rva", "0x1010"}, {"signature", "55 48 89 E5 ??"},
                              {"entry_check", "55 48 89 E5"}, {"status", "candidate"}}}}}};
}
void patterns() {
    std::vector<std::byte> bytes(64, std::byte{0xCC});
    for (auto start : {7, 31}) {
        const unsigned values[] = {0x01, 0x02, 0x55, 0x48, 0x89, 0xE5, 0x77};
        for (std::size_t i = 0; i < 7; ++i) bytes[start+i] = std::byte(values[i]);
    }
    // A fixed run after wildcards must come from the pattern, never the image prefix.
    const auto result = findMatches(bytes, "?? ?? 55 48 89 E5 ??");
    require(result.offsets == std::vector<std::size_t>({7, 31}), "masked anchor search regression");
    require(findMatches(bytes, "DE AD BE EF").offsets.empty(), "no-hit search");
    require(!parsePattern("55 GG 00 11"), "invalid pattern accepted");
    std::vector<std::byte> repeated(520, std::byte{0x55});
    const auto capped = findMatches(repeated, "55 55 55 55");
    require(capped.capped && capped.offsets.empty(), "scan cap must reject partial results");
}
void profiles(const fs::path& repository) {
    auto valid = profile();
    require(parseProfile(valid.dump())["symbols"].size() == 1, "valid profile rejected");
    require(parseProfile(std::string("\xEF\xBB\xBF") + valid.dump()).is_object(), "UTF-8 BOM rejected");
    auto invalid = valid;
    invalid["target"]["arch"] = "arm64";
    rejects([&]{ parseProfile(invalid.dump()); }, "wrong architecture accepted");
    invalid = valid; invalid["symbols"]["test"]["rva"] = "0x100000001";
    rejects([&]{ parseProfile(invalid.dump()); }, "overflowed RVA accepted");
    invalid = valid; invalid["symbols"]["test"]["entry_check"] = "?? ?? ?? ??";
    rejects([&]{ parseProfile(invalid.dump()); }, "wildcard-only entry guard accepted");
    invalid = valid; invalid["symbols"]["test"]["abi_verified"] = "true";
    rejects([&]{ parseProfile(invalid.dump()); }, "invalid boolean accepted");
    invalid = valid; invalid["target"]["text_size"] = 8192;
    rejects([&]{ parseProfile(invalid.dump()); }, "out-of-image text accepted");
    rejects([&]{ readU32("0x1000junk"); }, "trailing RVA text accepted");
    rejects([&]{ readU32(-1); }, "negative RVA accepted");
    for (const auto& version : {"1.26.52.3", "1.26.60.5"}) {
        std::string text, error;
        require(readBounded(repository / "data" / version / "windows-x64.json", text, maxProfileBytes, error), "repo profile read failed");
        require(parseProfile(text).is_object(), "shipped profile validation failed");
    }
}
void inbox(const fs::path& root) {
    const auto singular = root / "command";
    const auto plural = root / "commands";
    fs::create_directories(singular); fs::create_directories(plural);
    const auto a = singular / fs::path(u8"日本語-x64.JSON");
    const auto b = plural / "windows-x64.json";
    const auto cmd = plural / "01-detect.txt";
    const std::vector<fs::path> directories{singular, plural};
    Inbox inbox;
    const auto start = Inbox::Clock::now();
    put(a, "{"); put(b, profile().dump()); put(cmd, "detect\r\nlist\r\n");
    require(inbox.poll(directories, start).empty(), "new files read before settle");
    put(a, profile().dump());
    require(inbox.poll(directories, start + std::chrono::milliseconds(300)).empty(), "copy-in-progress processed");
    const auto initial = inbox.poll(directories, start + std::chrono::milliseconds(1100));
    require(initial.size() == 3, "both command folders / JSON extension / command not processed");
    require(initial[0].profile && initial[1].profile && !initial[2].profile, "JSON must precede commands");
    require(inbox.poll(directories, start + std::chrono::milliseconds(2200)).empty(), "unchanged inputs repeated");
    const auto stamp = fs::last_write_time(a);
    auto text = profile().dump();
    text[text.find("candidate")] = 'C';
    put(a, text); fs::last_write_time(a, stamp);
    const auto replaced = inbox.poll(directories, start + std::chrono::milliseconds(3300));
    require(replaced.size() == 1 && replaced[0].path == a, "same-size same-mtime replacement missed");
    inbox.retry(a);
    require(inbox.poll(directories, start + std::chrono::milliseconds(4400)).size() == 1, "failed output consumer cannot retry JSON");
    require(inbox.poll(directories, start + std::chrono::milliseconds(4500), true).size() == 2, "detect rescan must re-read JSON only");
    fs::remove(a);
    inbox.poll(directories, start + std::chrono::milliseconds(5500));
    put(a, text); fs::last_write_time(a, stamp);
    inbox.poll(directories, start + std::chrono::milliseconds(6600));
    require(inbox.poll(directories, start + std::chrono::milliseconds(7700)).size() == 1, "deleted/recreated file skipped");
    put(singular / "broken.json", "{broken");
    inbox.poll(directories, start + std::chrono::milliseconds(8800));
    const auto broken = inbox.poll(directories, start + std::chrono::milliseconds(9900));
    require(broken.size() == 1, "malformed JSON must be delivered for a useful error");
    rejects([&]{ parseProfile(broken[0].text); }, "malformed JSON accepted");
    put(singular / "broken.json", profile().dump());
    inbox.poll(directories, start + std::chrono::milliseconds(11000));
    require(inbox.poll(directories, start + std::chrono::milliseconds(12100)).size() == 1, "corrected JSON not retried");
    std::string content, error;
    require(!readBounded(b, content, 2, error), "bounded read exceeded limit");
    const auto lines = commandLines("\xEF\xBB\xBF  detect\r\n # comment\r\n\r\n probe Keyboard::feed \r\n");
    require(lines == std::vector<std::string>({"detect", "probe Keyboard::feed"}), "BOM/CRLF/multiline commands broken");
}
}
int main(int argc, char** argv) {
    fs::path temporary;
    try {
        if (argc != 2) throw std::runtime_error("usage: checksig-core-test <repository-root>");
        temporary = fs::temp_directory_path() / ("checksig-core-" + std::to_string(Inbox::Clock::now().time_since_epoch().count()));
        patterns(); profiles(fs::path(argv[1])); inbox(temporary);
        fs::remove_all(temporary);
        std::cout << "CheckSig core tests passed (patterns, profiles, inbox, commands)\n";
        return 0;
    } catch (const std::exception& e) {
        if (!temporary.empty()) fs::remove_all(temporary);
        std::cerr << e.what() << '\n';
        return 1;
    }
}

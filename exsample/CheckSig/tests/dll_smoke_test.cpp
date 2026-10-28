#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <bedrock/remote_catalog.hpp>
#include "check_sig_core.hpp"
#include <iomanip>
#include <iostream>
#include <thread>

namespace {
using namespace checksig;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void put(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    require(static_cast<bool>(out), "could not write fixture");
}
json readJson(const fs::path& path) {
    std::string text, error;
    if (!readBounded(path, text, 32 * 1024 * 1024, error)) return nullptr;
    try { return json::parse(text); } catch (...) { return nullptr; }
}
template<class F> void waitFor(F fn, const char* message) {
    const auto until = Inbox::Clock::now() + std::chrono::seconds(20);
    do {
        if (fn()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (Inbox::Clock::now() < until);
    throw std::runtime_error(message);
}
__declspec(noinline) bool __fastcall keyboardFixture(int key, bool down) {
    volatile int scratch[16]{};
    scratch[0] = key;
    SetLastError(static_cast<DWORD>(scratch[0]));
    return down && scratch[0] == key;
}
__declspec(noinline) int fixture(int value) {
    volatile int result = value * 3 + 17;
    return result;
}
}
int wmain(int argc, wchar_t** argv) {
    fs::path output;
    bool ownOutput = false;
    try {
        require(argc == 2, "pass the CheckSig DLL path");
        PWSTR documents{};
        require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &documents)), "Documents unavailable");
        output = fs::path(documents) / L"PEXData" / L"CheckSig";
        CoTaskMemFree(documents);
        // Never modify existing developer/user data. Hosted CI starts with no inbox.
        require(!fs::exists(output), "smoke test requires a clean CheckSig data directory");
        fs::create_directories(output / L"command");
        fs::create_directories(output / L"commands");
        ownOutput = true;
        bedrock::GameIdentity identity;
        std::string error;
        require(bedrock::inspectLoadedGame(identity, error), "host identity inspection failed");
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto address = reinterpret_cast<std::uintptr_t>(&fixture);
        require(fixture(2) == 23, "fixture failed");
        std::ostringstream pattern;
        const auto bytes = reinterpret_cast<const unsigned char*>(address);
        for (int i = 0; i < 16; ++i) {
            if (i) pattern << ' ';
            pattern << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]);
        }
        json profile = {{"schema", 1}, {"target", {
            {"product", "minecraft-bedrock"}, {"kind", "client"}, {"platform", "windows"}, {"arch", "x64"},
            {"version", "test-host"}, {"sha256", identity.fileSha256}, {"text_sha256", identity.textSha256},
            {"size_of_image", identity.imageSize}, {"text_rva", identity.textRva}, {"text_size", identity.textSize}}},
            {"symbols", {{"fixture", {{"rva", address-base}, {"signature", pattern.str()},
                {"entry_check", pattern.str()}, {"status", "candidate"}, {"abi_verified", false}, {"runtime_verified", false}}}}}};
        profile["native_smoke"] = {{"marker", "exact-current-build-metadata"}};
        const auto keyboardAddress = reinterpret_cast<std::uintptr_t>(&keyboardFixture);
        std::ostringstream keyboardPattern;
        const auto keyboardBytes = reinterpret_cast<const unsigned char*>(keyboardAddress);
        for (int i = 0; i < 32; ++i) {
            if (i) keyboardPattern << ' ';
            keyboardPattern << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(keyboardBytes[i]);
        }
        profile["symbols"]["Keyboard::feed"] = {{"rva", keyboardAddress-base},
            {"signature", keyboardPattern.str()}, {"entry_check", keyboardPattern.str()},
            {"status", "candidate"}, {"abi_verified", false}, {"runtime_verified", false}};
        const auto matchingPath = output / L"command" / L"windows-x64.json";
        put(matchingPath, profile.dump()); // Must also work when JSON predates DLL startup.
        require(LoadLibraryW(argv[1]) != nullptr, "CheckSig DLL failed to load");
        const auto reportPath = output / L"detect" / L"result.json";
        waitFor([&] {
            const auto report = readJson(reportPath);
            if (!report.is_object() || !report.contains("profiles") || report["profiles"].empty()) return false;
            const auto& result = report["profiles"][0];
            return result.value("status", std::string{}) == "exact-build-match" &&
                result.value("entry_guard_hit_count", 0) == 2 && result.value("readable_symbol_count", 0) == 2 &&
                result.value("hook_ready_count", -1) == 1;
        }, "JSON drop did not produce exact-build / readable / entry-match diagnostics");
        const auto badPath = output / L"commands" / L"bad.json";
        put(badPath, "{broken");
        waitFor([&] {
            const auto report = readJson(reportPath);
            if (!report.is_object() || !report.contains("profiles")) return false;
            for (const auto& result : report["profiles"])
                if (result.value("status", std::string{}) == "invalid-profile") return !report["active_profile"].is_null();
            return false;
        }, "malformed JSON was not reported or displaced the matching definition");
        auto mismatch = profile;
        mismatch["target"]["sha256"] = std::string(64, '0');
        put(badPath, mismatch.dump());
        waitFor([&] {
            const auto report = readJson(reportPath);
            if (!report.is_object() || !report.contains("profiles")) return false;
            for (const auto& result : report["profiles"])
                if (result.value("status", std::string{}) == "different-executable-build")
                    return !report["active_profile"].is_null();
            return false;
        }, "corrected JSON was not retried / mismatch hid the active profile");
        put(output / L"command" / L"next.txt", "\xEF\xBB\xBF detect\r\nlist\r\nprobe fixture\r\nread rva:" +
            std::to_string(address-base) + " bytes:4\r\n");
        waitFor([&] {
            std::string events, failure;
            return readBounded(output / L"events.jsonl", events, 32*1024*1024, failure) &&
                events.find("probe-result") != std::string::npos && events.find("memory-read") != std::string::npos &&
                !fs::exists(output / L"command" / L"next.txt");
        }, "BOM/multiline detect/list/probe/read commands failed");
        put(output / L"command" / L"port.txt", "source \"windows-x64.json\"\nport test-host\n");
        waitFor([&] {
            const auto generated = readJson(output / L"candidate-profile.json");
            if (!generated.is_object() || !generated.contains("generation")) return false;
            const auto& support = generated["generation"]["port_support"];
            return support.contains("Keyboard::feed") &&
                support["Keyboard::feed"].value("state", std::string{}) == "candidate" &&
                generated.value("native_smoke", json::object()).value("marker", std::string{}) == "exact-current-build-metadata";
        }, "source/port failed to find the fixture signature and generate a new definition");
        put(output / L"command" / L"hook.txt", "hook Keyboard::feed");
        waitFor([&] {
            const auto status = readJson(output / L"status.json");
            if (!status.is_object() || !status.contains("hooks")) return false;
            for (const auto& hook : status["hooks"])
                if (hook.value("symbol", std::string{}) == "Keyboard::feed") return hook.value("installed", false);
            return false;
        }, "explicit Hook command did not install the known ABI fixture");
        waitFor([&] {
            require(keyboardFixture(65, true), "transparent hook changed the original return value");
            std::string events, error;
            if (!readBounded(output / L"events.jsonl", events, 32*1024*1024, error)) return false;
            std::istringstream stream(events);
            for (std::string line; std::getline(stream, line);) {
                const auto event = json::parse(line, nullptr, false);
                if (event.is_discarded()) continue;
                if (event.value("type", std::string{}) == "hook-observation" &&
                    event.value("symbol", std::string{}) == "Keyboard::feed")
                    return event.value("key", 0) == 65 && event.value("result", 0) == 1;
            }
            return false;
        }, "hook arguments / return value were not observed");
        put(output / L"command" / L"unhook.txt", "unhook Keyboard::feed");
        waitFor([&] {
            const auto status = readJson(output / L"status.json");
            if (!status.is_object() || !status.contains("hooks")) return false;
            for (const auto& hook : status["hooks"])
                if (hook.value("symbol", std::string{}) == "Keyboard::feed") return !hook.value("installed", true);
            return false;
        }, "unhook did not unsubscribe the observer");
        const auto lockedPath = output / L"command" / L"locked.txt";
        struct FileLock {
            HANDLE handle = INVALID_HANDLE_VALUE;
            ~FileLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } held;
        held.handle = CreateFileW(lockedPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(held.handle != INVALID_HANDLE_VALUE, "could not create locked command fixture");
        DWORD written = 0;
        require(WriteFile(held.handle, "identity", 8, &written, nullptr) && written == 8, "locked command write failed");
        FlushFileBuffers(held.handle);
        waitFor([&] {
            std::string events, error;
            return readBounded(output / L"events.jsonl", events, 32*1024*1024, error) &&
                events.find("could not consume command file") != std::string::npos;
        }, "locked command was not deferred");
        CloseHandle(held.handle); held.handle = INVALID_HANDLE_VALUE;
        waitFor([&] {
            std::string events, error;
            return !fs::exists(lockedPath) &&
                readBounded(output / L"events.jsonl", events, 32*1024*1024, error) &&
                events.find("\"type\":\"identity\"") != std::string::npos;
        }, "command was not retried after unlock");
        put(output / L"commands" / L"quit.txt", "quit");
        waitFor([&] {
            const auto status = readJson(output / L"status.json");
            return status.is_object() && status.value("state", std::string{}) == "stopped";
        }, "quit did not stop the DLL worker");
        std::string finalEvents, readError;
        require(readBounded(output / L"events.jsonl", finalEvents, 32*1024*1024, readError), "event history missing");
        const std::string marker = "\"type\":\"identity\"";
        const auto firstIdentity = finalEvents.find(marker);
        require(firstIdentity != std::string::npos && finalEvents.find(marker, firstIdentity+1) == std::string::npos,
            "locked command executed more than once");
        fs::remove_all(output);
        std::cout << "CheckSig DLL smoke test passed (JSON drop, diagnostics, invalid/mismatch recovery, commands, hook observation)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        if (ownOutput) {
            // Preserve diagnostics for the CI job log on failure.
            std::string events, error;
            if (readBounded(output / L"events.jsonl", events, 1024*1024, error)) std::cerr << events;
        }
        return 1;
    }
}

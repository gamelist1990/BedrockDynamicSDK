#pragma once
// NativeInjectedCommands: client commands the server never defined,
// made real by rewriting the server's AvailableCommands packet
// before the client parses it (see detail/available_commands_wire).
// The client builds its native parse context from the rewritten
// packet, so injected commands get first-class completion
// (block ids with icons, custom hard enums, target selectors with
// live player names) and a native signature hint. Executing one
// sends an ordinary CommandRequest, which this service intercepts
// on the wire-send hook and cancels before it leaves the client;
// the registered handler receives the raw line and owns argument
// validation (the client forwards values without checking them
// against the enum for server-known commands).
//
// Verified live on 1.26.65.1 (2026-11-18) with the CmdArgsProbe
// experiment this service was productized from.
//
// Threading: handlers run on the packet send path. Keep them
// trivial (copy the line into a queue); do the work on your own
// tick thread.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <bedrock/detail/available_commands_wire.hpp>
#include <bedrock/native/packet_view.hpp>
#include <bedrock/packets/packet_read_events.hpp>
#include <bedrock/packets/wire_packet_events.hpp>
#include <bedrock/profiles/profile_manager.hpp>

namespace bedrock {

// A parameter type for an injected command: a raw type word, an
// enum that already exists in the server's packet (resolved by name
// at rewrite time, e.g. "Block"), or a custom enum registered with
// addEnum / addMergedEnum.
struct InjectedType {
    enum class Kind { Raw, PacketEnum, CustomEnum };
    Kind kind = Kind::Raw;
    std::uint32_t raw = 0;
    std::string name;

    static InjectedType rawWord(std::uint32_t word) {
        InjectedType t;
        t.kind = Kind::Raw;
        t.raw = word;
        return t;
    }
    static InjectedType integer() { return rawWord(0x100001); }
    static InjectedType target() { return rawWord(0x100008); }
    static InjectedType position() { return rawWord(0x100040); }
    static InjectedType message() { return rawWord(0x100044); }
    static InjectedType packetEnum(std::string name) {
        InjectedType t;
        t.kind = Kind::PacketEnum;
        t.name = std::move(name);
        return t;
    }
    static InjectedType customEnum(std::string name) {
        InjectedType t;
        t.kind = Kind::CustomEnum;
        t.name = std::move(name);
        return t;
    }
};

struct InjectedParam {
    std::string name;
    InjectedType type;
    bool optional = false;
    std::uint8_t options = 0;
};

class NativeInjectedCommands {
public:
    using Handler = std::function<void(const std::string& line)>;
    using Diagnostic = std::function<void(const std::string& message)>;

    // Stage B (D-1): `add`/`addEnum` are frozen after load(), but a plugin
    // reload must be able to change the injected definition set. The rewrite
    // already re-reads definitions on every AvailableCommands packet, and the
    // wire interceptor keyed off `active_`; swapping the vectors under the
    // definitions mutex makes both paths pick up the new set safely.
    struct EnumEntry {
        std::string name;
        std::vector<std::string> values; // fixed values, or extras when merged
        std::string mergedFrom;          // packet enum name when merged
        bool merged = false;
    };
    struct CommandEntry {
        std::string name;
        std::string description;
        std::vector<std::vector<InjectedParam>> overloads;
        Handler handler;
    };

    void setDiagnostic(Diagnostic diagnostic) { diagnostic_ = std::move(diagnostic); }

    // How many AvailableCommands packets this service has rewritten so
    // far. A session in which the count never moves has no server
    // command list on the wire path at all (singleplayer: the integrated
    // server hands the list to the client internally), which is the
    // signal the local-registry fallback in the Commands facade keys on.
    [[nodiscard]] std::uint64_t rewriteCount() const noexcept {
        return rewriteCount_.load(std::memory_order_acquire);
    }
    // When the most recent rewrite happened (steady clock). Session
    // signals (gameplay active / player loaded) fire AFTER a remote
    // login's rewrite, so the fallback compares this against the
    // session start instead of only counting from it.
    [[nodiscard]] std::chrono::steady_clock::time_point lastRewriteTime() const noexcept {
        return std::chrono::steady_clock::time_point{
            std::chrono::milliseconds{lastRewriteMs_.load(std::memory_order_acquire)}};
    }

    // True once load() has frozen the initial definition set (§6.3).
    [[nodiscard]] bool isLoaded() const noexcept { return loaded_; }

    // A hard enum with fixed values, offered by completion wherever
    // a parameter references it via InjectedType::customEnum(name).
    bool addEnum(std::string name, std::vector<std::string> values, std::string& error) {
        return addEnumImpl(std::move(name), std::move(values), {}, false, error);
    }
    // A hard enum whose values are the values of an enum already in
    // the server's packet (e.g. every block id from "Block") merged
    // with extra values. This is the shape a single-overload command
    // wants: one parameter completing both the packet's vocabulary
    // and the mod's action words. (Competing first-parameter types
    // across several overloads complete poorly; one merged enum on
    // one overload completes like a vanilla command.)
    bool addMergedEnum(std::string name, std::string packetEnum,
                       std::vector<std::string> extraValues, std::string& error) {
        return addEnumImpl(std::move(name), std::move(extraValues), std::move(packetEnum),
                           true, error);
    }
    // overloads may be empty (a bare command with no arguments is
    // one empty overload). Flags/permission mirror /me (0xc0, "any"),
    // which the client accepts for locally handled commands.
    bool add(std::string name, std::string description,
             std::vector<std::vector<InjectedParam>> overloads, Handler handler,
             std::string& error) {
        if (loaded_) { error = "commands must be added before load"; return false; }
        if (!handler || name.empty() || name.size() > 64 || description.size() > 1024 ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") !=
                std::string::npos ||
            description.find('\0') != std::string::npos || overloads.size() > 8) {
            error = "invalid injected command definition";
            return false;
        }
        for (const auto& overload : overloads)
            if (overload.size() > 16) { error = "too many parameters"; return false; }
        {
            std::lock_guard lock(definitionsMutex_);
            for (const auto& entry : commands_)
                if (entry.name == name) { error = "duplicate command: " + name; return false; }
            commands_.push_back({std::move(name), std::move(description), std::move(overloads),
                                 std::move(handler)});
        }
        error.clear();
        return true;
    }

    // Replace the complete definition set (including after load()). Callers
    // must pass the full set; removed commands stop being intercepted on the
    // very next CommandRequest, and completion catches up on the next
    // AvailableCommands packet. Existing Mods that never call this are
    // unaffected (§6.3 D-1).
    bool replaceDefinitions(std::vector<EnumEntry> enums,
                            std::vector<CommandEntry> commands,
                            std::string& error) {
        auto validEnumName = [](const std::string& name) {
            return !name.empty() && name.size() <= 64 &&
                name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                       "0123456789_") == std::string::npos;
        };
        std::unordered_set<std::string> enumNames;
        for (const auto& entry : enums) {
            if (!validEnumName(entry.name) || entry.values.size() > 8192 ||
                (!entry.merged && entry.values.empty())) {
                error = "invalid injected enum definition: " + entry.name;
                return false;
            }
            if (!enumNames.insert(entry.name).second) {
                error = "duplicate enum: " + entry.name;
                return false;
            }
        }
        std::unordered_set<std::string> commandNames;
        for (const auto& entry : commands) {
            if (!entry.handler || entry.name.empty() || entry.name.size() > 64 ||
                entry.description.size() > 1024 ||
                entry.name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") !=
                    std::string::npos ||
                entry.description.find('\0') != std::string::npos || entry.overloads.size() > 8) {
                error = "invalid injected command definition: " + entry.name;
                return false;
            }
            for (const auto& overload : entry.overloads)
                if (overload.size() > 16) { error = "too many parameters"; return false; }
            if (!commandNames.insert(entry.name).second) {
                error = "duplicate command: " + entry.name;
                return false;
            }
        }
        {
            std::lock_guard lock(definitionsMutex_);
            enums_ = std::move(enums);
            commands_ = std::move(commands);
        }
        error.clear();
        return true;
    }

    bool load(const RuntimeProfile& profile, std::string& error) {
        if (loaded_) { error = "already loaded"; return false; }
        try {
            const auto root = nlohmann::json::parse(profile.json());
            reads_.load(root);
            access_.load(root);
            wire_.load(root);
            const auto& layout = root.at("layouts").at("PacketRead");
            streamViewOff_ = native_profile::number(layout.at("stream_view_offset"));
            streamSizeOff_ = native_profile::number(layout.at("stream_size_offset"));
            streamCursorOff_ = native_profile::number(layout.at("stream_cursor_offset"));
            if (!reads_.subscribe(&onRead, this, &readToken_))
                throw std::runtime_error("Packet::read subscription failed");
            if (reads_.routed()) {
                const auto route = reads_.routes().find(76u); // AvailableCommands
                if (route == reads_.routes().end())
                    throw std::runtime_error("no decode route for AvailableCommands (76)");
                for (void* target : route->second.targets) {
                    BDS_HookToken token{};
                    if (!reads_.subscribeTarget(target, &onRead, this, &token))
                        throw std::runtime_error("AvailableCommands route subscription failed");
                    routeTokens_.push_back(token);
                }
            }
            if (!wire_.subscribe(&onWire, this, &wireToken_))
                throw std::runtime_error("wire send subscription failed");
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
        loaded_ = true;
        error.clear();
        return true;
    }

private:
    bool addEnumImpl(std::string name, std::vector<std::string> values,
                     std::string mergedFrom, bool merged, std::string& error) {
        if (loaded_) { error = "enums must be added before load"; return false; }
        if (name.empty() || name.size() > 64 ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "0123456789_") != std::string::npos ||
            (!merged && values.empty()) || values.size() > 8192) {
            error = "invalid injected enum definition";
            return false;
        }
        {
            std::lock_guard lock(definitionsMutex_);
            for (const auto& entry : enums_)
                if (entry.name == name) { error = "duplicate enum: " + name; return false; }
            enums_.push_back({std::move(name), std::move(values), std::move(mergedFrom), merged});
        }
        error.clear();
        return true;
    }

    void diag(const std::string& message) const {
        if (diagnostic_) diagnostic_(message);
    }

    // Rebuild the packet body with our enums and commands appended.
    // Returns nullopt when nothing was injected (packet untouched).
    std::optional<std::string> rewrite(const std::string& body) const {
        namespace acp = detail::acpwire;
        std::lock_guard definitionsLock(definitionsMutex_);
        std::string cur = body;
        bool changed = false;
        std::unordered_map<std::string, std::uint64_t> customIndex;
        for (const auto& entry : enums_) {
            if (const auto found = acp::findEnum(cur, entry.name)) {
                customIndex[entry.name] = found->index;
                continue;
            }
            std::uint64_t index = 0;
            std::optional<std::string> next;
            if (entry.merged)
                next = acp::appendMergedEnum(cur, entry.name, entry.mergedFrom, entry.values,
                                             &index);
            else
                next = acp::appendEnum(cur, entry.name, entry.values, &index);
            if (!next) {
                diag("injected commands: enum append failed for " + entry.name);
                return std::nullopt;
            }
            cur = std::move(*next);
            customIndex[entry.name] = index;
            changed = true;
        }
        std::unordered_set<std::string> injectedNow;
        for (const auto& entry : commands_) {
            if (acp::hasCommand(cur, entry.name)) {
                diag("injected commands: /" + entry.name +
                     " is already defined by the server; not injecting");
                continue;
            }
            acp::Command wireCommand;
            wireCommand.name = entry.name;
            wireCommand.description = entry.description;
            wireCommand.flags = 0xc0; // same as /me
            wireCommand.permission = "any";
            bool unresolved = false;
            for (const auto& overload : entry.overloads) {
                std::vector<acp::Param> params;
                for (const auto& param : overload) {
                    std::uint32_t word = 0;
                    switch (param.type.kind) {
                    case InjectedType::Kind::Raw:
                        word = param.type.raw;
                        break;
                    case InjectedType::Kind::PacketEnum: {
                        const auto found = acp::findEnum(cur, param.type.name);
                        if (!found) unresolved = true;
                        else word = static_cast<std::uint32_t>(found->index | 0x300000);
                        break;
                    }
                    case InjectedType::Kind::CustomEnum: {
                        const auto it = customIndex.find(param.type.name);
                        if (it == customIndex.end()) unresolved = true;
                        else word = static_cast<std::uint32_t>(it->second | 0x300000);
                        break;
                    }
                    }
                    if (unresolved) break;
                    params.push_back({param.name, word,
                                      static_cast<std::uint8_t>(param.optional ? 1 : 0),
                                      param.options});
                }
                if (unresolved) break;
                wireCommand.overloads.push_back(std::move(params));
            }
            if (unresolved) {
                diag("injected commands: parameter type unresolved for /" + entry.name +
                     "; skipped");
                continue;
            }
            auto next = acp::appendCommand(cur, wireCommand);
            if (!next) {
                diag("injected commands: command append failed for /" + entry.name);
                continue;
            }
            cur = std::move(*next);
            injectedNow.insert(entry.name);
            changed = true;
        }
        {
            std::lock_guard lock(activeMutex_);
            active_ = std::move(injectedNow);
        }
        if (!changed) return std::nullopt;
        return cur;
    }

    void beforeRead(BDS_PacketReadArgs& args) {
        if (access_.view(args.packet).rawId() != 76) return; // AvailableCommands
        std::uintptr_t bytes{};
        std::size_t size{}, cursor{};
        if (!native_profile::read(args.stream, streamViewOff_, bytes) ||
            !native_profile::read(args.stream, streamSizeOff_, size) ||
            !native_profile::read(args.stream, streamCursorOff_, cursor))
            return;
        if (!bytes || cursor > size || size - cursor > (16u << 20)) return;
        const std::string body(reinterpret_cast<const char*>(bytes) + cursor, size - cursor);
        auto out = rewrite(body);
        if (!out) return;
        // The buffer must outlive the game's read, which runs
        // synchronously inside next() on this same thread.
        thread_local std::string tls;
        tls = std::move(*out);
        const auto base = reinterpret_cast<std::uintptr_t>(tls.data());
        if (!poke(args.stream, streamViewOff_, base) ||
            !poke(args.stream, streamSizeOff_, tls.size()) ||
            !poke(args.stream, streamCursorOff_, std::size_t{0})) {
            diag("injected commands: stream rewrite failed");
            return;
        }
        rewriteCount_.fetch_add(1, std::memory_order_acq_rel);
        lastRewriteMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_release);
        diag("injected commands: AvailableCommands rewritten (" +
             std::to_string(body.size()) + " -> " + std::to_string(tls.size()) + " bytes)");
    }

    void onWirePacket(PacketWireSendEvent& event) {
        if (event.packet().rawId() != 77) return; // CommandRequest
        const auto payload = event.packet().payload();
        detail::acpwire::Reader reader{
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size(), 0};
        const auto line = reader.s();
        if (reader.fail) return;
        std::string rest = line;
        if (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
        const auto name = rest.substr(0, rest.find(' '));
        {
            std::lock_guard lock(activeMutex_);
            if (!active_.count(name)) return; // not ours (or not injected here)
        }
        Handler handler;
        {
            std::lock_guard lock(definitionsMutex_);
            for (const auto& entry : commands_)
                if (entry.name == name) { handler = entry.handler; break; }
        }
        if (handler) {
            handler(line); // never call JS while holding a definitions lock
            event.cancel(); // handled locally; never reaches the server
        }
    }

    static void BDS_HOOK_CALL onRead(void* user, void* raw, BDS_HookNext next, void* frame) {
        auto& self = *static_cast<NativeInjectedCommands*>(user);
        auto& args = *static_cast<BDS_PacketReadArgs*>(raw);
        try { self.beforeRead(args); } catch (...) {}
        next(frame, raw);
    }
    static void BDS_HOOK_CALL onWire(void* user, void* raw, BDS_HookNext next, void* frame) {
        auto& self = *static_cast<NativeInjectedCommands*>(user);
        auto& args = *static_cast<BDS_PacketWireArgs*>(raw);
        if (!self.wire_.dispatch(args,
                                 [&self](PacketWireSendEvent& event) {
                                     try { self.onWirePacket(event); } catch (...) {}
                                 },
                                 next, frame))
            next(frame, &args);
    }

    template <class T>
    static bool poke(void* object, std::size_t offset, const T& value) noexcept {
        if (!object) return false;
        __try {
            std::memcpy(static_cast<char*>(object) + offset, &value, sizeof(T));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::vector<EnumEntry> enums_;
    std::vector<CommandEntry> commands_;
    Diagnostic diagnostic_;
    PacketReadEvents reads_;
    NativePacketAccess access_;
    PacketWireEvents wire_;
    BDS_HookToken readToken_{}, wireToken_{};
    std::vector<BDS_HookToken> routeTokens_;
    std::size_t streamViewOff_{}, streamSizeOff_{}, streamCursorOff_{};
    bool loaded_ = false;
    mutable std::mutex definitionsMutex_; // enums_ / commands_ (Stage B D-1)
    mutable std::mutex activeMutex_;
    mutable std::unordered_set<std::string> active_; // names injected by the latest rewrite
    std::atomic<std::uint64_t> rewriteCount_{0};
    std::atomic<std::int64_t> lastRewriteMs_{0};
};

} // namespace bedrock

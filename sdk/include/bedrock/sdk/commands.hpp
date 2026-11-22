#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace bedrock::sdk { class Profile; }
namespace bedrock::sdk {
enum class CommandParameterType { Integer, Message, CustomEnum, PacketEnum };
struct CommandParameter { std::string name; CommandParameterType type{CommandParameterType::Message}; std::string enumName; bool optional{}; };
struct CommandDefinition { std::string name; std::string description; std::vector<std::vector<CommandParameter>> overloads; std::function<void(const std::string&)> handler; };
struct CommandEnum {
    std::string name;
    std::vector<std::string> values;
    std::string mergedFrom;
    bool merged{};
};
class Commands { public:
 Commands(); ~Commands(); Commands(const Commands&) = delete; Commands& operator=(const Commands&) = delete;
 bool load(const std::string& profileJson, const std::vector<CommandEnum>& enums,
          const std::vector<CommandDefinition>& commands, std::string& error);
 bool load(const Profile& profile, const std::vector<CommandEnum>& enums,
          const std::vector<CommandDefinition>& commands, std::string& error);
 bool replace(const std::vector<CommandEnum>& enums,
              const std::vector<CommandDefinition>& commands, std::string& error);
 bool isLoaded() const noexcept;
 // Local-registry fallback for sessions with no server command list
 // (singleplayer): the packet path above never engages there because
 // the integrated server never sends AvailableCommands over the wire
 // path, so the client rejects the commands as unknown. When enabled
 // (call before load()), every definition is ALSO registered directly
 // in the client's command registry — with typed parameters when the
 // overload set allows it (see Impl::mirrorLocal for the mapping), or
 // a single message parameter otherwise — and the definition's
 // handler receives the line, exactly as on the packet path.
 // Registration is driven per world session: call
 // noteLocalSessionStart() when a world is joined, then
 // driveLocalRegistry(clientInstance) on the client tick. There is
 // no local/remote classification: the registry collision rule is
 // the arbitration. On a remote session the packet path has already
 // placed these names in the registry, native registration reports
 // a collision, and the packet definitions (typed parameters,
 // enums, completion) are preserved; in singleplayer the names are
 // absent and native registration succeeds. Local
 // registration is best-effort: a failure is reported through
 // localRegistryError() and never fails load().
 void enableLocalRegistry(bool enable) noexcept;
 // Local-only mode (implies enableLocalRegistry): definitions are
 // NOT injected into the server's AvailableCommands packet at all.
 // Packet-injected names become server commands, and on the local
 // (integrated-server) path their requests are never intercepted —
 // they can be suggested but not executed in singleplayer. A
 // consumer whose commands are client-side by design (ScriptClient)
 // should prefer this mode: the commands execute locally in both
 // singleplayer and remote sessions. Definitions whose overloads are
 // non-empty and message-free register with typed parameters, so
 // completion shows real parameter types and enum suggestions;
 // definitions with a message parameter keep the single-message
 // registration (a message overload would capture every invocation
 // of the signature anyway).
 void preferLocalRegistry(bool prefer) noexcept;
 void noteLocalSessionStart() noexcept;
 void noteLocalSessionEnd() noexcept;
 void driveLocalRegistry(void* clientInstance);
 bool localRegistryActive() const noexcept;
 std::string localRegistryError() const;
 // Last local-registry drive state (diagnostics): "idle",
 // "waiting-client", "waiting-registry", "waiting-template",
 // "registered", "collision", "failed".
 std::string localRegistryState() const;
 private: struct Impl; std::unique_ptr<Impl> impl_;
};
}

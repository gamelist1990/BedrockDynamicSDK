#pragma once

#include "plugin_host.hpp"

#include <bedrock/sdk/client_host.hpp>
#include <bedrock/sdk/profiles.hpp>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace stageb {
class ModManager {
public:
    using Sync = std::function<void(const std::string&)>;
    using Log = std::function<void(const std::string&)>;
    ModManager(PluginHost& host, bedrock::sdk::ClientHost& client, bedrock::sdk::Profile profile,
               fs::path inbox, Sync sync, Log log);
    void prepare();
    void requestReload(std::string name);
    void run(); // ScriptClient's pinned loader thread; does not depend on world ticks.

private:
    enum class Action { Reload, Toggle, Install, Previous, Next };
    struct Request {
        Action action;
        std::string name{};
        fs::path package{};
        bool enabled{};
    };
    void enqueue(Request request);
    void apply(const Request& request);
    void refreshPage();

    PluginHost& host_;
    bedrock::sdk::ClientHost& client_;
    bedrock::sdk::Profile profile_;
    fs::path inbox_;
    Sync sync_;
    Log log_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Request> requests_;
    std::vector<fs::path> packages_;
    std::vector<ModView> mods_;
    std::size_t pageIndex_{};
    bool registered_{};
    std::string status_{"Select a package to install, or toggle an installed mod."};
};
} // namespace stageb

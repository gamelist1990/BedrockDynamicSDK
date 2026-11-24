#include "mod_manager.hpp"
#include "package.hpp"

#include <algorithm>
#include <chrono>
#include <map>

namespace stageb {
namespace {
constexpr std::size_t pageSize = 28; // 2 lists plus heading/status/navigation < 64 controls.
constexpr auto pageId = "scriptclient-mod-manager";
constexpr auto title = "Mod Manager";

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

std::string clip(std::string text, std::size_t limit) {
    if (text.size() <= limit) return text;
    while (limit && (static_cast<unsigned char>(text[limit]) & 0xc0) == 0x80) --limit;
    return text.substr(0, limit) + "...";
}

std::string rowId(char prefix, const std::string& value) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : value) hash = (hash ^ c) * 1099511628211ull;
    return prefix + std::to_string(hash);
}
} // namespace

ModManager::ModManager(PluginHost& host, bedrock::sdk::ClientHost& client, bedrock::sdk::Profile profile,
                       fs::path inbox, Sync sync, Log log)
    : host_(host), client_(client), profile_(std::move(profile)), inbox_(std::move(inbox)),
      sync_(std::move(sync)), log_(std::move(log)) {}

void ModManager::prepare() {
    try { refreshPage(); }
    catch (const std::exception& e) { log_(std::string("Mod Manager registration failed: ") + e.what()); }
}

void ModManager::enqueue(Request request) {
    {
        std::lock_guard lock(mutex_);
        requests_.push_back(std::move(request));
    }
    wake_.notify_one();
}

void ModManager::requestReload(std::string name) {
    enqueue({Action::Reload, std::move(name)});
}

void ModManager::apply(const Request& request) {
    if (request.action == Action::Previous) { if (pageIndex_) --pageIndex_; return; }
    if (request.action == Action::Next) { ++pageIndex_; return; }
    auto name = request.name;
    if (request.action == Action::Install)
        name = installPackage(inbox_ / request.package, host_.modsDir(), host_.gameVersion());

    std::string error;
    bool loaded = false;
    const auto result = client_.synchronize([&] {
        if (request.action == Action::Install || request.action == Action::Toggle)
            host_.setEnabled(name, request.action == Action::Install || request.enabled);
        loaded = host_.reload(name, error);
        sync_(name);
        mods_ = host_.list();
    });
    if (!result) throw std::runtime_error(result.error().message);

    if (request.action == Action::Install) {
        // Keep the original archive available without presenting it as pending.
        const auto done = inbox_ / "installed";
        std::error_code ec;
        fs::create_directories(done, ec);
        if (!ec && !fs::exists(done / request.package, ec) && !ec)
            fs::rename(inbox_ / request.package, done / request.package, ec);
        if (ec) log_("Package was installed, but the original could not be archived: " + ec.message());
    }

    status_ = name + (request.action == Action::Install ? " installed" :
                       request.action == Action::Toggle ? (request.enabled ? " enabled" : " disabled") :
                       loaded ? " reloaded" : " reload failed");
    if (!loaded) status_ += " (see ScriptClient.log)";
    log_(status_ + (error.empty() ? "" : ": " + error));
}

void ModManager::refreshPage() {
    using bedrock::sdk::Setting;
    using bedrock::sdk::SettingsPage;
    SettingsPage page(pageId, title);
    page.add(Setting::text("status", status_));
    std::map<std::string, Request> actions;
    auto action = [&](std::string id, std::string label, Request request) {
        if (actions.emplace(id, std::move(request)).second)
            page.add(Setting::action(std::move(id), std::move(label)));
    };
    const auto totalPages = (std::max(mods_.size(), packages_.size()) + pageSize - 1) / pageSize;
    pageIndex_ = std::min(pageIndex_, totalPages ? totalPages - 1 : 0);
    if (totalPages > 1) {
        page.add(Setting::text("page", "Page " + std::to_string(pageIndex_ + 1) + " of " + std::to_string(totalPages)));
        if (pageIndex_) action("previous", "Previous page", {Action::Previous});
        if (pageIndex_ + 1 < totalPages) action("next", "Next page", {Action::Next});
    }
    const auto start = pageIndex_ * pageSize;
    page.add(Setting::text("mods", mods_.empty() ? "No script mods installed" : "Installed ScriptClient mods"));
    for (std::size_t i = start; i < std::min(start + pageSize, mods_.size()); ++i) {
        const auto& mod = mods_[i];
        if (!isScriptModName(mod.name)) continue;
        auto id = rowId('m', mod.name);
        auto label = mod.name + " [" + statusName(mod.status) + "]";
        if (actions.emplace(id, Request{Action::Toggle, mod.name}).second)
            page.add(Setting::sessionToggle(std::move(id), std::move(label), mod.status != ModStatus::Disabled));
    }
    page.add(Setting::text("packages", packages_.empty() ? "No pending packages" : "Install from package",
                          clip(utf8(inbox_), 900)));
    for (std::size_t i = start; i < std::min(start + pageSize, packages_.size()); ++i) {
        const auto file = utf8(packages_[i]);
        action(rowId('p', file), "Install " + clip(file, 240), {Action::Install, {}, packages_[i]});
    }
    page.onChange([this, actions = std::move(actions)](std::string_view id, const auto& value) {
        // The settings bridge holds its store lock. Only copy a request.
        if (const auto it = actions.find(std::string(id)); it != actions.end()) {
            auto request = it->second;
            if (request.action == Action::Toggle) {
                const auto* next = std::get_if<bool>(&value);
                if (!next) return;
                request.enabled = *next;
            }
            enqueue(std::move(request));
        }
    });
    if (registered_) {
        if (const auto result = page.unregisterPage(); !result)
            throw std::runtime_error(result.error().message);
        registered_ = false;
    }
    const auto result = page.registerPage(profile_);
    if (!result) throw std::runtime_error(result.error().message);
    registered_ = true;
}

void ModManager::run() {
    // Called immediately after startup, before the first request can arrive.
    if (const auto result = client_.synchronize([&] { mods_ = host_.list(); }); !result)
        throw std::runtime_error(result.error().message);
    bool refresh = true;
    for (;;) {
        std::deque<Request> pending;
        {
            std::lock_guard lock(mutex_);
            pending.swap(requests_);
        }
        for (const auto& request : pending) {
            try { apply(request); }
            catch (const std::exception& e) {
                status_ = "Mod Manager operation failed (see ScriptClient.log)";
                log_(status_ + ": " + e.what());
            }
            refresh = true;
        }
        try {
            auto packages = listPackages(inbox_);
            if (packages != packages_) { packages_ = std::move(packages); refresh = true; }
            if (std::exchange(refresh, false)) refreshPage();
        } catch (const std::exception& e) {
            log_(std::string("Mod Manager refresh failed: ") + e.what());
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::seconds(1), [&] { return !requests_.empty(); });
    }
}
} // namespace stageb

#include "mod_manager.hpp"

#include <bedrock/sdk/window_file_drop.hpp>

#include <algorithm>
#include <chrono>
#include <map>

namespace stageb {
namespace {
constexpr std::size_t pageSize = 18; // Two controls per mod + packages + navigation < 64.
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

void ModManager::synchronized(const std::string& name, const std::function<void()>& operation) {
    std::exception_ptr failure;
    const auto result = client_.synchronize([&] {
        try { operation(); }
        catch (...) { failure = std::current_exception(); }
        sync_(name);
        mods_ = host_.list();
    });
    if (!result) throw std::runtime_error(result.error().message);
    if (failure) std::rethrow_exception(failure);
}

void ModManager::install(PreparedPackage& package, const fs::path& archive) {
    const auto& name = package.name();
    std::string error;
    bool loaded{};
    synchronized(name, [&] { loaded = host_.install(package, error); });
    status_ = name + (package.isUpdate() ? " updated to " : " installed: ") + package.version();
    if (!loaded) status_ += " (reload failed; see ScriptClient.log)";
    try { archivePackage(archive); }
    catch (const std::exception& e) {
        status_ += " (package not archived)";
        log_("Package was published, but the original could not be archived: " + std::string(e.what()));
    }
    log_(status_ + (error.empty() ? "" : ": " + error));
}

void ModManager::prepareInstall(const fs::path& archive) {
    auto package = PreparedPackage::prepare(archive, host_.modsDir(), host_.gameVersion());
    if (!package.isUpdate()) {
        install(package, archive);
        return;
    }
    status_ = "Confirm update";
    auto name = package.name();
    confirmation_.emplace(Confirmation{++nextConfirmation_, std::move(name), archive, std::move(package)});
}

void ModManager::apply(const Request& request) {
    if (request.action == Action::Drop) {
        const auto archive = stageDroppedPackage(request.package, inbox_);
        log_("Drop staged: " + utf8(archive.filename()));
        if (request.enabled && !confirmation_) prepareInstall(archive);
        else if (!confirmation_) status_ = "Package staged: " + utf8(archive.filename());
        return;
    }
    if (request.action == Action::Confirm || request.action == Action::ConfirmWithData ||
        request.action == Action::Cancel) {
        if (!confirmation_ || confirmation_->id != request.confirmation) return;
        auto confirmation = std::move(*confirmation_);
        confirmation_.reset();
        if (request.action == Action::Cancel) { status_ = "Cancelled"; return; }
        if (confirmation.update) {
            if (request.action == Action::Confirm) install(*confirmation.update, confirmation.archive);
        } else {
            std::string warning;
            const bool removeData = request.action == Action::ConfirmWithData;
            synchronized(confirmation.name, [&] { warning = host_.remove(confirmation.name, removeData); });
            status_ = confirmation.name + (removeData ? " deleted with mod-data" : " deleted (mod-data kept)");
            if (!warning.empty()) status_ = confirmation.name + " deleted with warnings (see ScriptClient.log)";
            log_(status_ + (warning.empty() ? "" : ": " + warning));
        }
        return;
    }
    if (confirmation_) return; // Ignore clicks from the pre-confirmation page.
    if (request.action == Action::Previous) { if (pageIndex_) --pageIndex_; return; }
    if (request.action == Action::Next) { ++pageIndex_; return; }
    if (request.action == Action::Install) { prepareInstall(inbox_ / request.package); return; }
    if (request.action == Action::Delete) {
        confirmation_.emplace(Confirmation{++nextConfirmation_, request.name, {}, {}});
        status_ = "Confirm deletion";
        return;
    }
    const auto& name = request.name;
    std::string error;
    bool loaded{};
    synchronized(name, [&] {
        if (request.action == Action::Toggle) host_.setEnabled(name, request.enabled);
        loaded = host_.reload(name, error);
    });
    status_ = name + (request.action == Action::Toggle ? (request.enabled ? " enabled" : " disabled") :
                     loaded ? " reloaded" : " reload failed");
    if (!loaded) status_ += " (see ScriptClient.log)";
    log_(status_ + (error.empty() ? "" : ": " + error));
}

void ModManager::refreshPage() {
    using bedrock::sdk::Setting;
    using bedrock::sdk::SettingsPage;
    SettingsPage page(pageId, title);
    page.add(Setting::text("status", clip(status_, 240)));
    std::map<std::string, Request> actions;
    auto action = [&](std::string id, std::string label, Request request) {
        if (actions.emplace(id, std::move(request)).second)
            page.add(Setting::action(std::move(id), std::move(label)));
    };
    if (confirmation_) {
        const auto& confirm = *confirmation_;
        const auto token = std::to_string(confirm.id);
        auto choice = [&](Action kind, const char* prefix, std::string label) {
            Request request{kind};
            request.confirmation = confirm.id;
            action(prefix + token, std::move(label), std::move(request));
        };
        if (const auto& update = confirm.update) {
            page.add(Setting::text("preview", clip(confirm.name + ": " +
                (update->oldVersion().empty() ? "(unknown version)" : update->oldVersion()) +
                " -> " + update->version(), 240), "Keeps mod-data and the current enable state."));
            choice(Action::Confirm, "update-", "Update " + confirm.name);
        } else {
            page.add(Setting::text("preview", "Delete " + confirm.name + "?",
                                   "This removes the installed code. Choose whether to keep its mod-data."));
            choice(Action::Confirm, "keep-", "Delete " + confirm.name + " (keep mod-data)");
            choice(Action::ConfirmWithData, "purge-", "Delete " + confirm.name + " AND its mod-data");
        }
        choice(Action::Cancel, "cancel-", "Cancel");
    } else {
        page.add(Setting::dropZone("drop-zone", "Drop a trusted .mods or .zip here"));
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
            auto label = clip(mod.name + (mod.version.empty() ? "" : " " + mod.version) +
                              " [" + statusName(mod.status) + "]", 240);
            if (actions.emplace(id, Request{Action::Toggle, mod.name}).second)
                page.add(Setting::sessionToggle(std::move(id), std::move(label), mod.status != ModStatus::Disabled));
            action(rowId('d', mod.name), "Delete " + mod.name + "...", {Action::Delete, mod.name});
        }
        page.add(Setting::text("packages", packages_.empty() ? "No pending packages" : "Install from package",
                               clip(utf8(inbox_), 900)));
        for (std::size_t i = start; i < std::min(start + pageSize, packages_.size()); ++i) {
            const auto file = utf8(packages_[i]);
            action(rowId('p', file), "Install " + clip(file, 240), {Action::Install, {}, packages_[i]});
        }
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
    auto nextScan = std::chrono::steady_clock::now();
    for (;;) {
        std::deque<Request> pending;
        {
            std::lock_guard lock(mutex_);
            pending.swap(requests_);
        }
        const auto drops = bedrock::sdk::pollWindowFileDropsForZone(pageId);
        for (const auto& drop : drops) pending.push_back({Action::Drop, {}, drop, true});
        for (const auto& request : pending) {
            try { apply(request); }
            catch (const std::exception& e) {
                status_ = "Mod Manager operation failed (see ScriptClient.log)";
                log_(status_ + ": " + e.what());
            }
            refresh = true;
        }
        try {
            const auto now = std::chrono::steady_clock::now();
            if (refresh || now >= nextScan) {
                nextScan = now + std::chrono::seconds(1);
                auto packages = listPackages(inbox_);
                if (packages != packages_) { packages_ = std::move(packages); refresh = true; }
            }
            if (std::exchange(refresh, false)) refreshPage();
        } catch (const std::exception& e) {
            log_(std::string("Mod Manager refresh failed: ") + e.what());
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(250), [&] { return !requests_.empty(); });
    }
}
} // namespace stageb

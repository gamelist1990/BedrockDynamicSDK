#include <bedrock/native/resource_packs.hpp>
#include <bedrock/api/resource_packs.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <bedrock/detail/owned_msvc_string.hpp>
#include <bedrock/detail/owned_native_callback.hpp>
#include <memory>
#include <intrin.h>
#include <bedrock/detail/pending_terrain_refresh.hpp>

#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
#error NativeResourcePacks requires the Release MSVC STL ABI (_ITERATOR_DEBUG_LEVEL=0)
#endif

namespace bedrock {
struct NativeResourcePacks::Binding {
    using Initialize = void(__fastcall *)(void *);
    using Exit = std::uint64_t(__fastcall *)(void *);
    using Event = void(__fastcall *)(void *, void *);
    using DestroyCallback = void(__fastcall *)(void *, bool);
    using Compose = std::uint64_t(__fastcall *)(void *, void *, void *, void *);
    using Construct = void *(__fastcall *)(void *, void *, bool, bool, const void *, bool);
    using Predicate = bool(__fastcall *)(void *);
    using ScreenName = void *(__fastcall *)(void *, void *);
    using LocalPlayer = void *(__fastcall *)(void *);
    using ClientGame = void *(__fastcall *)(void *);
    using GeometryReload = void(__fastcall *)(void *, bool);
    using AtlasUpdate = void(__fastcall *)(void *, const void *, bool);
    using Deallocate = void(__fastcall *)(void *);
    void *initTarget{};
    void *ctorTarget{};
    void *preGameTarget{};
    void *exitTarget{}, *eventTarget{}, *composeTarget{}, *reloadTarget{}, *languageTarget{};
    void *navigateTarget{}, *eventDestroyTarget{};
    void *atlasTarget{}, *atlasCaller{}, *frameTarget{};
    Initialize frame{};
    GeometryReload geometryReload{};
    Event notifyClientResources{};
    std::size_t clientsOffset{}, nodeLeftOffset{}, nodeNilOffset{}, nodeKeyOffset{}, nodeClientOffset{};
    std::size_t loaderOffset{}, loaderReadyOffset{}, loaderOutstandingOffset{};
    std::atomic<std::uint64_t> geometryReloaded{}, clientDefinitionsReloaded{}, resourceWaitFrames{};
    AtlasUpdate atlasUpdate{};
    ClientGame clientLevel{}, levelData{}, gamePacks{};
    Event refreshClient{};
    std::size_t experimentsOffset{}, terrainAtlasOffset{};
    std::mutex refreshMutex;
    detail::PendingTerrainRefresh pendingRefresh;
    std::atomic<bool> refreshPending{};
    std::atomic<std::uint64_t> terrainCompleted{}, blockTexturesReloaded{}, clientsRefreshed{}, refreshCancelled{};
    Event reloadBlockTextures{};
    Event navigate{};
    DestroyCallback eventDestroy{};
    detail::NativeCallbackLayout callbackLayout{};
    Exit exit{};
    Event event{};
    Compose compose{};
    Initialize reload{}, languageUpdate{};
    Initialize initialize{};
    Construct construct{};
    Predicate preGame{};
    ScreenName screenName{};
    LocalPlayer localPlayer{};
    ClientGame clientGame{};
    void *gameInGameTarget{}, *gameGateCaller{};
    Predicate gameInGame{};
    Deallocate deallocate{};
    std::size_t reloadOffset{}, stringSizeOffset{}, stringCapacityOffset{}, stringInlineBytes{},
        stringObjectBytes{}, maximumStringBytes{}, largeAllocationThreshold{}, alignmentOverhead{};
    std::size_t closureOwnerOffset{}, settingsModelOffset{}, modelClientOffset{}, includeGlobalOffset{};
    std::array<BDS_HookToken, 12> tokens{};
    std::atomic<bool> enabled{}, installed{};
    bool retained{};
    std::atomic<std::uint64_t> opened{}, overrides{}, failures{};
    std::atomic<std::uint64_t> exits{}, applyEvents{}, reloads{}, compositions{}, languageUpdates{}, globalIncluded{};
    std::atomic<std::uint64_t> navigations{}, liveApplications{}, inWorldEvents{};
    std::atomic<std::uint64_t> gameGateOverrides{};
    std::mutex policyMutex;
    std::map<void *, ResourcePackMenuPolicy> policies;

    static std::atomic<Binding *> &active() {
        static std::atomic<Binding *> value{};
        return value;
    }
    static std::mutex &installationMutex() {
        static std::mutex value;
        return value;
    }

    static void *&applyingClient() {
        static thread_local void *client{};
        return client;
    }
    struct ApplyScope {
        void *previous;
        explicit ApplyScope(void *client) : previous(applyingClient()) {
            if (client)
                applyingClient() = client;
        }
        ~ApplyScope() { applyingClient() = previous; }
    };
    void *clientFromModel(void *model) const noexcept {
        void *client{};
        return model && native_profile::read(model, modelClientOffset, client) ? client : nullptr;
    }
    static bool __fastcall onGameInGame(void *game) {
        // The game has a second gate independent of ClientInstance::isPreGame.
        // Scope the override to the one call in onFullPackStackInvalid. Other
        // callers, other game instances and the remaining native readiness
        // checks retain their original behavior.
        const auto caller = _ReturnAddress();
        auto &state = *active().load(std::memory_order_acquire);
        const bool original = state.gameInGame(game);
        const auto client = applyingClient();
        if (original && state.enabled.load(std::memory_order_acquire) &&
            caller == state.gameGateCaller && client &&
            state.clientGame(client) == game && state.localPlayer(client)) {
            ++state.gameGateOverrides;
            return false;
        }
        return original;
    }
    static void __fastcall onAtlasUpdate(void *game, const void *atlas, bool terrain) {
        const auto caller = _ReturnAddress();
        auto &state = *active().load(std::memory_order_acquire);
        state.atlasUpdate(game, atlas, terrain);
        if (!state.enabled.load(std::memory_order_acquire) || !terrain || caller != state.atlasCaller)
            return;
        void *currentAtlas{};
        if (!native_profile::read(game, state.terrainAtlasOffset, currentAtlas) || currentAtlas != atlas)
            return;
        std::lock_guard lock(state.refreshMutex);
        if (state.pendingRefresh.complete(game))
            ++state.terrainCompleted;
    }
    void *primaryClient(void *game) const noexcept {
        // Borrow the primary map entry only during startFrame on its owning
        // thread. This is the same key-zero lookup as the native App getter.
        void *head{}, *node{}, *client{};
        unsigned char nil{}, key{};
        if (!native_profile::read(game, clientsOffset, head) || !head ||
            !native_profile::read(head, nodeLeftOffset, node) || !node || node == head ||
            !native_profile::read(node, nodeNilOffset, nil) || nil ||
            !native_profile::read(node, nodeKeyOffset, key) || key != 0 ||
            !native_profile::read(node, nodeClientOffset, client))
            return nullptr;
        return client;
    }
    bool resourcesReady(void *game) const noexcept {
        void *loader{};
        unsigned char ready{};
        std::uint64_t outstanding{};
        return native_profile::read(game, loaderOffset, loader) && loader &&
               native_profile::read(loader, loaderReadyOffset, ready) && ready == 1 &&
               native_profile::read(loader, loaderOutstandingOffset, outstanding) && outstanding == 0;
    }
    void refreshAtFrame(void *game) {
        if (!enabled.load(std::memory_order_acquire) ||
            !refreshPending.load(std::memory_order_acquire))
            return;
        auto *client = primaryClient(game);
        auto *player = client ? localPlayer(client) : nullptr;
        auto *level = player && clientGame(client) == game ? clientLevel(client) : nullptr;
        void *manager{};
        bool cancelled{};
        const bool ready = resourcesReady(game);
        {
            std::lock_guard lock(refreshMutex);
            manager = pendingRefresh.take(game, level, GetCurrentThreadId(), cancelled, ready);
            refreshPending.store(pendingRefresh.pending(), std::memory_order_release);
        }
        if (cancelled)
            ++refreshCancelled;
        if (!manager) {
            if (!ready)
                ++resourceWaitFrames;
            return;
        }
        auto *data = levelData(level);
        if (!data || gamePacks(game) != manager) {
            ++failures;
            return;
        }
        // Full animation/geometry reload also rebuilds actor renderers, resumes
        // client render resources, invalidates ECS render data, and rebinds
        // each live LevelRenderer to the new resource definition groups.
        geometryReload(game, true);
        ++geometryReloaded;
        // Native per-client callback obtains/releases the current nonowner
        // definition-group reference itself. Its only capture is Game at +8.
        std::array<void *, 2> closure{nullptr, game};
        notifyClientResources(closure.data(), client);
        ++clientDefinitionsReloaded;
        reloadBlockTextures(manager, static_cast<std::byte *>(data) + experimentsOffset);
        ++blockTexturesReloaded;
        refreshClient(nullptr, client);
        ++clientsRefreshed;
    }
    static void __fastcall onStartFrame(void *game) {
        auto &state = *active().load(std::memory_order_acquire);
        // No actor render call is on the stack here. Replacing renderer objects
        // inside getLocalPlayer would invalidate its caller's borrowed objects.
        state.refreshAtFrame(game);
        state.frame(game);
    }
    static bool claimCallback(void *function, std::size_t offset, void *expected) noexcept {
        __try {
            auto **owner = reinterpret_cast<void **>(static_cast<std::byte *>(function) + offset);
            if (*owner != expected)
                return false;
            *owner = nullptr;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static void __fastcall onNavigateApply(void *model, void *function) {
        auto &state = *active().load(std::memory_order_acquire);
        if (state.enabled.load(std::memory_order_acquire)) {
            ++state.navigations;
            auto *client = state.clientFromModel(model);
            if (client && state.localPlayer(client) &&
                detail::consumeNativeCallback(
                    function, state.callbackLayout, state.eventTarget, state.eventDestroyTarget,
                    [](const void *base, std::size_t offset, void *&value) {
                        return native_profile::read(base, offset, value);
                    }, claimCallback,
                    [&](void *owner) {
                        ++state.liveApplications;
                        onApplyEvent(owner, model);
                    }, state.eventDestroy))
                return;
        }
        // Preserve ordinary title-screen navigation and all other functors.
        state.navigate(model, function);
    }
    static bool exchangeFlag(void *self, std::size_t offset, unsigned char value,
                             unsigned char &previous) noexcept {
        __try {
            auto *flag = static_cast<unsigned char *>(self) + offset;
            if (*flag > 1)
                return false;
            previous = *flag;
            *flag = value;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static std::uint64_t __fastcall onExit(void *closure) {
        auto &state = *active().load(std::memory_order_acquire);
        void *owner{}, *model{}, *client{};
        if (state.enabled.load(std::memory_order_acquire)) {
            ++state.exits;
            if (native_profile::read(closure, state.closureOwnerOffset, owner) && owner &&
                native_profile::read(owner, state.settingsModelOffset, model))
                client = state.clientFromModel(model);
            if (!client)
                ++state.failures;
        }
        ApplyScope scope(client);
        return state.exit(closure);
    }
    static void __fastcall onApplyEvent(void *closure, void *model) {
        auto &state = *active().load(std::memory_order_acquire);
        void *client{};
        if (state.enabled.load(std::memory_order_acquire)) {
            ++state.applyEvents;
            client = state.clientFromModel(model);
            if (!client)
                ++state.failures;
            else if (state.localPlayer(client))
                ++state.inWorldEvents;
        }
        // This event executes after navigation. Keep the same client eligible
        // throughout the game's queued stack update and reload notifications.
        ApplyScope scope(client);
        state.event(closure, model);
    }
    static void __fastcall onReload(void *self) {
        auto &state = *active().load(std::memory_order_acquire);
        if (state.enabled.load(std::memory_order_acquire))
            ++state.reloads;
        state.reload(self);
    }
    static void __fastcall onLanguageUpdate(void *self) {
        auto &state = *active().load(std::memory_order_acquire);
        state.languageUpdate(self);
        if (state.enabled.load(std::memory_order_acquire))
            ++state.languageUpdates;
    }
    static std::uint64_t __fastcall onCompose(void *self, void *full, void *global, void *level) {
        auto &state = *active().load(std::memory_order_acquire);
        const bool enabled = state.enabled.load(std::memory_order_acquire);
        if (enabled)
            ++state.compositions;
        unsigned char previous{};
        const auto client = applyingClient();
        const bool inWorld = enabled && client && state.localPlayer(client);
        const bool changed = inWorld && exchangeFlag(self, state.includeGlobalOffset, 1, previous);
        if (inWorld && !changed)
            ++state.failures;
        struct Restore {
            Binding &state;
            void *self;
            bool changed;
            unsigned char previous;
            ~Restore() {
                unsigned char ignored{};
                if (changed && !exchangeFlag(self, state.includeGlobalOffset, previous, ignored))
                    ++state.failures;
            }
        } restore{state, self, changed, previous};
        if (changed)
            ++state.globalIncluded;
        if (changed) {
            auto *game = state.clientGame(client);
            auto *clientLevel = state.clientLevel(client);
            if (game && clientLevel && state.gamePacks(game) == self) {
                std::lock_guard lock(state.refreshMutex);
                state.pendingRefresh.begin(game, clientLevel, self, GetCurrentThreadId());
                state.refreshPending.store(true, std::memory_order_release);
            } else {
                ++state.failures;
            }
        }
        return state.compose(self, full, global, level);
    }

    static bool writeFlag(void *self, std::size_t offset) noexcept {
        __try {
            auto *flag = static_cast<unsigned char *>(self) + offset;
            if (*flag > 1)
                return false;
            *flag = 1;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    // The game owns allocations made by its string getter. Never destroy these
    // with the DLL's CRT allocator, including large aligned MSVC allocations.
    std::string screen(void *client) {
        alignas(8) std::array<std::byte, 32> buffer{};
        screenName(client, buffer.data());
        const detail::OwnedMsvcStringLayout layout{stringSizeOffset,         stringCapacityOffset,
                                                   stringInlineBytes,        maximumStringBytes,
                                                   largeAllocationThreshold, alignmentOverhead};
        return detail::copyOwnedMsvcString(buffer, layout, deallocate,
                                           [](const void *address, void *&value) {
                                               return native_profile::read(address, 0, value);
                                           });
    }

    static void __fastcall onInitialize(void *self) {
        auto &state = *active().load(std::memory_order_acquire);
        if (state.enabled.load(std::memory_order_acquire)) {
            if (writeFlag(self, state.reloadOffset))
                ++state.opened;
            else {
                ++state.failures;
                state.enabled.store(false, std::memory_order_release);
            }
        }
        state.initialize(self);
    }
    static void *__fastcall onConstruct(void *self, void *model, bool global, bool reload,
                                        const void *initialPack, bool legacySync) {
        auto &state = *active().load(std::memory_order_acquire);
        if (state.enabled.load(std::memory_order_acquire))
            global = reload = true;
        // Forward the fifth and sixth arguments too: they live on the Windows
        // x64 stack and cannot be discarded when detouring this constructor.
        return state.construct(self, model, global, reload, initialPack, legacySync);
    }
    static bool __fastcall onPreGame(void *client) {
        auto &state = *active().load(std::memory_order_acquire);
        const bool original = state.preGame(client);
        if (state.enabled.load(std::memory_order_acquire) && applyingClient() == client &&
            state.localPlayer(client)) {
            if (!original)
                ++state.overrides;
            return true;
        }
        thread_local bool queryingScreen{};
        if (!state.enabled.load(std::memory_order_acquire) || queryingScreen)
            return original;
        struct QueryGuard {
            bool &value;
            explicit QueryGuard(bool &flag) : value(flag) {
                value = true;
            }
            ~QueryGuard() {
                value = false;
            }
        } guard{queryingScreen};
        try {
            const bool inWorld = state.localPlayer(client) != nullptr;
            const auto name = inWorld ? state.screen(client) : std::string{};
            std::lock_guard lock(state.policyMutex);
            if (!inWorld) {
                state.policies.erase(client);
                return original;
            }
            if (!state.policies.contains(client) && state.policies.size() >= 8)
                return original;
            const bool result = state.policies[client].preGame(original, name, inWorld);
            if (result && !original)
                ++state.overrides;
            return result;
        } catch (...) {
            ++state.failures;
            return original;
        }
    }
};

NativeResourcePacks::NativeResourcePacks() : binding_(new Binding) {}
NativeResourcePacks::~NativeResourcePacks() {
    binding_->enabled.store(false, std::memory_order_release);
    // Installed thunks and their trampolines have process lifetime. Retain the
    // binding after disabling so an in-flight game call can finish safely.
    if (!binding_->retained)
        delete binding_;
}
bool NativeResourcePacks::load(const RuntimeProfile &profile, std::string &error) {
    if (binding_->retained) {
        error = "resource pack hooks already retained; restart required";
        return false;
    }
    if (!profile.supports({Feature::ResourcePacks}, error))
        return false;
    try {
        auto next = std::make_unique<Binding>();
        const auto root = parseNativeProfile(profile.json());
        const auto &layout = root.at("layouts").at("ResourcePackManager");
        if (layout.at("adapter").get<std::string>() != "windows-live-reload-v6")
            throw std::runtime_error("unsupported resource pack adapter ABI");
        const auto get = [&](const char *field, const char *abi) {
            const auto &definition = layout.at("functions").at(field);
            if (definition.at("abi").get<std::string>() != abi)
                throw std::runtime_error(std::string("resource pack ABI mismatch: ") + field);
            return native_profile::checked(root, definition);
        };
        next->initTarget = get("initialize_settings", "void __fastcall(SettingsScreenController*)");
        next->ctorTarget = get("construct_general_settings",
                               "void* __fastcall(GeneralSettingsScreenController*, void* model, "
                               "bool, bool, const void* initialPack, bool)");
        next->preGameTarget = get("is_pre_game", "bool __fastcall(ClientInstance*)");
        next->clientGame = reinterpret_cast<Binding::ClientGame>(
            get("client_game", "void* __fastcall(ClientInstance*)"));
        next->gameInGameTarget = get("game_is_in_game", "bool __fastcall(MinecraftGame*)");
        // Check both the listener entry and the exact return-address guard.
        get("on_full_stack_invalid", "bool __fastcall(ResourcePackListener*)");
        next->gameGateCaller = native_profile::checked(root, layout.at("game_in_game_caller"));
        next->navigateTarget = get("navigate_apply_packs",
                                    "void __fastcall(MainMenuScreenModel*, void* callback)");
        next->eventDestroyTarget = get("destroy_apply_callback",
                                        "void __fastcall(void* callback, bool deallocate)");
        next->eventDestroy = reinterpret_cast<Binding::DestroyCallback>(next->eventDestroyTarget);
        const auto &callback = layout.at("callback_abi");
        next->callbackLayout = {
            native_profile::number(callback.at("owner_offset")),
            native_profile::number(callback.at("invoke_offset")),
            native_profile::number(callback.at("destroy_offset"))};
        if (native_profile::number(callback.at("object_bytes")) != 64 ||
            next->callbackLayout.ownerOffset != 56 || next->callbackLayout.invokeOffset != 16 ||
            next->callbackLayout.destroyOffset != 32)
            throw std::runtime_error("unsupported resource pack callback ABI");
        next->exitTarget = get("exit_settings", "uint64_t __fastcall(void* closure)");
        next->eventTarget = get("apply_stack_event", "void __fastcall(void* closure, void* model)");
        next->composeTarget = get("compose_full_stack",
                                   "uint64_t __fastcall(ResourcePackManager*, void*, void*, void*)");
        next->reloadTarget = get("reload_stack", "void __fastcall(ResourcePackManager*)");
        next->languageTarget = get("update_language_subpacks", "void __fastcall(ResourcePackManager*)");
        next->closureOwnerOffset = native_profile::number(layout.at("exit_closure_owner_offset"));
        next->settingsModelOffset = native_profile::number(layout.at("settings_model_offset"));
        next->modelClientOffset = native_profile::number(layout.at("model_client_offset"));
        next->includeGlobalOffset = native_profile::number(layout.at("include_global_stack_offset"));
        for (auto offset : {next->closureOwnerOffset, next->settingsModelOffset,
                            next->modelClientOffset, next->includeGlobalOffset})
            if (offset < 8 || offset > 0x10000)
                throw std::runtime_error("invalid resource pack apply layout");
        next->screenName = reinterpret_cast<Binding::ScreenName>(
            get("top_screen_name", "void* __fastcall(ClientInstance*, void* stringResult)"));
        next->localPlayer = reinterpret_cast<Binding::LocalPlayer>(get("local_player", "LocalPlayer* __fastcall(ClientInstance*)"));
        next->frameTarget = get("start_frame", "void __fastcall(MinecraftGame*)");
        next->geometryReload = reinterpret_cast<Binding::GeometryReload>(get("reload_geometry", "void __fastcall(MinecraftGame*, bool sync)"));
        next->notifyClientResources = reinterpret_cast<Binding::Event>(get("notify_client_resources", "void __fastcall(void* gameClosure, ClientInstance*)"));
        native_profile::checked(root, layout.at("primary_client_lookup_entry"));
        const auto &clients = layout.at("primary_clients");
        next->clientsOffset = native_profile::number(clients.at("map_offset"));
        next->nodeLeftOffset = native_profile::number(clients.at("left_offset"));
        next->nodeNilOffset = native_profile::number(clients.at("nil_offset"));
        next->nodeKeyOffset = native_profile::number(clients.at("key_offset"));
        next->nodeClientOffset = native_profile::number(clients.at("client_offset"));
        const auto &loader = layout.at("resource_loader");
        next->loaderOffset = native_profile::number(loader.at("game_offset"));
        next->loaderReadyOffset = native_profile::number(loader.at("ready_offset"));
        next->loaderOutstandingOffset = native_profile::number(loader.at("outstanding_offset"));
        for (auto offset : {next->clientsOffset, next->nodeLeftOffset, next->nodeNilOffset,
                            next->nodeKeyOffset, next->nodeClientOffset, next->loaderOffset,
                            next->loaderReadyOffset, next->loaderOutstandingOffset})
            if (offset > 0x10000)
                throw std::runtime_error("invalid frame reload layout");
        const auto &capture = layout.at("client_notification_capture");
        if (native_profile::number(capture.at("object_bytes")) != 2 * sizeof(void *) ||
            native_profile::number(capture.at("game_offset")) != sizeof(void *))
            throw std::runtime_error("unsupported client notification capture ABI");
        next->clientLevel = reinterpret_cast<Binding::ClientGame>(get("client_level", "void* __fastcall(ClientInstance*)"));
        next->levelData = reinterpret_cast<Binding::ClientGame>(get("level_data", "void* __fastcall(Level*)"));
        next->gamePacks = reinterpret_cast<Binding::ClientGame>(get("game_packs", "void* __fastcall(MinecraftGame*)"));
        next->reloadBlockTextures = reinterpret_cast<Binding::Event>(get("reload_block_textures", "void __fastcall(ResourcePackManager*, const void* experiments)"));
        next->refreshClient = reinterpret_cast<Binding::Event>(get("refresh_client", "void __fastcall(void* unused, ClientInstance*)"));
        next->atlasTarget = get("update_atlas_pbr", "void __fastcall(MinecraftGame*, const void* atlas, bool terrain)");
        next->atlasCaller = native_profile::checked(root, layout.at("terrain_completion_caller"));
        next->experimentsOffset = native_profile::number(layout.at("level_data_experiments_offset"));
        next->terrainAtlasOffset = native_profile::number(layout.at("terrain_atlas_offset"));
        if (!next->experimentsOffset || next->experimentsOffset > 0x10000 ||
            !next->terrainAtlasOffset || next->terrainAtlasOffset > 0x10000)
            throw std::runtime_error("invalid terrain refresh layout");
        next->deallocate =
            reinterpret_cast<Binding::Deallocate>(get("deallocate", "void __fastcall(void*)"));
        next->reloadOffset = native_profile::number(layout.at("reload_on_exit_offset"));
        const auto &string = layout.at("string_abi");
        next->stringObjectBytes = native_profile::number(string.at("object_bytes"));
        next->stringSizeOffset = native_profile::number(string.at("size_offset"));
        next->stringCapacityOffset = native_profile::number(string.at("capacity_offset"));
        next->stringInlineBytes = native_profile::number(string.at("inline_bytes"));
        next->maximumStringBytes = native_profile::number(string.at("maximum_bytes"));
        next->largeAllocationThreshold =
            native_profile::number(string.at("large_allocation_threshold"));
        next->alignmentOverhead = native_profile::number(string.at("alignment_overhead"));
        if (next->reloadOffset < 32 || next->reloadOffset > 0x10000 ||
            next->stringObjectBytes != 32 || next->stringSizeOffset != 16 ||
            next->stringCapacityOffset != 24 || next->stringInlineBytes != 16 ||
            next->maximumStringBytes == 0 || next->maximumStringBytes > 4096 ||
            next->largeAllocationThreshold != 4096 || next->alignmentOverhead != 39)
            throw std::runtime_error("unsupported resource pack layout or MSVC string ABI");
        delete binding_;
        binding_ = next.release();
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}
bool NativeResourcePacks::install(std::string &error) {
    std::lock_guard lock(Binding::installationMutex());
    if (binding_->installed.load()) {
        error.clear();
        return true;
    }
    if (!binding_->initTarget || !binding_->ctorTarget || !binding_->preGameTarget ||
        !binding_->screenName || !binding_->localPlayer || !binding_->deallocate ||
        !binding_->exitTarget || !binding_->eventTarget || !binding_->composeTarget ||
        !binding_->reloadTarget || !binding_->languageTarget ||
        !binding_->navigateTarget || !binding_->eventDestroy ||
        !binding_->clientGame || !binding_->gameInGameTarget || !binding_->gameGateCaller ||
        !binding_->atlasTarget || !binding_->atlasCaller || !binding_->clientLevel ||
        !binding_->frameTarget || !binding_->geometryReload || !binding_->notifyClientResources ||
        !binding_->levelData || !binding_->gamePacks || !binding_->reloadBlockTextures || !binding_->refreshClient) {
        error = "resource pack profile has not been loaded";
        return false;
    }
    if (Binding::active().load(std::memory_order_acquire)) {
        error = "resource pack adapter already owned";
        return false;
    }
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&Binding::onInitialize), &pinned)) {
        error = "could not pin resource pack hook module";
        return false;
    }
    binding_->retained = true;
    Binding::active().store(binding_, std::memory_order_release);
    const bool installed =
        BDS_HookDirectInstall(
            binding_->initTarget, reinterpret_cast<void *>(&Binding::onInitialize),
            reinterpret_cast<void **>(&binding_->initialize), &binding_->tokens[0]) &&
        BDS_HookDirectInstall(binding_->ctorTarget, reinterpret_cast<void *>(&Binding::onConstruct),
                              reinterpret_cast<void **>(&binding_->construct),
                              &binding_->tokens[1]) &&
        BDS_HookDirectInstall(binding_->preGameTarget,
                              reinterpret_cast<void *>(&Binding::onPreGame),
                              reinterpret_cast<void **>(&binding_->preGame), &binding_->tokens[2]) &&
        BDS_HookDirectInstall(binding_->exitTarget, reinterpret_cast<void *>(&Binding::onExit),
                              reinterpret_cast<void **>(&binding_->exit), &binding_->tokens[3]) &&
        BDS_HookDirectInstall(binding_->eventTarget, reinterpret_cast<void *>(&Binding::onApplyEvent),
                              reinterpret_cast<void **>(&binding_->event), &binding_->tokens[4]) &&
        BDS_HookDirectInstall(binding_->composeTarget, reinterpret_cast<void *>(&Binding::onCompose),
                              reinterpret_cast<void **>(&binding_->compose), &binding_->tokens[5]) &&
        BDS_HookDirectInstall(binding_->reloadTarget, reinterpret_cast<void *>(&Binding::onReload),
                              reinterpret_cast<void **>(&binding_->reload), &binding_->tokens[6]) &&
        BDS_HookDirectInstall(binding_->languageTarget, reinterpret_cast<void *>(&Binding::onLanguageUpdate),
                              reinterpret_cast<void **>(&binding_->languageUpdate), &binding_->tokens[7]) &&
        BDS_HookDirectInstall(binding_->navigateTarget,
                              reinterpret_cast<void *>(&Binding::onNavigateApply),
                              reinterpret_cast<void **>(&binding_->navigate), &binding_->tokens[8]) &&
        BDS_HookDirectInstall(binding_->gameInGameTarget,
                              reinterpret_cast<void *>(&Binding::onGameInGame),
                              reinterpret_cast<void **>(&binding_->gameInGame), &binding_->tokens[9]) &&
        BDS_HookDirectInstall(binding_->atlasTarget, reinterpret_cast<void *>(&Binding::onAtlasUpdate),
                              reinterpret_cast<void **>(&binding_->atlasUpdate), &binding_->tokens[10]) &&
        BDS_HookDirectInstall(binding_->frameTarget, reinterpret_cast<void *>(&Binding::onStartFrame),
                              reinterpret_cast<void **>(&binding_->frame), &binding_->tokens[11]);
    if (!installed) {
        // The broker's direct trampolines have process lifetime. Keep any
        // successfully installed thunk disabled and forwarding to the game;
        // removing its trampoline could race a game call during startup.
        error = "resource pack hook registration conflict; restart Minecraft before retrying";
        return false;
    }
    binding_->installed.store(true);
    binding_->enabled.store(true, std::memory_order_release);
    error.clear();
    return true;
}
NativeResourcePacks::Status NativeResourcePacks::status() const noexcept {
    return {binding_->installed.load(), binding_->opened.load(), binding_->overrides.load(),
            binding_->failures.load(), binding_->exits.load(), binding_->applyEvents.load(),
            binding_->reloads.load(), binding_->compositions.load(), binding_->languageUpdates.load(),
            binding_->globalIncluded.load(), binding_->navigations.load(),
            binding_->liveApplications.load(), binding_->inWorldEvents.load(), binding_->gameGateOverrides.load(),
            binding_->terrainCompleted.load(), binding_->blockTexturesReloaded.load(),
            binding_->clientsRefreshed.load(), binding_->refreshCancelled.load(),
            binding_->geometryReloaded.load(), binding_->clientDefinitionsReloaded.load(),
            binding_->resourceWaitFrames.load()};
}
} // namespace bedrock

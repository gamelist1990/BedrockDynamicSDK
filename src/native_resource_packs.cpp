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
#include <memory>

#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
#error NativeResourcePacks requires the Release MSVC STL ABI (_ITERATOR_DEBUG_LEVEL=0)
#endif

namespace bedrock {
struct NativeResourcePacks::Binding {
    using Initialize = void(__fastcall *)(void *);
    using Exit = std::uint64_t(__fastcall *)(void *);
    using Event = void(__fastcall *)(void *, void *);
    using Compose = std::uint64_t(__fastcall *)(void *, void *, void *, void *);
    using Construct = void *(__fastcall *)(void *, void *, bool, bool, const void *, bool);
    using Predicate = bool(__fastcall *)(void *);
    using ScreenName = void *(__fastcall *)(void *, void *);
    using LocalPlayer = void *(__fastcall *)(void *);
    using Deallocate = void(__fastcall *)(void *);
    void *initTarget{};
    void *ctorTarget{};
    void *preGameTarget{};
    void *exitTarget{}, *eventTarget{}, *composeTarget{}, *reloadTarget{}, *finalizeTarget{};
    Exit exit{};
    Event event{};
    Compose compose{};
    Initialize reload{}, finalize{};
    Initialize initialize{};
    Construct construct{};
    Predicate preGame{};
    ScreenName screenName{};
    LocalPlayer localPlayer{};
    Deallocate deallocate{};
    std::size_t reloadOffset{}, stringSizeOffset{}, stringCapacityOffset{}, stringInlineBytes{},
        stringObjectBytes{}, maximumStringBytes{}, largeAllocationThreshold{}, alignmentOverhead{};
    std::size_t closureOwnerOffset{}, settingsModelOffset{}, modelClientOffset{}, includeLevelOffset{};
    std::array<BDS_HookToken, 8> tokens{};
    std::atomic<bool> enabled{}, installed{};
    bool retained{};
    std::atomic<std::uint64_t> opened{}, overrides{}, failures{};
    std::atomic<std::uint64_t> exits{}, applyEvents{}, reloads{}, compositions{}, finalized{}, levelIncluded{};
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
    static void __fastcall onFinalize(void *self) {
        auto &state = *active().load(std::memory_order_acquire);
        state.finalize(self);
        if (state.enabled.load(std::memory_order_acquire))
            ++state.finalized;
    }
    static std::uint64_t __fastcall onCompose(void *self, void *full, void *level, void *global) {
        auto &state = *active().load(std::memory_order_acquire);
        const bool enabled = state.enabled.load(std::memory_order_acquire);
        if (enabled)
            ++state.compositions;
        unsigned char previous{};
        const auto client = applyingClient();
        const bool inWorld = enabled && client && state.localPlayer(client);
        const bool changed = inWorld && exchangeFlag(self, state.includeLevelOffset, 1, previous);
        if (inWorld && !changed)
            ++state.failures;
        struct Restore {
            Binding &state;
            void *self;
            bool changed;
            unsigned char previous;
            ~Restore() {
                unsigned char ignored{};
                if (changed && !exchangeFlag(self, state.includeLevelOffset, previous, ignored))
                    ++state.failures;
            }
        } restore{state, self, changed, previous};
        if (changed)
            ++state.levelIncluded;
        return state.compose(self, full, level, global);
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
        if (layout.at("adapter").get<std::string>() != "windows-settings-apply-v2")
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
        next->exitTarget = get("exit_settings", "uint64_t __fastcall(void* closure)");
        next->eventTarget = get("apply_stack_event", "void __fastcall(void* closure, void* model)");
        next->composeTarget = get("compose_full_stack",
                                   "uint64_t __fastcall(ResourcePackManager*, void*, void*, void*)");
        next->reloadTarget = get("reload_stack", "void __fastcall(ResourcePackManager*)");
        next->finalizeTarget = get("finalize_reload", "void __fastcall(ResourcePackManager*)");
        next->closureOwnerOffset = native_profile::number(layout.at("exit_closure_owner_offset"));
        next->settingsModelOffset = native_profile::number(layout.at("settings_model_offset"));
        next->modelClientOffset = native_profile::number(layout.at("model_client_offset"));
        next->includeLevelOffset = native_profile::number(layout.at("include_level_stack_offset"));
        for (auto offset : {next->closureOwnerOffset, next->settingsModelOffset,
                            next->modelClientOffset, next->includeLevelOffset})
            if (offset < 8 || offset > 0x10000)
                throw std::runtime_error("invalid resource pack apply layout");
        next->screenName = reinterpret_cast<Binding::ScreenName>(
            get("top_screen_name", "void* __fastcall(ClientInstance*, void* stringResult)"));
        next->localPlayer = reinterpret_cast<Binding::LocalPlayer>(
            get("local_player", "LocalPlayer* __fastcall(ClientInstance*)"));
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
        !binding_->reloadTarget || !binding_->finalizeTarget) {
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
        BDS_HookDirectInstall(binding_->finalizeTarget, reinterpret_cast<void *>(&Binding::onFinalize),
                              reinterpret_cast<void **>(&binding_->finalize), &binding_->tokens[7]);
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
            binding_->reloads.load(), binding_->compositions.load(), binding_->finalized.load(),
            binding_->levelIncluded.load()};
}
} // namespace bedrock

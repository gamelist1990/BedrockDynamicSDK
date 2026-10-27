#pragma once
#include <bedrock/game_type.hpp>
#include <bedrock/detail/native_player_context.hpp>

namespace bedrock {
// Independent of CameraRig, visual spectator rendering and gamma overrides.
// Invokes the native Player game-mode core setter; no raw component writes.
class NativeGameType final : public GameTypeAccess {
public:
    using Scope = native_player::Context::Scope;
    bool load(const RuntimeProfile& profile, std::string& error) {
        ready_ = false;
        if (!profile.supports({Feature::GameType}, error)) return false;
        try {
            const auto root = nlohmann::json::parse(profile.json());
            const auto& visual = root.at("native_camera").at("visual");
            typeHash_ = native_profile::number(visual.at("game_type_component_hash"));
            typeOffset_ = native_profile::number(visual.at("game_type_value_offset"));
            if (!typeHash_ || typeOffset_ > 4096 || visual.at("game_type_value_size") != 4)
                throw std::runtime_error("invalid game type component layout");
            setter_ = nullptr;
            if (visual.contains("set_player_game_type")) {
                const auto& setter = visual.at("set_player_game_type");
                if (setter.at("abi") != "void __fastcall(Player*, int)") throw std::runtime_error("unsupported game type setter ABI");
                setter_ = reinterpret_cast<Setter>(native_profile::checked(root, setter));
                requiredComponent_ = native_profile::number(setter.at("required_component_hash"));
                levelOffset_ = native_profile::number(setter.at("player_level_offset"));
                if (!requiredComponent_ || levelOffset_ > 65536) throw std::runtime_error("invalid game type setter precondition");
            }
            ready_ = context_.load(profile, error); return ready_;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
    void reset() noexcept { context_.reset(); }
    Scope tick(void* client, WorldEpoch epoch) { return context_.tick(ready_ ? client : nullptr, epoch); }
    std::optional<GameType> gameType() const override {
        if (!context_.valid()) return std::nullopt;
        int value{};
        return world_native::read(context_.component(typeHash_), typeOffset_, value) ? gameTypeFromNative(value) : std::nullopt;
    }
    bool canChangeGameType() const noexcept { return ready_ && setter_; }
    GameTypeChange setGameType(GameType value) override {
        if (!gameTypeFromNative(static_cast<int>(value))) return GameTypeChange::InvalidType;
        if (!context_.onThread()) return GameTypeChange::WrongThread;
        if (!context_.valid()) return GameTypeChange::WorldChanged;
        if (!setter_) return GameTypeChange::Unsupported;
        void* level{};
        if (!context_.component(requiredComponent_) || !gameType() ||
            !world_native::read(context_.actor(), levelOffset_, level) || !level) return GameTypeChange::NoPlayer;
        if (!invoke(setter_, context_.actor(), static_cast<int>(value))) return GameTypeChange::NativeFailure;
        const auto actual = gameType();
        return actual && *actual == value ? GameTypeChange::Applied : GameTypeChange::NativeFailure;
    }
private:
    using Setter = void(__fastcall*)(void*, int);
    static bool invoke(Setter fn, void* actor, int value) noexcept {
        __try { fn(actor, value); return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    native_player::Context context_;
    Setter setter_{};
    std::uint32_t typeHash_{}, requiredComponent_{};
    std::size_t typeOffset_{}, levelOffset_{};
    bool ready_{};
};
} // namespace bedrock

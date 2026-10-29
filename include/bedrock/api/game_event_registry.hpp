#pragma once
#include <bedrock/api/events.hpp>
namespace bedrock {
struct GameEventRegistry {
    struct EnumType {
        EventChannel<KeyEvent> Keyboard;
        EventChannel<NativeKeyEvent> NativeKeyboard;
        EventChannel<MouseEvent> Mouse;
        EventChannel<ActorNameTagEvent> ActorNameTag;
        EventChannel<ViewNameTagEvent> ViewNameTag;
        EventChannel<RenderViewActorEvent> RenderViewActor;
        EventChannel<UpdateEvent> Update;
        EventChannel<LevelTickEvent> LevelTick;
        EventChannel<GameplayEvent> Gameplay;
        EventChannel<FovEvent> Fov;
        EventChannel<WorldChangeEvent> WorldChange;
        EventChannel<ClientJoinLevelEvent> ClientJoinLevel;
        EventChannel<ClientExitLevelEvent> ClientExitLevel;
        EventChannel<ClientCancelJoinLevelEvent> ClientCancelJoinLevel;
        EventChannel<DimensionChangeEvent> DimensionChange;
    } Enum;
};

} // namespace bedrock

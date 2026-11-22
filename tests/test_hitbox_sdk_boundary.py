from pathlib import Path


def test_hitbox_actor_registry_uses_sdk_adapter():
    base = Path(__file__).resolve().parents[1] / "exsample/HitBox"
    source = (base / "src/hitbox.cpp").read_text()
    for forbidden in ("entt::", "EnTT::", "basic_registry", "hitbox_native",
                      "captureRegistryBatch", "copyTrackedPoses("):
        if forbidden == "copyTrackedPoses(":
            # The SDK entry point is registry_.copyTrackedPoses; the static
            # in-Mod implementation must be gone.
            assert "static std::size_t copyTrackedPoses" not in source
            continue
        assert forbidden not in source, forbidden
    # Final state (plugin-system completion): HitBox consumes only the
    # public ActorFacade; the native registry types it used
    # mid-migration must stay gone.
    for api in ("ActorFacade", "ActorSample"):
        assert api in source, api
    for gone in ("NativeActorRegistry", "actor_registry.hpp", "ActorRegistrySample"):
        assert gone not in source, gone
        assert gone not in (base / "include/hitbox/box_cache.hpp").read_text(), gone
    cmake = (base / "CMakeLists.txt").read_text()
    assert "BEDROCK_SDK_ENABLE_NATIVE_ACTOR_REGISTRY" in cmake
    assert "skypjack/entt" not in cmake

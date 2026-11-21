from pathlib import Path


def test_speedbuilder_world_access_uses_sdk_adapter():
    base = Path(__file__).resolve().parents[1] / "exsample/SpeedBuilder"
    source = (base / "src/speed_builder.cpp").read_text()
    # Raw native-access detail headers would bypass the adapter and are banned.
    # bedrock/detail/hook_dispatch.hpp is not one of them: it is the SDK's
    # standard event-hook wiring surface (also used via the public
    # bedrock/runtime/game_hooks.hpp by the other samples), unrelated to
    # world access.
    for forbidden in ("world_native", "speedbuilder_native", "native.hpp",
                      "bedrock/detail/native_world_access.hpp",
                      "bedrock/detail/client_access.hpp",
                      "bedrock/detail/native_player_context.hpp",
                      "bedrock/detail/native_profile_functions.hpp",
                      "bedrock/detail/auto_profile_x64.hpp",
                      "Bindings bindings", "Reader reader"):
        assert forbidden not in source, forbidden
    for api in ("NativeWorldAccess", "world_access.hpp", "readField", "writeField",
                "reserveCaches"):
        assert api in source, api
    # The compatibility forwarding header must stay deleted (AGENTS.md).
    assert not (base / "src/native.hpp").exists()
    cmake = (base / "CMakeLists.txt").read_text()
    assert "BEDROCK_SDK_ENABLE_NATIVE_WORLD" in cmake
    assert "bedrock::world" in cmake

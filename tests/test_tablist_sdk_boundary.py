from pathlib import Path


def test_tablist_uses_sdk_entry_points_without_native_abi():
    base = Path(__file__).resolve().parents[1] / "exsample/DynamicClient"
    source = (base / "src/tablist/roster.cpp").read_text()
    for forbidden in ("BDS_HookSubscribe", "BDS_HookToken", "__fastcall", "__try", "__except",
                      "VirtualQuery", "ReadProcessMemory", "safeRead", "NativeItemLayout",
                      "NativeActorLayout", "resolveSharedHook", "fetchSupportedProfile",
                      "parseNativeProfile", "EnTT::", "entt::"):
        assert forbidden not in source, forbidden
    # Final state (plugin-system completion): the tablist consumes only
    # public SDK facades; the native bridge types it used mid-migration
    # must stay gone.
    for api in ("RosterAccess", "PacketRoster", "Overlay", "equipmentSnapshot"):
        assert api in source
    for gone in ("NativeGameAccess", "RuntimeProfile", "GameHooks"):
        assert gone not in source, gone
    cmake = (base / "CMakeLists.txt").read_text()
    assert "BEDROCK_SDK_ENABLE_NATIVE_CLIENT" in cmake
    assert "skypjack/entt" not in cmake

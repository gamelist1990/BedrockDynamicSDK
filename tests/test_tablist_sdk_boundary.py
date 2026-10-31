from pathlib import Path


def test_tablist_uses_sdk_entry_points_without_native_abi():
    base = Path(__file__).resolve().parents[1] / "exsample/TabList"
    source = (base / "src/minecraft_client.cpp").read_text()
    for forbidden in ("BDS_HookSubscribe", "BDS_HookToken", "__fastcall", "__try", "__except",
                      "VirtualQuery", "ReadProcessMemory", "safeRead", "NativeItemLayout",
                      "NativeActorLayout", "resolveSharedHook", "fetchSupportedProfile",
                      "parseNativeProfile", "EnTT::", "entt::"):
        assert forbidden not in source, forbidden
    for api in ("NativeGameAccess", "ProfileManager", "GameHooks", "equipmentSnapshot"):
        assert api in source
    cmake = (base / "CMakeLists.txt").read_text()
    assert "BEDROCK_SDK_ENABLE_NATIVE_CLIENT" in cmake
    assert "skypjack/entt" not in cmake

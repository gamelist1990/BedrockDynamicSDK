from pathlib import Path


def test_speedbuilder_world_access_uses_sdk_adapter():
    base = Path(__file__).resolve().parents[1] / "exsample/SpeedBuilder"
    source = (base / "src/speed_builder.cpp").read_text()
    for forbidden in ("world_native", "speedbuilder_native", "native.hpp",
                      "bedrock/detail", "Bindings bindings", "Reader reader"):
        assert forbidden not in source, forbidden
    for api in ("NativeWorldAccess", "world_access.hpp", "readField", "writeField",
                "reserveCaches"):
        assert api in source, api
    # The compatibility forwarding header must stay deleted (AGENTS.md).
    assert not (base / "src/native.hpp").exists()
    cmake = (base / "CMakeLists.txt").read_text()
    assert "BEDROCK_SDK_ENABLE_NATIVE_WORLD" in cmake
    assert "bedrock::world" in cmake

from pathlib import Path


def test_whole_sdk_public_headers_hide_internal_tree():
    root = Path(__file__).resolve().parents[1]
    include = root / "sdk/include"
    forbidden = ("bedrock/native", "bedrock/detail", "bedrock/profiles",
                 "bedrock/packets", "bedrock/runtime", "bedrock/enums",
                 "bedrock/logging", "bedrock/commands", "bedrock/render",
                 "bedrock/analysis", "bedrock/generated")
    for path in include.rglob("*.hpp"):
        text = path.read_text(errors="replace")
        for prefix in forbidden:
            assert prefix not in text, f"{prefix} in {path.relative_to(root)}"

    cmake = (root / "sdk/CMakeLists.txt").read_text(errors="replace")
    assert "../internal/include" not in cmake
    assert "../third_party" not in cmake



def test_speedbuilder_links_only_bedrock_sdk():
    base = Path(__file__).resolve().parents[1] / "exsample/SpeedBuilder"
    source = (base / "src/speed_builder.cpp").read_text()
    # SpeedBuilder must include only the public SDK tree.
    for line in source.splitlines():
        if "#include" in line and "bedrock/" in line:
            assert ("bedrock/sdk.hpp" in line or "bedrock/sdk/" in line), line
    # Raw native/generated/profile internals are no longer included directly.
    for forbidden in ("generated/symbols.hpp", "profiles/profile_manager.hpp",
                      "profiles/remote_catalog.hpp", "native/world_access.hpp",
                      "detail/hook_dispatch.hpp", "runtime/game_hooks.hpp",
                      "detail/block_types.hpp", "packets/title_packets.hpp"):
        assert forbidden not in source, forbidden
    cmake = (base / "CMakeLists.txt").read_text()
    assert "bedrock::sdk" in cmake
    for forbidden_target in ("bedrock::world", "bedrock::logging", "bedrock::hook_runtime",
                             "bedrock::symbols", "bedrock::client", "bedrock::game_hooks",
                             "bedrock::actor_registry", "bedrock::internal_headers"):
        assert forbidden_target not in cmake, forbidden_target
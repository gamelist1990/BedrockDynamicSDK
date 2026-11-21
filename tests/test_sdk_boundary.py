from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SDK_INCLUDE = ROOT / "sdk/include"
FORBIDDEN_INTERNAL = re.compile(r"<bedrock/(native|detail|profiles|packets|runtime|enums|logging|commands|render|analysis|generated)(/|>)")


def test_all_sdk_headers_hide_internal_tree():
    for path in SDK_INCLUDE.rglob("*.hpp"):
        text = path.read_text(errors="replace")
        assert not FORBIDDEN_INTERNAL.search(text), f"internal include in {path.relative_to(ROOT)}"


def test_sdk_cmake_has_no_public_internal_path_or_link():
    cmake = (ROOT / "sdk/CMakeLists.txt").read_text(errors="replace")
    assert "../internal/include" not in cmake
    assert "../third_party" not in cmake
    public_section = cmake
    for target in ("bedrock::world", "bedrock::client", "bedrock::game_hooks",
                   "bedrock::symbols", "bedrock::hook_runtime", "bedrock::logging",
                   "bedrock::actor_registry", "bedrock::internal_headers"):
        assert not re.search(r"target_link_libraries\([^\n]*PUBLIC[^\n]*" + re.escape(target), public_section), target

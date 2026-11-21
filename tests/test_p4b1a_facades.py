from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT / "sdk/include/bedrock/sdk"

PUBLIC = ["game_hooks.hpp", "resolver.hpp", "actors.hpp"]
INTERNAL_MARKERS = ("native/", "detail/", "profiles/", "runtime/", "generated/")

for name in PUBLIC:
    text = (INCLUDE / name).read_text()
    assert not any(marker in text for marker in INTERNAL_MARKERS), name

umbrella = (INCLUDE.parent / "sdk.hpp").read_text()
for name in PUBLIC:
    assert f"bedrock/sdk/{name}" in umbrella

#!/usr/bin/env bash
# Build all BedrockDynamicSDK Windows x64 Mod DLLs entirely on Linux, using
# clang-cl + lld-link against an xwin-splatted MSVC CRT and Windows SDK.
# This mirrors .github/workflows/build-mods.yml (MSVC x64 Release) without
# needing a Windows machine.
#
# One-time prerequisites:
#   sudo apt-get install -y clang lld
#   xwin --accept-license --arch x86_64 --variant desktop \
#        --sdk-version 10.0.22621 splat --output ~/tools/xwin-splat
# (xwin binary used here lives at ~/tools/xwin/xwin, version 0.6.7; SDK
# 10.0.26100 fails in it, hence the 10.0.22621 pin. See BUILD-LINUX notes.)
#
# Version policy: LATEST ONLY. The default (and normally only) target is the
# newest supported game version; every Mod builds for it. Older game versions
# are not built by default -- pass --legacy to additionally build the previous
# version's set for regression comparison.
#
# Usage:
#   ./build-linux.sh           build all Mods for the latest version into dist/
#   ./build-linux.sh --legacy  also build the previous version's Mod set
#   ./build-linux.sh --clean   wipe build-linux* and dist/ first (combinable)
#   ./build-linux.sh --oreui-settings   build the standalone OreUI demo Mod
#   ./build-linux.sh --stage-a   build the SDK, DynamicClient and CheckSig only
#   ./build-linux.sh --stage-b   build the SDK, ScriptClient (JS/TS plugin host)
#                                and CheckSig; stage mods/EffectFake
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
BUILD_ID=${BEDROCK_BUILD_ID:-$(git rev-parse --short HEAD)}
TOOLCHAIN=$ROOT/linux-xwin-toolchain.cmake

LATEST=1.26.65.1
LEGACY=1.26.60.5
CLIENT_MODS=(DynamicClient AutoMine HitBox SpeedBuilder)
DEV_MODS=(CheckSig BlockProbe CraftProbe InputProbe StackProbe Fake Top GlobalResourcePack ModSettingsDemo)
JOBS=${BEDROCK_BUILD_JOBS:-$(nproc)}

command -v clang-cl >/dev/null || { echo "clang-cl missing: sudo apt-get install -y clang"; exit 1; }
command -v lld-link >/dev/null || { echo "lld-link missing: sudo apt-get install -y lld"; exit 1; }
[ -d "$HOME/tools/xwin-splat/crt" ] || { echo "xwin splat missing at ~/tools/xwin-splat (see header of this script)"; exit 1; }

CLEAN=0; WANT_LEGACY=0; OREUI_SETTINGS=0; STAGE_A=0; STAGE_B=0
for a in "$@"; do
  case "$a" in
    --clean) CLEAN=1 ;;
    --legacy) WANT_LEGACY=1 ;;
    --oreui-settings) OREUI_SETTINGS=1 ;;
    --stage-a) STAGE_A=1 ;;
    --stage-b) STAGE_B=1 ;;
    *) echo "unknown argument: $a" >&2; exit 1 ;;
  esac
done
if [ "$STAGE_A" = 1 ] && [ "$STAGE_B" = 1 ]; then
  echo "--stage-a and --stage-b are mutually exclusive" >&2; exit 1
fi
if [ "$OREUI_SETTINGS" = 1 ] && { [ "$CLEAN" = 1 ] || [ "$WANT_LEGACY" = 1 ]; }; then
  echo "--oreui-settings cannot be combined with --clean or --legacy" >&2
  exit 1
fi
if [ "$STAGE_A" = 1 ]; then
  if [ "$OREUI_SETTINGS" = 1 ] || [ "$WANT_LEGACY" = 1 ] || [ "$CLEAN" = 1 ]; then
    echo "--stage-a cannot be combined with --oreui-settings, --legacy or --clean" >&2
    exit 1
  fi
  CLIENT_MODS=(DynamicClient)
  DEV_MODS=(CheckSig)
fi
if [ "$STAGE_B" = 1 ]; then
  if [ "$OREUI_SETTINGS" = 1 ] || [ "$WANT_LEGACY" = 1 ] || [ "$CLEAN" = 1 ]; then
    echo "--stage-b cannot be combined with --oreui-settings, --legacy or --clean" >&2
    exit 1
  fi
  CLIENT_MODS=()
  DEV_MODS=(CheckSig)
fi
if [ "$CLEAN" = 1 ]; then rm -rf build-linux build-linux-* dist; fi

CMAKE_ARGS=(
  -G Ninja --toolchain "$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release
  -DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK=$ROOT
  -DBEDROCK_BUILD_ID=$BUILD_ID -DBEDROCK_HITBOX_BUILD_ID=$BUILD_ID
)

build_one() { # name sourcedir version [build-dir-suffix]
  local name=$1 src=$2 ver=$3 bdir=build-linux-$1${4:+-$4}
  if [[ $src == exsample/DevClient/* ]]; then bdir=build-linux-DevClient-$name${4:+-$4}; fi
  echo "=== $name (game $ver) ==="
  cmake -S "$src" -B "$bdir" "${CMAKE_ARGS[@]}" -DBEDROCK_GAME_VERSION=$ver
  cmake --build "$bdir" -j"$JOBS"
  mkdir -p "dist/$ver"
  find "$bdir" -maxdepth 1 -name '*.dll' -exec cp {} "dist/$ver/" \;
}

if [ "$OREUI_SETTINGS" = 1 ]; then
  build_one ModSettingsDemo exsample/DevClient/ModSettingsDemo "$LATEST"
  exit 0
fi

echo "=== SDK ==="
cmake -S . -B build-linux "${CMAKE_ARGS[@]}" -DBEDROCK_GAME_VERSION=$LATEST
cmake --build build-linux -j"$JOBS"
mkdir -p dist/sdk
cp build-linux/BedrockHookRuntime.dll build-linux/BedrockSymbolProbe.dll \
   build-linux/BedrockClientLocator.dll dist/sdk/

for m in "${CLIENT_MODS[@]}"; do
  build_one "$m" "exsample/$m" $LATEST
done
for m in "${DEV_MODS[@]}"; do
  build_one "$m" "exsample/DevClient/$m" $LATEST
done
if [ "$STAGE_A" = 1 ]; then
  file "dist/$LATEST/DynamicClient.dll" "dist/$LATEST/CheckSig.dll" \
       dist/sdk/BedrockHookRuntime.dll dist/sdk/BedrockSymbolProbe.dll dist/sdk/BedrockClientLocator.dll
  exit 0
fi
if [ "$STAGE_B" = 1 ]; then
  echo "=== ScriptClient (Stage B plugin host) ==="
  build_one ScriptClient exsample/ScriptClient "$LATEST"

  if [ -d "sdk/ts/node_modules" ]; then
    # This is a generated package tree, not a mod/user data directory.
    cmake -E rm -rf dist/sdk/ts
    cp -R sdk/ts dist/sdk/
    echo "staged TypeScript SDK packages: dist/sdk/ts/node_modules/@bedrock/{sdk,vanilla-data}"
  fi

  mkdir -p "dist/$LATEST/mods"
  cp -R tests/golden/EffectFake/. "dist/$LATEST/mods/EffectFake/"
  echo "staged golden mod: dist/$LATEST/mods/EffectFake"

  file "dist/$LATEST/ScriptClient.dll" "dist/$LATEST/CheckSig.dll" \
       dist/sdk/BedrockHookRuntime.dll dist/sdk/BedrockSymbolProbe.dll dist/sdk/BedrockClientLocator.dll
  exit 0
fi
# HitBox ships a loader executable next to the DLL in CI artifacts.
cp build-linux-HitBox/BedrockHitBoxLoader.exe dist/$LATEST/ 2>/dev/null || true

if [ "$WANT_LEGACY" = 1 ]; then
  echo "=== legacy set (game $LEGACY, opt-in) ==="
  for m in HitBox SpeedBuilder; do
    build_one "$m" "exsample/$m" $LEGACY legacy
  done
  for m in CheckSig Top; do
    build_one "$m" "exsample/DevClient/$m" $LEGACY legacy
  done
  cp build-linux-HitBox-legacy/BedrockHitBoxLoader.exe dist/$LEGACY/ 2>/dev/null || true
fi

echo
echo "=== Built DLLs ==="
find dist -name '*.dll' | sort
file dist/*/*.dll

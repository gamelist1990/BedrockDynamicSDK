# Creator Content Log probe

From the repository root, cross-build the SDK-only test DLL:

```sh
cmake -S exsample/DevClient/ContentLogProbe -B build-linux-DevClient-ContentLogProbe \
  -G Ninja --toolchain "$PWD/linux-xwin-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-DevClient-ContentLogProbe --target ContentLogProbe
```

Enable the game's Creator Content Log, then load `CheckSig.dll`,
`BedrockHookRuntime.dll`, and `ContentLogProbe.dll` in that order before
entering a local world. The probe calls `ContentLog::write` once on local
player spawn; there is no chat command. `ContentLogProbe.log` alongside the
DLL records `ContentLog::available()` and the unique `CPP_CONTENT_LOG_PROBE_…`
marker. Match that marker in the game's Content Log History or in the
game-generated `ContentLog*.txt` after quitting normally. The probe's own log
does not prove delivery.

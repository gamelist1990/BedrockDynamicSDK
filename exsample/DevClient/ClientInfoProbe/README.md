# Client diagnostics smoke mod

Read-only probe for the SDK client diagnostics surface. Every ~3 s while a
world is loaded it logs the local player's `fps`, `ping` (last / median /
samples / connected), `cps` (left/right) and `device` (platform, version,
window size) through Creator Content Log and to `ClientInfoProbe.log` next to
the DLL.

From the repository root, cross-build the DLL:

```sh
cmake -S exsample/DevClient/ClientInfoProbe -B build-linux-DevClient-ClientInfoProbe \
  -G Ninja --toolchain "$PWD/linux-xwin-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-DevClient-ClientInfoProbe --target ClientInfoProbe
```

(The default `./build-linux.sh` builds it with the rest of the DevClient set
and stages `dist/1.26.65.1/ClientInfoProbe.dll`.)

Enable the game's Creator Content Log, then load `CheckSig.dll`,
`BedrockHookRuntime.dll` and `ClientInfoProbe.dll` in that order and enter a
world. Look for `ClientInfoProbe` lines in the Content Log History, or in
`ClientInfoProbe.log` for the values the mod actually read.

Expected on 1.26.65.1:

- `fps` rises to the present rate within a second and drops to 0 when the
  world/window stops rendering.
- `ping` stays 0 / `connected=false` until the client's own
  NetworkStackLatency exchange has been observed (typically a remote world).
- `cps` follows real left/right clicks; synthetic SDK input is not counted.
- `device` shows `windows-x64`, `1.26.65.1` and the game window client area.

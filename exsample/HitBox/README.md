# BedrockHitBox

BedrockDynamicSDK用のWindows x64 HitBox表示Mod exampleです。公開物は `BedrockHitBox.dll` 単体です。

- プレイヤーに限定せず、SDKの `World::getActors()` が返すネイティブActorのAABBを描画
- Actorの取得・位置・AABB・向きは `GameApi` / `Actor` APIを使用
- Actor一覧のNative EnTT走査は最大4Hzに制限し、位置・AABBは描画フレームごとに更新して滑らかに描画
- 一時的に取得できなくなったActorの最終AABBを保持する機能はconfigで切替可能
- 最終位置の保持期限は既定10分。ワールドを移動した場合はキャッシュを破棄
- Actorポインタやコンポーネント読み取りで例外が発生した対象はスキップ

設定ファイル:

```text
C:\Users\<ユーザー名>\Documents\PEXData\HitBox\config.json
```

Linux上のBedrockOnLinux/Protonでは、`HOME` を基準にホスト側の
`~/Documents/PEXData/HitBox` を使用します。Wineの `Z:` ドライブ経由でLinuxパスへ解決します。
`BEDROCK_HITBOX_DIR` を設定すると保存先を明示的に上書きできます（Linux環境では `/home/...` 形式も可）。

```json
{
  "enabled": true,
  "debug": false,
  "nearby_distance": 32.0,
  "retain_last_position": false,
  "retention_minutes": 10,
  "line_thickness": 1.5,
  "live_color": "#4CE684E6",
  "retained_color": "#FFAE42D2"
}
```

`debug` を `true` にすると、近距離で初回検知したActorについて `HitBox.log` に
`entity_id` と取得できた名前を一度だけ記録します。名前タグがまだキャッシュされていない場合は
最大60描画フレーム待ち、それでも得られない場合は `name=<unavailable>` と一度だけ記録します。

プレイヤーだけに絞る設定は設けず、すべての取得可能なActorを対象にします。`retain_last_position` をtrueにすると、
近距離で最後に確認したAABBを最大 `retention_minutes` 分その位置に残し、再検出時は位置と期限を更新します。
Level更新時はワールド参照だけを更新します。Actorのハンドル一覧は250msごとに再列挙し、各描画フレームでは
そのキャッシュのActorから位置とAABBを更新します。ライン描画はその処理が作る不変スナップショットを使います。
ActorType component storageが対象レジストリから解決できない場合は、ActorOwner + RuntimeID + AABBShapeの交差と
Actorのregistry/entity ID照合を維持した3-component fallbackを使います。必須storageが欠ける場合は一覧を拒否し、
ログに不足状態を明示します。
表示座標への投影はローカルActorの位置・視点角度・FOVから計算するため、実機で投影方向やAABB位置の差が
見つかった場合は調整が必要です。

現在の `native_actors` プロファイル値は、ActorOwner/AABB/StateVector/Rotation componentの既知レイアウトと
対象EXEのコンポーネント名から作った候補です。すべての読み取りは範囲・値チェックとSEH境界を通しますが、
プロファイルの `status` が `candidate` の間は実機で完全に検証済みという意味ではありません。値が不正または
未対応なら該当Actorを描画せず、ゲーム側の更新に干渉しません。新しいMinecraft版では対象EXEの一致プロファイルに
`native_actors` レイアウトを追加して更新してください。

## Build

```powershell
cmake -S . -B build -A x64 -DBEDROCK_SDK_REF=main
cmake --build build --config Release --parallel
```

This also builds `BedrockHitBoxLoader.exe`, a small local test loader. It only
accepts a process whose executable or main-module path identifies
`Minecraft.Windows.exe`, checks that both process and DLL are x64, and reports
whether the module appeared in the target's module list. WineGDK can expose the
Minecraft process with an empty `PROCESSENTRY32::szExeFile`; the loader therefore
falls back to the process module path instead of silently missing the game.
First list the game process; use `list --all` to inspect Wine process entries if
needed; then explicitly inject the DLL into the identified PID:

```text
BedrockHitBoxLoader.exe list
BedrockHitBoxLoader.exe list --all
BedrockHitBoxLoader.exe inject <pid> <x64-DLL-path>
```

On BedrockOnLinux/Proton, run the loader through the same Proton prefix as the
game and pass the DLL path as a Wine path (for example
`Z:\home\<user>\WorkSpace\DevBox\BedrockDLLs\HitBox-main-<sha>\BedrockHitBox.dll`).
Use a local/offline test world. This loader is not a game launcher or a
compatibility layer; the HitBox DLL itself must match the target's C++ ABI.

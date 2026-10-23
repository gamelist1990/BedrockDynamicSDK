# BedrockHitBox

BedrockDynamicSDK用のWindows x64 HitBox表示Mod exampleです。公開物は `BedrockHitBox.dll` 単体です。

- プレイヤーに限定せず、SDKの `World::getActors()` が返すネイティブActorのAABBを描画
- Actorの取得・位置・AABB・向きは `GameApi` / `Actor` APIを使用
- 描画フレームごとにActor一覧・位置・AABBを更新し、そのフレーム用の不変スナップショットを描画
- 一時的に取得できなくなったActorの最終AABBを保持する機能はconfigで切替可能
- 最終位置の保持期限は既定10分。ワールドを移動した場合はキャッシュを破棄
- Actorポインタやコンポーネント読み取りで例外が発生した対象はスキップ

設定ファイル:

```text
C:\Users\<ユーザー名>\Documents\PEXData\HitBox\config.json
```

```json
{
  "enabled": true,
  "nearby_distance": 32.0,
  "retain_last_position": false,
  "retention_minutes": 10,
  "line_thickness": 1.5,
  "live_color": "#4CE684E6",
  "retained_color": "#FFAE42D2"
}
```

プレイヤーだけに絞る設定は設けず、すべての取得可能なActorを対象にします。`retain_last_position` をtrueにすると、
近距離で最後に確認したAABBを最大 `retention_minutes` 分その位置に残し、再検出時は位置と期限を更新します。
Level更新時はワールド参照だけを更新し、Actor列挙は描画フレームごとに行います。Actor/EnTTへアクセスするのは
フレーム取得処理だけで、ライン描画はその処理が作る不変スナップショットを使います。
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

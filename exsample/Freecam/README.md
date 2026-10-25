# Freecam (Windows x64)

SDKの通常のexample Modです。MSVC **Release**で`BedrockFreecam.dll`を作成します。
対象はMinecraft **1.26.52.3**。新規の座標書き込み・送信経路は解析中の候補であり、実機動作は未確認です。
解析根拠は[freecam-native.md](../../docs/freecam-native.md)を参照してください。

```powershell
cmake -S exsample/Freecam -B build-freecam -A x64 "-DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK=$PWD"
cmake --build build-freecam --config Release --parallel 2
```

## 使用

DLLをロードし、Escでメニューを開いてゲームに戻ってから **M** で切替。
WASDで移動、Spaceで上昇、Shiftで下降、Ctrlで高速移動、マウスで視点操作。
メニュー表示、ワールド/ディメンション変更、被弾、必要なネイティブ値の取得失敗で解除します。
Borion方式でクライアント側Playerを移動し、使用中は速度・落下距離をゼロ化し、AABBの大きさを保って移動します。
解除時はゲームスレッドで開始位置とAABBを復元します。ワールド変更後に旧座標を書き戻しません。
移動パケット（MovePlayer / PlayerAuthInput）は開始直前に観測した送信位置へ固定して転送します。
PlayerAuthInputの移動量・移動入力だけをゼロ化し、アイテム操作・取引・tick番号は維持します。
新鮮な送信位置を取得できるまでは移動を開始しません。
上昇・下降は0.5ブロック/tick、水平移動は設定速度を使います。

設定とログはWindowsのDocuments既知フォルダ内の`PEXData/Freecam/`です。
`config.json`変更を約500ms間隔で読み直します。変更時は一度解除します。

| 設定 | 既定値 | 内容 |
| --- | --- | --- |
| `enabled` | true | Mod機能の有効化（起動時FreecamはOFF） |
| `toggle_key` | 77 | 切替キー（M） |
| `allow_interaction` | false | 手動の攻撃・使用・ホイール等の入力をゲームに渡す |
| `disable_on_damage` | true | 自身のhurt time増加で解除。取得不能時も解除 |
| `player_hitboxes` | true | Freecam中のみ付近のPlayerに簡易枠を表示 |
| `spectator_mode` | false | Freecam中だけブロック内の視界を覆う描画を抑制（ゲームモード変更なし） |
| `full_bright` | true | Freecam中だけガンマを25へ上書き。解除時は本来の取得値 |
| `speed_blocks_per_second` | 8 | 移動速度 |
| `fast_multiplier` | 3 | 高速移動倍率 |
| `mouse_sensitivity` | 0.15 | 旧カメラ方式用。現行方式の視点操作はゲーム設定に従う |
| `hitbox_radius` | 32 | カメラからの距離（1～100） |
| `max_players` | 64 | 描画上限（1～128） |
| `capture_rows_per_update` | 8 | 1tick当たりの新Player発見走査上限（1～16） |
| `keys` | WASD/Space/Shift/Ctrl | 各InputKeyを数値で変更可能 |

`allow_interaction=true`で通常の手動入力を通し、PlayerAuthInput内の操作情報もそのまま送ります。
パケットをMod側で保留・後送りしません。サーバー側のリーチ判定は変更しません。
カメラ位置だけから、Player本体の通常の操作範囲外にあるチェストを開ける保証はありません。
クライアントが受信していない範囲のPlayerは表示できません。
簡易枠はRuntimeIDとEnTT世代で追跡します。発見済みPlayerの座標を毎LevelTick取得し、
描画時に50ms（1tick）で補間します。テレポート・250ms超の取得中断では補間をリセットします。
新Playerの発見走査だけを `capture_rows_per_update` 件ずつに分散します。
Actor消失時は次の取得で枠を除去し、取得全体が停止したスナップショットは250msで表示を止めます。
保存位置・名前表示はありません。

## ランタイム

Runtime **4.3.0**を内蔵します。互換性のある既存所有者へ接続できます。
配布セットの`BedrockHookRuntime.dll`をMod DLLと同じフォルダに置くと、
新しいSDKのModは初回API利用前に、ロード済みDLLの埋め込みランタイムと
同じフォルダの任意のRuntime DLLを比較し、ABI 4互換の最も新しいものを選択します。
既存Runtime 4.0のDLLも、先に起動したABI 4の上位所有者へ接続できます。
既に設置したHookの所有者は途中交換しないため、更新時はMinecraftを再起動してください。
Freecamには送信point 14が必要です。Runtime 4.1以前の所有者では起動できません。

旧既定キーV（86）の設定は、初回にM（77）へ移行して`config_revision:2`を保存します。
移行後は`toggle_key`を自由に変更できます。
`Freecam_GetStatus`（0:未起動、1:起動中、2:待機/稼働、3:停止、4:失敗）、
`Freecam_IsActive`、`Freecam_Stop`をエクスポートします。
ローカルPlayerの観測HookはDLL寿命中保持します。停止後も次のゲームtickで未完了の
位置復元を行えるようにコールバックを保持し、復元後は元処理のみ転送します。

参考：[Borion Freecam](https://github.com/Borion-Updated/Borion-OSS/blob/master/Horion/Module/Modules/Freecam.cpp)。
ネイティブ書き込みと送信経路の実機動作は未確認です。

### インタラクション診断

`Freecam packet flow`の`anchored`は位置固定して転送した移動パケット数、
`item_inputs`は操作フラグを残して転送したPlayerAuthInput数、`rejected`は検証失敗数です。
`BedrockHookRuntime:`行には任意DLLの探索パス・LoadLibraryExのエラー番号・
選択プロバイダと実際の所有者のバージョン・パスが出ます。
パケット配置とライブ通信・サーバーでの操作受理は実機未確認です。

## 地中表示・明るさ

```json
{
  "spectator_mode": true,
  "full_bright": true,
  "allow_interaction": true
}
```

既存のconfig.jsonにも不足している新設定を追記します。`spectator_mode`の既定はfalseです。
ゲームモードを変えず、カメラがブロック内にあるときのブロックテクスチャ表示を抑制します。
現在のClientInstanceでFreecamが有効な間だけ、カメラのブロック内表示許可判定をfalseにします。
解除・被弾・ワールド移動・停止で通常の判定へ戻ります。ActorのGameTypeは読み書きしません。
入口のバイト不一致・Hook競合時は無効化し、ログに理由を出します。
`Freecam inside-block overlay suppressed=N` は描画抑制した回数です（5秒ごと）。
地形の深度やカリングを一括無効化する機能ではなく、描画方式による見え方は実機確認が必要です。
古いDLLのGameType変更が残らないよう、差し替え時はMinecraftを再起動してください。

`full_bright`はOptionsのガンマ取得Hookだけで上書きし、options.txtの値は保存変更しません。
Hook競合や入口不一致の場合は、明るさ機能を使用せず理由をログへ出します。
地中表示と描画方式ごとの明るさは実機未確認です。

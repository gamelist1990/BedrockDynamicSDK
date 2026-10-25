# Freecam (Windows x64)

SDKの通常のexample Modです。MSVC **Release**で`BedrockFreecam.dll`を作成します。
対象はMinecraft **1.26.52.3**。新規カメラ経路は静的解析済みの候補であり、実機動作は未確認です。
解析根拠は[freecam-native.md](../../docs/freecam-native.md)を参照してください。

```powershell
cmake -S exsample/Freecam -B build-freecam -A x64 "-DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK=$PWD"
cmake --build build-freecam --config Release --parallel 2
```

## 使用

DLLをロードし、Escでメニューを開いてゲームに戻ってから **M** で切替。
WASDで移動、Spaceで上昇、Shiftで下降、Ctrlで高速移動、マウスで視点操作。
メニュー表示、ワールド/ディメンション変更、被弾、必要なネイティブ値の取得失敗で解除します。
解除時はゲーム本来のカメラ更新に戻ります。Player本体の座標を書き換えません。

設定とログはWindowsのDocuments既知フォルダ内の`PEXData/Freecam/`です。
`config.json`変更を約500ms間隔で読み直します。変更時は一度解除します。

| 設定 | 既定値 | 内容 |
| --- | --- | --- |
| `enabled` | true | Mod機能の有効化（起動時FreecamはOFF） |
| `toggle_key` | 77 | 切替キー（M） |
| `allow_interaction` | false | 手動の攻撃・使用・ホイール等の入力をゲームに渡す |
| `disable_on_damage` | true | 自身のhurt time増加で解除。取得不能時も解除 |
| `player_hitboxes` | true | Freecam中のみ付近のPlayerに簡易枠を表示 |
| `speed_blocks_per_second` | 8 | 移動速度 |
| `fast_multiplier` | 3 | 高速移動倍率 |
| `mouse_sensitivity` | 0.15 | マウス感度 |
| `hitbox_radius` | 32 | カメラからの距離（1～100） |
| `max_players` | 64 | 描画上限（1～128） |
| `capture_rows_per_update` | 8 | 1回のネイティブ取得上限（1～16） |
| `keys` | WASD/Space/Shift/Ctrl | 各InputKeyを数値で変更可能 |

インタラクション有効化は通常の入力を通す設定です。サーバーのリーチ判定を変更したり、
カメラの位置から遠隔操作するためにPlayerを移動させたりしません。
クライアントが受信していない範囲のPlayerは表示できません。
簡易枠はRuntimeIDで追跡し、約2秒観測できなければ消去します。保存位置・名前表示はありません。

## ランタイム

Runtime **4.1.0**を内蔵します。互換性のある既存所有者へ接続できます。
配布セットの`BedrockHookRuntime.dll`をMod DLLと同じフォルダに置くと、
新しいSDKのModは初回API利用前にそれを優先起動します。
既存Runtime 4.0のDLLも、先に起動したABI 4の上位所有者へ接続できます。
既に設置したHookの所有者は途中交換しないため、更新時はMinecraftを再起動してください。
Runtime 3所有者は描画point 13を提供しないので、Freecamは起動できません。

旧既定キーV（86）の設定は、初回にM（77）へ移行して`config_revision:2`を保存します。
移行後は`toggle_key`を自由に変更できます。
`Freecam_GetStatus`（0:未起動、1:起動中、2:待機/稼働、3:停止、4:失敗）、
`Freecam_IsActive`、`Freecam_Stop`をエクスポートします。
ネイティブカメラ/ローカルPlayerの観測HookはDLL寿命中保持し、停止後は元処理のみ転送します。

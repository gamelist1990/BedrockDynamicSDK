# Freecam (Windows x64)

現行版は **visual revision9** です。更新内容と確認ログは末尾のrevision9節を参照してください。

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
| `spectator_mode` | false | Freecam中だけ、本物のspectatorで観測した地形カリング方式5を使用（描画結果は実機確認待ち） |
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

## スペクテイターの地形カリング（visual revision 6）

本物のスペクテイターとの比較で、通常時はmode3/Vtable0xE8E3B10、スペクテイター時はmode5/Vtable0xE8DFE00を観測しました。
この組み合わせをEXEの生成処理・非同期更新関数と照合しました。
revision5では本体が毎フレームmode3を再指定し、約5秒で1600回の再生成が発生していました。revision6では主カメラの本体関数へのmode3要求を、Freecam中だけ入口でmode5へ置き換えます。すでにmode5なら本体の処理で再生成を避けます。以前のmode0は使用しません。
これはrevision6のカリング切替です。revision7では末尾の説明どおり、内側面の描画呼び出しも対象にしています。GameTypeは変更しません。

解除・被弾・停止時は、同一ワールド・同一カメラで保存した方式へ戻します。
本体側のゲームモード変更や別のカリング方式への変更を観測した場合は、保存値で上書きせず本体へ任せます。
本体の非同期カリングが実行中なら次のフレームで再試行し、待機ループは回しません。

ログ `Freecam spectator culler` の `mode=5 pending=0 faults=0` が方式切替の確認値です。視認成功そのものは意味しません。
`native_requests`/`rewritten`は本体の要求数/置換数、`changed`は実際の切替回数です。要求が増えても`changed`が増え続けないことを確認してください。1回の起動で8回切り替わった場合は`churn_guard=1`として今回の上書きを停止します。
`restored`は保存方式へ戻った回数です。`visual_revision=6`が今回の版です。

古いDLLのHookが残るため、Minecraftを完全終了してから差し替えてください。
`full_bright`は独立したガンマ設定です。Runtimeが`actual owner=4.3.0 ABI=4`なら互換Runtimeへ接続しています。
詳細は[解析記録](../../docs/freecam-native.md)を参照してください。

## スペクテイター比較用の記録

`visibility_diagnostics: true`で、通常視点でもFreecam中でも0.5秒ごとにカメラの候補フィールド・カリング方式・Actor GameTypeを記録します。
出力はDocuments/PEXData/Freecam/visibility.jsonlです。revision6は本体のカメラ描画から読み取った`raw_view_flags_7c_88`、`camera_view_flags_valid`、`draw_inside_cubes`も記録します。読み取りだけで描画フラグは変更しません。既定false。画面表示やゲームモードを変更しません。
ScanClientの既存監視はEXEの書き込み可能セクションが対象で、カメラのヒープを追わないため、この補助記録も必要です。
比較時はfull_bright=falseとし、同一ローカルワールド・同一描画方式で、通常の地上→本物のspectator地上→spectator地中→通常に戻してFreecam地中を、それぞれ10秒ずつ記録してください。
GameType=-1やvalid_camera_read=falseは読み取り不成立です。候補フィールドの意味・視認効果はこの記録だけで確定しません。


## visual revision7: 視点を囲むブロックの内側面

ブロックのフチでだけ外が見える報告に対応し、カリングとは別の内側面描画を扱います。
本体はViewRenderData+0x80で内側面の描画を制御します。生成側はNoClipに相当する能力の値を参照します。
revision7はFreecam中の主カメラ描画内、かつ呼出元0x4689093に限って内側面の描画を省略します。
共有のViewRenderObject、Playerの能力、ゲームモードは書き換えません。解除時は本体の描画をそのまま通します。
通常の地形やブロックの面を一括で透明化する処理ではありません。以前のpermission hookは使いません。

`Freecam inside-cube render`の`camera_calls`/`cube_calls`/`cube_skipped`は5秒ごとの件数です。
主カメラの対象呼び出しを省略した場合にだけ`cube_skipped`が増えます。0なら対象へ届いていません。
`visibility.jsonl`の`draw_inside_cubes`は本体が生成した元のフラグのままです。省略件数とは別に見てください。
地下表示が本物のスペクテイターと一致するかは実機未確認です。Minecraftを完全終了してDLLを差し替えてください。


## 現行 visual revision8: 本体が生成する描画データ

revision7は内側面の描画省略が実行されても改善しなかったため、そのHookは廃止しました。
追加の比較記録で、本物のspectatorは`draw_inside_cubes=false`、通常モードのFreecamは`true`と確認できました。
revision8は本体の`createViewRenderObject`が作成した直後の戻り値へ、Freecam中の主カメラに限り`false`を設定します。
呼び出し元が受け取る前の新しい出力バッファだけが対象です。ゲームモードやPlayerの能力、既に共有済みのViewRenderObjectは書き換えません。
解除後に生成するデータは本体の値を使います。追加の全件走査やworkerによるネイティブ読み書きはありません。

`Freecam view factory`の`created`は対象カメラの生成件数、`changed`はフラグ変更件数、`failed`は検証/書込失敗です。
`visibility.jsonl`は今回の版で`visual_revision=8`。Freecam中に`draw_inside_cubes=false`となって初めて消費側まで反映されたと確認できます。
本物のスペクテイターと地下表示が一致するかは今回も実機未確認です。Minecraftを完全終了して差し替えてください。


## 現行 visual revision9: RenderDragonのInsideBlockEffect

revision8はフラグの反映・解除後の復帰が記録で確認できても、ユーザーの画面では改善しませんでした。フラグ一致を表示成功とは扱いません。
別のRenderDragon経路に`InsideBlockEffect`専用の抽出・描画対象が見つかりました。
revision9はその型の呼び出しテーブルと2つの正確な呼出元が一致した場合に限り、Freecam中の抽出判定をfalseへ変更して描画対象から除外します。
同じ判定関数を使う他の型・呼出元は本体の結果を維持します。地形メッシュの透明化・ゲームモード/能力の書き換えはしません。

`Freecam RenderDragon inside effect`の`calls`は対象型の到達数、`eligible`は本体の描画許可数、`skipped`は今回除外した数です。
`other_caller`は未対応呼出元のため本体へ任せた数で、`caller_rva`も記録します。通常のvisibility診断をOFFにしてもFreecam稼働中はこのログを出します。
`skipped>0`は対象の除外を確認する値であり、視覚的改善の保証ではありません。実機の到達性と地下表示は未確認です。
Minecraftを完全終了して差し替えてください。設定は引き続き`spectator_mode: true`です。

### SDK共通Camera API

移動計算は `bedrock::moveCamera`、AABB追従は `translateBounds`、位置・前位置・
速度・当たり判定の更新は `native_motion::apply` に共通化しました。
[World / Block / Camera API](../../docs/world-block-camera-api.md) を参照してください。
パケット固定と描画Hookは引き続きFreecam側で管理します。

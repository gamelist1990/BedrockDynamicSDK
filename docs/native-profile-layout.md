# Native profile organization

`data/<version>/windows-x64.json` は、ModではなくMinecraft側の機能を単位に整理します。
同じ関数・構造定義を複数のMod用セクションへ重複して記載しません。

## セクションの順序と役割

| セクション | 内容 |
| --- | --- |
| `schema`, `target` | スキーマ・ゲームバージョン・EXE指紋・PEレイアウト |
| `symbols`, `virtual_methods` | 本体関数・仮想メソッド |
| `native_keyboard` | ネイティブの入力経路 |
| `native_actors`, `native_items`, `native_player_identity` | Actor・アイテム・プレイヤー照合 |
| `native_world` | ブロック取得・配置・状態属性・ワールド高さ |
| `native_motion.state_vector` | 座標・前座標・速度・落下距離 |
| `native_packets.sender` | 送信経路 |
| `native_packets.motion` | 移動パケット |
| `native_packets.command_request` | コマンド要求パケット |
| `native_packets.title` | タイトル受信 |
| `native_client_commands` | コマンド登録・引数・factoryのABI |
| `native_gui` | GUI取得・メッセージ表示 |
| `native_camera` | カメラ・視点・明るさ・地形カリング |
| `native_world_render`, `native_world_events` | ワールド描画・ライフサイクル |

各nativeセクションは `schema` / `pointer_size` / `revision` / `status` /
`runtime_verified` などのメタデータを先に記載し、機能定義、その後に根拠・注記を並べます。
存在しないメタデータを整形のために推測して補完することはしません。
RVAは `0x` + 大文字16進数、シグネチャ・エントリ検査は大文字のバイト表記です。
オフセットなど、既存の読み込み側が型を区別する項目のJSON型は保持します。

## 整理の範囲

旧Mod固有セクションは廃止し、現在のSDK・サンプルの参照先を更新しています。
旧キャッシュには新しいセクションがないため、サンプルは埋め込み定義へ切り替え、
Resolverで実際のゲームEXEとの一致を確認します。
すでに配布した旧DLLは再ビルドが必要です。旧DLLの埋め込み定義まで変更されるわけではありません。

RVA、シグネチャ、ABI、検証状態の意味は今回の整理では変更していません。
candidateをverifiedへ昇格させる整形処理ではありません。

## 解析根拠の所在

プロファイルのREA/Ghidra evidence IDは保持しています。詳細な解析履歴・元の比較対象は
以下の解析メモにあります。文書名は既存の調査履歴を維持していますが、配布JSON内は
SDKの機能名で統一しています。

- [カメラ・カリングの解析とvisibility.jsonlの指紋](freecam-native.md)
- [ブロック取得・状態属性・配置の解析](speedbuilder-native.md)
- [GUI・ワールド高さ・コマンド登録の解析](top-native.md)

プロファイル中の「visibility.jsonl SHA256」の記述は、上記カメラ解析メモに保存した
元記録を参照します。サーバー受理や未確認の描画経路を検証済みと解釈しないでください。

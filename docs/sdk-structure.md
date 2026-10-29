# SDKと定義ファイルの構造

## C++公開ヘッダー

| フォルダ | 内容 |
| --- | --- |
| `api/` | GameApi、Actor/World等の公開ファサード、値型 |
| `enums/` | PacketId、コマンドOrigin、入力、Actor、機能、更新方針等のEnum |
| `packets/` | PacketApi、wire読書き、protocol codec、PlayerList名簿 |
| `native/` | MinecraftのABI、ゲームスレッドでの読取・書込、構造体検査 |
| `profiles/` | 定義の選択・取得・分割ファイルの結合・Resolver |
| `runtime/` | 共有Hook、クライアント観測、ロード済みDLL識別 |
| `commands/` | コマンド登録・キュー・要求と応答 |
| `render/` | GUI、オーバーレイ、描画用の公開API |
| `analysis/` | CheckSig auto-devの探索と実行管理 |
| `logging/` | ログ・コンソール |
| `detail/` | SDK内部の共有処理。Modから直接使用しない |

旧includeの転送ヘッダーは削除しました。SDKと各Modは上表の新パスを使用します。
新しいModでは例えば`<bedrock/packets/packet_api.hpp>`を使用します。
クラス内部だけで使う状態Enumは、そのクラス内部に置きます。

## data

```text
data/
  manifest.json                        # 対応EXE、版別manifestのパス・SHA、既定版
  <game-version>/windows-x64/
    manifest.json                      # target、protocol参照、partsのパス・SHA
    classes/
      Actor.json
      Camera.json
      LevelRenderer.json
      PacketRead.json
      RuntimeActorList.json
      ...
    analysis/                          # 過去の調査・観測。通常のSDK起動では取得しない
    metadata/                          # 更新方針等
  protocols/<protocol-version>/
    protocol.json                      # upstreamの完全なJSONをそのまま保存
    version.json
  policies/movement.json               # SDKが抑制する入力の名前。bit番号はprotocol由来
  policies/packet_names.json           # 公開Enum名とprotocolの名前の対応
```

クラスファイルの`symbols`はMinecraft関数名、`layouts`はそのクラスの配置・ABIです。
別クラスの関数を使用する際は`{"symbol":"Class::function"}`で参照します。
同じ関数やlayoutを複数ファイルに重複定義すると結合時に失敗します。
ModにRVA・オフセット・vtable位置を追加しません。

### 更新手順

1. 対象クラスのJSONを編集する。
2. `python tools/profile_parts.py refresh data/<game-version>/windows-x64/manifest.json`
3. `bedrock-symbols index data --output data/manifest.json`で全体indexを更新する。
4. GitHub Runnerで対象版をビルドする。

`tools/profile_parts.py assemble <manifest> --output profile.json`で、CheckSigへ明示的に渡す調査用JSONを出力できます。通常の配布とビルドはmanifestとクラス別JSONを直接使い、旧windows-x64.jsonを生成しません。
SDKキャッシュ・CheckSigの結果JSONも単一JSONを使用できます。

## ビルド時の埋め込み

CMakeの`BEDROCK_GAME_VERSION`で版を選択します。空の場合は`data/manifest.json`の`default_version`を使います。
`tools/configure_sdk_data.py`が版別manifestを読み、選択したクラス定義とプロトコルをビルドディレクトリへ生成します。
生成ヘッダーは`bedrock/generated/symbols.hpp`、`protocol.hpp`、`packet_ids.inc`、`profile_sources.hpp`です。
版名付きのC++プロトコルファイルを追加する必要はありません。
プロトコルの取得元commitとSHAは版別manifestの`protocol`へ記録します。

## 公開APIの分割

- `api/geometry.hpp`：Vec3・ActorBounds・ActorRotation。
- `api/item_stack.hpp` / `server_address.hpp`：所有する値スナップショット。
- `api/game_access.hpp`：ゲーム固有アダプターのインターフェース。
- `api/game_event_registry.hpp`：イベントチャンネルの一覧。
- `api/actor_samples.hpp`：ワーカーへ渡せる追跡AABBフレーム。
- `native/actor_components.hpp`：版別hash/offsetを使うゲームtickのコンポーネント取得。
- `native/player_bounds_tracker.hpp`：Actorの世代・runtime IDを確認し、時間上限付きでPlayerのAABBを追跡。
- `packets/title_packets.hpp`：wire/旧native両方を覆うタイトル購読。

`game_api.hpp`はこれらを組み合わせるファサードです。コピー済み座標だけの処理はgeometry.hppを直接includeできます。

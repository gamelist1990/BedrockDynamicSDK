# BedrockDynamicSDK

Minecraft Bedrock Windows x64 用の C++ SDK と Mod サンプル。

## 対象バージョン

| バージョン | 状態 |
|---|---|
| 1.26.65.1 | 現行開発・ビルド対象 |
| 1.26.60.5 | 正式リリース。全 10 Mod 動作確認済み |
| 1.26.52.3 | レガシー。新 SDK 構造でビルド可能だが更新対象外 |

各版のサポート情報は `data/<version>/README.md` に記載している。

## リポジトリ構成

| パス | 内容 |
|---|---|
| `sdk/` | 公開 C++ SDK（Stage A）。CMake は `bedrock::sdk`、入口は `sdk/include/bedrock/sdk.hpp` |
| `include/bedrock/` | SDK と第一者 Mod 用の内部基盤（`detail/`, `native/`, `packets/`, `commands/` など） |
| `data/` | 版別プロファイル、`manifest.json`、protocol / vanilla データ |
| `exsample/` | DynamicClient（Stage A 参照 Mod）、DevClient、各種サンプル Mod |
| `docs/` | 設計メモ、API ガイド、更新手順 |
| `site/` | GitHub Pages 用の開発ノート索引（XML + index.html） |

外部の C++ Mod は `bedrock::sdk` をリンクし、`<bedrock/sdk.hpp>` を使う。
旧ファサードは内部に移動しており、旧インクルードパスや互換ヘッダーは提供しない。

## Stage B（プラグインシステム）の状況

Stage B コア実装は完了している。

- quickjs-ng 内蔵のプラグインホスト
- `mods/` の自動走査と manifest 検証
- TypeScript のロード時トランスパイル
- `node_modules` 対応の `require`
- `/mods list` / `/mods reload` / `/mods path` コマンド
- 1 JS Mod = 1 `sdk::Mod`

仕上げラウンド2も完了している。

- `settings()` / `keybinds` / `brightness.setGamma` の実 SDK への翻訳
- `world.afterEvents.playerSpawn` の JS 配信
- コンポーネント / dimension / block / `ItemStack` バインディング
- `WorldBackend` の per-Mod 分離

検証: ホスト単体テスト 96 チェック全緑、Stage A ホストテスト緑、Windows x64 クロスビルド成功。BOL 実地検証は未実施。

証跡: `~/Documents/PEXData/BedrockDynamicSDK-stage-b-evidence/validation.md`、`validation-round2.md`。

## ビルド

Linux から Windows x64 DLL をクロスビルドする。

```bash
./build-linux.sh
```

- デフォルトで最新版（`LATEST=1.26.65.1`）向けにすべての Mod をビルドする。
- `./build-linux.sh --stage-a` で SDK、DynamicClient、CheckSig のみをビルドする。
- `./build-linux.sh --legacy` で前版（`LEGACY=1.26.60.5`）の regression セットもビルドする。

CI は `.github/workflows/build-mods.yml` を参照。

## ドキュメント

- 公開 C++ API: [Stage A SDK 設計メモと最小例](docs/stage-a-sdk-design.md)
- 旧ネイティブ API（内部実装の参考資料）: https://gamelist1990.github.io/BedrockDynamicSDK/api/
- 開発ノート索引: https://gamelist1990.github.io/BedrockDynamicSDK/
- シグネチャ更新ガイド: [docs/signature-update-guide.md](docs/signature-update-guide.md)
- 詳細な版更新手順: [docs/version-update-workflow.md](docs/version-update-workflow.md)
- プロファイル保守: [docs/profile-maintenance.md](docs/profile-maintenance.md)
- SDK 構造: [docs/sdk-structure.md](docs/sdk-structure.md)

## データ更新手順

1. `data/<version>/windows-x64/classes/*.json` を編集する。
2. `python tools/profile_parts.py refresh data/<version>/windows-x64/manifest.json`
3. `bedrock-symbols index data --output data/manifest.json`
4. GitHub Runner または `build-linux.sh` で対象版をビルドし、BedrockOnLinux で動作確認する。

Mod に RVA・offset・vtable 位置を追加しない。詳細は [docs/profile-maintenance.md](docs/profile-maintenance.md) を参照。

# BedrockDynamicSDK

Minecraft Bedrock（Windows x64）用モッディング SDK。C++ ネイティブ Mod と、TypeScript で書ける ScriptClient Mod の両方に対応する。

## 対象バージョン

ビルド対象は最新版のみ: **1.26.65.1**

過去版のプロファイルデータは `data/` に残っている（各版の情報は `data/<version>/README.md`）。

## できること

- **C++ SDK**（`bedrock::sdk`）: ワールド / プレイヤー / パケット / レンダリングなどの公開 API。入口は `<bedrock/sdk.hpp>`
- **ScriptClient**: TypeScript Mod を `mods/` フォルダから自動ロード。`/mods list|reload`、Mod Manager、ドラッグ&ドロップ導入対応
- **クライアント診断 API**: `player.fps / ping / cps / device`（C++ / TS 両対応）

## ビルド

Linux から Windows x64 DLL をクロスビルドする。成果物は `dist/<version>/` に出る。

```bash
./build-linux.sh            # 全 Mod（最新版向け）
./build-linux.sh --stage-a  # SDK + DynamicClient + CheckSig のみ
```

## リポジトリ構成

| パス | 内容 |
|---|---|
| `sdk/` | 公開 C++ SDK |
| `include/bedrock/` | 内部基盤（SDK と同梱 Mod 用） |
| `exsample/` | 同梱 Mod とサンプル（DynamicClient、ScriptClient など） |
| `data/` | 版別プロファイル・vanilla データ |
| `docs/` | 設計メモ・API ガイド |

## ドキュメント

- C++ API と最小例: [docs/stage-a-sdk-design.md](docs/stage-a-sdk-design.md)
- ScriptClient（TS Mod）: [docs/scriptclient.md](docs/scriptclient.md)
- SDK 構造: [docs/sdk-structure.md](docs/sdk-structure.md)
- プロファイル保守: [docs/profile-maintenance.md](docs/profile-maintenance.md)
- 開発ノート: https://gamelist1990.github.io/BedrockDynamicSDK/

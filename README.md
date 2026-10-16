# BedrockDynamicSDK

Windows x64 Minecraft Bedrock向けの動的シンボルデータと、単一DLLへ静的リンクできるC++20 SDKライブラリを提供します。
**サンプルクライアントはSDK本体から分離した独立プロジェクト**です。

現在の実データ：**Bedrock 1.26.52.3**。Minecraft本体の実行ファイルはGitHubに含めません。

## サンプル: AutoSprint

AutoSprintは[独立リポジトリ BedrockAutoSprint](https://github.com/gamelist1990/BedrockAutoSprint)のサンプルModです。GUIやほかの機能はありません。
ビルド時にCMake FetchContentで**GitHubのBedrockDynamicSDKを取得**し、
SDKヘッダー(`include/`)と `bedrock_resolver` を静的リンクします。
実行時はローカルのSDK本体・ヘッダーを必要とせず、**DLL 1個**だけで動作します。
[exsample/AutoSprint](exsample/AutoSprint)は独立リポジトリを指すサブモジュールです。

- ゲームのウィンドウがアクティブ、カーソル非表示（ゲームプレイ中）、前進キー `W` を押している場合、`左Ctrl` の通常入力を自動的に保持
- 前進をやめる、ゲームが非アクティブになる、メニューを開く、Shiftを押すと自動的に解除
- コントローラーとカスタムキー割り当ては未対応。既定の `W` = 前進、`左Ctrl` = スプリントの操作環境向け
- メモリの移動速度を書き換える、スプリント状態フラグを直書きするなどの動作は**行わない**

### GitHubからの自動バージョン切替

```text
BedrockAutoSprint.dll が起動
    ↓
現在の Minecraft.Windows.exe のファイル/.text SHA256・PE構造を計測
    ↓
HTTPS: data/manifest.json を取得
    ↓
対応するバージョンを完全一致で特定
    ↓
data/<version>/windows-x64.json を取得
    ↓
ファイルSHA256とゲーム本体のハッシュ・PE構造を検証
    ↓
入力処理のシンボルのアドレスを検証 → AutoSprintを有効化
```

- **未対応バージョン・HTTPエラー・ハッシュ不一致は必ずスルー。** Modは何も操作しません。
- DLLのバイナリは**そのまま**で、GitHub上に対応するバージョンのJSONを追加するだけで、同じ入力処理の仕組みを使える新バージョンを認識できます。
- サンプルはWindowsの通常キー入力API（`SendInput`）を使うため、Minecraft内部のフィールドや不確かな関数ABIを実行しません。今後のMinecraftバージョンで仕様が変わった場合はDLL修正が必要になる可能性があります。
- HTTPSとmanifest記載のSHA256でデータの整合性を検査します。GitHubリポジトリの権限を持つ管理者がデータを更新する仕組みであり、独立した公開鍵署名はまだ未実装です。
- オフラインでは自動有効化せず、入力の状態を変更しません。

注：`BedrockAutoSprint.dll` は通常のWindows x64 DLLです。**これ自体に注入機能はありません**。すでに利用している正規のDLLローダーなどでロードする構成です。ロード後、API `AutoSprint_GetStatus()` で状態を確認できます。ゲームを終了するまでDLLはメモリからアンロードされません。

## バージョン別データは単一JSON

```text
data/
├── manifest.json
└── 1.26.52.3/
    └── windows-x64.json
```

`windows-x64.json` に統合：

| フィールド | 内容 |
|---|---|
| `target` | Windows x64のPEハッシュ・バージョン・.text情報 |
| `symbols` | 40件の関数RVA/AOB候補と出典 |
| `analysis.rtti` | 1,879件のRTTI型名 |
| `analysis.latite_compatibility` | Latiteの旧54シグネチャーの照合記録 |
| `analysis.layout_hints` | 旧SDKのフィールド/vtable配置の**未検証情報** |

`layout_hints` の値は実行時に使用しません。単に名前とシグネチャーが一致しても、対応するC++ ABIまで検証できたことにはなりません。

## データの更新と生成

Python 3.12以上 / [uv](https://docs.astral.sh/uv/) を使用。

```bash
uv sync --extra dev
git clone --depth 1 https://github.com/LatiteClient/Latite.git third_party/latite-reference

uv run bedrock-symbols client-profile /path/to/Minecraft.Windows.exe \
  --latite-addresses third_party/latite-reference/src/mc/Addresses.h \
  --latite-repo third_party/latite-reference \
  --version 1.26.52.3 --output data/1.26.52.3/windows-x64.json

uv run bedrock-symbols index data --output data/manifest.json
```

`client-profile --latite-repo` で関数・RTTI・照合結果・SDKレイアウト参考情報を**最初から単一JSONへ自動生成**します。新バージョンへ移植した関数は候補として分類され、勝手にABI検証済みにはしません。

Bedrock LensのSQLiteを利用する `lens-import`、既存シグネチャーを別ビルドで探す `port` なども利用できます。詳細は `tools/bedrock_symbols/cli.py` を参照。

## C++ライブラリ / DLL

- `bedrock_resolver`：C++20静的ライブラリ。EXEの身元確認、RVA/AOB検査、GitHubからのHTTPSダウンロード・検証を含みます。
- `BedrockAutoSprint.dll`：別リポジトリでビルドするサンプル。SDKに実装を含めません。
- `BedrockClientLocator.dll`：実機未検証のClientInstance捕捉用実験DLL。
- `BedrockSymbolProbe.dll`：メモリ読み取り専用の関数アドレス検査。
- `include/bedrock/keyboard.hpp`：ウィンドウメッセージ用キーボード観測API。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
uv run --extra dev pytest -q
```

SDK側のWindows/MSVC CIは `BedrockClientLocator.dll` と `BedrockSymbolProbe.dll` をビルドします。
**`BedrockAutoSprint.dll` は独立したBedrockAutoSprintリポジトリのCIでビルド**します。
LinuxでのC++テストとWindowsの模擬Hookテストも実行します。
**Minecraft本体への実機Inject試験は未実施**です。

### SDKをほかのModから使用する

CMakeの `FetchContent` でGitHubからSDKを取り込み、`bedrock_resolver` にリンクします。
SDKがサブプロジェクトとして読み込まれた場合は `BEDROCK_SDK_BUILD_TOOLS=OFF` が既定になり、
診断DLL・テストを自動ビルドしません。
CMakeサンプルは [BedrockAutoSprint](https://github.com/gamelist1990/BedrockAutoSprint) を参照。

## 技術的な注意

- ネットワーク上のシンボルデータの追加は、関数の呼び出し規約やメンバーレイアウト変更への万能な互換レイヤーではありません。
- 独自バージョン番号だけで判断せず、実際にロードされたPEのハッシュとセクションサイズで照合します。
- 解析ファイルに `status=candidate` とある関数は、呼び出して安全であることを保証しません。
- データの生成ソースは [Latite](https://github.com/LatiteClient/Latite) と [Bedrock Lens](https://github.com/bedrock-mc/lens) などを参考にしています。
- Microsoft配布物そのものをリポジトリ内で再配布しない方針です。


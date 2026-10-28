# BedrockDynamicSDK

Minecraft Bedrock **Windows x64** 向けのC++ SDKとMod DLLです。
ゲームのバージョンに対応するプロファイルとDLLを使用してください。

## ダウンロード

[バージョン別リリース](https://github.com/gamelist1990/BedrockDynamicSDK/releases) からModのZIPを取得します。
同じゲームバージョンの更新は、同じリリース内のZIPを上書きします。

| ゲーム | 定義・対応範囲 |
| --- | --- |
| 1.26.52.3 | 既存のネイティブアダプター定義。各ModのREADMEも参照 |
| 1.26.60.5 | 43定義の入口確認済み（重複を統合して40関数）。Keyboard::feedはHookと値の取得を確認済み。その他のABI・オブジェクトレイアウトには調査中の項目あり |

**1.26.60.5の全Modが実機動作確認済みという意味ではありません。**
Topなどが必要とするGUI・ローカル整数コマンド用CommandRegistryのレイアウトは未完成です。
起動ログの「missing Minecraft layout」に表示されたクラスの調査が必要です。

## CheckSig：JSONを置いて調査

1. `CheckSig.dll` を対象ゲームへロードします。
2. `Documents/PEXData/CheckSig/commands/` に `windows-x64.json` を置きます。
3. 自動detectの結果を `result/commands/windows-x64.result.json` で確認します。

調査用JSONではscan・read・probe・hook・unhook・旧定義からのportを指定できます。
入力ごとに別の結果ファイルを出力し、Hookの観測値も更新します。

- [CheckSigの使い方](exsample/CheckSig/README_ja.md)
- [調査JSONの書き方](docs/checksig-jobs.md)
- [プロファイルの構造](docs/native-profile-layout.md)

## Mod

`BlockOutline` / `Freecam` / `SpeedBuilder` / `Top` / `HitBox` / `TabList` /
`Zoom` / `AutoSprint` / `GuiDemo` / `ScanClient` / `CheckSig` / `Fake`

[Fake](exsample/Fake/README_ja.md) は1.26.60.5向けの調査版です。
`/fake メッセージ` で `/me メッセージ` をOrigin=5で要求し、
`/fake --origin 0 メッセージ` でOriginを指定できます。
[共有CommandRequests API](docs/command-requests.md) は `.then(...)` で応答を扱います。

## GitHub CIでビルド

[Build DLLs and releases](https://github.com/gamelist1990/BedrockDynamicSDK/actions/workflows/build-mods.yml)
の **Run workflow** で指定します。

- `version`：埋め込むゲームバージョン
- `mods`：`all` または `CheckSig,Fake` のようなMod名
- `release`：有効にすると、そのバージョンのリリースへZIPを上書き

ビルド・SDKチェック・リリースはこのCIに統合しています。Pagesは別CIです。
DLLはGitHubのWindows RunnerでMSVC x64 Releaseとして作成します。

## SDKを使う

- [APIドキュメント](https://gamelist1990.github.io/BedrockDynamicSDK/)
- [サンプルMod](exsample/)
- プロファイル：`data/<ゲームバージョン>/windows-x64.json`

プロファイルはGitHub取得とローカル取得に対応します。
SHA-256・PE範囲・入口ガードを照合し、別バージョンのアドレスを代用しません。

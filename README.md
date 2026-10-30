# BedrockDynamicSDK

Minecraft Bedrock **Windows x64** 向けのC++ SDKとMod DLLです。
ゲームのバージョンに対応するプロファイルとDLLを使用してください。

## ダウンロード

[バージョン別リリース](https://github.com/gamelist1990/BedrockDynamicSDK/releases) からModのZIPを取得します。
同じゲームバージョンの更新は、同じリリース内のZIPを上書きします。

| ゲーム | 対応状況 |
| --- | --- |
| 1.26.52.3 | 既存アダプターを新しいSDK構造でビルド |
| 1.26.60.5 | Freecam・Zoom・Fakeの正常動作をユーザー確認済み。使用経路をverifiedへ反映 |

[1.26.60.5の確認済み関数・入口特定・未検証項目](data/1.26.60.5/README.md)をチェックリストで管理しています。

### 今回の変更

- **TabList**：Packet ID別の仮想デコーダーを共有Hookし、PlayerList・skinをSDKの専用ワーカーで解析。新経路の受信と画面表示は実機確認待ちです。
- **SDK**：公開型・GameAccess・イベント・Actor component・Player bounds・TitlePacketsを分割。旧includeの転送ヘッダーを削除。
- **CheckSig**：commands内のJSONでメモリ範囲を読み、実測バイト列・保護属性・読めなかった理由を入力別resultへ出力。
- **定義**：クラス別JSONとmanifestを配布。旧windows-x64.jsonの自動生成とScanClientを廃止。
- **未確認**：HitBoxの実表示、SpeedBuilderの新版設置処理、Freecamの未解決culler/view経路等はチェックリストを参照。

**共有runtimeは4.8.0。併用するModを同じリリースへ更新し、Minecraftを完全終了して再起動してください。**
TabListはDLLロード後にワールドへ入り直すと、初回のPlayerListを受信できます。

## SDKと版別定義の更新

ネイティブのRVA・シグネチャ・offset・ABIはSDKと`data/<version>/windows-x64/classes/*.json`で管理します。参照パスとSHA256は`manifest.json`へ記録します。
PlayerList・名前/UUID・顔画像・LocalPlayer取得はSDK APIを使い、Modへネイティブ配置を追加しません。
関数は共通の識別名で参照し、版別定義を一箇所で更新します。
[探索順序・更新手順・ABIの制約・既存処理の移行残](docs/profile-maintenance.md)を参照してください。

## CheckSig：JSONを置いて調査

1. `CheckSig.dll` を対象ゲームへロードします。
2. `Documents/PEXData/CheckSig/commands/` に `profile.json` を置きます。
3. 自動detectの結果を `result/commands/profile.result.json` で確認します。

調査用JSONではscan・read・範囲memory・probe・hook・unhook・旧定義からのportを指定できます。
入力ごとに別の結果ファイルを出力し、Hookの観測値も更新します。

- [CheckSigの使い方](exsample/CheckSig/README_ja.md)
- [1.26.60.5・全Modの残作業と調査セット](docs/checksig-126605.md)
- [調査JSONの書き方](docs/checksig-jobs.md)
- [プロファイルの構造](docs/native-profile-layout.md)

## CheckSig auto-dev（実験）

`Documents/PEXData/CheckSig/status.json`の`"auto-dev"`を`true`にすると、SDKの探索処理が現在のEXEから定義候補を生成します。
結果は`auto-dev/profile.json`と`report.json` / `report.txt`。未解決項目には理由を残し、自動適用はしません。
[使い方・探索条件・生成できない項目](docs/checksig-auto-dev.md)を参照してください。

## Mod

`Freecam` / `SpeedBuilder` / `Top` / `HitBox` / `TabList` /
`Zoom` / `AutoSprint` / `CheckSig` / `Fake` / `BetterResourcePackManager`

[BetterResourcePackManager v21](exsample/BetterResourcePackManager/README_ja.md) は1.26.60.5向けです。設定を開いたままグローバルリソースを適用し、カスタムブロックのUV・モデルの描画参照を更新します。既存v21の動作はユーザー確認済みです。まれな未読み込みに対する完了待ち・再試行の追加修正は、自動テスト済み・実機確認待ちとして区別しています。[SDK API保守ガイド](docs/resource-packs.md)も参照してください。

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
- プロファイル：`data/<ゲームバージョン>/windows-x64/manifest.json`

プロファイルはGitHub取得とローカル取得に対応します。
SHA-256・PE範囲・入口ガードを照合し、別バージョンのアドレスを代用しません。

## SDKと定義の構成

[フォルダ構造と更新手順](docs/sdk-structure.md) · [パケットAPI](docs/packet-events.md)

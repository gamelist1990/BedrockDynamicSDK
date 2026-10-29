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

### 1.26.60.5の今回の更新

FreecamのInsideBlockEffectに新EXEの対象型・呼出箇所を追加。飛行中の対象エフェクトだけを除外し、実機の見た目は確認待ちです。

- HitBox: LocalPlayer取得を正確な関数へ変更。新EXEの描画定義とPlayerList位置を追加。対象取得・描画は更新後の実機確認待ち。
- SpeedBuilder: 共通`Packet::read` HookとID 88でタイトルを観測。現時点ではタイトル観測モードで、自動記録・設置は未対応。ホットバー選択・配置状態計算・HitResultのABI確認が必要。
- TabList: 受領ログでは起動とTab入力のみ確認。GUIは表示されなかったとの報告を受領。旧固定PlayerList位置を版別定義へ置換し、空一覧の診断を追加。更新後の表示確認待ち。
- 共有Hook runtimeは4.7.0。同時に使うMod DLLを同じリリースへ更新し、Minecraftを完全終了してから再起動してください。古いDLLが先にロードされると、そのruntimeが残ります。

## SDKと版別定義の更新

ネイティブのRVA・シグネチャ・offset・ABIはSDKと`data/<version>/windows-x64.json`で管理します。
PlayerList・名前/UUID・顔画像・LocalPlayer取得はSDK APIを使い、Modへネイティブ配置を追加しません。
関数は共通の識別名で参照し、版別定義を一箇所で更新します。
[探索順序・更新手順・ABIの制約・既存処理の移行残](docs/profile-maintenance.md)を参照してください。

## CheckSig：JSONを置いて調査

1. `CheckSig.dll` を対象ゲームへロードします。
2. `Documents/PEXData/CheckSig/commands/` に `windows-x64.json` を置きます。
3. 自動detectの結果を `result/commands/windows-x64.result.json` で確認します。

調査用JSONではscan・read・probe・hook・unhook・旧定義からのportを指定できます。
入力ごとに別の結果ファイルを出力し、Hookの観測値も更新します。

- [CheckSigの使い方](exsample/CheckSig/README_ja.md)
- [1.26.60.5・全Modの残作業と調査セット](docs/checksig-126605.md)
- [調査JSONの書き方](docs/checksig-jobs.md)
- [プロファイルの構造](docs/native-profile-layout.md)

## CheckSig auto-dev（実験）

`Documents/PEXData/CheckSig/status.json`の`"auto-dev"`を`true`にすると、SDKの探索処理が現在のEXEから定義候補を生成します。
結果は`auto-dev/windows-x64.json`と`report.json` / `report.txt`。未解決項目には理由を残し、自動適用はしません。
[使い方・探索条件・生成できない項目](docs/checksig-auto-dev.md)を参照してください。

## Mod

`Freecam` / `SpeedBuilder` / `Top` / `HitBox` / `TabList` /
`Zoom` / `AutoSprint` / `ScanClient` / `CheckSig` / `Fake`

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

# BedrockDynamicSDK

Minecraft Bedrock **Windows x64** 向けのC++ SDKとMod DLLです。
ゲームのバージョンに対応するプロファイルとDLLを使用してください。

## ダウンロード

[バージョン別リリース](https://github.com/gamelist1990/BedrockDynamicSDK/releases) からModのZIPを取得します。
同じゲームバージョンの更新は、同じリリース内のZIPを上書きします。

**サポート方針：常に対象は最新版のみです。** 現在の対象は **1.26.60.5** で、全Modの対応と動作確認が完了し、正式リリースとして公開しています。
1.26.52.3 は旧版としてリリースを残しますが、今後のビルド・更新の対象外です。

| ゲーム | 対応状況 |
| --- | --- |
| 1.26.60.5 | **全10 Mod対応・動作確認済み（正式リリース）** |
| 1.26.52.3 | 旧版（レガシー）。新しいSDK構造でビルド済み。以後の更新対象外 |

[1.26.60.5の確認済み関数・入口特定・未検証項目](data/1.26.60.5/README.md)をチェックリストで管理しています。

**共有runtimeは4.8.0。併用するModを同じリリースへ更新し、Minecraftを完全終了して再起動してください。**
TabListはDLLロード後にワールドへ入り直すと、初回のPlayerListを受信できます。

## 1.26.60.5のMod対応状況

2026-11-13〜14に、自動テスト環境（BedrockOnLinux＋DLL注入）での無人検証と、外部サーバー（The Hive）・ローカルワールドでの実地検証で確認しました。

| Mod | 状況 | 確認内容 |
| --- | --- | --- |
| Freecam | 対応・実証済み | terrain culling／view factory の定義を解決。外部サーバーで wire PacketHook 確立、Mキーでの有効化、パケット位置固定、カメラ飛行、解除時の復元まで実証 |
| HitBox | 対応・実証済み | ワールド内で対象取得（native capture）、NameTag描画、実名表示を確認 |
| TabList | 対応・実証済み | ワールド内で PlayerList の受信・解析を確認（通信PlayerList経由） |
| SpeedBuilder | 対応・実証済み | 設置ABIを解決。native builder 起動、タイトル記録、範囲キャプチャ、ホットバー読取りを確認。再構築フェーズも自前BDSで capture→破壊→diff→拾得→ネイティブ設置（accepted）まで実証済み |
| Top | 対応・実証済み | `/top` の登録・実行、テレポート文脈の解決、安全ゲートのゲーム内表示まで実証。実移動はクライアント側の状態書換え方式のため、サーバー権威移動（BDS既定）のリモートではサーバーに巻き戻され定着しない（2026-11-14 自前BDSで確認） |
| Zoom | 対応・実証済み | ユーザー確認済み。自動検証でもワールド内動作（FOV取得）を確認 |
| AutoSprint | 対応・実証済み | ワールド内で動作を確認（HIDController経路） |
| AutoMine | 対応・実証済み | 新規Mod。`/mine <ブロック名>` で探索→移動→採掘を自前BDSで実証（連続採掘・長距離移動・AutoTool切替・NeedTool計画＆自動再開）。入力は MouseDevice::feed 経路、位置はAABB基準 |
| CheckSig | 対応・実証済み | 1.26.60.5の定義解決作業全体で使用し、ジョブ実行・auto-dev生成・portを確認 |
| Fake | 対応・実証済み | ユーザー確認済み。ワールド内で `/fake` のCommandRegistry登録を確認 |
| GlobalResourcePack | 対応・実証済み | ユーザー確認済み（v21）。自動検証でプロファイル一致・Hook導入・ワールド内有効を確認 |

### 1.26.60.5対応で完了した作業

- **ModのSDK移行**：Freecamのカメラvisual経路を `NativeCameraVisual`、HitBoxのEnTTレジストリ取得を `NativeActorRegistry`、SpeedBuilderのworld読取・設置を `NativeWorldAccess` へ移行。Mod側に版別数値を追加していません。
- **未解決定義の解決**：GuiDataレイアウト（`client_gui_offset=0x668`）、CommandRegistry一式（登録・検索・任意引数。`make_parameter` は単体関数が存在しないためSDK側でインライン構築へ変更）、SpeedBuilder設置ABI（hotbar選択・ヒット結果・buildBlock・配置状態）、Freecam terrain culling／view factory（selector・spectator vtable・create_view入口）。
- **自動検証**：HomePC上のBedrockOnLinuxで「起動→DLL注入→メニュー／ワールド内ログ判定」を無人化し、全Modを 1.26.60.5 実機相当環境で検証。Freecamの有効化は外部サーバーで移動パケットが流れる条件が必要なため、The Hiveで実証しました。

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
`Zoom` / `AutoSprint` / `CheckSig` / `Fake` / `GlobalResourcePack` / `AutoMine`

[global resourcepack v21](exsample/GlobalResourcePack/README_ja.md) は1.26.60.5向けです。設定を開いたままグローバルリソースを適用し、カスタムブロックのUV・モデルの描画参照を更新します。既存v21の動作はユーザー確認済みです。まれな未読み込みに対する完了待ち・再試行の追加修正は、自動テスト済み・実機確認待ちとして区別しています。[SDK API保守ガイド](docs/resource-packs.md)も参照してください。

[Fake](exsample/Fake/README_ja.md) は1.26.60.5向けです。1.26.60.5での動作を確認済みです。
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

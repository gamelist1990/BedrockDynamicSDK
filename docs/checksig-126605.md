# 1.26.60.5：全exsample Modの残りを調べる

対象はWindows x64 / Minecraft 1.26.60.5です。EXE SHA-256:
`988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`

## まずこのJSONだけ置く

修正版CheckSig.dllへ更新し、Minecraftを完全終了してから読み込み直します。
`status.json` の `checksig_features_revision: 3` が、この調査機能に対応するDLLの印です。
`1.26.60.5-all-mods.json` を次へ置いてください。

```text
Documents/PEXData/CheckSig/commands/1.26.60.5-all-mods.json
```

このJSONは対象プロファイルと全Modの要求一覧を内包するため、**単体で使用可能**です。
CommandOutputPacket::readの補助候補も追加しているため、調査用のsymbol数は配布プロファイルより1件多くなります。
自動Hookは行いません。次の結果を作成します。

```text
result/commands/1.26.60.5-all-mods.result.json
result/commands/1.26.60.5-all-mods.result.txt
result/remaining-work.json
result/remaining-work.txt
```

JSONの`support_report.mods`がModごとの状態、`implementation_work`がアダプターの残作業、`support_report.items`が関数・レイアウトごとの残作業です。
`definition_blockers`は定義不足・不一致など、`remaining_requirements`は観測やレビューも含む件数です。
Hookの観測が増えると結果を更新します。Mod全体の動作を確認したことにはしません。
通常のwindows-x64.jsonを置いた場合も、detectの結果に残作業を付けます。

## 主な残作業

| 対象 | 主な影響 | 必要な確認 |
| --- | --- | --- |
| ClientInstanceのワールドイベント | Top / Freecam / BlockOutline / SpeedBuilder / HitBox | onPlayerLoaded等の本当の関数入口、onCancelJoinGame、終了時のClientInstance位置 |
| LevelRenderer / Tessellator / Material | Freecam / BlockOutline / HitBox | renderLevel・vertex・color等の関数、描画オフセット、マテリアルグループ |
| LocalPlayerのプレイヤー一覧 | HitBox、TabListの追加表示 | 一覧オフセット、UUID/名前/UniqueID、要素サイズ |
| BlockSource / Block / BlockType / NBT | Top / BlockOutline / SpeedBuilder | 読取・名前・高さ・状態・衝突・寿命。定義があっても候補のままの項目あり |
| GameModeの設置 | SpeedBuilder | selectHotbar、HitResult、buildBlock、placement state、vtable |
| SubChunk / BlockStorage | BlockOutline | storage getterの実体とABI。現在enabled=false |
| PlayerAuthInputPacket / MovePlayerPacket | Freecam | vtable、位置・入力・vehicleのフィールド、送信経路、無効化時の復帰 |
| SetTitlePacket | SpeedBuilder | readのABI、vtable、タイトル/サブタイトルの値 |
| GuiData | Top | getGuiData、displayClientMessageとオブジェクトの関係 |
| CommandRegistryのローカル整数コマンド | Top | Command/parameter/overload/factory/整数型・parse rule。Fakeのメッセージ登録とは別 |
| Actor / EnTT / ItemStack / Camera / GameCore | 複数Mod | 定義済み候補の値・型・slot・ゲーム中の挙動 |
| CommandRequest / CommandOutput / Fake登録 | Fake | 候補一覧、要求Origin、UUID/requestId、成功/失敗/timeout、接続切替 |

Zoom / AutoSprintも最終的なゲーム内動作確認が必要です。
GuiDemoはMinecraftの関数定義に依存しませんが、DX11/DX12・入力・描画切替を確認します。
ScanClientとCheckSigも出力完結・スキャン・観測を確認します。
結果には12種類すべての確認シナリオを出します。

## Hookによる観測を進める

調査セットZIPはCheckSigフォルダへ展開できます。
`optional/` は自動実行されません。**まず1つだけcommandsへコピー**してください。
各JSONにプロファイルを内包しています。
最初はCheckSigだけで調べてください。Freecam等の直接Hookと競合する場合はスキップします。
直接Observerはunhook後もdetourを保持する場合があるため、実際のModの動作確認へ移る前にゲームを完全再起動してください。

| ファイル | ゲーム内で行う操作 |
| --- | --- |
| 10-input.json | Escを開閉、キー・マウス入力、FOV変更。ClientInstanceのslot32/33/34、PacketSender、CommandRegistry候補も読む |
| 20-actors-world.json | ワールドで移動、Actor表示、ブロックを見る。LocalPlayer、Actor一覧、BlockSource、ブロック・チャンクの戻り値を観測 |
| 30-camera-gui.json | カメラ移動・設定画面・通常のコマンド。カメラ、gamma、ゲームモード、GUI、CommandRegistryを観測 |
| 40-world-events.json | ワールド参加・退出・ディメンション移動。未解決の入口や定義はスキップ理由として残す |
| 50-command-output.json | 通常のコマンドや/fakeを実行。CommandOutputPacket::readのdecode結果・成功件数等を観測 |
| 90-port-old.json | 旧定義から候補検索。最後に単独で実行。旧定義を内包するためJSON単体で実行可能 |

`observe`は対応Observerとガード・関数境界の条件を満たすものだけ設置します。
不足定義や未対応ABIは`observe-skipped`に理由を残して他の対象へ進みます。
Observerのない関数について、probeだけで「実際に呼べる」とは判断しません。

90-port-old.jsonは`candidate-profile.json`を生成してアクティブ定義を切り替えます。
通常の残作業一覧に戻すときは90-port-old.jsonをcommandsから外し、
1.26.60.5-all-mods.jsonを再保存して読み込ませてください。

## 値の読取

調査JSONの`watches`はHookで観測したポインタから読みます。
取得値は入力ごとの結果JSONの`watch_samples`に出します。

```json
{
  "label": "BlockSource minimum Y candidate",
  "symbol": "ClientInstance::getRegion",
  "field": "return_block_source",
  "offsets": [58],
  "type": "i16"
}
```

`offsets`が複数なら、最後以外をポインタとして辿ります。
1〜4段、各オフセットは最大0x10000、最終読取は最大64バイトです。
ページを確認してコピーし、読み書き可能か試すための書き込みや関数呼び出しは行いません。
観測後に解放・再利用されたオブジェクトの可能性は残るため、`lifetime_verified`はfalseです。
読めたというだけでオフセットの意味を確定しません。

## 送ってほしい結果

次をまとめて渡してください。

- `result/remaining-work.json` と `.txt`
- 実行したJSONに対応する `result/commands/*.result.json`
- `events.jsonl`（可能なら）
- 実行した操作と、Modの起動ログ・見た目・失敗した内容

`reference_only`は1.26.52.3の参考値です。新しいEXEへ自動適用しません。
`observed_needs_abi_and_lifetime_review`は値を観測できた段階で、全Mod対応完了ではありません。

## 2026-10-28の受領結果

実呼出しを記録した10関数は配布定義の `runtime_observation` に反映しています。
残作業一覧でも記録済み観測と、ABI・寿命・Mod全体の確認を区別します。
`50-command-output` はHook設置までで受信観測はありません。
`90-port-old` の参照ファイル不足は、旧定義の内包へ変更しています。
ClientInstance+0x1E0のslot2はRVA0x768F0のRETであり、送信先として使用しません。

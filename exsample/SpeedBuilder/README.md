# SpeedBuilder (Windows x64 example)

Hiveの記憶・再現ゲーム向けの候補実装です。対応EXEは **1.26.52.3**。
Titleのお題が空でなく、Subtitleに `記憶しましょう` を含むと記録を始めます。
ゲーム内での受信Hook・配置・向き・サーバー受理は実機未確認です。

## 動作

1. プレイヤーを中心に半径10ブロックの立方体を、tickごとに分割して記録。
2. 記録したブロックが消えたら元の記録を固定し、消失後の差分をワーカーで作成。
   完全な2回の観測で消失差分が一致し、ホットバーに対応ブロックの増加を確認して配置へ進みます。
   元から残っている床・建物は対象外です。
3. 各対象について、実際のホットバーブロックのaux、置く面、向き、クリック位置を使い、
   ゲームの配置状態計算APIが返す状態と記録を比較。
4. 一致する条件が見つかったら、ゲームの `PlayerInventory::selectSlot` でメインハンドを切り替え、
   実際のプレイヤーの向きも合わせて1tick待機。
5. 対象・支え・手持ち・距離・向き・ワールドを再確認して、メインハンド指定で
   `GameMode::buildBlock` を呼びます。通常の取引・配置パケット生成経路を利用します。
6. 配置後にブロック状態を観測して進捗を更新。失敗は最大3回、全体は時間制限付きです。

向き合わせは実プレイヤーの回転を使用するため、視点が一時的に動きます。
プレイヤーが途中で向きやスロットを変更した場合は、その変更を上書きして復元しません。
配置中に使った通常の回転・選択は、同じActor／RuntimeID／ワールドの場合だけ復元します。
ワールド変更・ディメンション変更・Actor変更時は記録と作業を破棄します。

## 導入・設定

独立ビルド時のSDK既定参照は `codex/speedbuilder-native`（候補定義を含む開発ブランチ）です。
MSVC Release x64でビルドした `BedrockSpeedBuilder.dll` をロードします。
初回はEscメニューを開いてゲームへ戻り、SDKへClientInstanceを認識させてください。

- 設定: `Documents\PEXData\SpeedBuilder\config.json`
- ログ: `Documents\PEXData\SpeedBuilder\SpeedBuilder.log`
- 中止: **Esc**（既定）。メニューを開いたときも中止します。
- 設定変更: 約500msごとに読み込み直し、進行中の作業は中止します。

```json
{
  "config_revision": 1,
  "enabled": true,
  "radius": 10,
  "max_blocks": 512,
  "capture_cells_per_tick": 256,
  "build_interval_ticks": 2,
  "max_reach": 4.5,
  "max_attempts": 3,
  "confirmation_ticks": 20,
  "max_steps_per_tick": 8,
  "max_wait_ticks": 2400,
  "max_build_ticks": 2400,
  "prompt": "記憶しましょう",
  "keys": { "cancel": 27 }
}
```

## 負荷・制約

- ゲームメモリの取得・操作はクライアントのtickだけで行います。
- ワーカーは所有するコピーの差分整理、ログ書き込み、設定読込を担当します。
- キャプチャは最大256セル/tickかつ約1000µsの協調予算。未処理分は次tickへ持ち越します。
- 配置状態の探索は最大8候補/tickかつ約350µsの協調予算。
  単一のネイティブ関数が長くかかる場合、予算を超える可能性があります。
- 待機中に新しいTitleがなければホットバーを毎tick走査しません。
- 通常の到達距離以内の支えが必要です。記録半径10と配置距離4.5は別の設定です。
  移動の自動化はなく、遠い対象にはプレイヤーが近づく必要があります。
- 状態を再現する条件が見つからない対象は誤配置せず、`placement unresolved` を記録します。
- ブロックエンティティのNBT内容、額縁等のEntity、カスタムアイテム固有動作の再現は含みません。
- 名前はブロックの識別子を使い、Stateはワールド内の不透明な識別トークンとして比較します。
  ワーカーはそのトークンをポインタとして参照しません。
- `observed` はクライアントが記録状態を観測した数です。サーバー受理のACKではありません。
- ネイティブ定義はcandidateです。対象EXE指紋・関数入口・vtableの不一致で停止します。
  `SetTitlePacket` のストリーム読取が実際の受信経路を通るかも、ログによる実機確認が必要です。

## ビルド

```powershell
cmake -S exsample/SpeedBuilder -B build-speedbuilder -A x64 -DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK=$PWD
cmake --build build-speedbuilder --config Release --parallel 2
```

REAの根拠・ABIは [解析メモ](../../docs/speedbuilder-native.md) を参照してください。

## 実機ログを受けた修正（native revision 2）

- 予測前に対象アイテムをメインハンドへ選択し、1tick待ってから予測します。
  選択は予測中保持し、中断・失敗時は同一World/RuntimeIDで自動選択が残っている場合だけ戻します。
- ブロック付きItemStackの補助値はゲーム本体と同じくBlockのデータを使います。
  raw auxが0x7FFFの場合はその特殊値を保ちます。
- 向きは配置を実行するtickで適用します。準備時の向きが次tickまで残ることを条件にしません。
- solidではない足場候補も、ネイティブの衝突形状がある場合は候補に含めます。
  記憶範囲全体で衝突形状を取得せず、配置付近だけを調べます。
- 複数の足場がある場合は近い打点を選択します。通常のリーチ制限は維持します。
- 日本語UTF-8中の0xA7を装飾コードとして削除していた文字化けを修正しました。
- 同じ内容のconfigを保存し直すだけではラウンドを中断しません。
- 時間切れ・採点開始などの字幕で前ラウンドの配置処理を中断します。

診断ログ:

- `build hotbar`: 使用可能なスロット・識別子・個数・補助値。
- `placement pipeline`: `no_support`（足場なし）、`out_of_reach`（届かない）、
  `missing_item`（アイテムなし）、`last_defer`（実行前に見送った理由）。
  visitedなどの選択カウンタは直近の候補探索、native_callsなどはWorld内の累積です。
- `placement unresolved`: 予測の呼出数、失敗数、状態不一致数、予測・記憶したブロックデータ。
- `placement native`: 本体関数を呼べたか、戻り値が成功か。これはサーバー受理ACKではありません。
- `world identity ... changed=`: キャッシュをリセットした識別条件。

自動移動は行いません。記憶範囲10blockと、現在位置からの配置リーチは別です。
`out_of_reach`の場合は対象へ近づく必要があります。全種類の向きやサーバーでの
配置受理は実機確認待ちです。古いキャッシュ定義はrevisionを見て内蔵定義へ切り替えます。

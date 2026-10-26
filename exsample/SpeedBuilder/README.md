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

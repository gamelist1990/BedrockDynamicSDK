# BlockOutline

設定したブロックIDと一致する周辺ブロックを、ネイティブのワールド描画でAABB枠として表示するModです。
ブロック名は設定でON/OFFにできます。日本語の名前も表示できます。
Windows x64 / MSVC Release、SDKのexact-buildプロファイルが対象です。実ゲームでの動作確認は未実施です。

## 設定

起動時に、WindowsのDocuments既知フォルダの次の場所へ作成します。

`C:\Users\<ユーザー>\Documents\PEXData\BlockOutline\config.json`

DocumentsをOneDrive等へ移動している場合は、そのDocumentsフォルダを使用します。
設定変更は約1秒ごとに読み直します。不正な設定はログへ理由を出し、直前の正常な設定を維持します。

```json
{
  "enabled": true,
  "range": 16,
  "show_names": true,
  "name_scale": 0.25,
  "color": "#4CE684",
  "scan_blocks_per_tick": 128,
  "scan_budget_us": 400,
  "max_results": 256,
  "max_labels": 64,
  "debug": false,
  "blocks": [
    {"id": "minecraft:diamond_ore", "name": "ダイヤモンド鉱石", "color": "#45DDEE"},
    {"id": "minecraft:deepslate_diamond_ore", "name": "深層ダイヤモンド鉱石", "color": "#45DDEE"},
    "minecraft:chest"
  ]
}
```

- `range`: プレイヤー位置からブロック中心までの球形範囲。1〜128 block。
- `blocks`: namespace付きIDの完全一致。文字列、または`id`・`name`・`color`のオブジェクト。最大64種類。
- `name`: 枠の上に表示する名前。未指定ならブロックIDを表示します。本体の翻訳名を自動取得する機能ではありません。
- `show_names`: 名前表示。OFFでも枠は表示します。
- `name_scale`: 文字の高さ（block単位）、0.1〜1。
- `color`: 既定の枠色。各ブロックで上書き可能。`#RRGGBB`、不透明。
- `scan_blocks_per_tick`: native読取の上限1〜512。既存の一致ブロックの再確認もこの件数に含みます。
- `scan_budget_us`: 協調時間予算100〜2000µs。単一の本体関数を途中中断する強制上限ではありません。
- `max_results`: 追跡・枠表示の上限1〜1024。近いSubChunk区画から走査し、上限到達後も既存結果の再確認を続けます。
- `max_labels`: 名前の上限0〜128。現在の結果のうち近いものを優先し、文字頂点数にも上限があります。
- `debug`: 約5秒ごとに検出数・今回の読取件数・完了した走査回数を記録します。

`blocks: []`または`enabled: false`で停止できます。ワールド・BlockSource・ローカルPlayerのRuntimeIDが変わると結果を初期化します。
空気の指定や重複ID、不正な値は受け付けません。IDは最大128バイト、表示名は192バイトです。

## 動作・負荷

- native読取はSDKの`NativeWorld`のLevelTickスコープ内だけで行います。
- コピー済みのプレイヤー位置を使い、近い16×16×16のSubChunk区画から連続走査します。球形範囲外はnative読取を省略します。
- 区画の原点だけを保持し、セル全体のoffset配列や毎フレームの全範囲走査を行いません。範囲が大きいほど一巡に時間がかかります。
- 3回の新規走査につき1回の割合で、既に見つけたブロックを再確認します。破壊・置換・読取不能の結果は削除します。
- 移動中もキャッシュを範囲で絞ります。大きく移動した場合は新しい中心から走査し直します。
- 描画用の線・カメラに向く文字はワーカーで作成し、約100msごとの不変スナップショットを描画します。
- DirectWrite/Direct2DでUnicode文字をCPUメッシュ化してキャッシュします。ワーカーはゲームのメモリを読みません。
- 描画は本体のview/projectionと`name_tag`マテリアルを使い、壁越しにも表示します。描画後に借りた色・平行移動の状態を戻します。
- 枠は各ブロックの**1×1×1セルのAABB**です。階段等の細かな当たり判定形状を再現するものではありません。
- 本体が読み込んでいないチャンクは取得しません。遠くのチャンクをサーバーに要求する機能ではありません。
- 検出や削除は予算付きの分散処理なので即時ではありません。範囲・対象密度・表示上限に応じて遅延します。
- rendererにfaultが出た場合は描画を止め、設定再読込またはワールド変更後に再試行します。ログは同じフォルダの`BlockOutline.log`です。

既存のHitBox・Freecamと共有のRenderLevel Hook brokerを使用します。新しい未検証シグネチャは追加していません。

## ビルド

```powershell
cmake -S exsample/BlockOutline -B build-block-outline -A x64 "-DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK=$PWD"
cmake --build build-block-outline --config Release --parallel 2
```

出力: `build-block-outline/Release/BedrockBlockOutline.dll`

DLLの差し替えはMinecraftを完全終了してから行ってください。nativeコールバックの寿命のため、実行中のアンロードは行いません。
`BlockOutline_Stop`でnative走査と描画を停止でき、`BlockOutline_GetStatus`は1=起動中、2=稼働、3=停止、4=失敗を返します。

## 探索専用SDK API

`NativeWorld::scanBlock(BlockPos)` は所有tickスコープと時間・件数予算を守り、
位置・ブロックID・air/blockの種別だけをコピーします。結果の`block`にコピー値が入り、`emptySection`と`unavailableSection`で区画全体の省略を判断できます。配置用プロパティ、NBT、
PlacementRecipeは取得しません。配置処理には従来どおり`getBlock`を使用してください。
チャンクの読み込み有無は同一tick内でXZごとに再利用し、次tickで再確認します。
ブロックやチャンクのポインタをワーカーや次tickへ持ち越しません。

exact-buildプロファイルに`native_world.subchunk_scan`がある場合は、チャンク内の
SubChunk配列からlayer 0ストレージを取り、セルごとのBlockSource::getBlockを省略します。
ストレージのないSubChunk、ワールド高さ外、未読込チャンクは区画単位で飛ばします。
不正なポインタや不正な配列サイズは空気と判断せず、読取不能として扱います。
区画の省略は次の走査で再確認し、新しいブロックの設置やチャンク読込に追従します。
SDK単体ではこの任意項目がない場合に従来の本体getBlockへ戻れます。
このModでは起動時にSubChunk対応を要求し、古いキャッシュの場合はProfileManagerが
更新を確認し、取得できなければexact-buildの埋め込み定義を試します。

1.26.52.3のレイアウトと呼出規約はREA/Ghidraによる静的候補です。実ゲーム確認は未実施です。
パレットのビット列を独自に復号せず、本体のストレージ読取関数を呼びます。
そのため割当済みで全て空気のストレージや、対象ブロックのないパレットを一括省略する
機能は含まれません。速度向上率は未測定です。半径100も設定可能ですが、
読み込まれた範囲内が対象で、予算を増やせばゲームtickの負荷も増えます。

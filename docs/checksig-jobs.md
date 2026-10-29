# CheckSig 調査JSON（Windows x64）

入力先は `Documents/PEXData/CheckSig/commands/` です。
従来の `command/` も使用できます。DLL起動後でも追加・変更を自動検出します。
JSONはUTF-8で保存してください。UTF-8 BOMにも対応します。

## 対応するプロファイルを置く

対象EXEと同じバージョンの `profile.json` を置くだけでdetectします。
結果は `result/commands/profile.result.json` へ出ます。
`detect/result.json` と `detect/result.txt` は全プロファイルの一覧です。

## 調査を指定する

同じフォルダへ、例えば `keyboard.json` を置きます。

```json
{
  "schema": 1,
  "kind": "checksig-job",
  "arch": "x64",
  "profile": "profile.json",
  "actions": [
    {"op": "probe", "symbol": "Keyboard::feed"},
    {"op": "hook", "symbol": "Keyboard::feed"}
  ]
}
```

結果は **`result/commands/keyboard.result.json`** です。
ゲームでキー入力するとHookの観測値が同じ結果に追記されます。
直近128イベントを保持し、それ以前も `events.jsonl` に記録します。
`command/keyboard.json` の結果は `result/command/keyboard.result.json` に分かれます。
入力ファイルは削除しません。変更すると再実行するため、Hook解除は明示してください。

## 操作

| op | 追加フィールド | 内容 |
| --- | --- | --- |
| identity | なし | 対象EXEの識別情報 |
| detect | なし | 現在の定義を再確認 |
| list | なし | 定義された関数の一覧 |
| probe | symbol | 入口ガード、周辺バイト、関数境界等を調査 |
| hook | symbol | 対応ABIの観測Hookを設置 |
| unhook | symbol | 観測を停止 |
| scan | pattern | `.text` 内のAOB検索。例 `48 8B ?? ?? 48 85 C0` |
| read | address, type | 例 `rva:0x331C20`, `u8`。安全なコピーで値を読む |
| memory | address, length, row_bytes（任意） | 領域別に最大1MiBの読み取り専用スナップショット |
| port | なし | sourceに指定した旧定義から候補を検索 |

readの型は既存のTXTコマンドと共通です。主な型は `u8/i8/u16/i16/u32/i32/u64/i64/ptr/f32/f64`。
Hookは既知の観測アダプターと入口条件が一致したものだけに設置できます。
JSONに任意のABIを書いて未対応関数を自動実行する機能はありません。

## 旧定義から探す

```json
{
  "schema": 1,
  "kind": "checksig-job",
  "arch": "x64",
  "source": "old-profile.json",
  "actions": [{"op": "port"}]
}
```

旧JSONも同じフォルダに置きます。旧定義のシグネチャ・アンカーから現EXEを検索し、
関数境界や周辺情報を含む候補を記録します。生成プロファイルは
`candidate-profile.json` です。候補はABIや実動作の検証完了を意味しません。

エラーも入力名に対応した結果JSONの `status: error` と `error` に出力します。
処理途中で失敗した場合、先行する操作で設置済みのHookは維持されます。

## 調査する関数を追加する

jobの `symbols` に、使用するプロファイルと同じ形式で関数を追加できます。
`profile` で対象EXEの一致を確認してから追加します。例：

```json
{
  "schema": 1,
  "kind": "checksig-job",
  "arch": "x64",
  "profile": "profile.json",
  "symbols": {
    "調査対象クラス::関数": {
      "rva": "0x123456",
      "entry_check": "48 89 5C 24 08",
      "signature": "",
      "cross_version_signature_available": false,
      "status": "candidate",
      "abi_verified": false,
      "runtime_verified": false
    }
  },
  "actions": [{"op": "probe", "symbol": "調査対象クラス::関数"}]
}
```

RVAとバイト列は実際の調査対象に置き換えてください。
周辺メモリは `{"op":"read","address":"rva:0x123436","type":"bytes:64"}` のように取得できます。
設置中のHookの定義を置き換える場合は、先にunhookして別のjobで再定義します。
追加した関数も、対応ABIの観測アダプターがなければHookは拒否されます。

## 全Modの残作業・観測セット

1.26.60.5の全Mod用の単体JSON・段階別の観測JSONは [全Mod調査](checksig-126605.md) を参照してください。

- profileにオブジェクトを渡すと、別のプロファイルファイルを置く必要がありません。
- `{"op":"audit"}` で関数・レイアウト・全Modごとの残作業を出します。
- `{"op":"observe","symbols":["Keyboard::feed"]}` は設置できない対象を理由付きでスキップしながら観測します。
- watchesで観測ポインタから最大64バイトの値を継続して読み取れます。
- support_reportは観測後も更新し、Mod全体の操作確認が必要な項目を残します。

## メモリ範囲を読む

`commands/level-range.json`の例。ロード中のEXEを読み取るだけならprofile指定は不要です。

```json
{
  "kind": "checksig-job",
  "schema": 1,
  "arch": "x64",
  "actions": [
    {"op": "memory", "label": "module-header", "address": "rva:0x0", "length": 512, "row_bytes": 64}
  ]
}
```

`address`は`rva:0x...`、絶対アドレス`0x...`、直近のActorサンプル`actor:0+0x...`。
範囲は1バイト〜1MiB、行は1〜4096バイト。`PAGE_GUARD`や読めないページには触れず、保護属性を変更しません。
`result/commands/level-range.result.json`の`memory`に、ラベル・アドレス・保護属性・実測バイト列・読めなかった理由を出します。
Actorサンプルの寿命は保証されないため、結果には出所を残し、読み取れた値だけでABI検証済みにはしません。

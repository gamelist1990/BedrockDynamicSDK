# CheckSig 調査JSON（Windows x64）

入力先は `Documents/PEXData/CheckSig/commands/` です。
従来の `command/` も使用できます。DLL起動後でも追加・変更を自動検出します。
JSONはUTF-8で保存してください。UTF-8 BOMにも対応します。

## 対応するプロファイルを置く

対象EXEと同じバージョンの `windows-x64.json` を置くだけでdetectします。
結果は `result/commands/windows-x64.result.json` へ出ます。
`detect/result.json` と `detect/result.txt` は全プロファイルの一覧です。

## 調査を指定する

同じフォルダへ、例えば `keyboard.json` を置きます。

```json
{
  "schema": 1,
  "kind": "checksig-job",
  "arch": "x64",
  "profile": "windows-x64.json",
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
  "source": "old-windows-x64.json",
  "actions": [{"op": "port"}]
}
```

旧JSONも同じフォルダに置きます。旧定義のシグネチャ・アンカーから現EXEを検索し、
関数境界や周辺情報を含む候補を記録します。生成プロファイルは
`candidate-profile.json` です。候補はABIや実動作の検証完了を意味しません。

エラーも入力名に対応した結果JSONの `status: error` と `error` に出力します。
処理途中で失敗した場合、先行する操作で設置済みのHookは維持されます。

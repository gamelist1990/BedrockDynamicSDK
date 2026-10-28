# CheckSig.dll — Windows x64

ゲーム内で定義のヒット、入口ガード、関数境界、Hook可否、引数・戻り値、周辺メモリを調べます。

## 最短の使い方

1. 対象ゲームバージョンのリリースからCheckSigのZIPを取得します。
2. `CheckSig.dll` を既存Modと同じ方法でゲームへ読み込みます。
3. `Documents/PEXData/CheckSig/commands/` に `windows-x64.json` を置きます。
4. 自動でdetectし、`result/commands/windows-x64.result.json` に結果を出します。

`commands/` は起動時に作成されます。DLL起動後の追加・修正も監視します。
UTF-8 BOMに対応します。従来の `command/` の結果は `result/command/` に分かれます。
別ビルドや壊れたJSONでは、既に読み込んだ一致定義を置き換えません。

## GitHub取得 / ローカル取得

同梱のPowerShellスクリプトでcommandsへ配置できます。

```powershell
./Get-CheckSigProfile.ps1 -Source GitHub -Version 1.26.60.5
./Get-CheckSigProfile.ps1 -Source Local -LocalPath C:\profiles\windows-x64.json
```

## 調査JSON

`commands/keyboard.json` の例です。プロファイルも同じフォルダに置きます。

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

`result/commands/keyboard.result.json` に処理結果とHookの観測値を記録します。
実際にゲームが対象処理を呼ぶと観測数が増えます。直近128イベントを保持し、
それ以前も `events.jsonl` に残します。完了は操作の完了を示し、実機検証の自動完了ではありません。

追加する関数の `symbols`、scan、read、旧定義からのportは、同梱の **checksig-jobs.md** を参照してください。
リポジトリでは [調査JSONの説明](../../docs/checksig-jobs.md) を開けます。

## TXTコマンドも使用可能

`commands/next.txt` に1行ずつ記述します。処理後のTXTは削除します。

```text
probe Keyboard::feed
read rva:0x331C20 bytes:32
hook Keyboard::feed
```

| コマンド | 内容 |
| --- | --- |
| detect / list / identity | 定義の確認・一覧・EXE識別 |
| probe 関数名 | signature/anchor、前後バイト、近接関数、相対参照 |
| scan パターン | .textのAOB検索（例：`48 8B ?? ?? 48 85 C0`） |
| hook / unhook 関数名 | 対応ABIの観測Hookを設置・停止 |
| read アドレス 型 | `rva:0x331C20 u8`、`0x7FF612345678 f32` など |
| source old.json / port | 旧定義から候補を探索 |
| quit | 監視・観測の停止 |

readは `u8/i8/u16/i16/u32/i32/u64/i64/ptr/f32/f64/bytes:N`、最大64バイトです。
対応HookでActorを観測した後は `read actor:0+0x20 bytes:16` も使えます。

## 結果の意味

- `exact-build-match`：EXE識別と定義が一致。
- `entry_guard_hit`：指定RVAのバイト列が一致。
- `hook_ready_for_explicit_test`：観測Hookを試せる条件を満たす。
- `hook-observation`：実際の呼び出しと値を観測。
- `verification_scope: entry-guard`：入口確認。`runtime-hook` は記録済みHook観測。

1.26.60.5の定義は43件の入口観測から重複を統合した40関数です。
`ClientInstance::getLocalPlayer`、`CommandRegistry::registerCommand`、
`GuiData::displayClientMessage`、`BlockSource::getBlock` などをMinecraftの名前で指定します。
名前の未確定な関数は `sub_<RVA>` として残しています。
未知のABIはprobeまでで、JSONにABIを書くだけで任意関数を実行する機能はありません。
専用Observerのunhookは観測を止め、detourを元処理へ通す状態で保持する場合があります。
DLLもゲーム終了まで保持します。

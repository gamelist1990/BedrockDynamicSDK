# CheckSig.dll

**使い方の入口：** `Documents/PEXData/CheckSig/commands/` にWindows x64のJSONを置きます。
プロファイルJSONは自動detectされ、入力別の `result/commands/<入力名>.result.json` に出力します。
調査操作を指定するJSONと旧定義からの探索は [調査JSONの書き方](../../docs/checksig-jobs.md) を参照してください。
`command/` も互換対応し、その結果は `result/command/` に分かれます。


Minecraft Bedrock Windows x64向けの診断DLLです。**x64プロファイルJSONを `command` フォルダーに置くと、自動で読み取り、`detect` フォルダーへ結果を出します。** JSON投入だけでHookが設置されることはありません。

## 最初に使う手順

1. GitHub Actionsの **All mod DLLs → CheckSig-windows-x64** を取得し、DLLローダーから `CheckSig.dll` をゲームにロードします。
2. Windowsの「ドキュメント」にできる `PEXData/CheckSig/command/` へ、対象バージョンの `windows-x64.json` をコピーします。DLL起動前から置いてあっても読み込みます。従来の `commands/` も同じ入力フォルダーとして使えます。
3. `PEXData/CheckSig/detect/result.txt` を開きます。JSONのコピーが落ち着いてから通常1〜2秒で結果が出ます。

このリポジトリの1.26.60.5用プロファイルは `data/1.26.60.5/windows-x64.json` です。ファイル名は変更してもよく、拡張子が `.json` なら読み込みます。サブフォルダーは検索しません。「ドキュメント」はWindowsが設定する実際の保存先を使うため、OneDriveなどへ移動している場合はそちらを確認してください。

```text
Documents/PEXData/CheckSig/
├─ command/
│  ├─ windows-x64.json      ← ここに置くだけで自動検出。JSONは消しません
│  └─ next.txt              ← 必要な場合だけコマンドを書く
├─ commands/               ← 従来の入力先も利用可能
├─ detect/
│  ├─ result.txt            ← 人が読む検出結果
│  └─ result.json           ← 候補ごとの詳細を含む検出結果
├─ events.jsonl            ← 検索結果・Hook観測・読み取り値・エラーの履歴
└─ status.json             ← 実行状態、現在使っている定義、Hook呼び出し数
```

## 自動検出で分かること

EXEのSHA-256とプロファイルを照合し、同じEXEなら各候補のRVAを読み取り、入口バイトの一致、`.pdata`関数開始位置、対応ABI、Hookランタイムを調べます。`result.txt` には候補名・RVA・入口一致/不一致・Hook試行可否とその理由を表示し、`result.json` には読み取り可否と入口バイトも保存します。

| 結果 | 意味 |
| --- | --- |
| `waiting-for-profile` | まだJSONがありません。表示された入力フォルダーに置いてください |
| `exact-build-match` | EXEと定義が一致し、候補ごとの検査を実行しました |
| `different-executable-build` | 別バージョンのEXE向けです。旧定義からの移植手順を使えます |
| `invalid-profile` | JSON形式、対象形式、RVA、入口ガード、PE情報などが不正です。具体的な理由を表示します |
| `profile-read-error` | ファイルを読めないか、16 MiBの上限を超えています |

JSONを更新すると自動で再検出します。サイズと更新時刻を保った上書きも内容の再確認で検出します。コピー途中は待ち、壊れたJSONを直すと再処理します。

複数のJSONを置いた場合はファイルごとに結果を残します。有効な一致プロファイルを読み込んだ後で、別ビルドや不正なJSONを見つけても、そのJSONでは現在の定義を置き換えません。複数の有効な一致JSONがある場合はパス順で最後のものを使い、結果の `active_profile` に実際の選択を明記します。

入口一致は関数の意味やABIの証明ではありません。`hook=ready` は明示的にHookを試せる条件を満たしたという意味で、実ゲームでの動作確認済みを意味しません。候補は自動で `verified` へ昇格しません。配布定義の `status:verified` は `verification_scope` の範囲で確認した印で、`entry-guard` は入口確認、`runtime-hook` は記録済みHook観測を示します。

## コマンドを使う

`command/next.txt` または `commands/next.txt` にコマンドを書きます。1ファイルに複数行を書けます。UTF-8 BOMとCRLFにも対応します。JSONを先に処理し、TXTをファイル名順、ファイル内は行順で処理します。処理したTXTは削除します。

```text
detect
list
probe Keyboard::feed
```

| コマンド | 動作 |
| --- | --- |
| `detect` | 入力JSONを再読込し、検出レポートを更新 |
| `detect "windows-x64.json"` | 指定JSONを照合。相対パスは入力フォルダーからも検索 |
| `list` | 現在の定義の入口・Hook可否を再検査してレポートと `candidate-list` を更新 |
| `probe <symbol>` | signature/anchorのヒット数、`.pdata`境界、前後バイト、近接関数、相対参照を記録 |
| `scan "55 48 83 EC ??"` | `.text`をパターン検索。512件を超える結果は部分候補にしない |
| `hook <symbol>` | 対応ABIと入口ガードを確認し、元処理を通す観測Hookを明示的に試行 |
| `unhook <symbol>` | 観測を停止 |
| `read rva:0x331C20 bytes:16` | 指定RVAの値を読み取り |
| `read 0x00007FF612345678 f32` | 指定VAの値を読み取り |
| `identity` | 読み込み中EXEの識別情報を出力 |
| `source "old.json"` | 移植元の旧定義を指定 |
| `port [version]` | 旧signature/anchorから現EXEの候補定義を生成 |
| `profile "path.json"` | 一致プロファイルをパス指定で読み込み |
| `quit` | コマンド監視と観測を停止 |

`probe` のヒットと周辺情報は `events.jsonl` の `probe-result`、Hook後の引数/戻り値は `hook-observation`、値の読み取りは `memory-read` に出ます。実際に観測対象の処理が呼ばれたときに呼び出し数が増えます。

読み取り型は `u8/i8/u16/i16/u32/i32/u64/i64/ptr/f32/f64/bytes:N`、1回最大64バイトです。ページ状態を確認して `ReadProcessMemory` で読み、読めない範囲はエラーにします。ポインタ先を自動で辿りません。Actorを返す対応Hookの観測後は `read actor:0+0x20 bytes:16` も使えます。Actorの寿命は未保証として結果に記録します。

SDK既知HookPointに加え、完全一致した1.26.60.5用プロファイルでは以下の静的ABI候補に専用Observerがあります。

- `native_camera.local_player`、`native_camera.visual.camera_render_observe`、`native_camera.update_player`
- `native_camera.visual.gamma`、`native_camera.visual.render_dragon_inside_effect.predicate`、`native_camera.visual.set_player_game_type`
- `native_client_commands.register_command`、`native_client_commands.find_command`、`native_gui.display_message`
- `native_world.get_region`、`native_world.get_block`、`native_world.get_chunk`

未知ABIの候補は検索・周辺調査までです。専用Observerの `unhook` は観測を止め、設置済みdetourは元処理を通す状態でプロセス終了まで残します。再度 `hook` すると観測を再開します。DLL自体もゲーム終了まで保持します。

## 旧定義から現EXEの候補を探す

旧JSONを `command/old.json` に置き、TXTに次の2行を書きます。

```text
source "old.json"
port
```

旧定義内のsignatureと複数anchorを現EXEの `.text` で検索し、`.pdata`関数単位に集約します。生成した `CheckSig/candidate-profile.json` は現在の定義として読み込み、検出レポートにも候補の入口とHook可否を出します。現EXEと一致する定義を既に読み込んでいる場合は統合し、RVAが競合した候補は `candidate::cross_version_port::...` 名で残します。構造体offsetやvtable slotを自動で確定しません。

## JSONをGitHubまたはローカルから配置する

手でコピーする代わりに、同梱の `Get-CheckSigProfile.ps1` を使えます。既定の保存先は `Documents/PEXData/CheckSig/command/<version>-windows-x64.json` です。

```powershell
# GitHubの最新x64プロファイルを配置
powershell -ExecutionPolicy Bypass -File .\Get-CheckSigProfile.ps1 -Source GitHub

# GitHubからバージョンを指定
powershell -ExecutionPolicy Bypass -File .\Get-CheckSigProfile.ps1 -Source GitHub -Version "1.26.60.5"

# ローカルJSONを配置
powershell -ExecutionPolicy Bypass -File .\Get-CheckSigProfile.ps1 -Source Local -LocalPath "C:\Downloads\windows-x64.json"
```

ローカルのフォルダー指定にも対応し、`-Version` がなければ最新バージョンを選びます。配置先を変更する場合は `-CommandDirectory`（従来の `-CommandsDirectory` も有効）、既存JSONの置き換えは `-Force` を指定します。GitHub取得ではmanifestにあるJSON自体のSHA-256も照合します。

DLLの取得をスクリプト化する場合は、リポジトリの `Get-CheckSig.ps1` を使います。JSON用は `Get-CheckSigProfile.ps1` です。

## ビルドと確認範囲

GitHub Actionsの **All mod DLLs** がMSVC Windows x64でDLLをビルドします。CheckSigでは入力処理・署名検索・プロファイル検証のテストに加え、生成したDLLをテスト用Windows EXEにロードし、JSON投入から検出結果生成、壊れたJSONからの復帰、コマンド処理、既知ABIのテスト関数に対するHookと引数・戻り値の観測まで確認します。取得用PowerShellもWindows PowerShell 5.1で確認します。

テスト用EXEでの確認と、Minecraft実機で候補ABI・値・寿命を確認することは別の検証です。Minecraft 1.26.60.5では提供された実機ログと既存の静的ABI証拠を照合し、`Keyboard::feed` のHook・key/down・戻り値0の観測を確認済みです。更新済みJSONを読み込むと当該関数に `verification=complete` と表示します。43件の入口照合成功を根拠に、ユーザー指定基準で全43件のsymbolを `status:verified` としています。ほか42件は `verification=entry-verified` と表示し、ABI・実行時確認フラグはfalseです。native keyboardのオブジェクトレイアウトは実行時未検証です。

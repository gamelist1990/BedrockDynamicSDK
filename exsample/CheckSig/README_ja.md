# CheckSig

Minecraft Bedrock Windows x64 の開発用診断Modです。DLLをゲームへロードすると
`Documents/PEXData/CheckSig/` に状態・診断ログ・コマンド入力用フォルダーを作ります。
ゲームメモリは読み取り専用で調べます。ゲームコードへフックを設置するのは
`hook` コマンドを明示した場合だけです。

## できること

- ゲームEXEのファイル/.text SHA-256、PE情報、`.pdata` 関数境界を記録
- 旧定義の `signature_anchors` を現在の `.text` で検索し、複数アンカーの投票を
  `.pdata` 関数へ集約して新しい候補プロファイルを生成
- 旧プロファイルのトップレベル `symbols` に加え、`native_*` 内にある関数定義も移植
  （構造体オフセットやvtable slotは自動で引き継がない）
- 候補関数の入口・アンカー一致位置の前後バイト、近接する関数入口、相対call/branchと
  `.rdata` 文字列への参照候補を記録
- プロファイルの正確なビルド一致、入口ガード、関数境界、SDKフックランタイム対応を検査
- SDKがABIを知っているHookPointに限り、元の処理をそのまま呼び出す透過フックを試行。
  呼び出し回数と既知の引数/戻り値フィールドを記録
- `Level::getRuntimeActorList` は旧・新EXEの機械語でABIを比較した候補HookPoint。
  元処理の後でMSVC x64 vectorヘッダーを安全に読み、件数と先頭8件までのActorポインタを記録
- 1.26.60.5では追加の静的候補としてActor+0x10/+0x18をページ検査付きで読み、
  registryポインタとEntity IDを記録。registry先は辿らず、レイアウトもcandidateのまま
- `ReadProcessMemory` と `VirtualQuery` を使い、読み取り可能なページから最大64バイトを
  型指定で読み取り
- `list` で候補ごとの入口一致、`.pdata`境界、既知HookPoint、現在のHook可否を一覧

署名ヒットだけでは関数の意味やABIを証明しません。全てのポート結果は
`candidate` / `abi_verified:false` / `runtime_verified:false` です。SDKが未知のABIを持つ
関数を呼び出したり、任意の関数へ推測のdetourを設置する機能はありません。

## ビルド

GitHub Actions の `All mod DLLs` workflow がWindows x64/MSVCでビルドし、
`CheckSig-windows-x64` artifactとしてDLLを保存します。ローカルでビルドする場合は
Visual Studio 2022 の x64 Native Tools Command Promptから実行します:

```powershell
cmake -S exsample/CheckSig -B build-checksig -A x64 `
  "-DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK=$PWD"
cmake --build build-checksig --config Release --target CheckSig
```

出力は `build-checksig/Release/CheckSig.dll` です。

## コマンド入力

`Documents/PEXData/CheckSig/commands/` にコマンドを1行書いた `.txt` ファイルを置きます。
CheckSigが処理すると入力ファイルは削除されます。ファイル名順に処理するので、
複数コマンドを連続で置く場合は `01-source.txt`、`02-port.txt` のように番号を付けます。
結果は `events.jsonl`、現在状態は `status.json` に書きます。

```text
source "C:\path\to\old\windows-x64.json"
```

旧版定義にあるトップレベルおよび `native_*` 内の関数を現在のEXEへ移植し、
`.pdata` 境界へ集約して候補プロファイルを作成します:

```text
port 1.26.60.5
```

既存の1.26.60.5候補も保持して移植結果と統合する場合は、先にexact profileを読み込みます。
同一RVAは既存定義を保ったまま移植根拠を追記し、RVA競合は別の
`candidate::cross_version_port::...` 名で保存します。exact profileを事前に読み込まない場合も、
既知の1.26.60.5 ActorTypeComponent静的候補は生成プロファイルへ追加されます。

```text
profile "C:\path\to\1.26.60.5\windows-x64.json"
source "C:\path\to\old\windows-x64.json"
port 1.26.60.5
```

生成した `candidate-profile.json` は自動で現在のターゲット定義になります。
旧定義からの移植をせず、既存の1.26.60.5候補を調べる場合は、リポジトリ内の
`data/1.26.60.5/windows-x64.json` を `profile "C:\path\to\windows-x64.json"` で読み込みます。

個別の確認例:

```text
list
probe Keyboard::feed
scan "55 48 83 EC ??"
hook Keyboard::feed
unhook Keyboard::feed
read rva:0x331C20 bytes:16
read 0x00007FF612345678 f32
identity
help
quit
```

`read` はVA、`rva:0x...`、`u8/i8/u16/i16/u32/i32/u64/i64/ptr/f32/f64/bytes:N`
を受け付けます。ポインター値を読んでも、その先を自動でたどりません。
`scan` は任意のmasked patternを `.text` から検索し、ヒット数、対応する `.pdata` 関数、
相対参照と `.rdata` 文字列を記録します。512件を超える検索は部分結果を候補にしません。
関数の引数や戻り値の値を追う場合は、`hook` が成功した後に `events.jsonl` の
`hook-observation` を確認します。フックできる候補は、現在のEXEと一致し、入口ガードと
`.pdata` の関数開始位置が一致し、SDK既知HookPointに対応するものに限ります。
`list` と `probe-result` は `source`、候補根拠、ABI/runtime検証状態も記録します。
1.26.60.5のexact profile、または `port 1.26.60.5` で生成した候補プロファイルを使うと、
`Level::getRuntimeActorList` の `hook-observation` にActor+0x10のregistryポインタと
Actor+0x18のEntity IDの読み取り可否・値も含まれます。ActorTypeComponentの内部は辿りません。

フック解除後も安全のためDLLはプロセス終了までアンロードされません。ゲームを再起動すると
解除されます。未知ABIの候補は `probe` で静的なヒット/周辺情報を調べられますが、
ライブフックはできません。

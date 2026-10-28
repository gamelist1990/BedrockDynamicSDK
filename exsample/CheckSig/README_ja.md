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
- 1.26.60.5の完全一致EXEでは、静的なABI候補がある9関数に専用Observerを用意。
  `native_camera.local_player`、`native_camera.visual.camera_render_observe`、
  `native_camera.update_player`、`native_camera.visual.gamma`、
  `native_camera.visual.render_dragon_inside_effect.predicate`、
  `native_camera.visual.set_player_game_type`、
  `native_client_commands.register_command`、`native_client_commands.find_command`、
  `native_gui.display_message` が対象
- 各候補のABI・EXE SHA・入口ガード・`.pdata`関数開始位置を確認し、明示的な `hook` のみで試行。
  元関数をそのまま呼び、引数や戻り値を記録
- local player Observerは戻り値Actorを記録し、`read actor:0+...`で調査可能
- camera render Observerは元の4引数を変更せずに呼び出した後、引数ポインタと
  ViewRenderObject先頭64バイトの読み取り可否/値を記録
- camera update Observerは元の3引数を変更せずに呼び出した後、引数ポインタを記録し、
  第3引数をActor候補として `read actor:0+...` の読み取り元に登録
- gamma Observerは元関数のfloat戻り値を記録。Dragon Effect predicate Observerは
  引数ポインタとbool戻り値を記録。game type ObserverはPlayer候補ポインタとint引数を記録
- command Observerはregistry・文字列オブジェクトのポインタ、permission/flags、find結果を記録。
  GUI message ObserverはGuiData・文字列/optionalの各ポインタとfilter contextを記録し、
  文字列オブジェクトの中身は自動で読みません
- `Level::getRuntimeActorList` は旧・新EXEの機械語でABIを比較した候補HookPoint。
  元処理の後でMSVC x64 vectorヘッダーを安全に読み、件数と先頭8件までのActorポインタを記録
- 1.26.60.5では追加の静的候補としてActor+0x10/+0x18をページ検査付きで読み、
  registryポインタとEntity IDを記録。registry先は辿らず、レイアウトもcandidateのまま
- `ReadProcessMemory` と `VirtualQuery` を使い、読み取り可能なページから最大64バイトを
  型指定で読み取り
- `list` で候補ごとの入口一致、`.pdata`境界、既知HookPoint、現在のHook可否を一覧

署名ヒットだけでは関数の意味やABIを証明しません。全てのポート結果は
`candidate` / `abi_verified:false` / `runtime_verified:false` です。SDKが未知のABIを持つ
関数を呼び出したり、任意の関数へ推測のdetourを設置する機能はありません。上記9つの
専用Observerも静的ABI候補を使うため、実行時の意味・寿命は未検証です。特にgamma戻り値、
predicateの呼び出し経路、game typeの引数意味はゲーム内で未確認です。対象EXEの完全一致と
入口ガードが通る場合にだけ利用し、値は候補確認用として扱ってください。

## ビルド

GitHub Actions の `All mod DLLs` workflow がWindows x64/MSVCで全Mod DLLをビルドします。
CheckSigは `CheckSig-windows-x64` artifact、他のModも各 `*-windows-x64` artifactとして保存します。

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
hook Level::getRuntimeActorList
read actor:0+0x20 bytes:16
unhook Level::getRuntimeActorList
probe native_camera.local_player
hook native_camera.local_player
read actor:0+0x20 bytes:16
unhook native_camera.local_player
probe native_camera.visual.camera_render_observe
hook native_camera.visual.camera_render_observe
unhook native_camera.visual.camera_render_observe
probe native_camera.update_player
hook native_camera.update_player
read actor:0+0x20 bytes:16
unhook native_camera.update_player
probe native_camera.visual.gamma
hook native_camera.visual.gamma
unhook native_camera.visual.gamma
probe native_camera.visual.render_dragon_inside_effect.predicate
hook native_camera.visual.render_dragon_inside_effect.predicate
unhook native_camera.visual.render_dragon_inside_effect.predicate
probe native_camera.visual.set_player_game_type
hook native_camera.visual.set_player_game_type
unhook native_camera.visual.set_player_game_type
probe native_client_commands.register_command
hook native_client_commands.register_command
unhook native_client_commands.register_command
probe native_client_commands.find_command
hook native_client_commands.find_command
unhook native_client_commands.find_command
probe native_gui.display_message
hook native_gui.display_message
unhook native_gui.display_message
read rva:0x331C20 bytes:16
read 0x00007FF612345678 f32
identity
help
quit
```

`read` はVA、`rva:0x...`、およびActor一覧Hookが最後に記録した最大8件のActorを基準にする
`actor:<index>+<offset>`（例: `actor:0+0x20`）を受け付けます。Actor相対読み取りは
`VirtualQuery` と `ReadProcessMemory` で指定範囲だけを確認します。サンプル後にオブジェクトの
寿命が続いていることまでは保証せず、結果にも `lifetime_verified: false` と記録します。
ポインター値を読んでも、その先を自動でたどりません。値を確認した後は、必要な場合だけ
別の `read <VA> <type>` を明示してください。読み取り型は
`u8/i8/u16/i16/u32/i32/u64/i64/ptr/f32/f64/bytes:N` です。
`scan` は任意のmasked patternを `.text` から検索し、ヒット数、対応する `.pdata` 関数、
相対参照と `.rdata` 文字列を記録します。512件を超える検索は部分結果を候補にしません。
関数の引数や戻り値の値を追う場合は、`hook` が成功した後に `events.jsonl` の
`hook-observation` を確認します。フックできる候補は、現在のEXEと一致し、入口ガードと
`.pdata` の関数開始位置が一致し、SDK既知HookPointまたは上記専用Observerに対応するものに限ります。専用Observerの `unhook` は
観測を止めます。Detourは処理中のゲームスレッドを安全に解放するため、元処理だけを呼ぶ
pass-throughとしてプロセス終了まで残ります。
`list` と `probe-result` は `source`、候補根拠、ABI/runtime検証状態も記録します。
1.26.60.5のexact profile、または `port 1.26.60.5` で生成した候補プロファイルを使うと、
`Level::getRuntimeActorList` の `hook-observation` にActor+0x10のregistryポインタと
Actor+0x18のEntity IDの読み取り可否・値も含まれます。ActorTypeComponentの内部は辿りません。

フック解除後も安全のためDLLはプロセス終了までアンロードされません。ゲームを再起動すると
SDK middlewareのフックは解除され、専用Observerは元処理だけを呼ぶpass-throughになります。
未知ABIの候補は `probe` で静的なヒット/周辺情報を調べられますが、
ライブフックはできません。

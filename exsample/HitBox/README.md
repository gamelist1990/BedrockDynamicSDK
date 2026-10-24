# BedrockHitBox

BedrockDynamicSDK用のWindows x64 HitBox表示Mod exampleです。公開物は `BedrockHitBox.dll` 単体です。

- 既定では100ブロック以内の取得できたActorを表示。NameTagに依存せずRuntimeIDで追跡
- `players_only=true` でPlayerのみに切替可能。ActorOwner / RuntimeID / AABBShapeを照合してAABBを取得
- 取得できたNameTagをRuntimeIDごとにキャッシュし、枠の上に表示。非表示になった後も取得済みの名前を使用
- SDKのバージョン別Nativeレイアウトと共有Hook runtimeを使用
- Latiteの方式を参考に、LevelRenderer::renderLevelの後でゲームのTessellatorへ直接描画
- ゲームのrender originとname_tagマテリアルを使用し、独自の画面投影・目線高・FOV計算を廃止
- 全Actorの一括再検証を廃止。1tickで1周する目標で分散し、1tickは最大32エントリ、Actor読み取りの処理予算は250µs
- 2本の常駐処理ワーカーがRuntimeIDで担当を分け、距離判定・保持管理・頂点生成・スナップショット公開を実行
- ログ専用ワーカーが最大250msごとにまとめて書き込み、ゲーム／描画側でflushしない
- ワーカーにはRuntimeID・AABB・視点などのコピーだけを渡し、ActorやEnTTのポインタを参照させない
- `retain_last_position=true` で、Playerの最新の有効な位置を即時更新し、最後の観測から10分保存。ワールド変更時は全リセット
- Native読み取りの例外や不正値を検知した対象はスキップ

設定ファイル:

```text
C:\Users\<ユーザー名>\Documents\PEXData\HitBox\config.json
```

Linux上のBedrockOnLinux/Protonでは、`HOME` を基準にホスト側の
`~/Documents/PEXData/HitBox` を使用します。Wineの `Z:` ドライブ経由でLinuxパスへ解決します。
`BEDROCK_HITBOX_DIR` を設定すると保存先を明示的に上書きできます（Linux環境では `/home/...` 形式も可）。

```json
{
  "enabled": true,
  "debug": false,
  "show_all_actors": false,
  "players_only": false,
  "show_name_tags": true,
  "name_tag_scale": 1.0,
  "nearby_distance": 100.0,
  "retain_last_position": false,
  "retention_minutes": 10,
  "line_thickness": 1.5,
  "live_color": "#4CE684FF",
  "retained_color": "#FFAE42FF"
}
```

`players_only=true` でPlayerだけを表示し、`false` で全Actorを表示します。ActorTypeコンポーネントの
32bit値が対象版のPlayer ID（319）と一致するかで判定します。NameTagが非表示でも名前の取得は不要です。
一度確認したPlayerは同じRuntimeIDが続く間、ActorTypeの変更や欠落後もPlayerとして扱います。
Playerのみの場合は、RuntimeIDとActorTypeを先に確認し、未確認の非PlayerのAABB読み取りを省きます。
Player判定履歴も最大1000件に制限し、ワールド変更時に消します。固定サイズの表を使い、取得ループでメモリ確保やロックは行いません。
別のRuntimeIDへ変更されたActorには引き継ぎません。

既定は `show_all_actors=false`、`nearby_distance=100.0` で100ブロック以内を表示します。
距離は4〜128ブロックで設定できます。`show_all_actors=true` では距離制限を解除します。
このモードでは距離計算用のローカル位置コンポーネントも読みません。
既存configの値は維持します。以前の32ブロック設定を広げる場合は
`show_all_actors=false` にし、`nearby_distance` を使います。ゲームに未ロードのActorや、RuntimeID・
所有者照合・有効な正の体積のAABBを取得できないActorは描画できません。全表示で距離や512枠による
打切りは行いませんが、表示対象が増えれば描画量も増えます。

`debug=true` では、Nativeコピーの所要時間・走査数と、各ワーカーの保持数を10秒間隔で記録します。
Nativeコピーが2ms以上かかった場合はdebug無効でも最大10秒に1回記録します。表示の追跡キーにはcomponentからコピーした64-bit RuntimeIDを使います。
名前は共有NameTag Hookが返した結果だけを利用し、名前getterを周期的に呼び出しません。

ActorOwnerのエントリは毎tick確認し、1tickで1周することを目標に取得します。
1tickの上限は32件、Native読み取りループの処理予算は250µsです。予算を超えた場合は続きから次のtickへ回します。
これはNative呼び出し1回を途中で中断する仕組みではありません。32件を超えるプールは複数tickかかり、
1000エントリなら予算内で32件ずつ処理できても最低32tickが必要です。全員の1tick更新は保証しません。
20TPSでは1tickは約0.05秒ですが、実際のLevelTickコールバック数で管理します。描画と補間は毎フレーム行います。
全Actorのハンドル一覧を500msごとに作り直す処理、各プロパティのSDKラッパーによる重複再検証、
対象ごとのStateVector取得を廃止しました。範囲判定にはコピー済みAABBの中心を使います。

2本の処理ワーカーは通常より低い優先度で常駐し、各ワーカーの待機入力は固定FIFOで最大8件を保持します。
キューが使用中のときゲームスレッドは待ちません。取得済みバッチは両ワーカーへの受け渡しが完了するまで
保持し、次のNative取得を延期します。受け渡し済みワーカーへの重複送信と未処理データの上書きを防ぎます。距離判定・期限切れライブ情報の削除・最終位置の更新・8頂点の生成・描画用の
スナップショット作成・古い大きなスナップショットの破棄をワーカーで処理します。
ワールドの世代が異なる入力・描画結果は拒否します。追跡は2本のワーカー合計で最大1000 Actorです。
ライブと保存位置は同じRuntimeIDなら1件として数え、片方のワーカーに偏っても1000枠を利用できます。
満杯の間は新しいRuntimeIDを受け入れず、追跡中のActorの更新は続けます。ライブ情報の期限切れ、
保存位置の10分経過、保存無効化、範囲外への移動で不要になった枠を解放します。
保存位置だけが残るActorも上限に含まれます。ワールド変更時は枠も全リセットします。
ワーカー間の共有枠管理はコピー済みデータの処理側だけで行い、ゲーム／描画スレッドにはロックを追加しません。
描画もワーカーのスナップショット切替中を含め合計1000枠を上限にします。
距離制限をOFFにしている場合は、保存枠も距離で除外しません。

読み取り回数を減らすため、各Actorの位置が表示に反映されるまでの時間は以前より長くなります。
有効期間はActor数と実際に走査できた件数に応じて調整し、分割走査中に枠がすぐ消えないようにします。
### 名前表示とキャッシュ

`show_name_tags=true`（既定）で枠の上にキャッシュ済みの名前を表示します。
`name_tag_scale` は0.25〜3.0（既定1.0）で、文字の大きさを調整します。
ゲームが `Actor::getNameTag` を呼んだ際の戻り値をSDKの共有Hookで観測し、Actorのポインタ・
EnTT世代付きentity ID・ワールド世代で取得バッチへ照合してからRuntimeIDへ結び付けます。
ゲーム側から観測できなかった名前は、取得バッチで確認したActorに対してゲームスレッドで最大100msに1件だけ補完取得します。キャッシュ済みの名前は再取得せず、空の名前は同じActorにつき2秒以上空けて再試行します。例外時は補完取得を停止します。
名前が空・装飾だけになっても、保存済みの名前は消しません。ポインタだけで別Actorへ引き継ぐことはありません。
名前の先頭行を使い、Minecraftの `§` 装飾コードを除去します。文字数は96文字、観測コピーは最大384バイトです。

ゲーム側のコールバックは名前のコピーとentity IDの確認だけを行い、待機しない固定容量メールボックスへ渡します。
文字整形・Windows DirectWriteによるフォント処理・Direct2DによるCPU上の三角形化は処理ワーカーで実行します。
日本語を含むUTF-8名はWindowsのフォントフォールバックを使用します。名前が変わった場合だけ文字形状を作り直し、
各ワーカーは1バッチにつき最大2件作成します。描画側はキャッシュ済み頂点を、視点に向いた板としてゲームの
Tessellatorへ渡します。DXGI/ImGuiのオーバーレイや、未確認のゲーム内フォント関数は使いません。
HitBox上端から0.2ブロック上に、両面描画の黒い背景と白い文字で表示し、保存位置の名前は保存色になります。名前の描画頂点も1フレーム98,304頂点に制限します。

名前キャッシュは各ワーカー最大1000 RuntimeID、観測メールボックスは固定2048スロットです。
上限時には古いキャッシュを入れ替えます。ワールド変更時はキャッシュを消し、旧世代の観測も受け付けません。
位置保存が有効なら保存枠にも名前を表示します。取得していない名前は表示しません。
NameTag Hookを登録できない場合も枠の表示は継続します。

### 名前が表示されない場合の診断ログ

`debug` の値にかかわらず `Documents/PEXData/HitBox/HitBox.log` に5秒ごとの名前診断を出します。
`NameTag resolved source` は関数の解決結果、`NameTag getter` は直接取得の成功・空・読み取り失敗、
`NameTag worker state` は枠数・照合済み名・文字メッシュ数、`NameTag rendering` は描画送信数とスキップ理由です。
`NameTag direct read` はActor/RuntimeID/entity ID/戻り値の例、`NameTag matched` は照合できた正規化名を間隔を空けて出します。
文字形状の作成に失敗した場合は `NameTag mesh failed` にDirectWrite/Direct2Dの段階とHRESULTを出します。
`submitted_labels` は描画APIへ送信した数で、画面で文字が見えたことを保証する値ではありません。
ワールドに入って15秒ほど待ち、`NameTag` 行を含むログを確認してください。

### Playerの最終観測位置の保存

`retain_last_position=true` の場合は、Playerの有効なAABBを取得するたびに最新の位置を保存します。
2秒の静止確認や観測間隔の条件はありません。移動中も同じRuntimeIDの1件を上書きし、移動経路は増やしません。
そのPlayerを取得できている間は現在位置だけを表示し、古い保存枠は重ねません。
ライブ枠の有効期間が終わると、最後に取得できた位置を保存色で表示します。これは現在位置の実測ではありません。
Playerと確認できていないActorや、不正・ゼロサイズのAABBからは保存位置を作りません。

記録は最後の有効な観測から10分で削除します。旧configの `retention_minutes` の値にかかわらず10分です。取得が止まった場合もワーカーが期限を処理します。
新しい観測で上書きするたび期限を更新し、表示範囲外の保存位置も期限で削除します。
ワールド（Level、ローカルActorのregistry、Actorポインタ、世代付きentity ID）が変わった場合やローカルActorが取得できなくなった場合は、期限を待たず判定履歴・保存枠・名前キャッシュをリセットします。ゲームtickが2秒を超えて途切れた後も、ポインタ再利用による旧記録の復活を防ぐためリセットします。メニュー等でtickが停止した場合にもリセットされることがあります。
`retain_last_position=false` への切り替えも保存記録を消します。
移動中は最新位置で保存記録を更新するため、10分の期限は最後に取得できた位置から数えます。
ライブ枠と保存枠の双方を描画時にも期限確認し、取得停止中も期限切れの枠を出しません。

### ブロックかくれんぼでの取得途切れ

[Hiveの公式説明](https://support.playhive.com/gamemodes/)では、隠れる側はブロックに変身し、
静止時には移動中の装飾が消える仕様です。公式説明から内部のActor送信方式までは確定できません。
以前の実装には、ActorTypeが変わると `players_only` で除外する経路がありました。
現在は同じRuntimeIDの確認済みPlayerなら分類を維持します。AABBが消えたりゼロサイズになった場合は
無効な枠を作らず、位置保存が有効なら最後の有効な位置に切り替わります。

サーバーがそのActorをクライアントから削除した場合、クライアント側の列挙だけでは現在位置を更新できません。
別RuntimeIDのブロックActorや通常の地形ブロックに置き換わる場合も、同一人物という対応情報なしには結び付けません。
`players_only=false` では、Player判定のないブロックActorもRuntimeID・所有者・AABBが揃えば表示対象になります。
地形だけになったものは対象になりません。

`debug=true` の10秒間隔ログに `native_player`（現時点でPlayer）、`remembered_player`（RuntimeID履歴でPlayer）、
`filtered_non_player`、`missing_runtime`、`invalid_owner`、`missing_shape`、`invalid_or_zero_bounds` を出します。
これらは前回報告以降の観測回数で、ユニークなActor人数ではありません。変身前後のログでどの取得条件が変わったか確認できます。
Actor自体が列挙から消えた原因の確定には、同じ実行時点の追加観測が必要です。

描画側ではコピー済み頂点から12辺（24頂点）を作り、ネイティブのrender originを差し引いて
ゲームのLineListへ渡します。全対象を生存／保持色にまとめ、1回のmesh submissionを最大512枠に分割します。512枠を超えても続きを描画します。
`selection_overlay` は対象版では深度判定がLessEqualで、背景色と混ぜる設定だったため廃止しました。
ゲームの `name_tag` マテリアル（depthFunc=Always / DisableDepthWrite / Positionのみ）を一度生成して使います。
`current_color` シェーダーへ生存／保持色を直接渡します。白いシェーダー色と未使用の頂点カラーに依存しません。
透明度は常に1.0とし、既存設定の色の末尾AA値も不透明として扱います。RGB指定は維持します。
DXGI/ImGui overlayは起動しません。実ゲームでの深度動作は未検証です。

描画は毎フレーム、コピー済みの前回表示位置から新しいAABBへ線形補間します。更新途中で次の値が来ても
その時点の補間位置を引き継ぎます。将来位置は予測しないため、動きの表示には最大で取得1周期程度の遅れが
あります。初回・8ブロックを超える移動・2秒を超える取得の途切れ・ワールド変更は補間せず反映します。
補間中もActorの追加読み取りは行いません。
`line_thickness` は以前の設定との互換性のため読み込みますが、この描画経路ではゲーム側の線幅を使います。

メモリ範囲の照会結果は1回のコールバック内だけ再利用し、後のtickには持ち越しません。
ローカルActorのキャッシュはclient/world/registry/entity IDを照合し、最大500msでgetterから再取得します。
FOV Hookとローカル回転の読み取りは不要になりました。遅いキャプチャのログは `local` / `position` /
`actors` / `publish` に分けて記録します。250µsはActorループの協調的予算であり、コールバック全体の
上限ではありません。実ゲームでのFPS・取得時間の改善量は未測定です。
全Actor表示ではActorTypeの有無や値を除外条件にせず、ActorOwner / RuntimeID / AABBShapeと所有者の
registry/entity IDを照合します。Playerとして初めて認識するにはActorType storageと値の読み取りが必要です。
確認済みRuntimeIDはActorType storageがなくても判定履歴で追跡を続けます。
必須storageが欠ける場合は読み取りを拒否します。ActorTypeの値オフセット・Player IDも同一EXE用の
プロファイルに記録し、欠落した旧キャッシュは更新します。

現在の `native_actors` プロファイル値は、ActorOwner/AABB/StateVector/Rotation componentの既知レイアウトと
対象EXEのコンポーネント名から作った候補です。すべての読み取りは範囲・値チェックとSEH境界を通しますが、
プロファイルの `status` が `candidate` の間は実機で完全に検証済みという意味ではありません。値が不正または
未対応なら該当Actorを描画せず、ゲーム側の更新に干渉しません。新しいMinecraft版では対象EXEの一致プロファイルに
`native_actors` レイアウトを追加して更新してください。

取得処理が失敗したときは `Actor capture waiting: reason=... stage=... exception_code=...` を出し、
10秒間隔に抑えて次のtickでも再試行します。`actor_owner_storage_missing` と `runtime_id_storage_missing` は
必須ストレージが見つからない場合、`guarded_native_exception` は `stage` の操作中に例外を捕捉した場合です。
一部をコピー済みならそのバッチは公開し、正常な取得に戻ったときは `Actor capture recovered` を出します。
起動ログの `build <revision> native ABI=Microsoft C++` で使用中のDLLも確認できます。

## Build

ゲーム内のEnTT/C++オブジェクトを直接読むため、Windows x64のMSVCまたはclang-clでReleaseビルドします。
MinGWのC++ ABI・標準ライブラリの配置はここではサポートしません。CMakeでMinGWを拒否し、
ソース側でもMicrosoft C++ ABIとReleaseのSTL配置（`_ITERATOR_DEBUG_LEVEL=0`）を必須にしています。
Linux上のコピー済みデータのテストは引き続き実行できます。
GitHub Actionsは同じコミットのSDKソースを使い、MSVCでビルドとテストを実行します。

```powershell
cmake -S . -B build -A x64 -DBEDROCK_SDK_REF=main
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

This also builds `BedrockHitBoxLoader.exe`, a small local test loader. It only
accepts a process whose executable or main-module path identifies
`Minecraft.Windows.exe`, checks that both process and DLL are x64, and reports
whether the module appeared in the target's module list. WineGDK can expose the
Minecraft process with an empty `PROCESSENTRY32::szExeFile`; the loader therefore
falls back to the process module path instead of silently missing the game.
First list the game process; use `list --all` to inspect Wine process entries if
needed; then explicitly inject the DLL into the identified PID:

```text
BedrockHitBoxLoader.exe list
BedrockHitBoxLoader.exe list --all
BedrockHitBoxLoader.exe inject <pid> <x64-DLL-path>
```

On BedrockOnLinux/Proton, run the loader through the same Proton prefix as the
game and pass the DLL path as a Wine path (for example
`Z:\home\<user>\WorkSpace\DevBox\BedrockDLLs\HitBox-main-<sha>\BedrockHitBox.dll`).
Use a local/offline test world. This loader is not a game launcher or a
compatibility layer; the HitBox DLL itself must match the target's C++ ABI.

ワールド描画のRVA・呼出規約・オフセットは対象1.26.52.3 EXEのREA/Ghidra解析とLatiteのソースで
照合しました。[解析根拠](../../docs/hitbox-world-render.md)に範囲と未確認事項を記載しています。
`native_world_render` は実機未検証の `candidate` です。全7関数の実メモリ上の入口24バイトを検査し、
不一致なら描画Hookを登録しません。借用したTessellatorが使用中ならその描画をスキップします。
ネイティブ描画で例外が起きた場合は以後の描画を無効化します。SEHであらゆる内部状態破損やクラッシュを
防げることを保証するものではありません。

DLLはプロセス終了まで固定します。Stop後の直接描画Hookは元関数を呼ぶだけの状態で残し、実行中の
trampolineを解放しません。更新DLLを使う際はMinecraftを再起動してください。

起動時はキャッシュのEXE一致だけでなく、HitBoxが必要とするNative item/Actor/world-renderレイアウト全体を検査します。
古いキャッシュで `value_offset` などの必須キーが欠ける場合は、GitHubの同一EXE用プロファイルを再取得します。
再取得できない場合は同梱プロファイルを検査し、実行中EXEの指紋と一致する場合だけ使用します。
どの候補も不足・不一致ならHookを登録せず起動を中止し、欠けたオフセットを既定値で補うことはありません。

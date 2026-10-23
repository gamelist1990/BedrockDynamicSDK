# BedrockHitBox

BedrockDynamicSDK用のWindows x64 HitBox表示Mod exampleです。公開物は `BedrockHitBox.dll` 単体です。

- 既定では取得できた全Actorを表示。PlayerもNameTag表示や距離に依存せずRuntimeIDで追跡
- `players_only=true` でPlayerのみに切替可能。ActorOwner / RuntimeID / AABBShapeを照合してAABBを取得
- SDKのバージョン別Nativeレイアウトと共有Hook runtimeを使用
- Latiteの方式を参考に、LevelRenderer::renderLevelの後でゲームのTessellatorへ直接描画
- ゲームのrender originとname_tagマテリアルを使用し、独自の画面投影・目線高・FOV計算を廃止
- 全Actorの一括再検証を廃止。5tickで1周する目標で分散し、1tickは最大32エントリ、Actor読み取りの処理予算は250µs
- 2本の常駐処理ワーカーがRuntimeIDで担当を分け、距離判定・保持管理・頂点生成・スナップショット公開を実行
- ログ専用ワーカーが最大250msごとにまとめて書き込み、ゲーム／描画側でflushしない
- ワーカーにはRuntimeID・AABB・視点などのコピーだけを渡し、ActorやEnTTのポインタを参照させない
- `retain_last_position=true` で、2秒静止を確認したPlayerの最後の位置をワールド変更まで保存
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
  "show_all_actors": true,
  "players_only": false,
  "nearby_distance": 32.0,
  "retain_last_position": false,
  "line_thickness": 1.5,
  "live_color": "#4CE684FF",
  "retained_color": "#FFAE42FF"
}
```

`players_only=true` でPlayerだけを表示し、`false` で全Actorを表示します。ActorTypeコンポーネントの
32bit値が対象版のPlayer ID（319）と一致するかで判定します。NameTagが非表示でも名前の取得は不要です。
Playerのみの場合は、Playerと分類できないActorを除外し、そのActorのRuntimeID/AABB読み取りも省きます。

`show_all_actors=true`（既定）では、読み取れたActorを32ブロックなどの距離で除外しません。
このモードでは距離計算用のローカル位置コンポーネントも読みません。
既存configにこれらのキーがなくても既定値が適用されます。距離で絞りたい場合のみ
`show_all_actors=false` にし、`nearby_distance` を使います。ゲームに未ロードのActorや、RuntimeID・
所有者照合・有効な正の体積のAABBを取得できないActorは描画できません。全表示で距離や512枠による
打切りは行いませんが、表示対象が増えれば描画量も増えます。

`debug=true` では、Nativeコピーの所要時間・走査数と、各ワーカーの保持数を10秒間隔で記録します。
Nativeコピーが2ms以上かかった場合はdebug無効でも最大10秒に1回記録します。名前の取得やNameTag Hookは
行わず、表示の追跡キーにはcomponentからコピーした64-bit RuntimeIDを使います。

ActorOwnerのエントリは5tickで1周することを目標に、毎tick `ceil(Actor数/5)` 件ずつ確認します。
1tickの上限は32件で、全件を5tickごとに一括更新しません。RuntimeIDとAABBだけをコピーします。
250µsの処理予算を超えた場合は次のエントリを後のtickへ回します。これはNative呼び出し1回を途中で
中断する仕組みではありません。予算内に1周できない場合も続きから処理し、途中で先頭に戻しません。
Actorが160件を超える場合や読み取りが遅い場合は1周が5tickより長くなります。20TPSでは5tickは約0.25秒ですが、
サンプリングは実際のLevelTickコールバック数で管理するため、ゲーム側のtick速度にも従います。
全Actorのハンドル一覧を500msごとに作り直す処理、各プロパティのSDKラッパーによる重複再検証、
対象ごとのStateVector取得を廃止しました。範囲判定にはコピー済みAABBの中心を使います。

2本の処理ワーカーは通常より低い優先度で常駐し、各ワーカーの待機入力は固定FIFOで最大8件を保持します。
キューが使用中のときゲームスレッドは待ちません。取得済みバッチは両ワーカーへの受け渡しが完了するまで
保持し、次のNative取得を延期します。受け渡し済みワーカーへの重複送信と未処理データの上書きを防ぎます。距離判定・期限切れライブ情報の削除・静止確認・8頂点の生成・描画用の
スナップショット作成・古い大きなスナップショットの破棄をワーカーで処理します。
ワールドの世代が異なる入力・描画結果は拒否します。ライブ情報は各ワーカー最大16384件で、上限到達時は
最も古いものを削除します。確定した静止記録は別のマップに保存し、このライブ上限や時間では削除しません。
距離制限をOFFにしている場合は、保存枠も距離で除外しません。

読み取り回数を減らすため、各Actorの位置が表示に反映されるまでの時間は以前より長くなります。
有効期間はActor数と実際に走査できた件数に応じて調整し、分割走査中に枠がすぐ消えないようにします。
### 静止したPlayerの位置保存

`retain_last_position=true` の場合は、Player自身の静止を2秒間確認してから位置を確定します。
AABBの最小・最大座標が最初の観測位置からそれぞれ5cm以内に収まり、観測間隔が1秒以内であることを条件にします。
取得が途切れて枠が止まっただけでは確定しません。移動や長い観測の途切れで、静止確認を最初からやり直します。

移動中のライブ枠は5tick目標の取得＋補間で更新します。そのPlayerを取得できている間は現在位置だけを表示し、
古い保存枠は重ねません。次の場所で再び2秒静止すると、同じRuntimeIDの記録を上書きします。移動経路は増やしません。
そのPlayerを取得できなくなった場合は、ライブ枠の有効期間が終わると、最後に確定した静止位置だけを保存色で表示します。
未確定のPlayerやPlayer以外のActorは、取得が止まっても保存枠に変換しません。

確定記録はワールド（LevelまたはローカルActorのregistry）が変わるまで維持します。旧設定の
`retention_minutes` は読み込まず、`1` が残っていても1分では消しません。`retain_last_position=false` へ切り替えると
記録と静止確認を消します。記録は各RuntimeIDに1件で、同一ワールド中に確認したPlayerが増えると保存件数も増えます。
ライブ枠の鮮度と保存枠は明確に分け、描画時も期限切れのライブ枠を除外します。

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
registry/entity IDを照合します。Playerのみ表示にはActorType storageと値の読み取りが必要です。
必須storageが欠ける場合は読み取りを拒否します。ActorTypeの値オフセット・Player IDも同一EXE用の
プロファイルに記録し、欠落した旧キャッシュは更新します。

現在の `native_actors` プロファイル値は、ActorOwner/AABB/StateVector/Rotation componentの既知レイアウトと
対象EXEのコンポーネント名から作った候補です。すべての読み取りは範囲・値チェックとSEH境界を通しますが、
プロファイルの `status` が `candidate` の間は実機で完全に検証済みという意味ではありません。値が不正または
未対応なら該当Actorを描画せず、ゲーム側の更新に干渉しません。新しいMinecraft版では対象EXEの一致プロファイルに
`native_actors` レイアウトを追加して更新してください。

## Build

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

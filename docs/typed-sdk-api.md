# 型付きSDK API

HTMLの説明・使用例は [API guide](site/index.html) を参照してください。

## コマンドBuilder

```cpp
auto definition = bedrock::CommandBuilder(bedrock::CommandName{"top"}, bedrock::ArgumentName{"blocks"})
    .description(bedrock::CommandDescription{"安全な上方移動"})
    .then([](std::optional<int> blocks) -> bedrock::CommandDecision {
        return teleport.request(blocks); // NativeCommandServiceがLevelTickへ配送
    });
commands.add(std::move(definition)); // bedrock::NativeCommandService
```

名前・説明・引数名は用途ごとの型です。optionalIntegerのハンドラーは
`optional<int> -> CommandDecision`、requiredIntegerは `int -> CommandDecision`。
ハンドラーを指定していないBuilderはNativeClientCommandsへ登録できません。
現在のnativeアダプターは整数引数1個に限定しています。

## 実行・ワールド切替をまとめるAPI

`NativeCommandService`はネイティブ候補表示、20tickごとの登録再試行、
パケットfallback、整数解析、16件の実行待ちキュー、200ms内の逆経路重複の組合せ抑制を担当します。
同じ経路からの連続入力は捨てません。引数はコピー済みの整数として保持します。

```cpp
auto scope = teleport.tick(client, level, epoch); // NativeSafeTeleport
if (!scope) return;
commands.update(client, epoch); // NativeCommandService
commands.dispatch(); // 最大1件。Builderのthenをこのスレッド上で実行
```

初期化時に`onDiagnostic`と`onRejected`で通知を接続できます。
`WorldChange`では両方に`reset(epoch)`を呼び、古いキューと探索を破棄します。
登録定義はHook開始前に固定し、サービスをModの寿命中保持してください。
低水準の`NativeClientCommands`は従来どおり残しています。

`NativeSafeTeleport`は`request(optional<int>)`で正確な上移動、引数省略で安全な上方の床を探索します。
`configure(SafeTeleportOptions)`で距離上限・tickごとの件数・時間予算を指定します。
`onResult`は型付きの`TeleportResult`/`TeleportError`を返し、Modが表示言語を決めます。
読込・RuntimeID・ワールド・移動中断・当たり判定・床・高さ・直前の再検査はSDKが担当します。
階段や半ブロック等の支持面を安全と推測せず、未対応の床形状は拒否します。
ネイティブ位置の更新後にサーバーが移動を受理することを保証するAPIではありません。

## SDKのプロファイル取得

`ProfileManager::prepare`へ、必要機能、更新方針、アダプター初期化関数を渡します。
キャッシュ・埋め込み・GitHub取得をModごとに実装する必要はありません。

1. ロードされたEXEを識別し、既存の安全な取得経路で候補を選択。
2. 必要なFeature・schema・revision・CandidatePolicyを確認。Gameplay / LevelTick / Inputなど、要求したHookの解決も事前に確認。
3. nativeアダプターのレイアウト・エントリ検査を実行。
4. 不足/不一致なら既存のsingle-flight・24時間制限の範囲でGitHubへ再照会。
5. 一致する埋め込み定義も確認し、成功した不変スナップショットだけを公開。

アダプター関数はHookを設置する前の検証処理として渡してください。
一部のbindingを更新して失敗した場合にも再実行可能である必要があります。
実行中のHookをprepareの中で設置・解除しないでください。
背景の通常更新は次回初期化向けのキャッシュ更新です。
稼働中のnativeハンドルへ新しいABIを自動適用しません。

`RuntimeProfile`はProfileManagerだけが構築できます。nativeのload(snapshot,error)
は保存済みのidentityとResolverの検証結果を使い、無用なEXE再ハッシュを避けます。
Modが手動で生JSONを渡す旧loadは互換用に残しています。

## 追加した型

`CommandName` / `ArgumentName` / `CommandDescription` / `BlockIdentifier` /
`HotbarIndex` / `Yaw` / `Pitch` / `Reach` / `WorldEpoch` / `TickBudget`。
更新・候補・読取・コマンド受付のboolは、`UpdatePolicy` / `CandidatePolicy` /
`ReadPolicy` / `CommandDecision`のenumで表します。

旧HotbarSlotはスロットのアイテム情報なので、番号用はHotbarIndexと区別しています。
値の構築は不正値なら例外を返します。nativeコールバック内で不正なユーザー値を
そのまま構築せず、コマンド値を検査してtick用キューへ渡してください。

## 今後の版への対応

同じ呼出規約・対応済み構造であれば、SDKが新しいwindows-x64.jsonを選択することで
Modソースの変更を減らせます。未対応のABI・EnTT・構造サイズ変更はSDK更新が必要です。
データが足りない場合は機能を無効化し、別ビルドのシグネチャを強行しません。

GameHooksのResolver参照はconstへ整理しました。型付きの新しい入口を追加し、
既存のGameAccess/C ABIを別のバイナリレイアウトへ変更する作業ではありません。

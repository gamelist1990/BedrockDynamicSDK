# 型付きSDK API

HTMLの説明・使用例は [API guide](site/index.html) を参照してください。

## コマンドBuilder

```cpp
auto definition = bedrock::CommandBuilder::named(bedrock::CommandName{"top"})
    .description(bedrock::CommandDescription{"安全な上方移動"})
    .optionalInteger(bedrock::ArgumentName{"blocks"})
    .handles([](std::optional<int> blocks) -> bedrock::CommandDecision {
        return queueRequest(blocks) ? bedrock::CommandDecision::Accepted
                                    : bedrock::CommandDecision::Rejected;
    });
commands.add(std::move(definition));
```

名前・説明・引数名は用途ごとの型です。optionalIntegerのハンドラーは
`optional<int> -> CommandDecision`、requiredIntegerは `int -> CommandDecision`。
ハンドラーを指定していないBuilderはNativeClientCommandsへ登録できません。
現在のnativeアダプターは整数引数1個に限定しています。

## SDKのプロファイル取得

`ProfileManager::prepare`へ、必要機能、更新方針、アダプター初期化関数を渡します。
キャッシュ・埋め込み・GitHub取得をModごとに実装する必要はありません。

1. ロードされたEXEを識別し、既存の安全な取得経路で候補を選択。
2. 必要なFeature・schema・revision・CandidatePolicyを確認。
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

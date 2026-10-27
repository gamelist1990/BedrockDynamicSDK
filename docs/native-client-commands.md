# SDK native client commands

`bedrock/native_client_commands.hpp` の `NativeClientCommands` は、Topにあった
本体CommandRegistry登録処理をSDKへ移したものです。`bedrock::sdk`で利用できます。
EnTTやWorld APIへの依存はありません。Windows x64 / MSVC Release向けです。

## 新しい型付き入口

新しいModでは [HTML APIガイド](site/index.html#commands) のCommandBuilderと
ProfileManagerを使用してください。以下の文字列/boolを使う入口は移行用です。

## 互換APIの使用例

```cpp
#include <bedrock/native_client_commands.hpp>

bedrock::NativeClientCommands commands; // 登録中・実行中に生存させる

// 初期化時: 名前、説明、整数引数の名前と省略可否、ハンドラー。
bool added = commands.add("top", "安全な上方移動", {"blocks", true},
    [](const bedrock::NativeCommandInvocation& invocation) -> bool {
        // invocation.name == "top"
        // invocation.integer は省略時nullopt。
        // 本体の実行スレッド上。コピー済みの値を自分のtick用キューへ渡す。
        return queueTopRequest(invocation.integer);
    });

std::string error;
if (!added || !commands.load(exactBuildProfileJson, true, error)) {
    // エラーを表示し、このnative登録機能は無効にする。
}

// ローカルclientのLevelTick上。毎tick呼ぶ必要はなく20tick程度の間隔でよい。
auto result = commands.update(clientInstance);

// ワールド変更イベント時:
commands.reset();
// 次のupdateで生存中のRegistryを確認し、必要なら登録し直す。
```

`load`は対象EXEとJSONの一致・関数エントリ・native callable表を検証します。
現時点の定義はcandidateのため、引数の `true` は明示的な利用許可です。
バージョン依存定義はJSONの `native_client_commands` に集約しています。

## 動作と範囲

- 本体の名前・説明・引数登録を通し、標準UIの候補・Tab補完の情報を提供する。
- ローカル実行用のSyncLocal / NotCheatフラグを使用する。
- 現在のアダプターは整数引数1個。省略可能なら `/top` と `/top 3` に対応。
- コマンド名は小文字英数字・`_`・`-`、最大64文字。
- 引数名も同じ文字種で最大15バイト。native SSO一時文字列を安全に扱うための制約。
- `add`は複数回使用可能。1 DLLあたり最大32定義。初期化前にすべて登録する。
- ハンドラーのtrueは「要求を受け付けた」の意味。nativeの成功件数へ反映する。
- ゲーム状態の読み書きはハンドラーから直接行わず、LevelTickへキューイングする。
- 文字列・enum・複数引数・引数なし専用overloadの登録は、現アダプターの対象外。
- ネイティブのチャット応答は別API。ハンドラーへforeign CommandOutputを公開しない。

既存の `ClientCommands` は入力文字列のローカル振り分け用として残します。
それだけでは本体の候補登録は行われません。Topはnative登録と従来のpacket fallbackを
併用し、二重実行の抑制はTop側で継続しています。

## 衝突・所有権・終了

`update`は unavailable / waiting / unchanged / registered / collision / failed を返します。
他のModや本体が同名を登録済みなら、既存登録を上書きしません。`error()`で理由を取得できます。
複数定義を登録した場合は全件を処理し、失敗・衝突があれば集約結果として返します。

本体のコマンドインスタンスがDLL内のvtableを参照するため、`load`成功時にDLLをpinします。
実行中のDLLアンロードはできません。nativeの文字列/配列は本体の関数に確保させます。
SDKのコマンドオブジェクトはSDK側で確保・破棄し、allocatorを混在させません。

マネージャー破棄時は各ハンドラーを失効させます。古いnativeインスタンスが別の登録へ
誤配信されないように、factoryスロットはプロセス内で再利用しません。32件の上限には
破棄済み定義も含みます。ホットリロード用の無制限な再生成には対応しません。
登録・load・破棄はイベント実行と並行させないでください。`reset`は登録定義が固定された
後のワールドイベントから呼べます。

## 移行と確認状況

Topの `src/native_commands.hpp` はSDKヘッダーへの互換includeだけになりました。
専用のNativeTopCommandやグローバルcommandReceiverはSDKの汎用factory/handlerへ置換しました。
元の解析根拠は [Top解析メモ](top-native.md) に保持しています。
新しい型・複数定義でのWindows実行と候補表示は、別途MSVCビルドと実機確認が必要です。

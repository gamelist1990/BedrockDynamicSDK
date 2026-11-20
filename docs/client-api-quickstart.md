# Client SDK クイックスタート

外部 Mod は `bedrock::sdk` をリンクし、`<bedrock/sdk.hpp>` をインクルードします。
Mod の初期化と公開イベントについては [Stage A 公開 SDK](stage-a-sdk-design.md) を参照してください。

## 設定と翻訳

設定の登録は `DllMain` ではなく Mod の初期化処理から行います。

```cpp
#include <bedrock/sdk.hpp>

bedrock::sdk::SettingsPage page("my-example", "title");
page.add(bedrock::sdk::Setting::toggle("enabled", "enable", true))
    .locale("en_US", {{"title", "Example Settings"}, {"enable", "Enable feature"}})
    .locale("ja_JP", {{"title", "サンプル設定"}, {"enable", "機能を有効にする"}});
const auto result = page.registerPage();
```

保存値は `page.value("enabled")` で取得できます。変更通知からゲームスレッド
専用の処理を直接呼ばないでください。Mod の終了時は `unregisterPage()` を呼びます。
[DynamicClient の設定例](../exsample/DynamicClient/src/settings.cpp) も参照してください。

## 第一者 Mod のネイティブフック（内部）

`bedrock::game_hooks` を使う第一者 Mod は `HookDispatch` に型を指定して購読し、
同じインスタンスを `GameHooks` に渡します。対象ビルドのプロファイルでフックを導入し、
終了時はディスパッチャーを破棄する前にフックを停止します。

```cpp
#include <bedrock/runtime/game_hooks.hpp>

bedrock::detail::HookDispatch dispatch;
bedrock::GameHooks hooks{dispatch};
dispatch.on<bedrock::KeyEvent>([](const bedrock::KeyEvent& event) {
    if (event.pressed && event.inputKey() == 'C') {
        // ゲームから C の押下が届いた。
    }
});
```

[ネイティブクライアントアクセス](native-client.md) と
[ワールドライフサイクル](world-events-and-packets.md) も参照してください。

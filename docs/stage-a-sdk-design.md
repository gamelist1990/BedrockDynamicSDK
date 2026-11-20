# Stage A SDK 設計メモ

Stage A は旧 `bedrock::api` の互換層を増やさず、`bedrock::sdk` を新しい公開面とする。
既存の `data/` プロファイル、解決器、パケット実装、NativeInjectedCommands、OreUI
アダプターは内部実装として残し、公開ヘッダーからは隠す。ゲーム固有の RVA や C++
レイアウトは SDK の下位層だけが扱い、Mod はそれらを直接参照しない。

## 新しいツリー

```text
sdk/
  CMakeLists.txt
  include/bedrock/sdk.hpp       # Mod 作成者向けの唯一の入口
  include/bedrock/sdk/
    result.hpp                   # Result<T>, Error, ErrorCode
    events.hpp                   # EventSignal<Args...>, Subscription
    system.hpp                   # tick scheduler
    keybinds.hpp                 # Mod key declarations and input transitions
    settings.hpp                 # 宣言的設定ページ
    mod.hpp                      # Mod の登録単位
  src/settings.cpp               # 旧 OreUI 実装への内部ブリッジ
  tests/core_test.cpp
data/                            # 既存のバージョンプロファイル（再利用）
src/, include/bedrock/detail/   # SDK 内部アダプター（公開 API ではない）
exsample/
  DynamicClient/                 # Stage A の参照 Mod
  DevClient/                     # 開発・診断用 Mod 群
```

`sdk/` は責務ごとに分割し、単一の巨大ヘッダーや Mod ごとのネイティブ定数を作らない。
`bedrock::stage_a` は既存の OreUI ストアへリンクするが、失敗は `Result` として返す。
コールバック境界で例外をゲームへ漏らさず、エラーを空値に変換して握り潰すこともしない。

## 最小 Mod

```cpp
#include <bedrock/sdk.hpp>

class MyMod final : public bedrock::sdk::Mod {
public:
    MyMod() : Mod("my-mod") {}

private:
    void onEnable() override {
        const auto result = settings("settings.title")
            .add(bedrock::sdk::Setting::toggle("enabled", "settings.enabled", true))
            .locale("en_US", {{"settings.title", "My Mod"}, {"settings.enabled", "Enabled"}})
            .locale("ja_JP", {{"settings.title", "自分の Mod"}, {"settings.enabled", "有効"}})
            .registerPage();
        if (!result) throw std::runtime_error(result.error().message);
    }

    void onDisable() override {
        const auto result = settings("settings.title").unregisterPage();
        if (!result) throw std::runtime_error(result.error().message);
    }

    void onUpdate() override { /* ゲーム tick ごとの処理 */ }
};
```

イベントは `onEnable` / `onDisable` / `onUpdate` / `onPlayerSpawn(Player&)` などを override する。
`ClientHost::start(mod)` と `stop()` はローダー側から（DLL ローダーロックの外で）呼ぶ。
ホストは `onEnable` / `onDisable` を呼び、ネイティブのクライアント Level tick から
`onUpdate` と `system().run` / `runTimeout` / `runInterval` を進める。
1.26.65.1 では直接の spawn フックが未解決のため、`onPlayerSpawn` はローカルプレイヤーの
Runtime ID が初めて読めた所有 tick、または新しい Runtime ID に切り替わった tick に通知する。
リモートプレイヤーのスポーンや同一 Runtime ID の即時復活を保証するものではない。
動的・対象限定イベントは `object.changed += handler` の戻り値 `Subscription` を保持する。
破棄すると次回の通知から解除される。

## クライアント側ゲーム API の対応範囲

`world().localPlayer()`、`getPlayers()` / `getAllPlayers()`、`currentDimension()` は
クライアントが読み込んでいる対象のみを扱う。ワールドはサーバー全体を表さない。
ネイティブ参照は所有 tick 内でのみ解決できる。座標、インベントリ、装備、ブロックの読み取り値は
コピーであり、別スレッドへ渡してもネイティブメモリに触れない。`Block::typeId()` は取得時点の値。

```cpp
void onUpdate() override {
    const auto player = world().localPlayer();
    if (!player) return;
    const auto location = player->location();
    const auto movement = player->getComponent<bedrock::sdk::MovementComponent>();
    // 未ロードまたは未対応の読み取りは std::nullopt。
}
```

World / Entity の DynamicProperty は Mod ごと・ワールドセッションごとのメモリ内ストア。
`std::nullopt` で削除する。セーブやサーバーへは書き込まず、ワールドを抜けると失われる。
現行のネイティブホストで読むことができるコンポーネントは Movement・Inventory（ローカル限定）・
Equippable。`sendMessage` はローカル HUD 宛てで、サーバーにチャットパケットを送らない。

名前付き次元の解決、全 Entity の型 ID、Health / Food、汎用テレポート、タグ、直接の
ブロック変更、Entity の生成、およびローカル Effect 操作はネイティブ未対応。
操作系は偽の成功を返さず `ErrorCode::Unavailable` を返す。
`Player` の効果操作（`addEffect` / `removeEffect` / `getEffects` / `clearEffects`）は
ローカルプレイヤーの所有 tick に限定する。
`ClientHost` のネイティブ実装は Windows x64 ビルドで
`BEDROCK_SDK_ENABLE_NATIVE_CLIENT` と `BEDROCK_SDK_ENABLE_NATIVE_WORLD` を有効にする。

## キーとバニラ ID

`keybinds().add(actionId, translationKey, Key::C)` でアクションを宣言し、`locale` に
日英ラベルを渡す。対応キーは英字・数字・Tab・Space・F1–F12。「キーを変更」の
入力待ち中に Esc を押すと、そのキーだけを初期値に戻して保存する。
ゲーム側の入力を届けるホストは `dispatchKey` を呼ぶ。DynamicClient の
Zoom、TabList、Freecam は SDK の割り当てを参照する。AutoSprint と Freecam の移動入力は
既存の操作を維持する。

`<bedrock/sdk/vanilla.hpp>` は CMake が生成する。例:
`bedrock::sdk::vanilla::MinecraftBlockTypes::Glass.value()` は `minecraft:glass`。
ブロック・アイテム・エンティティ・バイオーム・エフェクト・ディメンションの型は別々。
TypeScript も同じカタログから生成する（Stage C のパッケージ公開とは別工程）。
カタログの版と更新手順は `data/vanilla/README.md` を参照。

## DynamicClient の移植方針

Zoom、AutoSprint、TabList、Fullbright、Freecam の既存ネイティブ実装は Stage A の内部アダプターを
再利用し、設定登録を `sdk::SettingsPage` に統一する。翻訳キーは `en_US` と `ja_JP` を標準にし、
値 ID はローカライズしない。AutoMine、HitBox、SpeedBuilder は Stage A の公開面へ移植しない。
Fullbright は公開 `sdk::BrightnessOverride` を使い、ゲームの設定値は書き換えない。

## 移行と検証の境界

新 SDK のホストテストが通り、Windows x64 の DynamicClient DLL が生成できるまで旧ツリーを退避しない。
その後、旧公開ツリーは `~/Documents/PEXData/BedrockDynamicSDK-stage-a-legacy/` に退避し、
正位置には新公開ツリーを配置する。BOL では注入、タイトル維持、Mod Menu 表示、ワールド内の
各機能を個別に確認する。検証できない場合は成功扱いにせず、未確認項目と障壁を報告する。

Linux では `./build-linux.sh --stage-a` で SDK・DynamicClient・CheckSig をクロスビルドする。
退避後は `sdk/include/bedrock/sdk.hpp` が唯一の `<bedrock/sdk.hpp>` です。
内部ヘッダーは `include/bedrock/detail/` や `include/bedrock/native/` にあり、SDK と第一者 Mod 専用です。

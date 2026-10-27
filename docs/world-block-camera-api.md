# World / Block / Camera API

型付きの新しい入口は [HTML APIガイド](site/index.html#world) と
[移行ガイド](typed-sdk-api.md) を参照してください。

## 設計

ModにRVA・vtableスロット・メモリオフセットを複製せず、SDKのアダプターと
バージョン別JSONへ集約します。今回追加したAPIは自動Hookを設置しません。
既存のSDK GameHooksが通知するローカルclientのLevelTick内で使用します。

| API | 用途 |
| --- | --- |
| `World.getBlock(pos)` / `NativeWorld.getBlock(pos)` | ID・位置・状態属性・空気判定の所有コピー |
| `World.setBlock(pos, id, options)` | ホットバーから一致するアイテムを選び、隣接支持面を探して配置 |
| `World.placeBlock(request)` | 面・支持座標・ヒット位置・向きを指定して配置 |
| `NativeWorld.capabilities()` | 読み取り・配置の利用可否。候補であることも明示 |
| `FreeCameraRig` | カメラ状態・RuntimeID/epochの照合・被弾時解除。native操作はホストが担当 |
| `moveCamera()` | yawに沿う前後左右・上下移動。斜め移動を正規化 |
| `interpolateCamera()` | コピー済み座標の補間 |
| `translateBounds()` | 座標差に合わせてAABBを移動 |
| `native_motion::capture/apply` | native Actorの位置・前位置・速度・AABB・落下距離の一括更新 |

`setBlock`は**手持ちアイテムでの配置**です。サーバーの地形を任意のIDで
書き換えるAPIではありません。アイテム不足・未ロードチャンク・支持面なし・
距離超過・状態不一致はエラーを返します。空気への置換や既存ブロックの破壊は
未対応です。これらの操作を成功として返すことはありません。

## 組み込み

Windows x64 / MSVC Releaseが必要です。FetchContentでSDKを取り込む前に:

```cmake
set(BEDROCK_SDK_ENABLE_NATIVE_WORLD ON CACHE BOOL "" FORCE)
# FetchContent_MakeAvailable(bedrock_sdk) 後
target_link_libraries(MyMod PRIVATE bedrock::world)
```

EnTTの互換バージョンもこのターゲットが提供します。
純粋な値型は `bedrock/blocks.hpp`、カメラ計算は `bedrock/camera.hpp` にあります。
これらだけを使用する場合、EnTTやWindows専用アダプターは不要です。

## 最小例

```cpp
#include <bedrock/native_world.hpp>

bedrock::NativeWorld world;
std::string error;
// 起動時。対象EXEの一致をこのメソッド自身で検証します。
// 現在のnative定義は候補なので、明示的opt-inが必要です。
if (!world.load(exactBuildProfileJson, true, error)) {
    // errorをログへ。使用不可の状態でnative関数を呼ばない。
}

// ローカルclientのLevelTickコールバック内:
{
    auto scope = world.tick(clientInstance, event.level, worldEpoch,
                            {.operations=256, .microseconds=500});
    if (!scope) return;
    auto block = world.getBlock({10, 64, 20});
    if (block) {
        // block.block.identifier / block.block.properties->values
        // コピーのみをworkerへ渡せる。
    }

    auto result = world.setBlock({10, 65, 20}, "minecraft:stone");
    if (result) {
        // native呼び出しで受付済み。後続tickのgetBlockで実配置を確認。
    }
}
// scope終了後はworld.getBlock/placeBlockがstaleWorldを返す。
```

`World`ファサードを使う既存GameAccess実装では `worldBlock` / `worldPlaceBlock` /
`worldSetBlock` を上のNativeWorldに転送してください。省略したアダプターは
`unsupported`を返します。公開Worldへ新しいnative依存は強制しません。

## 状態と向き

`Block.properties`は `map<string, variant<int,string>>` の読み取り専用共有コピーです。
ブロック状態IDのビットを推測せず、名前のある属性を利用します。

```cpp
bedrock::BlockSetOptions options;
options.yaw = 90.f;
options.pitch = 0.f;
options.expectedProperties = capturedBlock.properties;
auto result = world.setBlock(target, capturedBlock.identifier, options);
```

配置前の本体予測と属性が一致しなければ `stateMismatch` になります。
壁の接続状態など配置後に変化する属性もあるため、単純なsetBlockはこれを
無理に無視しません。SpeedBuilderの依存順序・部分一致・再確認のスケジューラーは
Mod側に残しています。高レベル配置はメインハンドのアイテムを選択して本体処理を
呼び、元の選択と回転を復元します。`accepted`はサーバー確定ではありません。

## スレッド・キャッシュ・予算

- `tick()`を最初に呼ぶのはゲームスレッドです。そのスレッドへバインドします。
- native読み取り・予測・書き込みは有効なTickScope内だけで行います。
- 同じ座標はtick内でキャッシュ。`getBlock(pos, true)`で再取得できます。
- ワールドepoch・Level・BlockSource・RuntimeIDの変化でnative状態キャッシュを破棄。
- nativeブロック状態/型キャッシュにも件数上限を設定。
- 予算を使い切ったら `budgetExceeded`。呼び出し側が次のtickへ繰り越します。
- 時間予算は協調的です。既に開始した本体関数を途中で中断する保証はありません。
- workerへ渡せるのはコピー済みのBlock/属性/座標だけ。nativeハンドルを渡さない。
- `stateIdentity`は同じワールド内の比較用トークンです。永続保存や別ワールドとの比較は禁止。
- load/profile変更・破棄は使用中のtick/worker呼び出しと並行させないでください。

`BlockError`は unsupported / wrongThread / staleWorld / budgetExceeded /
unavailable / invalidRequest / missingItem / stateMismatch / nativeRejected を区別します。

## バージョン対応の範囲

`native_world`は既存の解析済みSpeedBuilder関数・構造定義をSDK用に移したものです。
`native_motion`も姿勢更新の構造定義を共有します。JSONは機能別のセクションだけを持ち、
旧Mod名のセクションは廃止しました。旧キャッシュを取得したサンプルは、EXEの一致確認を
行ったうえで新しい埋め込み定義に切り替えます。

新しいゲーム版では、対応するプロファイルとエントリガード・ABIを確認して追加します。
JSONだけで吸収できないABI・EnTT・HitResult形状変更はアダプター更新が必要です。
未検証の版を互換として強行する実装にはしていません。既存candidateはcandidateのままです。

Freecamのパケット固定・RenderDragon固有Hook・所有権管理は引き続きMod側です。
今回共通化したのは移動計算・補間・native姿勢更新であり、地下透過が新たに
検証済みになったわけではありません。

### 再ビルド

`GameAccess`のC++仮想インターフェースにブロック用の拡張を加えています。
このインターフェースをDLL間で共有する場合は利用側も一緒に再ビルドしてください。
BedrockHookRuntimeのC ABIやバージョン番号は変更していません。

# BetterResourcePackManager（Windows 実験版 v10）

添付 Android ARM64 版 BetterResourcePackManager v2.3.1 の更新順序を、Minecraft **1.26.60.5 / Windows x64 / Release MSVC** 向けに移植した BedrockDynamicSDK のサンプルです。対応 EXE SHA-256 は `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a` です。

## 更新方法

Android 版では、リソースの再読込後に AppPlatform の停止・復帰通知を発行し、`BlockGraphics::initBlocks` でブロック描画定義を作り直します。古い描画定義への参照は `getForBlock` を通る時に無効化し、次のカメラ更新で `preDimensionChanged` を実行します。v10 は Windows の対応する候補関数を使ってこの順序に合わせています。

Windows では地形アトラスとリソースローダーの完了を待ち、既存の描画タスクを終了させてから開始します。対象ワールド・スレッド・カメラと、アプリの停止／復帰通知先を確認します。カメラ更新は次の描画フレームで実行します。Android 版の雲テクスチャ参照の更新は Windows では対応先を特定できておらず、まだ移植していません。

v9 のモデル再読込、クライアント定義通知、個別のビルダー再生成、ブロックテクスチャ単独更新、全チャンク再描画通知は v10 の経路から外しました。v8 の全ブロック内部データ再構築やフラグ書換えも使用しません。描画定義の参照無効化は全生成したオブジェクトの古い参照を外すために行います。性能向上や表示の正常化は実機未確認です。

## 実機確認

Minecraft を完全終了してから新しい DLL をロードしてください。ログ先頭に `adapter v10` と build-info.json のコミット番号が出たことを確認し、ワールド内の「設定 → グローバルリソース」でパックを切り替えて、同じワールドに戻ります。

ログの `terrain_atlas_completed`、`platform_cycles`、`block_graphics_rebuilt`、`cameras_refreshed` が増えることを確認できます。`graphics_cache_resets` は古い描画参照を外した数、`resource_wait_frames` は読み込み完了までの待機、`refresh_cancelled` はワールド変更による取消し、`failures` は SDK が検出した失敗の数です。各カウンターは表示が正常であることを保証しません。

**v10 のクラッシュ有無、モデルとカスタムブロックの表示、連続切替は実機未確認です。** 問題が起きた場合はログ末尾と、どの表示が崩れたかを確認してください。ネイティブ定義は candidate / runtime_verified: false のままです。

ログ: `Documents/PEXData/BetterResourcePackManager/BetterResourcePackManager.log`。
`BetterResourcePackManager_Status()` の 2 は Hook 登録済み、4 は初期化失敗です。

## ビルド・検証

```powershell
cmake -S exsample/BetterResourcePackManager -B build-brpm -A x64 "-DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK=$PWD" "-DBEDROCK_GAME_VERSION=1.26.60.5"
cmake --build build-brpm --config Release --parallel 2
ctest --test-dir build-brpm -C Release --output-on-failure
```

Windows CI はビルド・単体テスト・対象外プロセスへの DLL ロード拒否を確認します。Minecraft の実際の描画は実行しません。解析記録は開発ホストの `~/Documents/PEXData/REA/brpm-v9-android/` と `~/Documents/PEXData/REA/brpm-windows-1.26.60.5/` に保存しています。元の `.so` やゲーム本体は配布物に同梱しません。

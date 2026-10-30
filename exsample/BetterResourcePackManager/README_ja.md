# BetterResourcePackManager（Windows 実験版 v11）

Minecraft 1.26.60.5 / Windows x64 向け BedrockDynamicSDK サンプルです。対応 EXE SHA-256: `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`。

v10 は Android 版に合わせてアプリ全体へ停止・復帰を通知していました。利用者から、ローカルワールドでは維持される一方、外部サーバーでは退出と再接続が発生すると報告されました。ログでも通知直後に `refresh_cancelled=1` となり、ブロックとカメラの更新は実行されませんでした。

v11 はアプリ全体への停止・復帰通知を外し、以前の Windows 版で実行されたモデル・クライアント定義の更新と、描画側だけの復帰処理を使います。Android 版と同じブロック描画定義の再生成、古い描画参照の無効化、後続フレームのカメラ更新を組み合わせています。地形アトラスとローダーの完了待ち、ワールドの同一性確認、描画タスクの完了待ちは継続します。Android 版の雲テクスチャ参照更新は未移植です。

**v11 の外部サーバー接続維持、カスタムブロックやプレイヤーモデルの表示、連続切替は実機未確認です。** ネイティブ定義は candidate / runtime_verified: false のままです。

Minecraft を完全終了してから DLL を読み込み、ログ先頭の `adapter v11` を確認してください。切替後は `geometry_reloaded`、`client_definitions_reloaded`、`block_graphics_rebuilt`、`level_builders_recreated`、`clients_refreshed`、`cameras_refreshed` を確認できます。`graphics_cache_resets` は古い描画参照の無効化数、`refresh_cancelled` は確認時にワールドなどが変わり更新を中止した数、`failures` は SDK が検出した失敗の数です。これらの数値は見た目の正常さや接続維持を保証しません。

ログ: `Documents/PEXData/BetterResourcePackManager/BetterResourcePackManager.log`。

```powershell
cmake -S exsample/BetterResourcePackManager -B build-brpm -A x64 "-DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK=$PWD" "-DBEDROCK_GAME_VERSION=1.26.60.5"
cmake --build build-brpm --config Release --parallel 2
ctest --test-dir build-brpm -C Release --output-on-failure
```

Windows CI はビルド・単体テスト・対象外プロセスへの DLL ロード拒否を確認します。Minecraft の描画やオンライン接続は実行しません。

## クラッシュ診断版

このビルドは v11 と同じ更新処理を使い、各ネイティブ呼出しの直前・直後を `Documents/PEXData/BetterResourcePackManager/BetterResourcePackManager-stage.log` に即時書き込みます。クラッシュの解消版ではなく、同じ操作で再びクラッシュする可能性があります。ログの末尾を確認し、再起動前に控えてください。次回起動時にはログを上書きします。

# BetterResourcePackManager（Windows 最適化版 v9）

添付 Android ARM64 版 BetterResourcePackManager v2.3.1 を再解析し、
Windows 側の追加処理を整理した BedrockDynamicSDK のサンプルです。
対応は Minecraft **1.26.60.5 / Windows x64 / Release MSVC**。
EXE SHA-256: `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`。

## v8 から削除した処理

- 全ブロックの列挙、内部フラグ書換え、クライアント描画データの再生成。
- 材質リポジトリ・BlockGeometry グループの強制初期化。
- `BlockGraphics::initBlocks` による描画オブジェクトの全再生成。
- `getForBlock` と共有読み取り関数のキャッシュ無効化 Hook。
- 通過回数を記録するだけのリロード入口・言語サブパック更新 Hook。
- プレイ前判定カウンターが増えるだけで繰り返していたログ出力。

Hook は **14 個から 10 個**になります。
全ブロック列挙と Mod 側の描画キャッシュ書換えはありません。
処理量を削減した版であり、実機の FPS・切替時間の改善量は未計測です。

## Android 版との比較

再添付ファイルは以前のパックと SHA-256 が一致しました。
元 Mod の完了 Hook はテクスチャ更新後に `initBlocks` を呼び、
`getForBlock` Hook は古い ClientData 内の描画参照を外します。
カメラ更新時には雲の参照を交換し、`preDimensionChanged` を呼びます。
v8 の全ブロック内部データ再生成に相当する処理は確認できませんでした。

Windows には既存 BlockGraphics を保持してテクスチャ参照を更新する
`reloadBlockTextures` があり、標準のアトラス更新経路でも使われます。
v9 はこの処理を使うため、オブジェクト全再生成と、それに伴う古い参照の無効化を省きます。
Windows のカメラのディメンション変更処理はワールド参照の切離しも行うため、
そのままプレイ中に呼ばず、以前の実機で実行された標準の描画ビルダー再生成経路を残しています。

## 残した更新経路

標準 UI でグローバルパックを編集し、適用イベント中だけ SDK が対象クライアントの
プレイ前判定とグローバルスタック包含を調整します。対象コールバックの所有権はゲーム側の方式で処理します。

地形アトラスとローダーの完了後、同じワールド・同じスレッドのフレーム開始時に一度だけ、
描画タスクの完了待ち、モデル再読込、クライアント定義の更新、ブロックのテクスチャ参照更新、
描画ビルダーの再生成、再描画要求を実行します。
v6 でプレイヤーモデルが直ったと報告された Windows 用の復旧処理を維持しています。
待機中にワールドが変われば取り消し、生のクライアント・プレイヤー参照を非同期保持しません。
ネイティブ関数・ABI・offset・ガードは SDK と版別 JSON が管理します。

## 実機確認

Minecraft を完全終了し、新しい DLL を一度だけロードしてください。
旧 DLL の Hook はプロセス終了まで残るため、起動中の差替えはできません。
標準の「設定 → グローバルリソース」で変更して、同じワールドへ戻ります。

ログ先頭が `adapter v9` と配布物の build-info.json のコミット番号に一致することを確認します。
`terrain_atlas_completed` → `geometry_reloaded` / `client_definitions_reloaded` →
`block_textures_reloaded` / `level_builders_recreated` / `clients_refreshed` を確認できます。
`resource_wait_frames` は完了待ち、`refresh_cancelled` はワールド変更による取消し、
`failures` は SDK が検出した失敗の数です。カウンターは表示の正しさを保証しません。
削除した `graphics_cache_resets`・`custom_resources_rebuilt`・`block_client_data_rebuilt`・
`block_graphics_rebuilt`・`reload_calls`・`language_subpacks_updated` は出力しません。

**v9 のクラッシュ解消・カスタムブロックの表示・複数回切替は実機未確認です。**
v8 のログは全再生成の後、追加した内部データ再構築の完了前に止まっていますが、
正確な障害命令はクラッシュダンプがないため断定しません。
版別定義は `candidate` / `runtime_verified: false` を維持します。

ログ: `Documents/PEXData/BetterResourcePackManager/BetterResourcePackManager.log`。
`BetterResourcePackManager_Status()` の 2 は Hook 登録済み、4 は初期化失敗です。

## ビルド・検証

```powershell
cmake -S exsample/BetterResourcePackManager -B build-brpm -A x64 "-DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK=$PWD" "-DBEDROCK_GAME_VERSION=1.26.60.5"
cmake --build build-brpm --config Release --parallel 2
ctest --test-dir build-brpm -C Release --output-on-failure
```

出力: `build-brpm/Release/BetterResourcePackManager.dll`。
Windows CI はビルド・単体テスト・対象外プロセスへの DLL ロード拒否を確認します。
Linux では画面判定・文字列所有権・コールバック所有権・完了待ち状態のテストを実行します。
Minecraft の実際の描画は CI では実行しません。

解析記録は開発ホストの `~/Documents/PEXData/REA/brpm-v9-android/` と
`~/Documents/PEXData/REA/brpm-windows-1.26.60.5/` に保存しています。
入力パック SHA-256: `0995969634128e548ac912f54d92e8b22f99badb0d015ec827a1fbd56a6ac4ee`。
元の `.so` やゲーム本体は配布物に同梱しません。

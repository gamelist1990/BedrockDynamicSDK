# BetterResourcePackManager Windows 実験版 v13

Minecraft 1.26.60.5 / Windows x64 用。対応 EXE SHA-256: `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`。

v12 で外部マルチプレイ時の切断・再接続（refresh_cancelled）が解消され、バニラブロックの更新とカメラ再バインドが通ることを実機ログで確認しました。

**v13 では、v12 のライフサイクル抑制・接続維持を維持したまま、以下の描画修復を追加しました：**
1. **プレイヤーモデルおよび手の復帰**:
   `MinecraftGame::reloadAnimationAndGeometryData` (`geometryReload`) によるモデル・アニメーション・レンダラー再バインド、および `MinecraftGame::_notifyClientResourcePacksChanged` (`notifyClientResources`) によるリソース定義グループ更新。
2. **カスタムブロックの描画・アトラスUV修復**:
   `BlockGeometry::Group::initialize`、`ClientBlockPipeline::MaterialRepository::initialize` によるジオメトリと材質（アトラスUV）の初期化、および `LevelRenderer::_initializeBlockTypeClientData`、`BlockTypeRegistry::forEachBlock` による各ブロックのクライアントデータ再構築。

Minecraft を完全終了し DLL を入れ替えた後、ログ先頭の `adapter v13` を確認します。外部サーバーでパック切り替え後、プレイヤーの手・モデルの表示、カスタムブロックのテクスチャ/モデルの正常化を確認してください。
ログに `geometry_reloaded=1`、`client_definitions_reloaded=1`、`custom_resources_rebuilt=1`、`block_client_data_rebuilt` が記録されます。

`Documents/PEXData/BetterResourcePackManager/BetterResourcePackManager-stage.log` に処理段階を即時保存します。通常ログは同じフォルダーの `BetterResourcePackManager.log` です。

# BetterResourcePackManager Windows 実験版 v12

Minecraft 1.26.60.5 / Windows x64 用。対応 EXE SHA-256: `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`。

ローカルで動作したと報告された v10 のアプリ通知・BlockGraphics 再生成・カメラ更新の順序を基準とします。外部マルチプレイと判断した場合だけ、Mod が自ら発行する同期的な停止・復帰通知の間、対象 MinecraftGame の3つのライフサイクル通知を通さず、他の通知先は通常通り動かします。実際のOSの停止・復帰通知は通します。ローカルではフィルターをかけません。

**v12 は実験版で、外部サーバーでの接続維持、クラッシュ有無、カスタムブロックとモデルの表示、ローカルワールド動作は未検証です。** 選択したゲーム側通知が再接続の唯一の原因であることも未確認です。Android 版にある雲テクスチャ参照更新は未移植です。

Minecraft を完全終了し DLL を入れ替えた後、ログ先頭の `adapter v12` を確認します。まず外部サーバーでパックを切り替え、退出・再接続の有無、モデルとカスタムブロックを確認します。`remote_reloads`、`game_lifecycle_skipped`、`platform_cycles`、`block_graphics_rebuilt`、`cameras_refreshed` と `refresh_cancelled` を確認できます。これらは表示や接続が正常であることを保証しません。

`Documents/PEXData/BetterResourcePackManager/BetterResourcePackManager-stage.log` に処理段階を即時保存します。`mode_remote_filtered` と `skip_game_*` はこの操作のゲーム本体通知の抑制、`mode_local_unfiltered` は通常の通知を表します。Minecraft の次回起動時にこのログは上書きされるので、クラッシュ後は再起動前に内容を控えてください。通常ログは同じフォルダーの `BetterResourcePackManager.log` です。

Windows CI はビルド・単体テスト・対象外プロセスへの DLL ロード拒否を確認します。ゲーム実行やネットワークの実機確認は含みません。

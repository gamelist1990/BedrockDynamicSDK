# BetterResourcePackManager Windows 実験版 v14

Minecraft 1.26.60.5 / Windows x64 用。対応 EXE SHA-256: `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`。

v13 の利用者ログでは外部サーバー向けの通知抑制、モデル・クライアント更新、ブロック描画定義の再生成が戻った後、カスタムブロックの内部データ再構築中にログが途切れています。最後の定期ログには `block_client_data_rebuilt=34` がありました。障害命令や障害スレッドは未特定です。

v14 は、その未検証の全ブロック内部データ再構築とフラグ書換えを取り除きます。既存のリソース更新、接続維持のための通知抑制、モデル・カメラ更新は残します。変更によってクラッシュが解消するかは実機未確認です。**カスタムブロックの表示修正は未完了で、正常な表示を保証できません。**

Minecraft を完全終了して DLL を入れ替え、ログ先頭の `adapter v14` を確認します。パックを切り替え、クラッシュ・再接続の有無、プレイヤーモデルとカスタムブロックの表示を確認してください。通常ログは `Documents/PEXData/BetterResourcePackManager/BetterResourcePackManager.log`、逐次ログは同じフォルダーの `BetterResourcePackManager-stage.log` です。後者は次回 Minecraft 起動時に上書きされます。

逐次ログには `custom_rebuild_disabled`、`camera_pending`、`camera_rebind begin/end` などが記録されます。カウンターやログは描画結果の正常性を保証しません。Windows CI はビルドと単体テストを行いますが、ゲーム内の描画や外部サーバーへの接続は実行しません。

# BetterResourcePackManager Windows 実験版 v15

Minecraft 1.26.60.5 / Windows x64 用。対応 EXE SHA-256: `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`。

Android版の停止・復帰、ブロック描画初期化、次フレームのカメラ更新に近い v10 の流れを使用します。外部サーバーでは、更新中の停止処理内の２か所に限り、セッション判定の候補関数を真にします。候補がAndroid版の教育版マルチプレイ判定と同じかは未確認です。通常の停止・復帰通知には影響しません。

**実験版です。再接続の防止、カスタムブロックとプレイヤーの正常表示、クラッシュが起きないことはゲーム内で未検証です。** v14 で観測されたクラッシュの原因も特定できていません。

Minecraft を完全終了して DLL を入れ替え、ログ先頭の `adapter v15` を確認してください。通常ログは `Documents/PEXData/BetterResourcePackManager/BetterResourcePackManager.log`、逐次ログは同じフォルダーの `BetterResourcePackManager-stage.log` です。逐次ログは次回起動時に上書きされます。

外部サーバーで `suspend_gate_overrides` が増えれば候補分岐を通っています。これは接続維持や描画の正常性を保証するものではありません。Windows CI はビルドと自動テストを行いますが、Minecraft 内での動作は検証しません。

# BetterResourcePackManager Windows 実験版 v16

Minecraft 1.26.60.5 / Windows x64 用。対応 EXE SHA-256: `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`。

v15 の利用者テストでは再接続表示とクラッシュがなく、プレイヤーモデルとカスタムブロックは正常に表示されませんでした。v16 は v15 の接続維持処理に加え、v6 でプレイヤーモデルが復帰した際のジオメトリ再読込とクライアント通知を戻します。Android版とWindows側候補判定の同一性は未確認です。

**実験版です。モデル修復と接続維持はゲーム内で未検証で、カスタムブロックの修正は未完了です。** 今回戻した２処理はクラッシュした v13/v14 にも含まれていますが、クラッシュした処理は特定できていません。

Minecraft を完全終了して DLL を入れ替え、ログ先頭の `adapter v16` を確認してください。通常ログは `Documents/PEXData/BetterResourcePackManager/BetterResourcePackManager.log`、逐次ログは同じフォルダーの `BetterResourcePackManager-stage.log` です。逐次ログは次回起動時に上書きされます。

外部サーバーで `suspend_gate_overrides` が増えれば候補分岐を通っています。これは接続維持や描画の正常性を保証するものではありません。Windows CI はビルドと自動テストを行いますが、Minecraft 内での動作は検証しません。

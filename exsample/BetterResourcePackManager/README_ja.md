# BetterResourcePackManager Windows 診断版 v17

Minecraft 1.26.60.5 / Windows x64 用。対応 EXE SHA-256: `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`。

v16 の利用者ログは `camera_rebind end` まで進んでいますが、クラッシュした命令やスレッドは不明です。v17 は更新処理を変更せず、診断ログに時刻、スレッド、設定終了・リソース更新の前後、および取得できた一部のWindows例外情報を記録します。

**v17 は診断版であり、クラッシュやカスタムブロックを修正する変更は含みません。** Windows例外ログの `first_chance` は回復する例外も含み、クラッシュを意味するとは限りません。fail-fastを含む一部の終了は観測されない場合があります。

Minecraft を完全終了して DLL を入れ替え、ログ先頭の `adapter v17 diagnostic` を確認してください。通常ログは `Documents/PEXData/BetterResourcePackManager/BetterResourcePackManager.log`、逐次ログは同じフォルダーの `BetterResourcePackManager-stage.log` です。逐次ログは次回起動時に上書きされます。

外部サーバーで `suspend_gate_overrides` が増えれば候補分岐を通っています。これは接続維持や描画の正常性を保証するものではありません。Windows CI はビルドと自動テストを行いますが、Minecraft 内での動作は検証しません。

# ネイティブ定義の構造

編集元は`data/<version>/windows-x64/manifest.json`と、その`parts`が参照するクラス別JSONです。
旧windows-x64.jsonは廃止しました。

- `symbols`：安定した`Class::function`識別名、版別RVA・signature・入口ガード・ABI・検証範囲。
- `layouts`：Minecraftクラスのoffset・vtable・構造体の制約。
- `analysis`：入口照合、実機観測、未解決理由。実行時の取得対象外。
- `protocol`：data内の完全なupstream protocol.jsonのパス・commit・SHA。

ModはSDKの型付きAPIを使い、アドレスやoffsetを保持しません。
バージョン選択と生成ヘッダーはCMakeがdataから作ります。

[構造と更新コマンド](sdk-structure.md) · [探索順序と検証](profile-maintenance.md) · [現状](../data/1.26.60.5/README.md)

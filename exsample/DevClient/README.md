# DevClient

SDK 開発・検証用サンプルの置き場です。単一 DLL ではなく、必要なツールだけを個別にビルド・ロードします。

| ツール | 用途 |
| --- | --- |
| [CheckSig](CheckSig/README_ja.md) | プロファイル・シグネチャ・実行時観測の診断 |
| [BlockProbe](BlockProbe/) / [CraftProbe](CraftProbe/) / [InputProbe](InputProbe/) / [StackProbe](StackProbe/) | ブロック／クラフト／入力／アイテムスタックの観測 |
| [Fake](Fake/README_ja.md) / [Top](Top/README.md) | クライアントコマンド・ワールド API のサンプル |
| [GlobalResourcePack](GlobalResourcePack/README_ja.md) | グローバルリソースの適用例 |
| [ModSettingsDemo](ModSettingsDemo/README.md) | OreUI Mod Settings の全コントロール例 |
| [ClientInfoProbe](ClientInfoProbe/README.md) | `player.fps` / `ping` / `cps` / `device` を ContentLog へ定期出力する診断 Mod |

SDK ルートの `./build-linux.sh` は最新対象版で全ツールをビルドします。個別ビルドの場合は `exsample/DevClient/<名前>` を CMake のソースディレクトリに指定してください。

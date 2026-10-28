# Windows x64プロファイル

`data/<version>/windows-x64.json` が唯一の配布プロファイルです。

| 項目 | 内容 |
| --- | --- |
| schema | ファイル形式。現在1 |
| target | バージョン、SHA-256、PEの範囲 |
| symbols | Minecraft関数名ごとのRVA・シグネチャ・入口ガード |
| layouts | Minecraftクラス名ごとのオブジェクトレイアウト |
| analysis | 確認の範囲・出典の要約（存在する場合） |

`layouts` には `Actor`、`ItemStack`、`BlockSource`、`PacketSender`、
`CommandRegistry`、`GuiData`、`ClientInstance`、`CommandRequestPacket` などを置きます。
各クラスの内容は既存アダプターで使うフィールドです。
共有パケット経路はPacketSender、要求・応答の組み立てはCommandRequestPacketにまとめます。
互換読み込みのため、SDK内部では従来の区分へ変換します。
既存のschema 1ファイルの `native_*` 形式も引き続き読み込めます。
同じレイアウトを新旧の両形式で重複定義するとエラーになります。

`status: verified` の意味は `verification_scope` と合わせて確認してください。
`entry-guard` は入口一致、`runtime-hook` は観測Hookまで確認した状態です。
`abi_verified`・`runtime_verified` は独立しています。
関数入口の一致だけでオブジェクトのオフセットや仮想関数スロットを確定しません。

1.26.60.5には未完成のレイアウトがあります。必要なクラスがない場合、
SDKは `missing Minecraft layout: <クラス・関数名>` を返します。
古いプロファイルのオフセットを新しいEXEに代用する処理はありません。
重複していた候補履歴の原本はPEXData/REAの解析スナップショットに保存しています。

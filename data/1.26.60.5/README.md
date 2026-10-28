# 1.26.60.5 Windows x64 — 調査中

`windows-x64.json` はEXE識別情報と解析途中の候補を収録した定義です。
`symbols` は空で、候補は `analysis` に保持しています。マニフェストには
EXE識別用に登録していますが、Modが動作するためのシンボルやnativeレイアウトは未完成です。
`windows-x64.candidate.json` は調査の原本で、自動フック対象として使わないでください。

- 元のパッケージ表記: `1.26.6005.0`
- EXE VERSIONINFOのFileVersion/ProductVersion: `1.26.60.5`
- EXE SHA-256: `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`
- `.text` SHA-256: `7ca5d1baef049979e9785d5bbafe7100a829e1107a3d374975962cc64ecc76fe`
- 旧版アンカー移植とカーソル関数の構造比較によるシンボル候補: 11件
- ワールド遷移・ローカルPlayer・BlockSource取得関数のvtable候補を4件記録（実行時の動作は未確認）
- ActorTypeComponentの識別子を参照する関数の旧版・新版比較を記録（構造全体は未確認）
- ワールド描画のvertex候補を比較。色設定では旧SDKのポインタ渡しと異なる引数形式の候補が見つかり、互換性は未確定
- 他のネイティブ関数候補: `analysis.native_function_candidates` に記録
- ScanClientで同じEXEの静的セクションを取得し、BlockSource候補のvtableポインタを照合済み
- 旧版の未確認ネイティブ構造、vtable位置、オフセットは引き継いでいません。

Ghidraで `Level::getRuntimeActorList` 候補を旧版と比較すると、返却バッファと
ポインタ列挙の流れは似ていますが、所有オブジェクト内の参照位置は旧版の
`+0x4B0` から `+0x4B8` に変化しています。新しいvtableとActor型の確定、
動作時の検証は未完了です。`MultiPlayerLevel::_subTick` 候補も旧版と制御フローを
比較していますが、実行時の呼び出しは確認していません。

BlockSourceのgetBlock候補は `0x32CE440`、関連vtableのslot 2とchunk取得slot 41を
旧版と新EXE、およびScanClientの読み込み済み.rdataで照合しました。
SubChunk配列の開始・終了参照は旧版の `+0x140/+0x148` から新版の
`+0x1C8/+0x1D0` へ変わっています。実オブジェクトと呼び出しは未確認です。

カーソル操作、ワールド遷移、ActorとPlayerの各コンポーネント、アイテム、
ブロック／SubChunk、ワールド描画、パケット、コマンド、カメラなどには、
このEXEに対する追加検証が必要です。提供済みの1.26.52.3向けMod DLLが
そのまま動作することは確認されていません。

解析記録のEXEハッシュと関数範囲・REA/GhidraのEvidenceはローカルの
`Documents/PEXData/REA/1.26.6005.0/` に保存しています。ゲーム中のScanClient
出力のEXEハッシュと読み込み済み.rdataは照合済みです。候補の意味・寿命・呼び出し経路を確認してから
実行用プロファイルへ移します。

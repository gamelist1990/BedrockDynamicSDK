# 1.26.60.5 Windows x64 — 調査中

`windows-x64.json` はEXE識別情報と41件の診断エントリーを収録した定義です。
旧版アンカーで移植したトップレベル/native内関数と、既存の手調査候補を
`symbols` からCheckSigで個別に確認できます。31件の移植候補のうち20件は既存RVAと一致し、
10件を追加しました。`ClientInstance::onPlayerLoaded` はRVA候補が異なるため、
既存定義を保ったまま `candidate::cross_version_port::` に別候補を保存しています。
旧版アンカーからの生の候補一覧は `windows-x64.candidate.json` です。
すべて `candidate`、`abi_verified:false`、`runtime_verified:false` のままです。
native keyboardのイベントレイアウトもcandidateとして登録しています。全Modに必要な
nativeレイアウトは未完成なので、この定義だけで全SDK機能が使える状態ではありません。
Actor / Item / Motionのnative layoutは1.26.52.3から候補として移し、このEXEに対する
静的証拠と旧版由来の値をプロフィール内で区別しています。値は `candidate` /
`runtime_verified:false` のままです。CheckSigやAllowCandidates対応Modから試行できますが、
実ゲームでの値・寿命・ABIは確認できていません。

Windows上で現在のEXEへ旧signatureを当て、ヒット関数・周辺参照・読み取り値を調べる場合は
[CheckSig](../../exsample/CheckSig/README_ja.md) を使います。`list` で全候補の入口ガード、
`.pdata` 関数開始、HookPoint対応を一覧し、`probe <symbol>` で周辺情報を確認します。
`Level::getRuntimeActorList`を含むABI既知候補は、正確なEXE一致・入口ガード・`.pdata`境界を確認したうえで
`hook <symbol>` を明示すると、元処理を通す観測フックを試せます。`onPlayerLoaded` は
`.pdata`関数開始が見つかっていないため、CheckSigはフックを拒否します。
観測結果であっても自動で `verified` へ昇格させません。

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

`candidate::ActorTypeComponent` のGhidra証拠（`actor-type-candidate-ghidra.json`）は、
RVA `0xB94CA0` の関数がActorTypeComponent hash `0x4F6BA419` を解決し、32-bit値を
offset `0` から読み、319と比較することを示します。同じコードはActor+`0x10`からregistry、
Actor+`0x18`からentity idを読みます。関数引数をActor*とみなすこと、およびそのActorが
`Level::getRuntimeActorList`の要素と同じ型であることは推定です。CheckSigはこの2値を
読み取り専用で記録しますが、registryやコンポーネントの内部には触れません。
プロファイル上もcandidateのままで、runtime verificationはありません。

`native_actors`では新版EXEのActorType候補関数 (RVA `0xB94CA0`) が
`0x4F6BA419`を解決してoffset `0`の32-bit値を319と比較し、別の新版関数
(RVA `0x3A11540`) はRuntimeID / UniqueID / ActorType / StateVector / AABB /
Rotationのcomponent hashを参照します。新版`.text`内でそれらのhashを確認しています。
Actor contextについても候補関数が引数+`0x10`と+`0x18`を読むコードを示しますが、
引数がActor*であること、およびActor一覧の要素との同一性は推定です。
残りのcomponent storage/value offsetsは1.26.52.3から移した未検証候補です。

`native_items`はEquipment hashの新版`.text`内出現と、ClientInstanceのlocal-player
getter候補 (table slot 34) を静的証拠として記録し、その他のItemStack / Inventory /
Supplies offsetsは旧プロフィール由来の候補として保持しています。`native_motion`の
StateVector / FallDistance offsetsも旧版由来です。どの項目も動作時のread/writeを
検証していません。候補を有効にするとModが処理を試しますが、実行できる保証にはなりません。

`native_camera`にも新版のlocal-player / update-player / gamma / game-type setter候補を
接続しました。Camera object offsetsは旧版由来の候補です。新しい関数開始位置が見つからない
culler setter、inside-cube renderer、view factory、spectator predicateは呼び出し先として
登録せず、CheckSigのsymbol候補と解析記録だけに残しています。描画・Camera機能全体は未完成です。

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
出力のEXEハッシュと読み込み済み.rdataは照合済みです。候補の意味・寿命・呼び出し経路を
確認してから、候補を更新またはverifiedへ昇格します。

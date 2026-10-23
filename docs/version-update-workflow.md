# Minecraft Bedrock クライアント・バージョン更新手順

この文書は、新しい `Minecraft.Windows.exe` を受け取った後に、Ghidra、
Bedrock Lens、`bedrock-symbols` を使ってシンボル候補を移植し、
`data/<version>/windows-x64.json` と `data/manifest.json` を安全に更新する手順です。

## 自動化できる範囲

自動処理で次の情報を取得できます。

- EXE全体と `.text` のSHA-256
- PEイメージサイズ、`.text` RVAとサイズ
- x64 `.pdata` に記録された関数境界
- 旧プロファイルのmasked AOBが新EXEで一意にヒットする位置
- 新しいRVA、可変値を `??` にしたsignature、entry check
- Ghidraが検出した関数、参照文字列、関数サイズ
- BSimによる旧版と新版の構造的類似候補
- MSVC RTTI型名

一意のAOBまたはBSim候補だけでは、関数の意味、引数、戻り値、メンバー配置は
証明されません。自動移植したシンボルは必ず `candidate` のまま生成し、ABI確認後に
必要なものだけ `verified` へ昇格します。

## ローカル構成

このHomePCでは次の構成を使用します。

```text
/home/pexserver/.local/share/ghidra/12.1.4
/home/pexserver/.local/bin/ghidraRun
/home/pexserver/.local/bin/analyzeHeadless
/home/pexserver/WorkSpace/DevBox/BedrockDynamicSDK
└── third_party/bedrock-lens
```

Ghidraプロジェクトはドットで始まるディレクトリ内に置かないでください。
Ghidra 12.1はそのようなローカルプロジェクトパスを拒否します。

## 1. 初回セットアップ

SDKとLensのPython環境を準備します。

```bash
cd /home/pexserver/WorkSpace/DevBox/BedrockDynamicSDK
uv sync --extra dev

cd third_party/bedrock-lens
uv sync --extra analysis
```

LensがGhidraを検出できることを確認します。

```bash
uv run python -c \
  "from bedrock_lens.ghidra import find_ghidra_install, GhidraBackend; p=find_ghidra_install(); print(p); print(GhidraBackend(p, 'ghidra-projects').version)"
```

期待される出力は、インストール先とバージョンです。

```text
/home/pexserver/.local/share/ghidra/12.1.4
12.1.4
```

## 2. 更新用パスを決める

以下は例です。`NEW_VERSION` と `NEW_EXE` を実際の値へ変更します。
現在のCLIはEXEのVERSIONINFOからバージョン文字列を自動採用しないため、
`NEW_VERSION` は明示してください。

```bash
export SDK_ROOT=/home/pexserver/WorkSpace/DevBox/BedrockDynamicSDK
export OLD_VERSION=1.26.52.3
export NEW_VERSION=1.26.60.1
export OLD_EXE="$SDK_ROOT/local-binaries/$OLD_VERSION/Minecraft.Windows.exe"
export NEW_EXE="$SDK_ROOT/local-binaries/$NEW_VERSION/Minecraft.Windows.exe"
export UPDATE_WORK="$SDK_ROOT/version-update-work/$NEW_VERSION"
export LENS_ROOT="$SDK_ROOT/third_party/bedrock-lens"
export LENS_DB="$SDK_ROOT/lens-data/lens.db"
export GHIDRA_PROJECTS="$SDK_ROOT/ghidra-projects"
export BSIM_DB="$SDK_ROOT/bsim/lens"
```

MinecraftのEXEはリポジトリへコミットしません。`local-binaries/`、Lens DB、
Ghidraプロジェクト、BSim DBも解析用のローカル成果物です。

## 3. 新EXEの身元とPE構造を確認する

```bash
cd "$SDK_ROOT"
uv run bedrock-symbols inspect "$NEW_EXE"
```

最低限、以下を確認します。

- x64 AMD64 PEである
- `.text` が存在する
- `sha256` と `text_sha256` が出力される
- `pdata_function_count` が0ではない
- 暗号化されたStore上のプレースホルダーではなく、先頭が `MZ` の解析可能なEXEである

同じバージョン表記でもEXEハッシュが異なる場合は、同じプロファイルとして公開しません。
manifest schema v1は1バージョン／1プラットフォームにつき1つの実行ファイルを扱います。

## 4. 旧signatureを新EXEへ自動移植する

最初に高速なmasked AOB移植を実行します。

```bash
cd "$SDK_ROOT"
uv run bedrock-symbols port "$NEW_EXE" \
  --old "data/$OLD_VERSION/windows-x64.json" \
  --version "$NEW_VERSION" \
  --output "$UPDATE_WORK/ported.json"
```

この処理は各旧signatureを新EXEの `.text` 全体から検索します。

- 1件だけヒット: 新RVAから新しいsignatureとentry checkを生成
- 0件ヒット: warningsへ記録
- 複数ヒット: 曖昧なのでwarningsへ記録
- 関数境界外: 採用しない、または要確認

生成された全シンボルは `candidate` です。過去バージョンの `verified` 状態を
自動継承しないのは意図した安全動作です。

## 5. Latite由来パターンとRTTIを併用する

SDKにあるLatite参照から、別経路の候補と解析メタデータを生成します。

```bash
cd "$SDK_ROOT"
uv run bedrock-symbols client-profile "$NEW_EXE" \
  --latite-addresses third_party/latite-reference/src/mc/Addresses.h \
  --latite-repo third_party/latite-reference \
  --version "$NEW_VERSION" \
  --output "$UPDATE_WORK/latite-profile.json"
```

`ported.json` と `latite-profile.json` のRVAが一致する候補は強い補助証拠になりますが、
それだけでABI検証済みにはしません。`latite-profile.json` の
`analysis.layout_hints` は歴史的な参考値であり、ランタイムでは使用しません。

## 6. Bedrock Lensへ旧版と新版を登録する

```bash
cd "$LENS_ROOT"

uv run lens --database "$LENS_DB" add "$OLD_EXE" \
  --version "$OLD_VERSION" --kind client

uv run lens --database "$LENS_DB" add "$NEW_EXE" \
  --version "$NEW_VERSION" --kind client

uv run lens --database "$LENS_DB" list
```

出力された旧版と新版の数値 `id` を控えます。以下では例として
`OLD_ARTIFACT_ID=1`、`NEW_ARTIFACT_ID=2` とします。同じSHA-256のEXEを再登録した場合は
既存artifactが再利用されます。

## 7. Ghidra解析とBSimインデックスを作る

初回解析はMinecraftクライアントのサイズにより長時間かかります。
プロジェクトと解析結果はEXEのSHA-256単位で再利用されます。

```bash
cd "$LENS_ROOT"

uv run lens --database "$LENS_DB" analyze 1 \
  --project-dir "$GHIDRA_PROJECTS" \
  --bsim-database "$BSIM_DB"

uv run lens --database "$LENS_DB" analyze 2 \
  --project-dir "$GHIDRA_PROJECTS" \
  --bsim-database "$BSIM_DB"
```

解析中断後は同じコマンドを再実行できます。GhidraプロジェクトはSHA-256で分離されます。

## 8. AOBで見つからなかった関数をBSimで探す

旧プロファイルに記録された旧RVAを使い、構造的に似た新版関数を検索します。

例として、旧版 `MouseDevice::feed` が `0x361AD0` の場合:

```bash
cd "$LENS_ROOT"
uv run lens --database "$LENS_DB" match 1 0x361AD0 \
  --project-dir "$GHIDRA_PROJECTS" \
  --bsim-database "$BSIM_DB" \
  --min-similarity 0.7 \
  --min-significance 0.0
```

候補は次の順で評価します。

1. 対象artifact IDが新版EXEである
2. BSim similarityとsignificanceが他候補より明確に高い
3. 新RVAが `.pdata` の関数開始位置である
4. 参照文字列、定数、呼び出し元・呼び出し先が旧版と一致する
5. デコンパイル結果の引数使用と戻り値が期待ABIに一致する

新版候補をデコンパイルする例:

```bash
uv run lens --database "$LENS_DB" decompile 2 0xNEW_RVA \
  --project-dir "$GHIDRA_PROJECTS"
```

`0xNEW_RVA` は実際のRVAへ置き換えます。BSimの最高得点だけを理由に採用しません。

## 9. Lens解析結果からsignature候補を生成する

Lens DB内で意味のある名前が付いた新版関数は `lens-import` で取り込めます。

```bash
cd "$SDK_ROOT"
uv run bedrock-symbols lens-import "$NEW_EXE" \
  --database "$LENS_DB" \
  --artifact-id 2 \
  --version "$NEW_VERSION" \
  --kind client \
  --limit 500 \
  --output "$UPDATE_WORK/lens-profile.json"
```

Ghidraが `FUN_...` とした匿名関数は標準では除外されます。BSimで意味を確認した関数は、
最終プロファイルへ正しいシンボル名、RVA、検証根拠とともに反映します。

## 10. 最終プロファイルを作る

最終ファイルは次のパスです。

```text
data/<NEW_VERSION>/windows-x64.json
```

`ported.json` を基本に、LatiteとLensで補完した結果を統合します。各シンボルには最低限、
次のフィールドを残します。

```json
{
  "rva": "0x...",
  "signature": "... ?? ...",
  "entry_check": "...",
  "cross_version_signature_available": true,
  "status": "candidate",
  "source": "pattern-port:<OLD_VERSION>",
  "function_boundary": true,
  "signature_length": 32,
  "category": "input",
  "confidence": "unique-AOB-and-pdata-boundary",
  "abi_verified": false
}
```

ABIを確認した関数だけ、根拠とprototypeを明記して次のように昇格します。

```json
{
  "status": "verified",
  "abi_verified": true,
  "source": "new-version callsites, decompiled control flow and live smoke test",
  "abi": "bool __fastcall(int InputKey, bool down)"
}
```

### 現在のexampleでABI確認が必須のシンボル

| シンボル | 使用箇所 | 必要状態 |
|---|---|---|
| `Keyboard::feed` | Zoom、TabList | `verified` + ABI確認 |
| `GameCore::keyboardEvent` | AutoSprint | `verified` + `native_keyboard`の配置・ABI確認 |
| `MouseDevice::feed` | Zoom、MouseChatter | `verified` + ABI確認 |
| `LevelRendererPlayer::getFov` | Zoom | `verified` + ABI確認 |
| `ClientInstance::grabCursor` | Zoom、TabList、MouseChatter、GUI | exact RVA候補以上 |
| `ClientInstance::releaseCursor` | Zoom、TabList、MouseChatter、GUI | exact RVA候補以上 |
| `GameCore::handleMouseInput` | MouseChatter、GUI | exact RVA候補以上 |
| `MultiPlayerLevel::_subTick` | TabList | exact RVA候補以上 |

`candidate` を許す関数でも、EXEハッシュ完全一致、entry check一致、一意RVA、期待する
関数境界は必須です。

### TabList固有の確認

TabListには関数シンボル以外に、プレイヤー一覧やスキンを読む内部レイアウトがあります。
新バージョンでは少なくとも次を再確認します。

- `Level` 内のplayer list位置
- `PlayerListEntry` の名前とskin位置
- skin implementation内の画像位置
- `NativeSkinImage` のwidth、height、data、size配置

これらは現在ランタイムResolverのシンボルJSONから取得していないため、変更された場合は
TabListのコード更新とDLL再ビルドが必要です。

## 11. JSONとmanifestを検証・生成する

最終JSONを配置した後、JSON構文、SDKテスト、manifest生成を実行します。

```bash
cd "$SDK_ROOT"

uv run python -m json.tool \
  "data/$NEW_VERSION/windows-x64.json" >/dev/null

uv run --extra dev pytest -q

uv run bedrock-symbols index data \
  --output data/manifest.json

git diff --check
git diff -- data/manifest.json "data/$NEW_VERSION/windows-x64.json"
```

manifestにはJSON自体のSHA-256、EXE全体と `.text` のSHA-256、PE構造が入ります。
JSONを変更した後は、必ずmanifestを再生成してください。

## 12. ランタイムとexampleを確認する

最低限、以下を確認します。

- 対応外EXEではフックを開始しない
- 対応EXEではGitHubから新しいプロファイルを取得できる
- Zoom: Cキー、FOV、スクロール、ホットバー抑止
- TabList: Tabキー、一覧、顔、カーソル非表示維持
- MouseChatter: Minecraftの `MouseDevice::feed` 経路で倍率が反映される
- メニュー、ワールド離脱、再参加、ゲーム終了で入力状態が残らない

Zoom、TabList、MouseChatter、AutoSprintは起動時にGitHub `main` のmanifestと
バージョン別JSONを取得します。関数ABIと固定レイアウトが互換なら、DLLを再ビルドせず
データ更新だけで新バージョンへ対応できます。取得成功後は
`%LOCALAPPDATA%\PEXData\BedrockDynamicSDK\profile-cache` へEXE単位で保存され、全サンプルで
共有されます。通常のキャッシュ再確認は共有ロックと永続マーカーで最大1日1回です。

サンプルが必須シンボルをキャッシュ定義から解決できない場合は、GitHub `main` を改めて確認し、
その実行ファイルのSHA-256または `.text` SHA-256とPE構造に完全一致する最新プロファイルを取得して
再解決します。この更新確認もEXE単位で最大1日1回に制限します。別バージョンのRVAをそのまま
流用することはなく、最新の完全一致プロファイルでも必須シンボルが見つからない場合はフックを
開始しません。定義JSONだけで直せるRVA/signatureの更新と、ABI変更によりDLL修正が要るケースを
区別してください。

## 13. 実例: `Level::getRuntimeActorList` の特定

この節は1.26.52.3での調査記録です。次のバージョンにも同じRVAやsignatureが使えるという意味では
ありません。新しいEXEごとに同じ検証をやり直してください。

### EXEを取り違えない

プロファイルのバージョン名だけでEXEを選ばず、必ず対象プロセスと同じ配布物かをfile SHA-256で
確認します。この調査で使ったSDK内EXEは次の値でした。

```text
version:    1.26.52.3
file SHA:   4a92bfa3ce2428b40ee517b7c1125d9f6f79274382de03846351992663b2e2e4
```

同じバージョン番号でも別配布物のEXEは異なることがあります。manifestのfile/text hashと一致しない
実行ファイルで見つけたRVAやAOBを、そのまま本番プロファイルへ転記しないでください。

### 失敗した推測を捨てる

- 旧Solsticeソースから持ってきたsignatureは、このEXEでは0件でした。旧版由来のAOBを新バージョンで
  無理に使わず、0件という結果を記録して次の調査に進みます。
- 以前試したvtable slot `326` は実行時に `Level::getRuntimeActorList call failed` となりました。
  slot番号を別の型やバージョンから推測して呼び出すのではなく、失敗した呼び出し経路を外し、対象EXEの
  関数を直接解決する方針に切り替えます。
- Flarialの公開チュートリアルにある短いprefix
  `40 53 48 83 EC 30 48 81` は対象EXEの `.text` に2件ヒットしました。これだけでは一意ではありません。
  参照元は[シグネチャ取得チュートリアル](https://github.com/flarialmc/dll-oss/blob/main/src/Utils/Memory/Game/Tutorial/HowToGetSigsAndOffsets.md)です。

この例では、候補関数の命令列と `+0x2E8` のフィールド参照を確認してpatternを延長し、次のAOBに
絞りました。

```text
40 53 48 83 EC 30 48 81 C1 E8 02 00 00 48 8B DA E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 30 5B C3
```

対象EXEの `.text` では1件だけ一致し、RVAは `0xE440F30`、`.pdata` の関数終端は `0xE440F4E` でした。
現在の定義は[data/1.26.52.3/windows-x64.json](../data/1.26.52.3/windows-x64.json)にあります。
これらは対象EXE内で位置が一意で関数境界内にある根拠であり、関数の意味やABIが正しい証明ではありません。

### ABIと実機動作は別に検証する

逆アセンブル上では `RCX`（Level）に `0x2E8` を加算し、`RDX` を保持したままhelperを呼び出す形が見えたため、
`void(__fastcall*)(Level*, std::vector<Actor*>*)` をABI候補として記録しています。しかし、候補patternが一意でも、
このprototype、vectorの型/寿命、返されるActorの範囲はまだ確定しません。そのため現行JSONは意図的に
`status: candidate`、`abi_verified: false`、`cross_version_signature_available: false` です。

HitBox exampleはこの不安定な `getRuntimeActorList` 呼び出しを使わず、ローカルActorの
`EntityContext` が指すEnTT registryから `ActorOwnerComponent` storageを列挙する別経路を追加しました。
この経路のActor context offset、Owner/AABB/StateVector/Rotation component hashとfield offsetは
`native_actors` profile sectionで管理します。1.26.52.3の値も既知component宣言と型名から作った候補であり、
SEH境界・数値検証はクラッシュのリスクを下げるだけで、正しいstorage/hash/layoutの証明ではありません。
実機でActor数・位置・AABBを照合できるまでは `status: candidate` のままにしてください。

実機確認では、少なくとも以下を順番に記録します。

1. 対象EXEのfile hashと選択されたプロファイルのhashが一致する。
2. resolverがsignatureを一意に解決し、呼び出し先RVAが `0xE440F30` になる。
3. ゲームスレッド上など、関数の所有権・スレッド要件に沿った安全なタイミングで呼ぶ。
4. 複数回の呼び出しで例外、フリーズ、破損がなく、返却vectorの構築/破棄が正しい。
5. ワールド内Actor数と取得数、NameTag読み取り数をログで比較し、TabListの色対応まで実際に確認する。

`world Actor list ready` はresolver準備完了のログにすぎず、Actor一覧取得成功を意味しません。成功ログには実際の
取得件数を含め、失敗時には例外/失敗状態を区別してください。実機でABIと意味を確認するまでは `verified` に
昇格せず、別バージョンへpatternを流用しないでください。

この調査を再現するときは、まず `bedrock-symbols inspect "$NEW_EXE"` でhash/PE情報を保存し、次に
`bedrock-symbols` の既存port/scan機能で旧AOBと候補AOBを対象 `.text` に照合します。ヒット数、RVA、entry bytes、
`.pdata`境界を別々に記録します。候補が0件なら再解析、複数件なら固定バイトを追加して再scanし、1件になっただけで
ABI検証を省略しないでください。調査の再現にGhidra/Lensを使った場合は、使用したEXE hash、Ghidra分析結果、
callsite/逆アセンブル根拠も更新記録へ残します。

## 14. 公開時のチェックリスト

- [ ] Minecraft EXEをコミットしていない
- [ ] 新EXEのfile SHA-256と `.text` SHA-256を記録した
- [ ] 旧AOBの0件／複数件ヒットを放置していない
- [ ] 必須シンボルがすべて新EXEの関数境界を指している
- [ ] `verified` はABI確認済みのものだけに付けた
- [ ] TabListの固定レイアウトを再確認した
- [ ] JSON変更後にmanifestを再生成した
- [ ] Python、C++、Windows CIが成功した
- [ ] 実機smoke testを行った
- [ ] JSONとmanifestを同じコミットでpushした

## 障害時の見方

| 症状 | 主な原因 |
|---|---|
| `missing MZ` | Store上の暗号化／プレースホルダーEXE |
| AOBが0件 | コンパイラ変更、関数の大幅変更、古いsignature |
| AOBが複数件 | signatureの固定バイトが不足 |
| BSim候補がない | 両EXEが同じBSim DBへ登録されていない、関数が大幅変更 |
| `unsupported executable hash` | manifest未更新、別チャネルまたは別EXE |
| profile checksum不一致 | JSON更新後にmanifestを再生成していない |
| symbol unavailable | 必須シンボル不足、`verified`要件未達、entry check不一致 |
| TabListだけ壊れる | 関数ではなくクラス内フィールド配置が変化した可能性 |


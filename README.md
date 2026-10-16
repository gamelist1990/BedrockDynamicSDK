# BedrockDynamicSDK

Minecraft Bedrock **Windows x64** の関数を解析し、**LeviLamina / PreLoader不要の単一DLL**へシンボル解決ライブラリを組み込むプロジェクトです。

**検証バージョン:** 1.26.52.3 (1.26.5203.0)  
**解析基準:** Minecraft.Windows.exe SHA-256 `4a92bfa3ce2428b40ee517b7c1125d9f6f79274382de03846351992663b2e2e4`

## 現在できること

- Latiteの公開済みAOBパターンを対象EXEへ再照合し、対応する関数RVAと**新版用の一意なAOB**を生成
- PEのSHA-256・`.text`・x64 `.pdata` 関数境界による機械的な検証
- C++20の静的Resolverから、ASLR対応で関数アドレスを取得
- ディスク上のEXEが暗号化されるGDK/Proton環境向けに、ロード済み`.text`全体のSHA-256でも照合可能
- Win32キーメッセージ観測用 `KeyboardObserver`（キーリピート・フォーカス喪失対応）
- 検証用 `BedrockSymbolProbe.dll`（ゲームの関数を**呼ばず**、候補RVAの可読性を確認）
- MSVC RTTI型名データ、旧Latite SDKレイアウトの*未検証*ヒント、GitHub用 `manifest.json`
- Bedrock Lens SQLiteからの関数RVA取り込み、Python・C++自動テスト

### 1.26.52.3の結果

| データ | 件数 | 状態 |
|---|---:|---|
| Latite旧シグネチャー | 54 | 最新EXEで再照合 |
| 一意にヒットした旧パターン | 45 | 自動照合 |
| 関数境界（.pdata）を満たす候補 | **40** | 新しいAOBを40件生成 |
| MSVC RTTI 型名 | **1,879** | 型名・TypeDescriptor位置を抽出 |
| ClientInstance取得起点 | **未解決** | 旧Platform_GameCoreパターンは一致せず |
| 旧SDKのフィールド・vtableオフセット | 複数 | **現在版で未検証・自動使用禁止** |

**重要:** パターン一意一致は**同一関数だと断言できる証拠ではない**。データは原則 `candidate`、`abi_verified=false`。バージョンを判別できても、ABI・フィールドレイアウト・ゲーム内動作を自動保証するものではありません。

## ディレクトリ

```text
BedrockDynamicSDK/
├── data/
│   ├── manifest.json
│   └── 1.26.52.3/
│       ├── windows-x64.json               # 40個の関数候補・RVA/AOB
│       ├── latite-compatibility.json      # 54個の旧パターンの検証結果
│       ├── types-windows-x64.json         # 1,879個のMSVC RTTI型名
│       └── layout-hints-unverified.json   # 旧SDK由来、未検証のオフセット
├── include/bedrock/
│   ├── resolver.hpp
│   ├── keyboard.hpp
│   └── generated_symbols.hpp             # DLL埋め込み用の自動生成JSON
├── src/
│   ├── resolver.cpp
│   ├── windows.cpp
│   └── probe_dll.cpp
├── tools/bedrock_symbols/                # Python CLI
└── tests/
```

Minecraft本体の実行ファイルは、ローカル `local-binaries/` 以下にのみ置き、Gitに含めません。

## 再生成（最新版のEXEが平文PEの場合）

Python 3.12以上・[uv](https://docs.astral.sh/uv/)・CMake 3.20以上が必要です。

```bash
uv sync --extra dev
git clone --depth 1 https://github.com/LatiteClient/Latite.git third_party/latite-reference

# 自分で正規に入手した、復号済みWindows x64のPEを入力
EXE="/path/to/Minecraft.Windows.exe"
uv run bedrock-symbols inspect "$EXE"

# 古いLatiteシグネチャーから、実際の新しいEXE用RVA/AOBを自動生成
uv run bedrock-symbols client-profile "$EXE" \
  --latite-addresses third_party/latite-reference/src/mc/Addresses.h \
  --version 1.26.52.3 \
  --output data/1.26.52.3/windows-x64.json \
  --compatibility-output data/1.26.52.3/latite-compatibility.json

uv run bedrock-symbols rtti "$EXE" --version 1.26.52.3 \
  --output data/1.26.52.3/types-windows-x64.json
uv run bedrock-symbols layout-hints \
  --latite-repo third_party/latite-reference \
  --output data/1.26.52.3/layout-hints-unverified.json
uv run bedrock-symbols index data --output data/manifest.json

# JSONをDLLへ埋め込み（配布するものはDLLだけ）
uv run bedrock-symbols embed data/1.26.52.3/windows-x64.json \
  --output include/bedrock/generated_symbols.hpp
```

他に [Bedrock Lens](https://github.com/bedrock-mc/lens)でのGhidra/IDA解析結果をSQLiteに収録できる場合：

```bash
uv run bedrock-symbols lens-import "$EXE" \
  --database /path/to/lens.db --artifact-id 2 \
  --version 1.26.52.3 --limit 200 \
  --output data/1.26.52.3/windows-x64.json
```

Bedrock LensのバイナリSHA256が現在のEXEと一致する場合だけ読み込みます。自動生成された類似関数はあくまで候補です。

## ビルドとテスト

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
uv run --extra dev pytest -q
```

Windows x64 (MSVC)では追加で `BedrockSymbolProbe.dll` が作られ、登録済みの40関数を**読み取り専用で照合**します。関数を呼び出したりHookを設置することはありません。デバッグ出力を確認してください。Linuxでは静的Resolverとテストのみがビルドされます。

### C++ API

```cpp
#include <bedrock/resolver.hpp>
#include <bedrock/generated_symbols.hpp>

bedrock::Resolver api;
std::string error;
if (api.loadJson(bedrock::generated::kSymbolDatabase, error) &&
    api.attachWindowsMainModule(error)) {
    // 一致するPE上のRVA。ただし候補として明示的に取得する。
    auto wndProc = api.resolve("MainWindow::_windowProcCallback", true);
    // wndProc->verified は false：関数呼び出しABIの確認前に実行しない。
}
```

キーボードイベントは `bedrock::KeyboardObserver` に既存WndProcからのメッセージを渡して使えます。Observer自体はHookを設置しません。

## 未実装・注意事項

- **ClientInstanceのオブジェクトポインタ取得**（Latiteの古い `Platform_GameCore` アンカーは未発見）
- 過去SDKのオフセット・vtableの現在版でのABI実証と自動修正
- Ghidra/BSimの完全な自動更新パイプライン（Lensへの取り込み機能は用意）
- GitHub HTTPSの動的ダウンロードと署名付きmanifest検証
- Windows上での実機DLL注入・ゲーム内Hookテスト

ゲームバイナリの再配布・暗号化解除機能は含まず、正規にアクセス可能な解析用PEを入力としてください。

参照：[Latite](https://github.com/LatiteClient/Latite) /
[Bedrock Lens](https://github.com/bedrock-mc/lens) /
[LeviLamina](https://github.com/LiteLDev/LeviLamina)

詳しいデータ定義：[docs/data-format.md](docs/data-format.md)

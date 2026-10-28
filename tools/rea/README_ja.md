# REA：標準のネイティブ解析ツール

[REA](https://github.com/morluto/rea) とGhidraプロバイダーを使い、Minecraft Bedrockの対象バージョンの実行ファイルを静的解析します。REAは調査の流れと根拠付きの問い合わせを提供し、Ghidraがネイティブコードを逆アセンブル・逆コンパイルします。ScanClientは実行時観測に使い、同じ実行ファイルのビルド識別子とRVAで両方の結果を照合します。

## 必要なもの

- Node.js 22.19以降（または24.11以降／26以降）とnpm
- Ghidra 12.1系と対応するフルJDK
- 調査対象と同じビルドの `Minecraft.Windows.exe`
- Ghidraプロバイダーを設定したREAとCodex接続

この開発環境ではREA 6.7.0、Ghidra 12.1.4、対応するNode.jsを確認済みです。準備状態は以下で確認します。

```sh
npx --yes rea-agents@latest doctor --provider ghidra --json
npx --yes rea-agents@latest providers --json
```

別のPCでは最新のREAをCodexへ登録できます。

```sh
npx --yes rea-agents@latest setup --client codex
```

表示される設定変更の内容を確認して承認し、Codexを再起動してください。セットアップはCodexのユーザー設定にREA MCPを登録し、REAの解析ワークフローをインストールします。このリポジトリの `AGENTS.md` がREA/Ghidraを標準手段として指定します。

## Minecraft実行ファイルを解析する

PowerShell：

```powershell
.\tools\rea\analyze.ps1 analyze D:\path\to\Minecraft.Windows.exe
.\tools\rea\analyze.ps1 search D:\path\to\Minecraft.Windows.exe Actor
.\tools\rea\analyze.ps1 function D:\path\to\Minecraft.Windows.exe 0x140123456
.\tools\rea\analyze.ps1 decompile D:\path\to\Minecraft.Windows.exe 0x140123456
.\tools\rea\analyze.ps1 xrefs D:\path\to\Minecraft.Windows.exe 0x140123456
```

Linux/macOS：

```sh
tools/rea/analyze.sh analyze /path/to/Minecraft.Windows.exe
tools/rea/analyze.sh search /path/to/Minecraft.Windows.exe Actor
tools/rea/analyze.sh function /path/to/Minecraft.Windows.exe 0x140123456
tools/rea/analyze.sh decompile /path/to/Minecraft.Windows.exe 0x140123456
tools/rea/analyze.sh xrefs /path/to/Minecraft.Windows.exe 0x140123456
```

新しい実行ファイルへ既知シンボルの候補を移すには、関数入口と関数本体のアンカーを`.pdata`境界と組み合わせます。結果は候補であり、ABI確認や実装照合は別途必要です。

```sh
.venv/bin/python -m bedrock_symbols.cli port NEW-Minecraft.Windows.exe \
  --old data/1.26.52.3/windows-x64.json \
  --source-binary OLD-Minecraft.Windows.exe --version NEW_VERSION \
  --base-profile data/NEW_VERSION/windows-x64.json \
  --output /tmp/windows-x64-candidates.json
```

`--source-binary`には旧プロファイルと一致する実行ファイルを渡します。
SHA-256と`.text`ハッシュを照合してから、旧EXE内の関数本体から一意な入口・本体アンカーを生成し、
トップレベル定義と`native_*`内の関数を新版`.pdata`境界へ候補移植します。
新しい候補はABIや構造体オフセットを検証済みにせず、旧レイアウトも引き継ぎません。
`--base-profile`を指定すると既存の定義を上書きせず、同じRVAは照合記録へ、異なるRVAは別名候補として追加します。

ラッパーはREAのGhidraプロバイダーとSHA-256接頭辞別の再利用可能な解析スナップショットを使います。各コマンドのJSON結果はWindowsでは `%USERPROFILE%\Documents\PEXData\REA`、Linux/macOSでは `$HOME/Documents/PEXData/REA` に保存されます。保存先を変える場合、PowerShellでは `-EvidenceDirectory`、shellでは4番目の引数にディレクトリを指定します。関数名も検索する場合はREA CLIの `search --kind procedures` を使ってください。

## 解析の進め方

まず `analyze` で全体像を取り、既知の文字列や関数名を検索します。候補の関数を `function` または `decompile` で確認し、`xrefs` から呼び出し元や参照先を追います。結果にはゲームバージョン、実行ファイルのSHA-256、プロバイダー、アドレス/RVA、確度を記録してください。REAの疑似コードや名前は解析根拠であり、SDKの検証済み定義ではありません。ABIと構造体レイアウトは対象ビルドおよび実行時情報で確認します。

ScanClientはMinecraftイメージの`.text`・`.rdata`・`.pdata`だけを読み取り可能な範囲ごとに保存します。ワールド関連文字列への参照や関数範囲は実行時アドレスの候補であり、検証済みシンボルではありません。REAのPE解析には元の`Minecraft.Windows.exe`を指定し、ScanClient出力は取得バイト列とロードアドレスの照合に使ってください。スキップまたは読み取り不能な範囲は実測データとして扱わないでください。

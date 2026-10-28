# BedrockDynamicSDK

Windows x64 Minecraft Bedrock向けの動的シンボルデータと、単一DLLへ静的リンクできるC++20 SDKを提供します。
**サンプルクライアントはSDK本体から分離した独立プロジェクト**です。

バージョンプロファイルは **Bedrock 1.26.52.3** と **1.26.60.5** を収録しています。Minecraft本体の実行ファイルはGitHubに含めません。

1.26.60.5はEXE識別情報と41件の診断エントリーを含む部分プロファイルです。旧版アンカーから31件を候補移植し、10件を追加、RVAが競合する候補は別名で保持しています。いずれも実ゲーム上の動作は未検証で、`ProfileOptions` の既定値 `CandidatePolicy::VerifiedOnly` では有効になりません。明示的に `AllowCandidates` を選んだ場合も候補機能としての扱いです。Actors、Items、World、Camera、Motion、Packets、Commands、native GUI、World Events、World Renderの1.26.60.5用レイアウトは未収録です。1.26.52.3向けDLLやネイティブ配置が新しい版でも動くとは限りません。詳細は[1.26.60.5の調査記録](data/1.26.60.5/README.md)を参照してください。

公開サンプル：

- [BlockOutline](exsample/BlockOutline): 設定したブロックを範囲内で分散走査し、AABBとオプションの日本語名をワールド描画。

- [Top example](exsample/Top/README.md) — `/top`・`/top N` による安全な上方移動（1.26.52.3候補）
- [SpeedBuilder example](exsample/SpeedBuilder/README.md) — お題の記録と消失差分による自動配置（1.26.52.3候補・実機未確認）

- [BedrockAutoSprint](https://github.com/gamelist1990/BedrockAutoSprint)
- [BedrockGuiDemo](https://github.com/gamelist1990/BedrockGuiDemo)
- [BedrockZoom](https://github.com/gamelist1990/BedrockZoom)
- [BedrockTabList](https://github.com/gamelist1990/BedrockTabList) — Tabキー中のシンプルなプレイヤー一覧
- [HitBox example](exsample/HitBox) — 全ActorのネイティブAABB表示と任意の最終位置保持
- [ScanClient](exsample/ScanClient) — 実行中プロセスの読み取り専用メモリ調査と候補プロファイル出力
- [CheckSig](exsample/CheckSig/README_ja.md) — 旧版署名の実行中EXEへの候補移植、周辺バイト調査、既知HookPointの透過フック検証

ネイティブ実行ファイルの調査は[REA + Ghidra](tools/rea/README_ja.md)を標準ツールとして使用します。REA MCPは関数・文字列・参照先の解析と根拠付き出力を提供し、ScanClientの実行時観測とMinecraftの正確なビルド識別子で照合します。

ScanClientはWindows x64の開発用DLLです。起動時に静的取得と並行して、本体の書き込み可能セクションを`stop`まで低優先度で繰り返し読み、変化ページを時刻・RVA・前後バイト付きで`dynamic_changes.jsonl`に記録します。ヒープや他モジュールは走査しません。ワールド／ディメンション関連文字列、その`.text`からの参照候補、`.pdata`関数範囲、関数ポインタ表候補も`%USERPROFILE%\\Documents\\PEXData\\ScanClient\\<version>\\windows-x64`へ保存します。候補はABI検証済みのSDK定義ではありません。詳細は[ScanClient README](exsample/ScanClient/README.md)を参照してください。

## ワールド移動イベント / PacketList

`GameHooks::installWorldEvents(resolver, profileJson, error)`で、参加・退出・参加中止・
ディメンション変更を`api.Event.Enum.WorldChange`から購読できます。
`DimensionChange`等の個別イベントも提供し、通知時はSDKのActor／名前／描画キャッシュを
失効させて`api.worldEpoch()`を進めます。HitBoxもこの通知を利用します。
1.26.52.3用の関数と多重継承のthis補正を静的解析から追加しましたが、実機での発火は未検証です。

`<bedrock/packet_ids.hpp>`には253種類の`PacketId`（別名`PacketList`）、
`packetName`、`packetIdFromValue`、`packetIdFromName`を用意しています。
使い方・出典・検証範囲は[ワールドイベントとPacket API](docs/world-events-and-packets.md)を参照してください。

## Console / Logger API（必要なModだけ使用）

コンソールとデバッグ出力APIは**ヘッダーオンリー・独立モジュール**です。
JSON取得、MinHook、シンボルResolver、イベントループには依存しません。

```cpp
#include <bedrock/console.hpp>
#include <bedrock/logger.hpp>
#include <bedrock/file_log_sink.hpp>

void initializeDebug() { // DllMainではなく、初期化用ワーカースレッドから実行
    bedrock::Console::open(L"Bedrock Debug"); // 明示的に開いたときだけ表示

    bedrock::Logger log{"MyClient"};
    log.debug("Client initialized");
    log.info("Version: ", "1.26.52.3");
    log.warn("ClientInstance not available");
    log.error("Failed to resolve function: ", "getLocalPlayer");

    log.setLevel(bedrock::LogLevel::Warn);
    // DEBUGとINFOはフィルタされ、WARNとERRORのみ出力。
    // Console::close() はこのDLLが作成したコンソールだけを閉じる。
}

// Optional persistent log. ClearOnOpen is the default; use Append explicitly
// only when preserving prior runs is intended.
bedrock::Logger fileLogger{"MyClient"};
bedrock::FileLogSink fileSink;
void initializeFileLog() {
    if (fileSink.open(L"MyClient.log")) fileSink.attach(fileLogger);
}
```

- `bedrock::Console::open(L"タイトル")` / `isOpen()` / `close()`
- `bedrock::Logger logger{"ModName"}`、`debug/info/warn/error`
- `logger.setLevel(bedrock::LogLevel::Info)` などで出力レベルを変更
- `logger.setSink(callback, context)` による外部ログ出力への差し替えも可能
- `bedrock::FileLogSink` は起動時に前回履歴を消すのが標準。追記したい場合だけ `OpenMode::Append` を指定
- Win32では `OutputDebugStringA` に常時出力し、コンソールがある場合だけそこにも出力
- Linuxではデフォルトでstderrへ出力し、Consoleオープンは非対応（false）

**ゲームのstdin/stdout/stderrを付け替えません。** `CONOUT$` に直接書き込み、
別のモジュールが既に開いているコンソールを勝手に閉じることもありません。
多スレッドログは出力を直列化します。ログの文字列生成は該当レベルが
有効なときだけ行われます。ロガー自身は自動スレッドやネットワーク処理を開始しません。
ファイル出力は `FileLogSink` を明示的に開いてLoggerへ接続します。

CMakeは `bedrock::logging` のみをリンクすればOK。これはINTERFACEターゲットのため
静的ライブラリのビルドも不要です。

```cmake
target_link_libraries(MyClient PRIVATE bedrock::events bedrock::logging)
```

コンソール表示はMod側が任意で選択できます。DllMain内部で開かず、
DLLが完全にロードされてから使用してください。

## オプション: GUI API（Dear ImGui / DirectX 11）

高品質なGUIを自由に作りたいModだけ、**別モジュール `bedrock::gui`** を有効化できます。
デフォルトはOFF。OFFの場合、Dear ImGui・DirectXのライブラリは取得もビルドもされません。

```cpp
#include <bedrock/gui.hpp>

void renderClientUi() {
    bedrock::gui::Window win("My Client");
    if (win) {
        static bool enabled = true;
        static float opacity = 0.7f;

        bedrock::gui::heading("Client Settings");
        bedrock::gui::toggle("Enable", enabled);
        bedrock::gui::slider("Opacity", opacity, 0.1f, 1.f);
        bedrock::gui::Panel panel("Module list", {350, 180});
        if (panel)
            bedrock::gui::text("My modules");
    }
}
```

使える機能：

- `Window`・`Panel`・`Tabs`・`Tab`：入れ子、ドッキング、タブ
- `toggle`・`button`・`slider`・`combo`・`input`・`colorPicker`・`progress`
- `Theme::dark(accent)`：角丸・余白・カラーの統一テーマ
- `draw::filledRect`・`gradient`・`circle`・`line`・`text`：自由描画
- `smooth(..., deltaSeconds)`：時間差分を考慮した滑らかなアニメーション
- **`ImGui::` をそのまま呼べる**：独自ウィジェット、DockSpace、フォント、描画リスト、
  テクスチャ、グラフなどを拡張可能

CMakeの使い方：

```cmake
# FetchContent_Declare(bedrock_sdk ...) を呼ぶ前に
set(BEDROCK_SDK_ENABLE_GUI ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(bedrock_sdk)

target_link_libraries(MyClient PRIVATE bedrock::gui)
```

Dear ImGuiのバージョンは `v1.92.9b-docking` に固定しているため、ビルドが再現可能です。
`bedrock::gui` はUI機能だけで、ウィンドウ作成・入力処理・レンダリングは
**呼び出し側のゲーム描画Adapterまたはホストが提供**します。

Windowsで独立ウィンドウを表示するための任意ターゲット `bedrock::gui_host`
（Win32/DirectX 11）も用意しました。

```cpp
#include <bedrock/gui.hpp>
#include <bedrock/gui_host.hpp>

std::string error;
bedrock::gui::HostOptions options;
bedrock::gui::GuiHost::run(options, [] {
    bedrock::gui::Window panel("Example");
    if (panel) bedrock::gui::text("Hello, Bedrock!");
}, error);
```

`GuiHost::run` は呼び出したUIスレッドを使い、コンソール・ゲームの裏側で
勝手にワーカースレッドを起動しません。DllMain内では呼ばないでください。

### 起動直後・ホーム画面からの描画

LatiteのDXGI初期化と同じく、
`IDXGIFactory2::CreateSwapChainForHwnd` の引数からDX12キューを捕捉します。
実際のMinecraftでは注入時にSwapChainがすでに作られている場合があるので、
`ExecuteCommandLists` だけではなく、`ID3D12CommandQueue::Signal/Wait`
からもキューを捕捉します。ゲームのホーム画面でも既存の
`Present` に即座に描画できるようにするためです。

さらに**DXGIのフックをネイティブCursor/MouseのGitHubプロファイル取得より先に
有効化**し、そのネットワーク処理中は描画側のロックを保持しません。
プロファイル取得に失敗しても描画は有効なまま、未検証のネイティブ入力
フックだけ無効にします。対応版の入力フックが確保できたら後から有効化します。
実際のMinecraft/Proton内での起動直後表示はユーザー実機で要確認です。

### Minecraft画面内へ直接描画する（DX11 / DX12）

**ゲーム側のクリック・ホットバー操作抑止（Latiteと同じ入力境界）:**
`OverlayOptions::captureInput` を有効化したGUIでは
`GameCore::handleMouseInput` のバージョン照合済み関数候補をフックします。
GUI表示中でMinecraftがフォーカスされているときにだけゲームへのマウス操作を遮断。
GUIを閉じれば元のゲーム入力を通します。
ImGuiにはWindowsから取得したマウス位置・クリックとWndProcのホイール入力を渡します。
さらにWndProc側で `WM_LBUTTON` / `WM_RBUTTON` / `WM_MOUSEWHEEL` /
`WM_INPUT`（マウス由来のみ）のゲームへの伝播を遮断します。
`Overlay::gameMouseBlocked()` はネイティブ遮断フックの導入状態を返します。
候補のABIが変化しているMinecraftバージョンでは実機での検証が必要です。

Latiteは `MouseDevice::inputs` 配列からキャンセルされたイベントを削除しますが、
現在の解析データでは `MouseDevice::_instance` の取得と配列レイアウトが未検証です。
そのため、その不確かな構造に触らず、入力処理関数の呼出し前に遮断する形にしています。

**GUIマウス操作のネイティブ経路（Latite方式）:**
`OverlayOptions::nativeCursor=true` にすると、GitHubから正しい
`windows-x64.json` を取得し実EXEのSHA-256を照合した後、
`ClientInstance::grabCursor/releaseCursor` にHookを掛けます。
通常のカーソルイベントが発生した際の `this` を捕捉し、
F10を開いたら `releaseCursor()`、閉じたら `grabCursor()` を呼び、
GUI表示中の自動 `grabCursor` を抑制します。
これはLatiteのScreenManagerと同じ考え方で、
以前の `user32!ClipCursor/SetCursorPos` だけの対処は廃止しました。

`Overlay::nativeCursorReady()` で捕捉の可否を確認できます。
まだMinecraftが自然な `grabCursor/releaseCursor` を呼んでいないときは
ポインタを取得できず、ゲーム内の画面を1度切り替える等のイベントが必要です。
`data` にない未対応ビルドではフックを有効にせず終了します。
なお対応データの候補シンボルはすべてのMinecraft実機でABI確認されたものではなく、
プロファイルとシグネチャを照合したバージョンでのみ試験的に使用します。


`bedrock::gui_overlay` をリンクすれば、**別ウィンドウを使わず**
Minecraftの既存DXGI SwapChainの `Present` 内でGUIを描画できます。
DX11では通常のImGui/DX11描画、DX12ではゲームのDirectコマンドキューを捕捉して
`D3D11On12` でImGuiを合成します。
リサイズ時のバックバッファ再作成、ゲームへの入力イベント転送、
標準F10で表示切替にも対応するよう実装しています。

```cpp
#include <bedrock/gui.hpp>
#include <bedrock/gui_overlay.hpp>

void renderGui(void*) {
    bedrock::gui::Window window("My Client");
    if (window) {
        static bool enabled = true;
        bedrock::gui::toggle("Enabled", enabled);
    }
}

void initialize() { // DLLロード完了後のワーカースレッド
    std::string error;
    bedrock::gui::Overlay::install(&renderGui, nullptr, error);
}
```

```cmake
set(BEDROCK_SDK_ENABLE_GUI ON CACHE BOOL "" FORCE)
# FetchContent_MakeAvailable(bedrock_sdk) の後
target_link_libraries(MyClient PRIVATE bedrock::gui_overlay)
```

`Overlay::install` はMinecraft.Windows.exeプロセスでのみ有効化し、
独自の別ウィンドウを表示しません（フックの関数アドレス取得用の**不可視**
Win32ウィンドウは初期化時だけ作成して即破棄します）。
`renderGui` は**ゲームのPresentスレッド**で呼ばれるので、
ゲーム内メモリの操作やスレッド同期には慎重に対応してください。
Modの実装は単純なUIコールバックだけにできます。

**注意：** 実際のMinecraft 1.26.52.3＋GDK Proton上での表示・マウス操作は
まだ未検証。DXGI/D3D12実装の差異やマウスロックによっては調整が必要です。
メモリ改変を伴う互換性未確認のゲーム関数は利用していません。
別ウィンドウが必要なら従来の `bedrock::gui_host` も選択可能です。

サンプル：<https://github.com/gamelist1990/BedrockGuiDemo>
（ダッシュボード、設定画面、タブ、グラフ風描画、グラデーションなど）

## 軽量なEvent API（推奨）

SDKにはクライアント固有のスプリント動作・DLLエントリポイント・
更新ループを持たせません。型付きイベントを扱う**ヘッダーオンリー**
`<bedrock/events.hpp>` のみを公開します。

```cpp
#include <bedrock/events.hpp>
bedrock::ClientEvents events;

events.on<bedrock::KeyEvent>([](const auto& event) {
    if (event.virtualKey == 'W' && event.pressed) {
        // クライアント固有の動作を書く
    }
});

// 入力を実際に受信するクライアント側やHookアダプターで発行
events.emit(bedrock::KeyEvent{'W', true, false});
```

- `KeyEvent`：キーコード・押下/解放・リピート
- `FocusEvent`：ウィンドウのフォーカス
- `GameplayEvent`：ゲームプレイ中の入力可否
- `UpdateEvent`：クライアント側の任意周期で発行する更新イベント
- `EventBus<MyEvent>`：ユーザー独自イベントも追加可能
- `on<T>(callback)`, `emit<T>(event)`, `off<T>(listenerId)`

**`UpdateEvent` はMinecraft本体のtickをHookしたイベントではありません。**
スレッド・イベント源はクライアント側が管理します。将来的な
Minecraft-native Hookイベントは独立したAdapterで実装可能です。
コールバックは発行元スレッドで実行されるので、異なるスレッドから
イベントを発行する場合は自分のイベントキューへ集約してください。

CMake：

```cmake
target_link_libraries(MyClient PRIVATE bedrock::events)
```

`bedrock::events` はINTERFACEターゲットで、シンボル解析・HTTP・
WinHTTP・MinHookの依存を一切取り込みません。
バージョン別シンボル解決が必要なときだけ `bedrock::symbols` を追加してください。

## サンプル: AutoSprint

AutoSprintは[独立リポジトリ BedrockAutoSprint](https://github.com/gamelist1990/BedrockAutoSprint)のサンプルModです。GUIやほかの機能はありません。
ビルド時にCMake FetchContentで**GitHubのBedrockDynamicSDKを取得**し、
SDKヘッダー(`include/`)と `bedrock_resolver` を静的リンクします。
実行時はローカルのSDK本体・ヘッダーを必要とせず、**DLL 1個**だけで動作します。
[exsample/AutoSprint](exsample/AutoSprint)は独立リポジトリを指すサブモジュールです。

- ゲームのウィンドウがアクティブ、カーソル非表示（ゲームプレイ中）、前進キー `W` を押している場合、`左Ctrl` の通常入力を自動的に保持
- 前進をやめる、ゲームが非アクティブになる、メニューを開く、Shiftを押すと自動的に解除
- コントローラーとカスタムキー割り当ては未対応。既定の `W` = 前進、`左Ctrl` = スプリントの操作環境向け
- メモリの移動速度を書き換える、スプリント状態フラグを直書きするなどの動作は**行わない**

### GitHubからの自動バージョン切替

```text
BedrockAutoSprint.dll が起動
    ↓
現在の Minecraft.Windows.exe のファイル/.text SHA256・PE構造を計測
    ↓
検証済みローカルキャッシュを確認
    ↓（なければ内蔵プロファイルを検証）
起動後、共有クールダウンに従ってGitHubをバックグラウンド確認
    ↓
対応するバージョンを完全一致で特定
    ↓
data/<version>/windows-x64.json を取得
    ↓
ファイルSHA256とゲーム本体のハッシュ・PE構造を検証
    ↓
入力処理のシンボルのアドレスを検証 → AutoSprintを有効化
```

- **未対応バージョン・HTTPエラー・ハッシュ不一致は必ずスルー。** Modは何も操作しません。
- DLLのバイナリは**そのまま**で、GitHub上に対応するバージョンのJSONを追加するだけで、同じ入力処理の仕組みを使える新バージョンを認識できます。
- 入力サンプルはMinecraftの `Keyboard::feed` / `MouseDevice::feed` を使い、OSのキー状態取得や `SendInput` に依存しません。関数ABIが変わった場合は定義更新だけでは安全に動作できないため、そのシンボルを `verified` にする前にDLL側の対応が必要です。
- HTTPSとmanifest記載のSHA256でデータの整合性を検査します。GitHubリポジトリの権限を持つ管理者がデータを更新する仕組みであり、独立した公開鍵署名はまだ未実装です。
- 検証済みJSONは `%LOCALAPPDATA%\PEXData\BedrockDynamicSDK\profile-cache` にEXEの `.text` SHA256別で保存します。キャッシュは24時間そのまま使用し、GitHubの再確認は全サンプルで共有する永続マーカーにより最大1日1回です。失敗時も24時間は再試行しません。
- キャッシュ定義でサンプル必須シンボルを解決できない場合は、GitHub `main` のmanifestから同じ実行ファイル識別情報に完全一致する最新JSONを即時確認します。この追加確認もEXE単位で共有し最大1日1回です。別バージョンのRVAを誤用せず、完全一致定義でも解決できない場合は安全のため起動しません。
- 同梱したビルド用JSONも最終フォールバックになるため、初回起動時にGitHubへ接続できなくても、そのDLLのビルド対象バージョンは起動できます。キャッシュ／同梱データも現在のEXEハッシュとPE構造が完全一致しなければ拒否します。

注：`BedrockAutoSprint.dll` は通常のWindows x64 DLLです。**これ自体に注入機能はありません**。すでに利用している正規のDLLローダーなどでロードする構成です。ロード後、API `AutoSprint_GetStatus()` で状態を確認できます。ゲームを終了するまでDLLはメモリからアンロードされません。

## バージョン別データは単一JSON

```text
data/
├── manifest.json
└── 1.26.52.3/
    └── windows-x64.json
```

`windows-x64.json` に統合：

| フィールド | 内容 |
|---|---|
| `target` | Windows x64のPEハッシュ・バージョン・.text情報 |
| `symbols` | 40件の関数RVA/AOB候補と出典 |
| `analysis.rtti` | 1,879件のRTTI型名 |
| `analysis.latite_compatibility` | Latiteの旧54シグネチャーの照合記録 |
| `analysis.layout_hints` | 旧SDKのフィールド/vtable配置の**未検証情報** |

`layout_hints` の値は実行時に使用しません。単に名前とシグネチャーが一致しても、対応するC++ ABIまで検証できたことにはなりません。

## データの更新と生成

Python 3.12以上 / [uv](https://docs.astral.sh/uv/) を使用。

```bash
uv sync --extra dev
git clone --depth 1 https://github.com/LatiteClient/Latite.git third_party/latite-reference

uv run bedrock-symbols client-profile /path/to/Minecraft.Windows.exe \
  --latite-addresses third_party/latite-reference/src/mc/Addresses.h \
  --latite-repo third_party/latite-reference \
  --version 1.26.52.3 --output data/1.26.52.3/windows-x64.json

uv run bedrock-symbols index data --output data/manifest.json
```

`client-profile --latite-repo` で関数・RTTI・照合結果・SDKレイアウト参考情報を**最初から単一JSONへ自動生成**します。新バージョンへ移植した関数は候補として分類され、勝手にABI検証済みにはしません。

Bedrock LensのSQLiteを利用する `lens-import`、既存シグネチャーを別ビルドで探す `port` なども利用できます。詳細は `tools/bedrock_symbols/cli.py` を参照。

Ghidra、Bedrock Lens、AOB移植、ABI確認、manifest公開までを通した実運用手順は
[`docs/version-update-workflow.md`](docs/version-update-workflow.md) にまとめています。

## C++ライブラリ / DLL

- `bedrock::events`：ヘッダーオンリーのEvent API（依存なし）。
- `bedrock::hook_runtime`：MinHookとランタイム実装を各Mod DLLへ静的同封し、プロセス内の共有API窓口でHook所有権を一本化します。配布物は単一DLLで、複数Modを同時ロードできます。詳細は [`docs/shared-hook-runtime.md`](docs/shared-hook-runtime.md)。
- `bedrock::gui`：オプションのDear ImGui GUI（通常はビルドしない）。
- `bedrock::gui_host`：GUIが有効なWindowsのみの別ウィンドウ描画ホスト。
- `bedrock::symbols` / `bedrock_resolver`：追加選択するC++20静的ライブラリ。EXEの身元確認、RVA/AOB検査、GitHubからのHTTPSダウンロード・検証を含みます。
- `BedrockAutoSprint.dll`：別リポジトリでビルドするサンプル。SDKに実装を含めません。
- `BedrockClientLocator.dll`：実機未検証のClientInstance捕捉用実験DLL。
- `BedrockSymbolProbe.dll`：メモリ読み取り専用の関数アドレス検査。
- `include/bedrock/keyboard.hpp`：ウィンドウメッセージ用キーボード観測API。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
uv run --extra dev pytest -q
```

SDK側のWindows/MSVC CIは独立DLLに埋め込まれるHook runtimeを複数クライアント間で検証し、`BedrockClientLocator.dll` と `BedrockSymbolProbe.dll` をビルドします。
**`BedrockAutoSprint.dll` は独立したBedrockAutoSprintリポジトリのCIでビルド**します。
LinuxでのC++テストとWindowsの模擬Hookテストも実行します。
**Minecraft本体への実機Inject試験は未実施**です。

### SDKをほかのModから使用する

CMakeの `FetchContent` でGitHubからSDKを取り込み、`bedrock_resolver` にリンクします。
SDKがサブプロジェクトとして読み込まれた場合は `BEDROCK_SDK_BUILD_TOOLS=OFF` が既定になり、
診断DLL・テストを自動ビルドしません。
CMakeサンプルは [BedrockAutoSprint](https://github.com/gamelist1990/BedrockAutoSprint) を参照。

## 技術的な注意

- ネットワーク上のシンボルデータの追加は、関数の呼び出し規約やメンバーレイアウト変更への万能な互換レイヤーではありません。
- 独自バージョン番号だけで判断せず、実際にロードされたPEのハッシュとセクションサイズで照合します。
- 解析ファイルに `status=candidate` とある関数は、呼び出して安全であることを保証しません。
- データの生成ソースは [Latite](https://github.com/LatiteClient/Latite) と [Bedrock Lens](https://github.com/bedrock-mc/lens) などを参考にしています。
- Microsoft配布物そのものをリポジトリ内で再配布しない方針です。


## Freecam example

[Freecam](exsample/Freecam/README.md) adds configurable detached camera movement,
damage auto-disable, and bounded nearby Player boxes for Windows x64.
The current Borion backend moves a local Player proxy and anchors outgoing movement to its pre-flight pose while forwarding interactions; live validation is pending.
Runtime 4.3.0 compares compatible runtime providers in loaded Mod DLLs and shares renderLevel/sendToServer middleware. Place the compatible optional
`BedrockHookRuntime.dll` beside the mods and restart the game when upgrading the owner.

### World / block / camera SDK APIs

[World・配置・Camera API](docs/world-block-camera-api.md) を追加しました。
`World.getBlock()` / `World.setBlock()` / `World.placeBlock()` と、tick単位の
`NativeWorld` アダプターを利用できます。ブロック状態・NBT属性は所有コピー、
バージョン依存の配置処理はSDKの `native_world` プロファイルへ集約しています。
SpeedBuilder / Top は共通ブロック層、Freecam / Top は共通位置更新層を使用します。

### Native client command registration

Topで使用している本体CommandRegistryへの登録処理もSDKへ移動しました。
`CommandBuilder(CommandName{"top"}, ArgumentName{"blocks"}).then(handler)` で定義し、
`NativeCommandService.add(definition)` に登録します。候補表示・パケットfallback・
実行キュー・二重実行防止はSDKが担当し、client tickの `dispatch()` で実行します。
Topの安全な上方探索・移動も `NativeSafeTeleport` に共通化しました。
詳細は [Native client commands](docs/native-client-commands.md) を参照してください。

## Typed SDK API and HTML guide

- [HTML API guide](docs/site/index.html): command builders, typed values, World,
  Actor/event usage, and native profile updates with downloadable C++ examples.
- `CommandBuilder(...).then(handler)` produces a complete typed `CommandDefinition`.
- `NativeCommandService` owns autocomplete registration, packet fallback and tick dispatch.
- `NativeSafeTeleport` owns budgeted upward search, destination checks and guarded movement.
- `CameraRig` + `NativeCamera` provide time-based camera movement, interpolation and guarded player-proxy operations.
- `NativeGameType` invokes the native game-mode core setter; `NativeBrightness` manages owner-scoped gamma overrides.
  See [Camera / GameType / Brightness](docs/camera-game-type-brightness.md) for candidate status and examples.
- `ProfileManager` owns exact-build profile acquisition and capability checks;
  adapters can request a throttled GitHub refresh when their initialization fails.
- `RuntimeProfile` carries an immutable validated snapshot and Resolver, so native
  adapters do not need to rehash the executable for every feature.
- GitHub Pages deployment: `.github/workflows/docs.yml`; configure Pages to use
  GitHub Actions. This is a static HTML/CSS/JS site without a Node build step.

Legacy overloads remain available for source migration. Future RVA/layout updates
within a supported ABI can be delivered through the profile; new ABIs still need
an SDK adapter update and may require rebuilding consumer mods.

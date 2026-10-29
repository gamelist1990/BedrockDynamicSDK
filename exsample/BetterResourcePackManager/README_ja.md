# BetterResourcePackManager（移植初期版）

添付された Android ARM64 版 `BetterResourcePackManager v2.3.1` を解析し、
BedrockDynamicSDK の API を使って Windows x64 向けに再実装するサンプルです。
**現在はネイティブ設定画面の入口を実装した段階です。元 Mod の完全移植・実機正常動作確認済みの版ではありません。**

対応プロファイルは **Minecraft 1.26.60.5 / Windows x64 / Release MSVC** のみ。
EXE SHA-256 は `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`。
1.26.52.3 や Android のアドレスを代用しません。

## 実装した経路

- Minecraft 標準の設定画面でグローバルリソースの表示・編集を許可する引数を SDK が設定します。
- 親の SettingsScreenController の「終了時にリロードする」フラグを設定し、ゲーム自身の終了時処理にリロードを要求します。
- グローバルリソースのタブと、それに続く一時画面でのみプレイ前状態を上書きします。他の画面とクライアントへ状態を持ち越しません。
- 関数、ABI、フィールド位置、文字列形式は版別 JSON と SDK が管理します。Mod ソースにはネイティブアドレス・offset を置きません。
- ゲームが確保した画面名の文字列はゲーム側で解放します。DLL の CRT でゲームの割当を解放しません。

一覧表示・有効化・無効化・順番の変更・保存の操作は Minecraft 標準 UI を利用します。
独立したパック一覧ウィンドウや独自のディスク上のスタック編集は追加していません。

## 未完了・未確認

元 Android 版には `_composeFullStack` と設定終了時の分岐パッチ、リロード完了後の
BlockGraphics 再初期化、雲テクスチャ・描画参照の修復があります。
**この Windows 初期版には、それらと同等だと確認できた処理はまだありません。**
親のリロードフラグを使う経路は静的解析に基づく候補です。プレイ中のスタック反映、
ブロック・雲・アイテムの描画更新、再読み込みの完了、複数回切替、ワールド退出の
正常動作は実機で確認する必要があります。元 Mod と同じように動くとはまだ保証できません。
プロファイルも `candidate` / `runtime_verified: false` を維持しています。

## ビルド

リポジトリのルートから実行します。

```powershell
cmake -S exsample/BetterResourcePackManager -B build-brpm -A x64 "-DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK=$PWD" "-DBEDROCK_GAME_VERSION=1.26.60.5"
cmake --build build-brpm --config Release --parallel 2
ctest --test-dir build-brpm -C Release --output-on-failure
```

出力は `build-brpm/Release/BetterResourcePackManager.dll`。
Windows CI でも MSVC Release のビルドと同じ単体テストを実行します。
Linux では DLL は生成せず、画面判定と文字列の所有権に関するテストを実行します。

## 実機調査時の操作

1. 対応ビルドの Minecraft を起動し、DLL を一度ロードします。
2. DLL ロード後に設定画面を開き直します。ロード前から開いている設定画面には初期化 Hook が適用されません。
3. 標準の「設定 → グローバルリソース」でパックを切り替えて戻ります。
4. ログの `settings`、`pre_game_overrides`、`failures` と、変更後の実際の描画を照合します。
5. 変更なしで閉じる、追加・削除・並替を繰り返す、ワールドを退出・再入場する操作も確認します。

ログは `Documents/PEXData/BetterResourcePackManager/BetterResourcePackManager.log`。
DLL の `BetterResourcePackManager_Status()` は、0=開始前、1=初期化中、2=Hook 登録済み、
4=初期化失敗。**2 はパック反映成功の意味ではありません。**
対象外 EXE、入口ガード不一致、Hook 競合は初期化を停止してログに残します。
Hook コードはプロセス終了まで保持します。DLL を変更する場合は Minecraft を完全終了してください。

## 解析根拠

入力パック SHA-256: `0995969634128e548ac912f54d92e8b22f99badb0d015ec827a1fbd56a6ac4ee`。
内部 `.so` SHA-256: `e9de1594947f09c3cd3a5ec049189effc2cb1c2985b0832c9b44ac8eea6992ad`。
Windows 定義は `data/1.26.60.5/windows-x64/classes/ResourcePackManager.json`。

解析記録は開発ホストの `~/Documents/PEXData/REA/brpm-2.3.1-arm64-evidence.json` と
`~/Documents/PEXData/REA/brpm-windows-1.26.60.5/` に保存しています。
Windows の解析は対象 EXE から明示的に抽出した関数範囲で行っています。
raw import の既定 ABI は Windows の ABI を証明しないため、レジスターとスタック引数は
元 PE の命令列でも照合しています。解析記録は実行確認の代わりにはなりません。
元の `.so` とゲーム本体はサンプルへ同梱しません。

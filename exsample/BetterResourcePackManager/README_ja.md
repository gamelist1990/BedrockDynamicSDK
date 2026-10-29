# BetterResourcePackManager（Windows プレイ中再読み込み v6）

添付された Android ARM64 版 `BetterResourcePackManager v2.3.1` を解析し、
BedrockDynamicSDK の API を使って Windows x64 向けに再実装するサンプルです。
v5 の実機ログでは地形更新とブロックテクスチャ更新が実行され、通常ブロックの表示は
復旧しました。一方、プレイヤーモデルとサーバーのカスタムブロックに不具合が残りました。
v6 はアトラス更新だけでなく、ネイティブローダーの準備完了・未完了タスクなしを待ちます。
その後、`MinecraftGame::startFrame` の開始時に、ゲーム標準の同期モデル・アニメーション再読込、
エンティティ描画の再構築、既存 ECS 描画データの更新、クライアントのリソース変更通知を実行し、
最後にブロックテクスチャと全チャンクの描画を更新します。
描画オブジェクトを破棄・再生成する処理をプレイヤー取得関数の途中で実行しません。
待機中にワールドが変わった場合は取り消します。非同期待機にプレイヤー・クライアントの
生ポインターを保持せず、フレーム開始時のゲーム所有クライアント表から借用します。
**v6 のプレイヤー表示・カスタムブロック修復は実機確認待ちです。**

対応プロファイルは **Minecraft 1.26.60.5 / Windows x64 / Release MSVC** のみ。
EXE SHA-256 は `988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a`。
1.26.52.3 や Android のアドレスを代用しません。

## 実装した経路

- Minecraft 標準の設定画面でグローバルリソースの表示・編集を許可する引数を SDK が設定します。
- 親の SettingsScreenController の「終了時にリロードする」フラグを設定し、ゲーム自身の終了時処理にリロードを要求します。
- グローバルリソースのタブと一時画面に加え、設定終了およびゲーム側の遅延適用イベントの実行中も、該当クライアントのプレイ前判定を維持します。イベント終了時に状態を復元します。
- 適用用進捗画面へ渡すコールバックが SDK 対応のパック適用処理と完全一致し、ローカルプレイヤーが存在する場合に限り、そのゲームスレッド上で適用します。コールバックの所有権を取得してから実行し、ゲーム側のデストラクターで一度だけ解放します。
- 適用中のスタック合成ではグローバルパックを含めるフラグを一時的に有効にし、合成終了後に元に戻します。
- 適用イベント、リロード呼び出し、スタック合成、地形更新と描画参照の修復までの通過回数をログに記録します。Hook 登録と実際の適用経路の実行を区別できます。
- 関数、ABI、フィールド位置、文字列形式は版別 JSON と SDK が管理します。Mod ソースにはネイティブアドレス・offset を置きません。
- ゲームが確保した画面名の文字列はゲーム側で解放します。DLL の CRT でゲームの割当を解放しません。

一覧表示・有効化・無効化・順番の変更・保存の操作は Minecraft 標準 UI を利用します。
独立したパック一覧ウィンドウや独自のディスク上のスタック編集は追加していません。

## 未完了・未確認

設定終了とスタック合成の Windows 経路は、対象 EXE の命令列と REA/Ghidra の解析に基づきます。
Windows 標準のブロックテクスチャ更新処理で既存の BlockGraphics を更新し、
オブジェクトを作り直す Android 版とは異なる経路で参照の有効性を維持します。
サーバーパックを含む full stack をそのまま使用します。サーバーパック固有の原因かは未確定です。別のサーバー用マネージャーをクライアント用スタックへ混ぜる処理は追加していません。
既存・新設ブロック、アイテム・雲、複数回切替、ワールド退出の実機確認が必要です。
旧ログの `reload_finalized` は言語サブパック更新関数からの復帰回数でした。
v5 では誤解を避けるため `language_subpacks_updated` に改名しました。
`terrain_atlas_completed` は対象の地形テクスチャ更新コールバックの完了、
`block_textures_reloaded` / `clients_refreshed` はブロック更新処理からの復帰回数です。
v6 の `geometry_reloaded` は同期モデル・アニメーション読込と既存描画データの再接続、
`client_definitions_reloaded` はクライアントへのリソース変更通知からの復帰回数です。
`resource_wait_frames` は未完了リソースを待って修復を延期したフレーム数です。
これらも実際の表示が正しいことを保証するものではありません。
プロファイルは `candidate` / `runtime_verified: false` を維持しています。

## ビルド

リポジトリのルートから実行します。

```powershell
cmake -S exsample/BetterResourcePackManager -B build-brpm -A x64 "-DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK=$PWD" "-DBEDROCK_GAME_VERSION=1.26.60.5"
cmake --build build-brpm --config Release --parallel 2
ctest --test-dir build-brpm -C Release --output-on-failure
```

出力は `build-brpm/Release/BetterResourcePackManager.dll`。
Windows CI でも MSVC Release のビルドと同じ単体テストを実行します。
Linux では DLL は生成せず、画面判定・文字列・適用コールバックの所有権・地形更新待機のテストを実行します。

## 実機調査時の操作

1. 対応ビルドの Minecraft を起動し、DLL を一度ロードします。
2. DLL ロード後に設定画面を開き直します。ロード前から開いている設定画面には初期化 Hook が適用されません。
3. 標準の「設定 → グローバルリソース」でパックを切り替えて戻ります。
4. 設定画面を閉じてワールドへ戻り、実際のテクスチャとログを照合します。
   `exits` は終了ボタン、`apply_events` は遅延適用、`reload_calls` はリロード入口、
   `stack_compositions` は実際の合成、`global_stack_included` は適用中のグローバルパック包含、
   `language_subpacks_updated` は言語サブパック更新からの復帰回数です。
   `apply_navigations` は進捗画面への受け渡し要求、`live_applications` はその場で実行した適用処理、
   `in_world_apply_events` はローカルプレイヤーが存在する状態での適用イベントです。
   同じワールドで変更して戻り、`live_applications` / `in_world_apply_events` / `global_stack_included` と実際の表示を照合します。
   `game_gate_overrides` は特定のゲーム側拒否判定を上書きした回数です。
   v2/v3 の `level_stack_included` は表示名が誤っていました。対象フラグはグローバルパックの包含です。
   変更なしの場合は適用回数が増えないことがあります。
   `settings` と `pre_game_overrides` だけでは反映成功は分かりません。
   v6 は `terrain_atlas_completed` → `geometry_reloaded` / `client_definitions_reloaded` →
   `block_textures_reloaded` / `clients_refreshed` の増加と、
   既存・新設・カスタムブロック、三人称・インベントリのプレイヤー表示、他プレイヤー・動物を確認します。
   `refresh_cancelled` は待機中にワールドが変わり修復を取り消した回数です。
   反映しない場合は新しいログを確認します。
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

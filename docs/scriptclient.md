# ScriptClient — TypeScript Mod ガイド

ScriptClient は Minecraft Bedrock クライアント内で JavaScript / TypeScript Mod を動かす Windows DLL Mod です。このガイドでは EffectFake を例に、導入と `@bedrock/sdk` の使い方を説明します。各 Mod は専用の QuickJS 実行環境で動きます。

## 配置

`ScriptClient.dll` を通常の DLL Mod と同じ方法で読み込ませます。**`Minecraft.Windows.exe` があるフォルダー**が ScriptClient の保存先です。EXE のフォルダーに書き込めない場合にだけ `ScriptClient.dll` のフォルダーに切り替わります。

| フォルダー | 用途 |
| --- | --- |
| `mods/<ModName>/` | インストール済みのスクリプト Mod |
| `mod-data/<ModName>/` | Mod ごとの永続データ |
| `packages/` | インストール前のパッケージ |

旧版から移行する場合は、DLL 側の `mods/` と `mod-data/` に保存していた内容を、ゲームを終了してから EXE 側の同名フォルダーへ移してください。`BEDROCK_SDK_MODS_DIR` は使用しません。

### Mod Manager でインストールする

1. PEX Mod Studio のインストール後、ScriptClient 用 `.mods` をダブルクリックします。`.zip` は Studio のファイル選択・ドラッグ＆ドロップで開けます。どちらも直接 `packages/` に置くこともできます。
2. Minecraft の **設定 → Mod Menu → Mod Manager** でパッケージの **Install …** 行を選び、実行します。インストールした Mod はその場で有効になり、元のファイルは `packages/installed/` に移動します。同名の Mod は上書きしません。
3. 同じページのスイッチで有効・無効を変更できます。変更はゲームの再起動なしで反映され、無効化した Mod は次回起動時にも読み込まれません。

### ScriptClient パッケージを作る

通常の ZIP（`.mods` または `.zip`）に次の配置でファイルを入れます。

```text
scriptclient-package.json   {"format":"scriptclient-mod","version":1}
mod/manifest.json           ScriptClient の Mod マニフェスト
mod/src/index.ts            例: マニフェストの entry が src/index.ts の場合
```

ZIP の中に余分な親フォルダーを作らないでください。フォルダー名はマニフェストの `name` から決まり、ファイル名には Windows で使用できる ASCII 文字を使います。上限は ZIP が 32 MiB、展開後が 64 MiB、項目数が 4096 です。リンク、DLL などのネイティブバイナリは対象外です。Mod はコードを実行するため、信頼できるパッケージだけをインストールしてください。これは Minecraft の `.mcpack` や PEX Mod Studio の旧 `.mods` ライブラリ形式とは異なります。

```text
<Minecraft.Windows.exe directory>/
├── Minecraft.Windows.exe
├── mods/
│   └── EffectFake/
│       ├── manifest.json
│       └── src/
│           └── index.ts
├── mod-data/
│   └── EffectFake/    # ファイル API の初回利用時に作成
└── packages/
```

ホストは検索先の直下だけを走査します。`.` または `_` で始まるディレクトリと、`manifest.json` がないディレクトリは読み込みません。

## 最初の Mod: EffectFake

[リポジトリ内の EffectFake](https://github.com/gamelist1990/BedrockDynamicSDK/tree/main/tests/golden/EffectFake) は、ローカルプレイヤーにだけポーション効果を適用するサンプルです。上の構成に `manifest.json` と `src/index.ts` をコピーすれば、サンプルの `.ts` をそのまま読み込めます。以下は同じ仕組みを使った最小版です。完全版には持続時間や強さの指定と `clear` 分岐もあります。

### manifest.json

```json
{
  "name": "EffectFake",
  "version": "1.0.0",
  "apiVersion": "1.0.0",
  "gameVersions": ["1.26.65.1"],
  "entry": "src/index.ts"
}
```

- `name`: フォルダ名と完全一致。先頭は英数字、以降は英数字・`.`・`_`・`-`、全体で最大 64 文字です。
- `version`: Mod のバージョンを示す空でない文字列です。`apiVersion` はホストの manifest 形式（メジャー `1`）です。TypeScript パッケージのバージョン `2.0.0` とは別です。
- `gameVersions`: 対応ゲームバージョンの空でない文字列配列です。実行中のバージョンに完全一致する値か `*` が必要です。`*` は manifest の照合を通すだけで、DLL のビルド互換性を保証しません。
- `entry`: Mod フォルダ内にある `.ts`、`.js`、`.mjs` のファイルを指定します。
- 省略可: `description`、`displayName`（既定は `name`）、`memoryMiB`（既定は 64）。

### src/index.ts

```ts
import { system, TicksPerSecond, world } from "@bedrock/sdk";

system.commands.register("effectfake", [{
  literals: ["give"],
  parameters: [{ name: "effect", kind: "effect" }],
  execute(ctx) {
    const player = world.getLocalPlayer();
    if (!player) return;
    player.addEffect(ctx.get<string>("effect"), 30 * TicksPerSecond);
    player.sendMessage("[EffectFake] Effect applied for 30 seconds.");
  },
}]);
```

ワールドに入って `/effectfake give night_vision` を実行します。効果はサーバーに送られません。完全版では `/effectfake give night_vision 30 0 true`、`/effectfake clear night_vision`、`/effectfake clear` も使えます。最小版と完全版は同じ Mod 名なので、どちらか一方を配置してください。

エントリのトップレベルでコマンドやイベントを登録できます。必要なら名前付き関数 `onEnable` / `onDisable` を公開できます。`.ts` はロード時に変換されますが型検査は行われません。開発時は [TS SDK パッケージ](https://github.com/gamelist1990/BedrockDynamicSDK/tree/main/sdk/ts) を使って型検査できます。`@bedrock/sdk` の実行時モジュールはホストが提供します。

## /mods コマンド

- `/mods list`: 認識した Mod 名、バージョン、状態、API バージョン、エラーがあればその内容を出力します。
- `/mods reload EffectFake`: 認識済みの `EffectFake` をアンロードして読み直します。JS の状態は作り直されますが、データディレクトリは残ります。差し替えに失敗しても旧インスタンスへは戻りません。新規 Mod の初回読み込みにはゲームを起動し直してください。
- `/mods path`: 現在の Mod 検索先を出力します。

これらの応答と `ctx.respond()` はホストのログに出力されます。ゲーム内のプレイヤーチャットへ明示的に表示するには `player.sendMessage()` を使います。

## TypeScript API

`@bedrock/sdk` は `world`・`system`・クラス・イベントを中心にした **クライアント用** API です。公開 API は [型宣言](https://github.com/gamelist1990/BedrockDynamicSDK/blob/main/sdk/ts/node_modules/%40bedrock/sdk/index.d.ts) と [OfficialStyle サンプル](https://github.com/gamelist1990/BedrockDynamicSDK/tree/main/exsample/ScriptClient/examples/OfficialStyle) を参照してください。サーバー版 Script API と同じ機能を提供するものではありません。

旧 SDK から移行するときは、基底クラスを継承する代わりにエントリのトップレベルで登録し、コマンドとキー割り当ては `system`、Mod データは `files` から扱います。ローカルプレイヤーの取得には `world.getLocalPlayer()` を使い、効果時間を秒で管理していたコードでは `TicksPerSecond` を掛けてから渡してください。

### イベントとスケジューラー

```ts
import { system, TicksPerSecond, world } from "@bedrock/sdk";

const spawn = world.afterEvents.playerSpawn;
const greet = spawn.subscribe(({ player, initialSpawn }) => {
  if (!initialSpawn) return;
  spawn.unsubscribe(greet);
  system.run(() => {
    if (player.isValid) player.sendMessage(`Hello at tick ${system.currentTick}`);
  });
});

const runId = system.runInterval(() => {
  if (world.getAllPlayers().length > 0) system.clearRun(runId);
}, TicksPerSecond);
```

- `subscribe(callback)` はコールバック自身を返し、`unsubscribe(callback)` が以後の配信を解除します。同じ関数の二重登録は行いません。`playerSpawn` はこのクライアントで検知したローカルのスポーンで、`initialSpawn` はその Mod の初回通知を示します。`system.afterEvents.keybindChanged` は `{ id, pressed }` を配信します。
- `system.run()`、`runTimeout()`、`runInterval()`、`clearRun()` が使えます。遅延・間隔は整数ティックです。`run()` と遅延 `0` は同期実行ではなく次のスケジューラー処理が対象です。`currentTick` は Mod ごとのスケジューラーのカウントで、最初は `0`、リロードでリセットされます。ワールド全体のティックではありません。
- `console.log()`、`info()`、`warn()`、`error()` は DLL 隣の `<ModName>.log` と、接続・表示設定に応じてゲームの Creator Content Log に出力されます。

### World、Player、Entity、Dimension

- `world.getLocalPlayer()` はクライアント拡張です。プレイヤーがまだ使えなければ `undefined` を返します。`getPlayers({ name, excludeNames })` / `getAllPlayers()` はクライアントから見えるプレイヤーが対象です。`getDimension()` / `currentDimension` は現在のクライアントディメンションのみアクセスできます。
- `Player` は `Entity` を継承します。`location` はブロック単位、`getRotation()` は度です。`getComponent()` で体力・空腹値・速度・インベントリ・装備・ドロップアイテムを読み取れます。`ItemStack` は `typeId` と `amount` のみ、`EquipmentSlot` は装備部位の定数です。`Dimension.getBlock()` は小数座標を切り下げます。`Block.setType()` はクライアントの地形操作が利用できる場合のみで、既存のスナップショットは更新されません。
- `player.addEffect()` / `Effect.duration` は **ティック** 単位です。`TicksPerSecond` は `20` です。`Infinity` を指定できますが、ネイティブが有限の最大値として保存する場合は読み取りにもその値が返ります。効果の操作はローカルプレイヤーのティック内に限られます。`sendMessage()` は文字列または `text` / `rawtext` のローカル表示です。
- 動的プロパティは Mod ごと・ワールドセッション内のメモリにのみ保存されます。永続データには `files` を使います。サーバー操作、`beforeEvents`、スポーン以外のワールドイベント、汎用のテレポート・タグ付け・エンティティ生成、アイテムメタデータ、インベントリ/装備の書き換え、翻訳/スコアメッセージは公開していません。
- `system.keybinds` は `add()`、`locale()`、`key()`。`system.settings(titleKey)` と `Setting` はクライアント設定ページの宣言用です。実際の表示はネイティブ設定バックエンドの利用可否に依存します。`brightness.setGamma()` は 0〜25 の上書きで、引数なしで解除。`dev` はプラットフォーム依存の診断用で、隔離機構ではありません。

内蔵の `@bedrock/vanilla-data` が提供するのは `dataVersion`、`MinecraftEffectTypes`、`allEffectIds()` です。生成済みパッケージの型宣言には他のカタログも載っていますが、ホスト内蔵版にはありません。

### クライアントコマンド

エントリの読み込み時に `system.commands.register(name, branches)` を使います。`name` に `/` を付けず、各分岐に `literals`、順番付きの `parameters`、`execute(ctx)` を指定します。解除は `system.commands.unregister(name)` です。

- `kind` は `effect`、`duration`、`int`、`bool`、`string`、`message`、`float`。`duration` の **チャット入力・補完候補は秒** ですが、**コード内の `default` とコマンドハンドラー内の値はティック** です。`infinity: true` で `Infinity` を許可します。`int` のみ `min` / `max` を強制できます。`message` は現在のパーサーでは一つのトークンです。
- `optional: true` で省略可能にします。`ctx.get<T>(name)` は既定値を含み、`ctx.getOptional<T>(name)` は入力がないと `undefined` です。`ctx.rawLine()` と `ctx.respond()` もハンドラーの呼び出し中だけ使います。
- シングルプレイではローカルレジストリを使います。Boolean を除く `CustomEnum` の引数は UI で値候補を保証しません。

## ファイル API とサンドボックス

`import { files } from "@bedrock/sdk"` を使います。`files.dataDir` は遅延作成されるデータルートの絶対パスを返しますが、他のメソッドに渡すパスは、このルートからの相対パスです。空文字列、絶対パス、ドライブやルートの指定は不可です。`..` は使わないでください。実装は正規化後にルート外へ出るパスを拒否する方式で、ルート内に収まる `..` まで一律に拒否するわけではありません。

```ts
import { files } from "@bedrock/sdk";

files.writeJson("config/options.json", { enabled: true });
const options = files.readJson("config/options.json");
console.info(JSON.stringify(options));
```

- テキスト: `readText()`、`writeText()`、`appendText()`。バイト列: `readBytes()`、`writeBytes()`、`appendBytes()`（書き込み入力は `Uint8Array`）。書き込みは必要に応じて親ディレクトリを作ります。
- JSON: `readJson()`、`writeJson()`。JSON の構文エラーなどは JavaScript の例外で、SDK の `code` が付くとは限りません。
- パス操作: `exists()`、`stat()`、`list()`（既定は `.`）、`mkdir()`、`rename()`、`copy()`、`remove()`。ディレクトリの削除には `{ recursive: true }` が必要です。
- ファイル API の `error.code`: `Invalid` は無効なパスや削除オプション、`NotFound` は読み取れないファイルや目的のパスがない場合など、`WrongType` は `writeBytes()` の入力型が不正な場合、`Storage` はディレクトリ作成・書き込み・名前変更などの I/O 失敗です。`exists()` は存在確認エラーでも `false` を返す場合があります。

## 実行環境の制限

ScriptClient は Node.js ではありませんが、`require("fs")`、`require("path")`、`require("events")`、最小限の `require("os")` が使えます。`node:` 接頭辞も使えます。生ソケットの `net`、プロセスを起動する `child_process`、`worker_threads` は `Unavailable` です。Node ストリーム互換の `http` / `https` は内蔵していません。HTTP 通信には下記の API を使ってください。

- `fs` は `files` と同じ Mod データディレクトリ `mod-data/<ModName>/` に限定されます。`readFileSync`、`writeFileSync`、`appendFileSync`、`existsSync`、`readdirSync`、`mkdirSync`、`rmSync`、`renameSync`、`statSync`、`copyFileSync` に対応します。既定の読み取り結果は `Uint8Array`、`"utf8"` を指定すると文字列です。`statSync` は `size`、`mtime`、`isFile()`、`isDirectory()` を返します。エラーは `Error` で、`code` は上記のファイル API と共通です。
- `fs.promises` / `fs/promises` とコールバック形式は同じ同期処理を JS ジョブキューに遅延するだけです。ディスク I/O をゲームスレッドから分離するものではありません。大きなファイルの処理には注意してください。ファイルディスクリプタ、ストリーム、`Buffer`、UTF-8 以外のエンコーディング、カスタムの open フラグはありません。
- `path` は `posix` / `win32` の純粋な文字列操作です。`resolve` は実際の作業ディレクトリではなく仮想ルート（Windows は `C:\`、Linux ホストは `/`）を使います。`fs` には `resolve` の絶対パスではなく `join` などで作った相対パスを渡してください。
- `events` は `EventEmitter` の `on`、`once`、`off`、`emit`、リスナーの列挙・削除などを提供します。`os` は `platform()`、`arch()`、`EOL` のみです。

ファイルのパス制限や QuickJS の制限は、信用できないコードを安全に実行するための **OS レベルの隔離ではありません**。Mod は信頼できるものだけを読み込んでください。

`memoryMiB` は各 Mod の QuickJS ランタイムに適用されるメモリ上限です。8〜1024 の整数を設定でき、範囲外や不正な型は警告のうえ 64 MiB に戻ります。上限に達すると QuickJS の割り当てが失敗します。DLL 全体やネイティブ側のメモリをこの値で制限するものではありません。別に JS 呼び出しのウォッチドッグがあり、長い実行は中断されます。更新処理で異常終了した Mod は `faulted` となり、`/mods reload <name>` で読み直せます。

## HTTP / HTTPS リクエスト

外部ネットワークには内蔵モジュール `@minecraft/server-net` の `http.request()` / `http.get()` とグローバルの `fetch()` でアクセスできます。公式の `@minecraft/server-net` は権限の設定が必要な Bedrock Dedicated Server 専用モジュールですが、ScriptClient ではクライアント Mod の内蔵機能です。BDS の権限ゲートはなく、読み込んだ Mod は直接ネットワークにアクセスできます。`SecretString` と BDS のテスト用フックはありません。

```ts
import { http, HttpRequest, HttpRequestMethod } from "@minecraft/server-net";

async function showStatus() {
  const request = new HttpRequest("https://example.com/")
    .setMethod(HttpRequestMethod.GET)
    .setTimeout(5); // 秒
  const response = await http.request(request);
  console.log(response.status, response.body);
}
showStatus().catch(error => console.error(error.code, error.message));
```

CommonJS 変換のためエントリでトップレベル `await` は使えません。`fetch(url, { method, headers, body, timeoutMs, maxResponseBytes })` のレスポンスは `status`、`ok`、`headers.get()`、`text()`、`json()`、`arrayBuffer()` を提供します。ブラウザの完全な Fetch API ではなく、ストリーム、Cookie ストア、CORS、`AbortSignal` はありません。

通信は非同期のみで、Windows では WinHTTP をワーカースレッドで実行し、Promise はゲームのティックで完了します。タイムアウトの既定は 10 秒、上限は 30 秒です。レスポンス本文は既定 2 MiB、設定上限 8 MiB（`HttpRequest.maxResponseBytes`、または `fetch` の同名オプション）です。独自ヘッダーのないリクエストだけ最大 5 回リダイレクトし、HTTPS から HTTP へは遷移しません。`http.cancelAll(reason)` はその Mod のリクエストを中止します。通信・制限エラーは `code`（例: `ETIMEDOUT`、`EMSGSIZE`）と `message` を持つエラーで reject され、404 などの HTTP ステータスは通常レスポンスとして返ります。

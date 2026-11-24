# ScriptClient — TypeScript Mod ガイド

ScriptClient は Minecraft Bedrock クライアント内で JavaScript / TypeScript Mod を動かす Windows DLL Mod です。このガイドでは EffectFake を例に、導入と `@bedrock/sdk` の使い方を説明します。各 Mod は専用の QuickJS 実行環境で動きます。

## 配置

`ScriptClient.dll` を通常の DLL Mod と同じ方法で読み込ませます。スクリプト Mod は既定で DLL と同じディレクトリにある `mods/` に置きます。`BEDROCK_SDK_MODS_DIR` をゲームプロセスの環境に設定すると、スクリプトの検索先だけを変更できます。ファイル API の保存先は変わらず、DLL に隣接する `mod-data/<ModName>/` です。

```text
<DLL directory>/
├── ScriptClient.dll
├── mods/
│   └── EffectFake/
│       ├── manifest.json
│       └── src/
│           └── index.ts
└── mod-data/
    └── EffectFake/    # ファイル API の初回利用時に作成
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
- `version`: Mod のバージョンを示す空でない文字列です。`apiVersion` は SDK のバージョンで、ホストはメジャー `1` を受け付けます。
- `gameVersions`: 対応ゲームバージョンの空でない文字列配列です。実行中のバージョンに完全一致する値か `*` が必要です。`*` は manifest の照合を通すだけで、DLL のビルド互換性を保証しません。
- `entry`: Mod フォルダ内にある `.ts`、`.js`、`.mjs` のファイルを指定します。
- 省略可: `description`、`displayName`（既定は `name`）、`memoryMiB`（既定は 64）。

### src/index.ts

```ts
import { Arg, Mod, world } from "@bedrock/sdk";

export default class EffectFake extends Mod {
  onEnable(): void {
    this.commands.register("effectfake", (cmd) => {
      cmd.literal("give")
        .argument("effect", Arg.effect())
        .executes((ctx) => {
          const player = world.localPlayer();
          if (!player) return;
          player.addEffect(ctx.get<string>("effect"), 30);
          player.sendMessage("[EffectFake] Effect applied for 30 seconds.");
        });
    });
    console.info("EffectFake enabled");
  }
}
```

ワールドに入って `/effectfake give night_vision` を実行します。効果はサーバーに送られません。完全版では `/effectfake give night_vision 30 0 true`、`/effectfake clear night_vision`、`/effectfake clear` も使えます。最小版と完全版は同じ Mod 名なので、どちらか一方を配置してください。

ホストはエントリの default export を引数なしで生成し、`onEnable()` を呼びます。`.ts` はロード時に変換されますが、型検査は行われません。編集時の型情報には [生成済みの TS SDK](https://github.com/gamelist1990/BedrockDynamicSDK/tree/main/sdk/ts) を使えます。`@bedrock/sdk` は実行時にはホストが提供します。

## /mods コマンド

- `/mods list`: 認識した Mod 名、バージョン、状態、API バージョン、エラーがあればその内容を出力します。
- `/mods reload EffectFake`: 認識済みの `EffectFake` をアンロードして読み直します。JS の状態は作り直されますが、データディレクトリは残ります。差し替えに失敗しても旧インスタンスへは戻りません。新規 Mod の初回読み込みにはゲームを起動し直してください。
- `/mods path`: 現在の Mod 検索先を出力します。

これらの応答と `ctx.respond()` はホストのログに出力されます。ゲーム内のプレイヤーチャットへ明示的に表示するには `player.sendMessage()` を使います。

## TypeScript API

[バインディング定義](https://github.com/gamelist1990/BedrockDynamicSDK/blob/main/exsample/ScriptClient/data/binding-registry.json) と [`@bedrock/sdk` の型宣言](https://github.com/gamelist1990/BedrockDynamicSDK/blob/main/sdk/ts/node_modules/%40bedrock/sdk/index.d.ts) が入口です。型宣言に載っていても、操作によってはクライアント側で `Unavailable` になります。

- `Mod`: `onEnable()`、`onDisable()`、`onUpdate()`、`onKeybind(id, pressed)`。`commands`、`keybinds`、`settings(titleKey)` などの入口を持ちます。
- `world`: `localPlayer()`、`getPlayers()` / `getAllPlayers()`、`currentDimension`、`getDimension()`、メッセージ、動的プロパティ。プレイヤーやディメンションがまだ利用できない場合を考慮してください。
- `Player` / `Entity` / `Dimension` / `Block`: 座標・回転、`getComponent()`、プレイヤーの `addEffect()` / `removeEffect()` / `getEffects()`、`getBlock()` / `getEntities()` など。効果の持続時間はティックではなく秒です。`ItemStack`、`EquipmentSlot` も公開されています。インベントリや装備は読み取り用で、汎用の `spawnEntity()`、`teleport()`、`addTag()` はクライアントで利用できません。
- `system`: `run()`、`runTimeout()`、`runInterval()`、`clearRun()`。遅延・間隔の単位はティックです。
- `brightness.setGamma()` は 0〜25 の上書き、引数なしで解除。`keybinds` は `add()`、`locale()`、`key()` を提供します。`Setting` と `settings()` は設定の宣言用 API を持ちますが、ScriptClient の Windows アダプターに設定ページを表示する接続はありません。
- `files`: Mod データの同期 I/O。詳細は下記を参照してください。
- `dev`: 開発用の `memory`、`symbols`、`module.info()`。生メモリ操作には範囲ガードがありますが、Mod をプロセスから隔離する仕組みではありません。

スクリプト内蔵の `@bedrock/vanilla-data` が提供するのは `dataVersion`、`MinecraftEffectTypes`、`allEffectIds()` です。生成済みパッケージの型宣言にはブロックやアイテムなど他のカタログも載っていますが、そちらはホスト内蔵版にはありません。

### イベントとログ

- `world.afterEvents.playerSpawn.subscribe(handler)` のイベントには `player` と `initialSpawn` が入ります。
- `this.keybinds.changed.subscribe(handler)` のイベントには `id` と `pressed` が入ります。購読解除には戻り値の `remove()` を使います。
- `console.log()`、`console.info()`、`console.warn()`、`console.error()` を使えます。Mod ログは、ファイルを開けた場合は DLL 隣の `<ModName>.log` に追記され、ゲームの Creator Content Log にも転送されます。Creator Content Log はゲーム側の接続と表示設定に依存します。

### 型付きコマンド

`this.commands.register(name, builder)` に `/` なしの名前を渡します。ビルダーで `literal()`、`argument(name, Arg.…())`、`executes(handler)` を組み合わせます。`executes()` で一つの分岐が確定するので、同じビルダーに続けて別の分岐を書けます。解除は `this.commands.unregister(name)` です。

- `Arg.effect()`: エフェクト ID の文字列。名前空間を省くと `minecraft:` が付きます。
- `Arg.duration()`: 秒数。`infinity: true` のとき `Infinity` を指定できます。
- `Arg.int()`: 整数。`min` / `max` で範囲を指定できます。
- `Arg.bool()`: `true` または `false`。ハンドラーでは `boolean` です。
- `Arg.string()` / `Arg.message()`: 文字列。現行の ScriptClient パーサーでは `message` も一つのトークンとして読むため、未引用の残りの行がすべて入るとは限りません。
- `Arg.float()`: ハンドラーでは `number`。ローカルコマンド UI では Float 引数は数値欄ではなくテキスト扱いで、`min` / `max` は現行の Float パーサーでは強制されません。

`optional: true` で引数を省略可能にし、`default` で既定値を設定できます。`ctx.get<T>(name)` は省略時の既定値を含む値を返し、`ctx.getOptional<T>(name)` は入力されなかった引数に対して `undefined` を返します。`ctx.rawLine()` で受け取った行を参照できます。コンテキストはハンドラー呼び出し中に使います。

シングルプレイでは、統合サーバー向けのパケット注入ではなくローカルレジストリを使います。このモードの `CustomEnum` 引数には値候補が付きません（`true` / `false` の組はネイティブの Boolean 扱い）。`suggestions` に値を書いても、ローカル UI の候補を保証するものではありません。

## ファイル API とサンドボックス

`this.files` または `import { files } from "@bedrock/sdk"` を使います。`files.dataDir` は遅延作成されるデータルートの絶対パスを返しますが、他のメソッドに渡すパスは、このルートからの相対パスです。空文字列、絶対パス、ドライブやルートの指定は不可です。`..` は使わないでください。実装は正規化後にルート外へ出るパスを拒否する方式で、ルート内に収まる `..` まで一律に拒否するわけではありません。

```ts
import { files } from "@bedrock/sdk";

files.writeJson("config/options.json", { enabled: true });
const options = files.readJson("config/options.json");
console.info(JSON.stringify(options));
```

- テキスト: `readText()`、`writeText()`、`appendText()`。バイト列: `readBytes()`、`writeBytes()`（型宣言の入力は `Uint8Array`）。書き込みは必要に応じて親ディレクトリを作ります。
- JSON: `readJson()`、`writeJson()`。JSON の構文エラーなどは JavaScript の例外で、SDK の `code` が付くとは限りません。
- パス操作: `exists()`、`stat()`、`list()`（既定は `.`）、`mkdir()`、`rename()`、`remove()`。ディレクトリの削除には `{ recursive: true }` が必要です。
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

## Writing mods like the Script API

`import { world, system } from "@minecraft/server"` をそのまま使えます。公式のスタイルで書かれた [OfficialStyle サンプル](https://github.com/gamelist1990/BedrockDynamicSDK/tree/main/exsample/ScriptClient/examples/OfficialStyle) と、実装済み範囲だけを記載した [型宣言](https://github.com/gamelist1990/BedrockDynamicSDK/blob/main/sdk/ts/node_modules/%40minecraft/server/index.d.ts) を参照してください。`@bedrock/sdk` は従来どおり利用できます。

- `world.getPlayers()` / `getAllPlayers()` が返すのはクライアントから読めるプレイヤーです。`getPlayers` の絞り込みは `name` / `excludeNames` のみ。`world.getDimension()` は現在のクライアントディメンションだけ利用できます。`localPlayer()` は公式互換モジュールにはなく、`@bedrock/sdk` に残ります。
- `world.afterEvents.playerSpawn.subscribe(callback)` はコールバックを返し、`unsubscribe(callback)` で解除できます。クライアントが検知したスポーンだけが対象で、`beforeEvents` には対応イベントがありません。`system.run` / `runTimeout` / `runInterval` / `clearRun` はティック単位で使えます。
- `Player.sendMessage` は文字列と `text` / `rawtext` を受け付けます。`Player.addEffect` と `getEffects` の持続時間は **ティック** です（20 ティック = 1 秒）。`@bedrock/sdk` で指定・取得する効果時間は引き続き **秒** です。効果やメッセージはクライアント側だけに適用されます。
- `Dimension.getBlock`、`Block.typeId` / `location`、インベントリなどの `ItemStack.typeId` / `amount` は読み取り用です。サーバー側の操作や未対応イベント、アイテムの追加メタデータは使えません。

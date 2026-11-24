# ScriptClient — Stage B JS/TS プラグインホスト

`DESIGN.md`（`~/Documents/PEXData/stage-b-design/DESIGN.md`）に従う Stage B の成果物。
C++ 参照実装 `DynamicClient` とは独立した DLL で、AutoMine には依存しない。

## 構成

```
exsample/ScriptClient/
  core/                 ゲーム非依存のホスト本体（Phase 1 テスト可能）
    manifest.*            manifest.json 検証（apiVersion ^1.0.0）
    mod_scanner.*         mods/ 1階層走査
    js_runtime.*          quickjs-ng Runtime/Context・メモリ上限・interrupt
    transpiler.*          Sucrase（別 internal runtime）＋ (path,mtime,size,sha256) キャッシュ
    module_loader.*       CommonJS require（node_modules / 循環 / json / TS）
    sdk_bindings.*        JS SDK が使うネイティブバインディング
    command_model.*       コマンド宣言 → wire 定義・行パース
    plugin_host.*         複数 Mod 統括・ライフサイクル・/mods
    vanilla_catalog.hpp   data/vanilla/1.26.60.json 由来のエフェクト ID
  src/
    main.cpp              DllMain + ワーカー（§8.2 初期化順）
    command_service.*     NativeInjectedCommands への定義適配・キュー
    script_mod.hpp        1 JS Mod = 1 sdk::Mod
  tests/stage_b_test.cpp   Phase 1 ホスト単体テスト
  data/binding-registry.json  旧 Stage B/C バインディングのインベントリ
third_party/quickjs-ng/   vendored（commit dce8c0bb…）
third_party/sucrase/      Sucrase 3.35.1 の esbuild IIFE バンドル（MIT）
```

## ビルド

```bash
./build-linux.sh --stage-b
# dist/1.26.65.1/ScriptClient.dll
# dist/1.26.65.1/mods/EffectFake/   （リポジトリ内のサンプルを配置）
```

## Phase 1（ゲーム無し）

```bash
cmake -S exsample/ScriptClient -B /tmp/stageb -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/stageb
ctest --test-dir /tmp/stageb --output-on-failure
```

## TypeScript SDK

公開 API は [`sdk/ts/node_modules/@bedrock/sdk`](../../sdk/ts/node_modules/@bedrock/sdk) の
`index.js` / `index.d.ts` が正本。`tools/generate_ts_sdk.py` は正本を書き換えず、JS を
ホスト用ヘッダーに埋め込み、vanilla-data パッケージを生成する。
Manifest の `apiVersion` (`1.0.0`) と TS パッケージの `2.0.0` は別。
ホストは実行前に型検査を行わないため、開発時に `tsc --noEmit` を使う。

例とクライアント固有の制約は [ScriptClient ガイド](../../docs/scriptclient.md) を参照。

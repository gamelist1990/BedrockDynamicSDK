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
    sdk_bindings.*        @bedrock/sdk（apiVersion 1.0.0 凍結表面）
    command_model.*       ビルダー宣言 → wire 定義・行パース
    plugin_host.*         複数 Mod 統括・ライフサイクル・/mods
    vanilla_catalog.hpp   data/vanilla/1.26.60.json 由来のエフェクト ID
  src/
    main.cpp              DllMain + ワーカー（§8.2 初期化順）
    command_service.*     NativeInjectedCommands への定義適配・キュー
    script_mod.hpp        1 JS Mod = 1 sdk::Mod
  tests/stage_b_test.cpp   Phase 1 ホスト単体テスト
  data/binding-registry.json  Stage C 生成用の機械可読バインディング表
third_party/quickjs-ng/   vendored（commit dce8c0bb…）
third_party/sucrase/      Sucrase 3.35.1 の esbuild IIFE バンドル（MIT）
```

## ビルド

```bash
./build-linux.sh --stage-b
# dist/1.26.65.1/ScriptClient.dll
# dist/1.26.65.1/mods/EffectFake/   （無変更のゴールデンケースを配置）
```

## Phase 1（ゲーム無し）

```bash
cmake -S exsample/ScriptClient -B /tmp/stageb -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/stageb
/tmp/stageb/stage_b_test
```

## 既知の制約（Stage C / BOL に引き継ぐ）

- 設定ページ・キーバインド・brightness は JS 側ビルダー呼び出しを記録する実装で、
  実 `SettingsPage` / `KeyBindings` への完全な翻訳は未完了（EffectFake は不使用）。
- `WorldBackend` はホスト内で 1 共有。厳密な per-Mod `World` 束縛は ScriptMod 側の
  `world()` を最初の Mod から採る形に留まる。
- item-entity-detection の ItemStack 読み取りは Stage A 側 API 追加待ち。
- 型検査は行わない（Stage C の `.d.ts` + `tsc --noEmit` が担当）。
- BOL 実地検証（DESIGN §10.3 Phase 2）は本リポジトリでは実行しない。

#!/usr/bin/env python3
"""Stage C test suite:
1. Determinism & idempotency of tools/generate_ts_sdk.py.
2. Complete parity between exsample/ScriptClient/data/binding-registry.json and
   the generated @bedrock/sdk (.d.ts + index.js + generated_sdk_bootstrap.hpp).
3. Complete parity of @bedrock/vanilla-data (.d.ts + index.js + package.json),
   including the minecraft:item entity supplement.
4. Honest reflection of unsupportedLimitations (absence of nonexistent methods).
5. TypeScript type-checking (`tsc --noEmit`) against:
   a) Unmodified golden project ~/WorkSpace/ts-mods/EffectFake (copied into a
      temp dir with the generated node_modules/@bedrock/{sdk,vanilla-data},
      verifying standard Node package resolution for `"@bedrock/sdk": "*"`).
   b) Comprehensive positive surface test exercising every registered export,
      member, component, event, setting, keybind, brightness, unit conversion,
      and @bedrock/vanilla-data export.
   c) Negative `@ts-expect-error` test confirming that Stage A unsupported
      operations (ItemStack.nameTag/lore, InventoryComponent.setItem/addItem,
      EquippableComponent.setEquipment) are rejected at compile time.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import generate_ts_sdk  # noqa: E402


# The golden EffectFake lives in the developer workspace; CI runners do not
# have that layout, so an identical copy is vendored under tests/golden/ and
# used whenever the workspace copy is absent. GOLDEN_MD5 pins both.
_WORKSPACE_GOLDEN_DIR = Path.home() / "WorkSpace/ts-mods/EffectFake"
_REPO_GOLDEN_DIR = Path(__file__).resolve().parent / "golden/EffectFake"
GOLDEN_EFFECTFAKE_DIR = (
    _WORKSPACE_GOLDEN_DIR if _WORKSPACE_GOLDEN_DIR.is_dir() else _REPO_GOLDEN_DIR
)
GOLDEN_MD5 = {
    "manifest.json": "0c94695308b86a509a61fae960983590",
    "package.json": "fd6432d5c15af0ac507a3f9b026b4240",
    "README.md": "cefc65adc433c5a59872daca20668a9d",
    "src/index.ts": "fc5812e1ad7200ebd19cc1f4f63ac82d",
}


def find_tsc() -> str:
    found = shutil.which("tsc")
    if found:
        return found
    candidates = [
        Path.home() / ".bun/install/cache/typescript@5.8.2@@@1/bin/tsc",
        Path.home() / ".bun/install/cache/typescript@5.9.3@@@1/bin/tsc",
        Path.home() / ".bun/install/cache/typescript@6.0.3@@@1/bin/tsc",
        Path.home() / "WorkSpace/PEXModStudio/node_modules/.bin/tsc",
        Path.home() / "WorkSpace/FileShare/node_modules/.bin/tsc",
        Path.home() / "WorkSpace/DevBox/Discordbot/node_modules/.bin/tsc",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("Could not locate TypeScript compiler (tsc)")


def md5_file(path: Path) -> str:
    return hashlib.md5(path.read_bytes()).hexdigest()


def verify_golden_unmodified() -> None:
    assert GOLDEN_EFFECTFAKE_DIR.is_dir(), f"Golden EffectFake missing at {GOLDEN_EFFECTFAKE_DIR}"
    for rel, expected in GOLDEN_MD5.items():
        actual = md5_file(GOLDEN_EFFECTFAKE_DIR / rel)
        assert actual == expected, f"Golden file {rel} modified! expected {expected}, got {actual}"


def test_generator_idempotency_and_parity() -> None:
    registry_path = ROOT / "exsample/ScriptClient/data/binding-registry.json"
    out_dir = ROOT / "sdk/ts"
    bootstrap_path = ROOT / "exsample/ScriptClient/core/generated_sdk_bootstrap.hpp"

    generate_ts_sdk.generate_all(ROOT, registry_path, out_dir, bootstrap_path)

    sdk_dir = out_dir / "node_modules/@bedrock/sdk"
    vanilla_dir = out_dir / "node_modules/@bedrock/vanilla-data"

    sdk_dts = (sdk_dir / "index.d.ts").read_text(encoding="utf-8")
    sdk_js = (sdk_dir / "index.js").read_text(encoding="utf-8")
    sdk_pkg = json.loads((sdk_dir / "package.json").read_text(encoding="utf-8"))

    assert sdk_pkg["name"] == "@bedrock/sdk"
    assert sdk_pkg["version"] == "1.0.0"
    assert sdk_pkg["types"] == "./index.d.ts"
    assert sdk_pkg["main"] == "./index.js"

    # Re-run into a fresh temp directory and verify byte-for-byte identity
    with tempfile.TemporaryDirectory(prefix="stagec-idem-") as tmp:
        tmp_path = Path(tmp)
        tmp_boot = tmp_path / "generated_sdk_bootstrap.hpp"
        generate_ts_sdk.generate_all(ROOT, registry_path, tmp_path / "ts", tmp_boot)
        assert (tmp_path / "ts/node_modules/@bedrock/sdk/index.d.ts").read_text(encoding="utf-8") == sdk_dts
        assert (tmp_path / "ts/node_modules/@bedrock/sdk/index.js").read_text(encoding="utf-8") == sdk_js
        assert tmp_boot.read_text(encoding="utf-8") == bootstrap_path.read_text(encoding="utf-8")

    # Verify parity with binding-registry.json
    registry = json.loads(registry_path.read_text(encoding="utf-8"))
    for exp in registry["exports"]:
        name = exp["name"]
        assert name in sdk_dts, f"Export {name!r} missing from @bedrock/sdk/index.d.ts"
        if exp["kind"] == "value":
            assert f"{name}:" in sdk_js or f"class {name}" in sdk_js or f"const {name}" in sdk_js, (
                f"Value export {name!r} missing from @bedrock/sdk/index.js"
            )

    for code in registry["errorCodes"]:
        assert f'"{code}"' in sdk_dts, f"ErrorCode {code!r} missing from index.d.ts"

    for arg_kind in registry["commandArgs"]:
        assert f"{arg_kind}(" in sdk_dts, f"Arg.{arg_kind} missing from index.d.ts"
        assert f"{arg_kind}: function" in sdk_js, f"Arg.{arg_kind} missing from index.js"

    assert "export declare const files: Files;" in sdk_dts
    assert "files: filesObject" in sdk_js
    assert "H.filesReadText" in sdk_js and "H.filesWriteBytes" in sdk_js

    for member in registry["members"]:
        assert member["bound"] is True
        tail = member["path"].split(".")[-1]
        if tail == "new ItemStack":
            assert "class ItemStack" in sdk_dts
        else:
            assert tail in sdk_dts, f"Member {member['path']!r} missing from index.d.ts"

    # Verify unsupported limitations are NOT falsely exposed as methods/properties
    assert "nameTag" not in sdk_dts.split("export declare class ItemStack")[1].split("}")[0]
    assert "lore" not in sdk_dts.split("export declare class ItemStack")[1].split("}")[0]
    assert "setItem(" not in sdk_dts
    assert "addItem(" not in sdk_dts
    assert "setEquipment(" not in sdk_dts

    # Verify @bedrock/vanilla-data
    vanilla_dts = (vanilla_dir / "index.d.ts").read_text(encoding="utf-8")
    vanilla_js = (vanilla_dir / "index.js").read_text(encoding="utf-8")
    vanilla_pkg = json.loads((vanilla_dir / "package.json").read_text(encoding="utf-8"))
    assert vanilla_pkg["name"] == "@bedrock/vanilla-data"
    assert 'Item: "minecraft:item"' in vanilla_dts
    assert 'Item: "minecraft:item"' in vanilla_js
    assert "MinecraftEffectTypes" in vanilla_dts
    assert "allEffectIds" in vanilla_dts


def test_tsc_golden_and_surface() -> None:
    verify_golden_unmodified()
    tsc = find_tsc()
    sdk_node_modules = ROOT / "sdk/ts/node_modules"

    # 1. Direct tsc --noEmit against the unmodified golden file in place
    cmd_direct = [
        tsc,
        "--noEmit",
        "--strict",
        "--target",
        "ES2022",
        "--module",
        "ESNext",
        "--moduleResolution",
        "node",
        "--baseUrl",
        str(sdk_node_modules),
        str(GOLDEN_EFFECTFAKE_DIR / "src/index.ts"),
    ]
    res_direct = subprocess.run(cmd_direct, capture_output=True, text=True)
    assert res_direct.returncode == 0, (
        f"Direct tsc on golden EffectFake failed:\n{res_direct.stdout}\n{res_direct.stderr}"
    )

    # 2. Standard Node package resolution test: copy EffectFake to a temp folder,
    # link/copy node_modules/@bedrock/{sdk,vanilla-data}, and run tsc with no baseUrl/paths tricks.
    with tempfile.TemporaryDirectory(prefix="stagec-tsc-") as tmp:
        tmp_dir = Path(tmp)
        ef_copy = tmp_dir / "EffectFake"
        shutil.copytree(GOLDEN_EFFECTFAKE_DIR, ef_copy)
        shutil.copytree(sdk_node_modules, ef_copy / "node_modules")

        cmd_pkg = [
            tsc,
            "--noEmit",
            "--strict",
            "--target",
            "ES2022",
            "--module",
            "ESNext",
            "--moduleResolution",
            "node",
            str(ef_copy / "src/index.ts"),
        ]
        res_pkg = subprocess.run(cmd_pkg, cwd=ef_copy, capture_output=True, text=True)
        assert res_pkg.returncode == 0, (
            f"Package-resolved tsc on EffectFake failed:\n{res_pkg.stdout}\n{res_pkg.stderr}"
        )

        # 3. Comprehensive positive + negative (@ts-expect-error) type test
        surface_test = ef_copy / "src/surface_check.ts"
        surface_test.write_text(
            """import {
  Arg,
  EquipmentSlot,
  ItemStack,
  Mod,
  Setting,
  brightness,
  files,
  system,
  world,
} from "@bedrock/sdk";
import type {
  Block,
  BlockPosition,
  FileEntry,
  FileStat,
  Files,
  CommandBuilder,
  CommandContext,
  Dimension,
  DynamicProperty,
  Effect,
  EffectOptions,
  Entity,
  ErrorCode,
  EquippableComponent,
  FoodComponent,
  HealthComponent,
  InventoryComponent,
  ItemEntityComponent,
  KeybindEvent,
  MovementComponent,
  Player,
  PlayerSpawnAfterEvent,
  RemoveOptions,
  Rotation,
  SdkError,
  SettingValue,
  Subscription,
  Vector3,
} from "@bedrock/sdk";
import {
  MinecraftBlockTypes,
  MinecraftDimensionTypes,
  MinecraftEffectTypes,
  MinecraftEntityTypes,
  MinecraftItemTypes,
  allEffectIds,
  allEntityIds,
  dataVersion,
} from "@bedrock/vanilla-data";
import type { EffectId, EntityId } from "@bedrock/vanilla-data";

export default class FullSurfaceMod extends Mod {
  private sub?: Subscription;

  onEnable(): void {
    const ver: string = dataVersion;
    const itemEntityId: EntityId = MinecraftEntityTypes.Item;
    const nightVisionId: EffectId = MinecraftEffectTypes.NightVision;
    const allEffects: ReadonlyArray<EffectId> = allEffectIds();
    const allEntities: ReadonlyArray<EntityId> = allEntityIds();
    void ver;
    void itemEntityId;
    void allEffects;
    void allEntities;

    this.commands.register("testcmd", (cmd: CommandBuilder) => {
      cmd
        .literal("run")
        .argument("eff", Arg.effect({ optional: false }))
        .argument("dur", Arg.duration({ optional: true, default: 30, infinity: true }))
        .argument("amp", Arg.int({ optional: true, default: 0, min: 0, max: 255, suggestions: [0, 1] }))
        .argument("vis", Arg.bool({ optional: true, default: true }))
        .argument("tag", Arg.string({ optional: true, default: "none" }))
        .argument("ratio", Arg.float({ optional: true, default: 1.0 }))
        .argument("msg", Arg.message({ optional: true }))
        .executes((ctx: CommandContext) => {
          const eff = ctx.get<string>("eff");
          const dur = ctx.get<number>("dur");
          const maybe = ctx.getOptional<string>("msg");
          const line: string = ctx.rawLine();
          ctx.respond(`${eff}:${dur}:${maybe ?? ""}:${line}`);
        });
    });
    this.commands.unregister("testcmd");

    const page = this.settings("mod.title")
      .modName("mod.name")
      .add(Setting.toggle("enabled", "mod.enabled", true))
      .add(Setting.slider("speed", "mod.speed", 1.0, 0.0, 10.0, 0.5))
      .add(Setting.choice("mode", "mod.mode", "a", [["a", "mod.a"], ["b", "mod.b"]]))
      .add(Setting.text("note", "mod.note", "mod.note.desc"))
      .locale("en_US", { "mod.title": "Title" })
      .locale("ja_JP", [["mod.title", "タイトル"]])
      .onChange((id: string, val: SettingValue) => {
        world.sendMessage(`${id}=${String(val)}`);
      })
      .registerPage();

    const currentSpeed: number = page.value<number>("speed");
    void currentSpeed;
    page.unregisterPage();

    this.keybinds
      .add("zoom", "key.zoom", "KeyC")
      .locale("en_US", { "key.zoom": "Zoom" });
    const mappedKey: string | undefined = this.keybinds.key("zoom");
    void mappedKey;
    const keySub: Subscription = this.keybinds.changed.subscribe((ev: KeybindEvent) => {
      const k: string = ev.id;
      const p: boolean = ev.pressed;
      void k;
      void p;
    });
    keySub.remove();

    this.sub = world.afterEvents.playerSpawn.subscribe((ev: PlayerSpawnAfterEvent) => {
      const p: Player = ev.player;
      const init: boolean = ev.initialSpawn;
      p.sendMessage(`spawned:${init}`);
    });

    brightness.setGamma(15.0);
    brightness.setGamma(undefined);

    const surface: Files = files;
    const root: string = surface.dataDir;
    surface.writeText("nested/note.txt", "hello");
    surface.appendText("nested/note.txt", "!");
    const text: string = surface.readText("nested/note.txt");
    surface.writeBytes("nested/blob.bin", new Uint8Array([1, 2, 3]));
    const bytes: Uint8Array = surface.readBytes("nested/blob.bin");
    surface.writeJson("nested/data.json", { ok: true });
    const parsed: unknown = surface.readJson("nested/data.json");
    const present: boolean = surface.exists("nested/note.txt");
    const stat: FileStat = surface.stat("nested/note.txt");
    const entries: FileEntry[] = surface.list("nested");
    const rootEntries: FileEntry[] = surface.list();
    surface.mkdir("nested/deeper");
    const removeOptions: RemoveOptions = { recursive: true };
    surface.remove("nested/deeper", removeOptions);
    surface.rename("nested/blob.bin", "nested/blob2.bin");
    void root;
    void text;
    void bytes[0];
    void parsed;
    void present;
    void stat.size;
    void stat.directory;
    void stat.modifiedMs;
    void entries.length;
    void rootEntries.length;

    const tid = system.run(() => {});
    const tid2 = system.runTimeout(() => {}, 20);
    const tid3 = system.runInterval(() => {}, 40);
    const cleared: boolean = system.clearRun(tid);
    void tid2;
    void tid3;
    void cleared;

    const p: Player | undefined = world.localPlayer();
    const allP: Player[] = world.getAllPlayers();
    const allP2: Player[] = world.getPlayers();
    void allP;
    void allP2;

    if (p) {
      const name: string | undefined = p.name;
      const loc: Vector3 | undefined = p.location;
      const rot: Rotation | undefined = p.rotation;
      const dim: Dimension | undefined = p.dimension;
      void name;
      void loc;
      void rot;
      void dim;

      const opts: EffectOptions = { amplifier: 2, showParticles: false };
      p.addEffect(nightVisionId, Number.POSITIVE_INFINITY, opts);
      p.addEffect(MinecraftEffectTypes.Speed, 30, { amplifier: 1, showParticles: true });
      const active: Effect[] = p.getEffects();
      for (const e of active) {
        const sec: number = e.duration;
        const amp: number = e.amplifier;
        void sec;
        void amp;
        p.removeEffect(e.typeId);
      }
      p.clearEffects();

      const hp: HealthComponent | undefined = p.getComponent("minecraft:health");
      const food: FoodComponent | undefined = p.getComponent("minecraft:food");
      const mov: MovementComponent | undefined = p.getComponent("minecraft:movement");
      const inv: InventoryComponent | undefined = p.getComponent("minecraft:inventory");
      const eq: EquippableComponent | undefined = p.getComponent("minecraft:equippable");
      const itemComp: ItemEntityComponent | undefined = p.getComponent("minecraft:item");
      void hp?.currentValue;
      void food?.currentValue;
      void mov?.velocity.x;
      void itemComp?.itemStack.amount;

      if (inv) {
        const s0: ItemStack | undefined = inv.getItem(0);
        const s1: ItemStack | undefined = inv.container.getItem(1);
        void s0;
        void s1;
        // @ts-expect-error Stage A InventoryComponent is read-only; setItem must not exist
        inv.setItem(0, new ItemStack(MinecraftItemTypes.Apple, 1));
        // @ts-expect-error Stage A InventoryContainer is read-only; addItem must not exist
        inv.container.addItem(new ItemStack(MinecraftItemTypes.Apple, 1));
      }

      if (eq) {
        const mainHand: ItemStack | undefined = eq.getEquipment(EquipmentSlot.MainHand);
        void mainHand;
        // @ts-expect-error Stage A EquippableComponent is read-only; setEquipment must not exist
        eq.setEquipment(EquipmentSlot.MainHand, new ItemStack(MinecraftItemTypes.Apple, 1));
      }
    }

    const stack = new ItemStack(MinecraftItemTypes.Diamond, 3);
    const stType: string = stack.typeId;
    const stAmt: number = stack.amount;
    void stType;
    void stAmt;
    // @ts-expect-error Stage A ItemStack does not expose nameTag
    void stack.nameTag;
    // @ts-expect-error Stage A ItemStack does not expose lore
    void stack.lore;

    const curDim: Dimension | undefined =
      world.currentDimension ?? world.getDimension(MinecraftDimensionTypes.Overworld);
    if (curDim) {
      const bpos: BlockPosition = { x: 0, y: 64, z: 0 };
      const blk: Block | undefined = curDim.getBlock(bpos);
      if (blk) {
        const bt: string = blk.typeId;
        const bl: BlockPosition = blk.location;
        void bt;
        void bl;
        blk.setType(MinecraftBlockTypes.Stone);
      }
      const entities: Entity[] = curDim.getEntities();
      for (const ent of entities) {
        if (ent.typeId === MinecraftEntityTypes.Item) {
          const dropped: ItemStack | undefined = ent.getItemStack();
          void dropped?.typeId;
        }
      }
    }

    const dp: DynamicProperty | undefined = world.getDynamicProperty("k");
    world.setDynamicProperty("k", dp ?? { x: 1, y: 2, z: 3 });

    const code: ErrorCode = "Unavailable";
    const err: SdkError = Object.assign(new Error("unavailable"), { code });
    void err;
  }

  onDisable(): void {
    this.sub?.remove();
  }
}
""",
            encoding="utf-8",
        )

        cmd_surface = [
            tsc,
            "--noEmit",
            "--strict",
            "--target",
            "ES2022",
            "--module",
            "ESNext",
            "--moduleResolution",
            "node",
            str(ef_copy / "src/index.ts"),
            str(surface_test),
        ]
        res_surface = subprocess.run(cmd_surface, cwd=ef_copy, capture_output=True, text=True)
        assert res_surface.returncode == 0, (
            f"Surface + negative @ts-expect-error tsc check failed:\n{res_surface.stdout}\n{res_surface.stderr}"
        )

    verify_golden_unmodified()


if __name__ == "__main__":
    test_generator_idempotency_and_parity()
    test_tsc_golden_and_surface()
    print("stage_c_ts_sdk_test: ALL CHECKS PASSED")

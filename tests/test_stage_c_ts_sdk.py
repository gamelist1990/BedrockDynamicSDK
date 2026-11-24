#!/usr/bin/env python3
"""Check the SDK packaging, generated vanilla catalog, and TS contracts."""

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


GOLDEN_EFFECTFAKE_DIR = ROOT / "tests/golden/EffectFake"
GOLDEN_MD5 = {
    "manifest.json": "0c94695308b86a509a61fae960983590",
    "package.json": "fd6432d5c15af0ac507a3f9b026b4240",
    "README.md": "cefc65adc433c5a59872daca20668a9d",
    "src/index.ts": "3d907a415b16f4ad19b1977704a620c6",
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
    out_dir = ROOT / "sdk/ts"
    bootstrap_path = ROOT / "exsample/ScriptClient/core/generated_sdk_bootstrap.hpp"
    sdk_dir = out_dir / "node_modules/@bedrock/sdk"
    vanilla_dir = out_dir / "node_modules/@bedrock/vanilla-data"
    sdk_sources = {name: (sdk_dir / name).read_bytes() for name in ("index.js", "index.d.ts", "package.json")}
    sdk_pkg = json.loads(sdk_sources["package.json"])
    assert sdk_pkg["name"] == "@bedrock/sdk"
    assert sdk_pkg["version"] == "2.0.0"
    assert sdk_pkg["types"] == "./index.d.ts"
    assert sdk_pkg["main"] == "./index.js"

    with tempfile.TemporaryDirectory(prefix="stagec-idem-") as tmp:
        tmp_path = Path(tmp)
        tmp_boot = tmp_path / "generated_sdk_bootstrap.hpp"
        paths = generate_ts_sdk.generate_all(ROOT, tmp_path / "ts", tmp_boot)
        assert tmp_boot.read_bytes() == bootstrap_path.read_bytes()
        assert sdk_sources["index.js"] in tmp_boot.read_bytes()
        for name, expected in sdk_sources.items():
            assert (paths["sdk_dir"] / name).read_bytes() == expected
        for name in ("index.js", "index.d.ts", "package.json"):
            assert (paths["vanilla_dir"] / name).read_bytes() == (vanilla_dir / name).read_bytes()
        outputs = [tmp_boot, *paths["sdk_dir"].iterdir(), *paths["vanilla_dir"].iterdir()]
        timestamps = [p.stat().st_mtime_ns for p in outputs]
        generate_ts_sdk.generate_all(ROOT, tmp_path / "ts", tmp_boot)
        assert timestamps == [p.stat().st_mtime_ns for p in outputs]
        generate_ts_sdk.generate_all(ROOT, out_dir, None)
        assert sdk_sources == {name: (sdk_dir / name).read_bytes() for name in sdk_sources}

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

    # 1. Direct tsc --noEmit against the updated golden fixture in place
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
  EquipmentSlot,
  ItemStack,
  Setting,
  brightness,
  dev,
  TicksPerSecond,
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

let sub: ((event: PlayerSpawnAfterEvent) => void) | undefined;

export function onEnable(): void {
  const ver: string = dataVersion;
  const itemEntityId: EntityId = MinecraftEntityTypes.Item;
  const nightVisionId: EffectId = MinecraftEffectTypes.NightVision;
  const allEffects: ReadonlyArray<EffectId> = allEffectIds();
  const allEntities: ReadonlyArray<EntityId> = allEntityIds();
  void ver;
  void itemEntityId;
  void allEffects;
  void allEntities;

  system.commands.register("testcmd", [{
    literals: ["run"],
    parameters: [
      { name: "eff", kind: "effect" },
      { name: "dur", kind: "duration", optional: true, default: 30 * TicksPerSecond, infinity: true },
      { name: "amp", kind: "int", optional: true, default: 0, min: 0, max: 255, suggestions: [0, 1] },
      { name: "vis", kind: "bool", optional: true, default: true },
      { name: "tag", kind: "string", optional: true, default: "none" },
      { name: "ratio", kind: "float", optional: true, default: 1.0 },
      { name: "msg", kind: "message", optional: true },
    ],
    execute(ctx: CommandContext) {
      const eff = ctx.get<string>("eff");
      const dur = ctx.get<number>("dur");
      const maybe = ctx.getOptional<string>("msg");
      const line: string = ctx.rawLine();
      ctx.respond(`${eff}:${dur}:${maybe ?? ""}:${line}`);
    },
  }]);
  system.commands.unregister("testcmd");

  const page = system.settings("mod.title")
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

  system.keybinds
    .add("zoom", "key.zoom", "KeyC")
    .locale("en_US", { "key.zoom": "Zoom" });
  const mappedKey: string | undefined = system.keybinds.key("zoom");
  void mappedKey;
  const keySub = system.afterEvents.keybindChanged.subscribe((ev: KeybindEvent) => {
    const k: string = ev.id;
    const p: boolean = ev.pressed;
    void k;
    void p;
  });
  system.afterEvents.keybindChanged.unsubscribe(keySub);

  sub = world.afterEvents.playerSpawn.subscribe((ev: PlayerSpawnAfterEvent) => {
    const p: Player = ev.player;
    const init: boolean = ev.initialSpawn;
    p.sendMessage({ rawtext: [{ text: `spawned:${init}` }] });
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
  surface.appendBytes("nested/blob.bin", new Uint8Array([4]));
  surface.rename("nested/blob.bin", "nested/blob2.bin");
  surface.copy("nested/blob2.bin", "nested/blob3.bin", { exclusive: true });
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

  const tick: number = system.currentTick;
  const tid = system.run(() => { void tick; });
  const tid2 = system.runTimeout(() => {}, 20);
  const tid3 = system.runInterval(() => {}, 40);
  const cleared: boolean = system.clearRun(tid);
  void tid2;
  void tid3;
  void cleared;

  const p: Player | undefined = world.getLocalPlayer();
  const allP: Player[] = world.getAllPlayers();
  const allP2: Player[] = world.getPlayers({ name: "Alex", excludeNames: [] });
  void allP;
  void allP2;

  if (p) {
    const name: string | undefined = p.name;
    const loc: Vector3 | undefined = p.location;
    const rot: Rotation | undefined = p.getRotation();
    const dim: Dimension | undefined = p.dimension;
    void name;
    void loc;
    void rot;
    void dim;
    void p.id;
    void p.typeId;
    void p.isValid;
    // @ts-expect-error General teleport is not available on the client
    p.teleport({ x: 0, y: 64, z: 0 });
    // @ts-expect-error Client tags are not available
    p.addTag("untested");

    const opts: EffectOptions = { amplifier: 2, showParticles: false };
    p.addEffect(nightVisionId, Number.POSITIVE_INFINITY, opts);
    p.addEffect(MinecraftEffectTypes.Speed, 30, { amplifier: 1, showParticles: true });
    const active: Effect[] = p.getEffects();
    void p.getEffect(nightVisionId)?.showParticles;
    for (const e of active) {
      const ticks: number = e.duration;
      const amp: number = e.amplifier;
      void ticks;
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
      const s0: ItemStack | undefined = inv.container.getItem(0);
      const s1: ItemStack | undefined = inv.container.getItem(1);
      void s0;
      void s1;
      // @ts-expect-error Stage A InventoryComponent is read-only; setItem must not exist
      inv.setItem(0, new ItemStack(MinecraftItemTypes.Apple, 1));
      // @ts-expect-error Stage A InventoryContainer is read-only; addItem must not exist
      inv.container.addItem(new ItemStack(MinecraftItemTypes.Apple, 1));
    }

    if (eq) {
      const mainHand: ItemStack | undefined = eq.getEquipment(EquipmentSlot.Mainhand);
      void mainHand;
      // @ts-expect-error Stage A EquippableComponent is read-only; setEquipment must not exist
      eq.setEquipment(EquipmentSlot.Mainhand, new ItemStack(MinecraftItemTypes.Apple, 1));
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
    // @ts-expect-error Client entity spawning has no substrate
    curDim.spawnEntity("pig", { x: 0, y: 64, z: 0 });
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
        const dropped: ItemStack | undefined = ent.getComponent("minecraft:item")?.itemStack;
        void dropped?.typeId;
      }
    }
  }

  const dp: DynamicProperty | undefined = world.getDynamicProperty("k");
  world.setDynamicProperty("k", dp ?? { x: 1, y: 2, z: 3 });

  const code: ErrorCode = "Unavailable";
  const err: SdkError = Object.assign(new Error("unavailable"), { code });
  void err;
  dev.memory.readBytes(0, 1);
  dev.memory.readU32(0);
  dev.memory.readI64(0);
  dev.memory.readF32(0);
  dev.memory.readPtr(0);
  dev.memory.writeBytes(0, new Uint8Array());
  dev.memory.scanAob("AA");
  dev.symbols.list();
  dev.symbols.status("example");
  dev.module.info();
  // @ts-expect-error no before phase is available
  void world.beforeEvents;
  // @ts-expect-error no old localPlayer method
  world.localPlayer();
}

export function onDisable(): void {
  if (sub) world.afterEvents.playerSpawn.unsubscribe(sub);
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

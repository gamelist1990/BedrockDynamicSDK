#!/usr/bin/env python3
"""Stage C generator: build @bedrock/sdk and @bedrock/vanilla-data packages and
the Stage B runtime bootstrap header from exsample/ScriptClient/data/binding-registry.json
and the pinned vanilla-data catalog (DESIGN.md §11).

This script is deterministic and idempotent. Running it again only rewrites
files whose content actually changed.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
from typing import Any


CATEGORIES = {
    "block": "Block",
    "item": "Item",
    "entity": "Entity",
    "biome": "Biome",
    "effect": "Effect",
    "dimension": "Dimension",
}

CONSTANT_RE = re.compile(r'^\s+([A-Z][A-Za-z0-9_]*) = "(minecraft:[a-z0-9_./-]+)",?$')


def write_if_changed(path: Path, text: str) -> bool:
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return True


def load_registry(registry_path: Path) -> dict[str, Any]:
    doc = json.loads(registry_path.read_text(encoding="utf-8"))
    if doc.get("schema") != "bedrock-dynamic-sdk/stage-b/binding-registry/1":
        raise ValueError(f"Unsupported binding registry schema in {registry_path}")
    if doc.get("apiVersion") != "1.0.0":
        raise ValueError(f"Unexpected apiVersion in {registry_path}: {doc.get('apiVersion')!r}")
    for member in doc.get("members", []):
        if not member.get("bound", False):
            raise ValueError(f"Unbound member in registry: {member.get('path')!r}")
    return doc


def load_vanilla_catalog(root: Path, game_version: str) -> tuple[str, str, dict[str, Any]]:
    manifest = json.loads((root / "data/manifest.json").read_text(encoding="utf-8"))
    version = game_version or manifest["default_version"]
    source_rel = manifest["versions"][version]["windows-x64"]["vanilla_data"]
    catalog = json.loads((root / source_rel).read_text(encoding="utf-8"))
    if catalog.get("schema") != 1 or set(catalog.get("ids", {})) != set(CATEGORIES):
        raise ValueError(f"Unexpected vanilla catalog schema in {source_rel}")
    return version, catalog["source"]["version"], catalog


def build_runtime_core_js(registry: dict[str, Any]) -> str:
    api_version = registry["apiVersion"]
    cmd_args = registry.get("commandArgs", ["effect", "duration", "int", "bool", "string", "message", "float"])

    arg_entries = []
    for arg_kind in cmd_args:
        arg_entries.append(
            f"    {arg_kind}: function (o) {{ return Object.assign({{ __kind: '{arg_kind}' }}, o || {{}}); }}"
        )
    arg_block = ",\n".join(arg_entries)

    return f"""// Generated from exsample/ScriptClient/data/binding-registry.json (apiVersion {api_version}).
// Do not edit by hand; re-run tools/generate_ts_sdk.py.
(function (root) {{
  "use strict";

  function installSdkModule(H) {{
    if (!H) {{
      throw new Error("@bedrock/sdk requires the BedrockDynamicSDK ScriptClient host runtime");
    }}

    class CommandBuilder {{
      constructor(id) {{
        this.__id = id;
        this.__literals = [];
        this.__args = [];
      }}
      literal(value) {{
        this.__literals.push(String(value));
        return this;
      }}
      argument(name, spec) {{
        this.__args.push({{ name: String(name), spec: spec }});
        return this;
      }}
      executes(handler) {{
        H.executes(this.__id, this.__literals, this.__args, handler);
        this.__literals = [];
        this.__args = [];
        return this;
      }}
    }}

    class ItemStack {{
      constructor(typeId, amount) {{
        if (typeof typeId !== 'string' || typeId.length === 0) {{
          const err = new Error('ItemStack requires a non-empty typeId');
          err.code = 'Invalid';
          throw err;
        }}
        const amt = amount === undefined ? 1 : Number(amount);
        if (!Number.isFinite(amt) || amt < 0 || Math.trunc(amt) !== amt) {{
          const err = new Error('ItemStack amount must be a non-negative integer');
          err.code = 'Invalid';
          throw err;
        }}
        Object.defineProperty(this, '__typeId', {{ value: String(typeId), enumerable: false }});
        Object.defineProperty(this, '__amount', {{ value: amt >>> 0, enumerable: false }});
      }}
      get typeId() {{ return this.__typeId; }}
      get amount() {{ return this.__amount; }}
    }}

    class Block {{
      constructor(dimId, pos, typeId) {{
        Object.defineProperty(this, '__dimId', {{ value: String(dimId), enumerable: false }});
        Object.defineProperty(this, '__pos', {{
          value: {{ x: Math.trunc(pos.x), y: Math.trunc(pos.y), z: Math.trunc(pos.z) }},
          enumerable: false
        }});
        Object.defineProperty(this, '__typeId', {{ value: String(typeId), enumerable: false }});
      }}
      get typeId() {{ return this.__typeId; }}
      get location() {{ return {{ x: this.__pos.x, y: this.__pos.y, z: this.__pos.z }}; }}
      setType(typeId) {{
        return H.blockSetType(this.__dimId, this.__pos.x, this.__pos.y, this.__pos.z, String(typeId));
      }}
    }}

    class Dimension {{
      constructor(dimId) {{
        Object.defineProperty(this, '__dimId', {{ value: String(dimId), enumerable: false }});
      }}
      get id() {{ return this.__dimId; }}
      get typeId() {{ return this.__dimId; }}
      getBlock(pos) {{
        if (!pos || typeof pos !== 'object') {{
          const err = new Error('getBlock expects a BlockPosition {{x,y,z}}');
          err.code = 'Invalid';
          throw err;
        }}
        const t = H.dimensionGetBlock(this.__dimId, Math.trunc(pos.x), Math.trunc(pos.y), Math.trunc(pos.z));
        if (t === undefined) return undefined;
        return new Block(this.__dimId, pos, t);
      }}
      getEntities() {{
        const list = H.dimensionGetEntities(this.__dimId) || [];
        return list.map(function (entry) {{
          return entry.isPlayer ? new Player(entry.id) : new Entity(entry.id);
        }});
      }}
      spawnEntity(typeId, pos) {{
        if (!pos || typeof pos !== 'object') {{
          const err = new Error('spawnEntity expects a Vector3 {{x,y,z}}');
          err.code = 'Invalid';
          throw err;
        }}
        const id = H.dimensionSpawnEntity(this.__dimId, String(typeId), Number(pos.x), Number(pos.y), Number(pos.z));
        return new Entity(id);
      }}
    }}

    class Entity {{
      constructor(id) {{
        Object.defineProperty(this, '__id', {{ value: Number(id), enumerable: false }});
      }}
      get id() {{ return String(this.__id); }}
      get isValid() {{ return Boolean(H.entityIsValid(this.__id)); }}
      get typeId() {{ return H.entityTypeId(this.__id); }}
      get location() {{ return H.entityLocation(this.__id); }}
      get rotation() {{ return H.entityRotation(this.__id); }}
      get dimension() {{
        const d = H.entityDimension(this.__id);
        return d === undefined ? undefined : new Dimension(d);
      }}
      getRotation() {{ return H.entityRotation(this.__id); }}
      getItemStack() {{
        const item = H.entityGetItemStack(this.__id);
        return item === undefined ? undefined : new ItemStack(item.typeId, item.amount);
      }}
      teleport(pos) {{
        if (!pos || typeof pos !== 'object') {{
          const err = new Error('teleport expects a Vector3 {{x,y,z}}');
          err.code = 'Invalid';
          throw err;
        }}
        return H.entityTeleport(this.__id, Number(pos.x), Number(pos.y), Number(pos.z));
      }}
      addTag(tag) {{
        return H.entityAddTag(this.__id, String(tag));
      }}
      getDynamicProperty(key) {{
        return H.entityGetDynamicProperty(this.__id, String(key));
      }}
      setDynamicProperty(key, value) {{
        return H.entitySetDynamicProperty(this.__id, String(key), value);
      }}
      getComponent(componentId) {{
        const raw = H.entityGetComponent(this.__id, String(componentId));
        if (raw === undefined) return undefined;
        if (raw.__kind === 'inventory') {{
          const slots = (raw.slots || []).map(function (s) {{
            return s ? new ItemStack(s.typeId, s.amount) : undefined;
          }});
          const container = {{
            size: slots.length,
            getItem: function (slot) {{
              const idx = Math.trunc(Number(slot));
              return (idx >= 0 && idx < slots.length) ? slots[idx] : undefined;
            }}
          }};
          return {{
            typeId: 'minecraft:inventory',
            size: slots.length,
            container: container,
            getItem: container.getItem
          }};
        }}
        if (raw.__kind === 'equippable') {{
          const map = raw.slots || {{}};
          return {{
            typeId: 'minecraft:equippable',
            getEquipment: function (slot) {{
              const key = String(slot);
              const s = map[key];
              return s ? new ItemStack(s.typeId, s.amount) : undefined;
            }}
          }};
        }}
        if (raw.typeId === 'minecraft:item' && raw.itemStack) {{
          return {{
            typeId: 'minecraft:item',
            itemStack: new ItemStack(raw.itemStack.typeId, raw.itemStack.amount)
          }};
        }}
        return raw;
      }}
    }}

    class Player extends Entity {{
      constructor(id) {{
        super(id);
      }}
      get name() {{ return H.playerName(this.__id); }}
      sendMessage(text) {{ return H.playerSendMessage(this.__id, String(text)); }}
      addEffect(typeId, durationSeconds, options) {{
        const opts = options || {{}};
        const amp = opts.amplifier === undefined ? 0 : Number(opts.amplifier);
        const vis = opts.showParticles === undefined ? true : Boolean(opts.showParticles);
        return H.playerAddEffect(this.__id, String(typeId), Number(durationSeconds), amp, vis);
      }}
      removeEffect(typeId) {{
        return H.playerRemoveEffect(this.__id, String(typeId));
      }}
      clearEffects() {{
        return H.playerClearEffects(this.__id);
      }}
      getEffects() {{
        return H.playerGetEffects(this.__id);
      }}
    }}

    const Setting = {{
      toggle: function (id, labelKey, initial, descriptionKey) {{
        return {{
          kind: 'toggle',
          id: String(id),
          labelKey: String(labelKey),
          initial: Boolean(initial),
          descriptionKey: descriptionKey === undefined ? '' : String(descriptionKey)
        }};
      }},
      slider: function (id, labelKey, initial, minimum, maximum, step, descriptionKey) {{
        return {{
          kind: 'slider',
          id: String(id),
          labelKey: String(labelKey),
          initial: Number(initial),
          minimum: Number(minimum),
          maximum: Number(maximum),
          step: step === undefined ? 1 : Number(step),
          descriptionKey: descriptionKey === undefined ? '' : String(descriptionKey)
        }};
      }},
      choice: function (id, labelKey, initial, choices, descriptionKey) {{
        return {{
          kind: 'choice',
          id: String(id),
          labelKey: String(labelKey),
          initial: String(initial),
          choices: Array.isArray(choices) ? choices : [],
          descriptionKey: descriptionKey === undefined ? '' : String(descriptionKey)
        }};
      }},
      text: function (id, labelKey, descriptionKey) {{
        return {{
          kind: 'text',
          id: String(id),
          labelKey: String(labelKey),
          descriptionKey: descriptionKey === undefined ? '' : String(descriptionKey)
        }};
      }}
    }};

    class SettingsPageBuilder {{
      constructor(titleKey) {{
        this.__titleKey = String(titleKey);
      }}
      add(setting) {{
        H.settingsAdd(this.__titleKey, setting);
        return this;
      }}
      locale(localeId, entries) {{
        H.settingsLocale(this.__titleKey, String(localeId), entries);
        return this;
      }}
      onChange(callback) {{
        H.settingsOnChange(this.__titleKey, callback);
        return this;
      }}
      modName(key) {{
        H.settingsModName(this.__titleKey, String(key));
        return this;
      }}
      registerPage() {{
        H.settingsRegisterPage(this.__titleKey);
        return this;
      }}
      unregisterPage() {{
        H.settingsUnregisterPage(this.__titleKey);
        return this;
      }}
      value(settingId) {{
        return H.settingsValue(this.__titleKey, String(settingId));
      }}
    }}

    const keybindsObject = {{
      add: function (id, labelKey, defaultKey) {{
        H.keybindAdd(String(id), String(labelKey), defaultKey);
        return keybindsObject;
      }},
      locale: function (localeId, entries) {{
        H.keybindLocale(String(localeId), entries);
        return keybindsObject;
      }},
      key: function (id) {{
        return H.keybindKey(String(id));
      }},
      changed: {{
        subscribe: function (fn) {{
          const subId = H.keybindSubscribe(fn);
          return {{
            remove: function () {{ H.keybindUnsubscribe(subId); }}
          }};
        }}
      }}
    }};

    const brightnessObject = {{
      setGamma: function (gamma) {{
        H.brightnessSetGamma(gamma);
      }}
    }};

    // Synchronous, sandboxed per-Mod file surface. The host owns the data root
    // and rejects absolute/escaping paths; readJson/writeJson are thin JS
    // wrappers over the same readText/writeText primitives.
    const filesObject = {{
      get dataDir() {{ return H.filesDataDir(); }},
      readText: function (path) {{ return H.filesReadText(String(path)); }},
      writeText: function (path, text) {{ return H.filesWriteText(String(path), String(text)); }},
      appendText: function (path, text) {{ return H.filesAppendText(String(path), String(text)); }},
      readBytes: function (path) {{ return H.filesReadBytes(String(path)); }},
      writeBytes: function (path, bytes) {{ return H.filesWriteBytes(String(path), bytes); }},
      readJson: function (path) {{ return JSON.parse(H.filesReadText(String(path))); }},
      writeJson: function (path, value) {{ return H.filesWriteText(String(path), JSON.stringify(value)); }},
      exists: function (path) {{ return H.filesExists(String(path)); }},
      stat: function (path) {{ return H.filesStat(String(path)); }},
      list: function (path) {{ return H.filesList(path === undefined ? '.' : String(path)); }},
      mkdir: function (path) {{ return H.filesMkdir(String(path)); }},
      remove: function (path, options) {{
        return H.filesRemove(String(path), Boolean(options && options.recursive));
      }},
      rename: function (from, to) {{ return H.filesRename(String(from), String(to)); }}
    }};

    const worldSingleton = {{
      localPlayer: function () {{
        const id = H.worldLocalPlayer();
        return id === undefined ? undefined : new Player(id);
      }},
      getAllPlayers: function () {{
        const ids = H.worldGetAllPlayers() || [];
        return ids.map(function (id) {{ return new Player(id); }});
      }},
      getPlayers: function () {{
        const ids = H.worldGetAllPlayers() || [];
        return ids.map(function (id) {{ return new Player(id); }});
      }},
      get currentDimension() {{
        const d = H.worldCurrentDimension();
        return d === undefined ? undefined : new Dimension(d);
      }},
      getDimension: function (id) {{
        const d = H.worldGetDimension(String(id));
        return d === undefined ? undefined : new Dimension(d);
      }},
      sendMessage: function (text) {{
        return H.worldSendMessage(String(text));
      }},
      getDynamicProperty: function (key) {{
        return H.worldGetDynamicProperty(String(key));
      }},
      setDynamicProperty: function (key, value) {{
        return H.worldSetDynamicProperty(String(key), value);
      }},
      afterEvents: {{
        playerSpawn: {{
          subscribe: function (fn) {{
            const subId = H.worldSubscribePlayerSpawn(fn);
            return {{
              remove: function () {{ H.worldUnsubscribePlayerSpawn(subId); }}
            }};
          }},
          unsubscribe: function (handle) {{
            if (handle && typeof handle.remove === 'function') handle.remove();
          }}
        }}
      }}
    }};

    class Mod {{
      constructor() {{
        const scope = H.createScope();
        this.__scope = scope;
        this.commands = {{
          register(name, callback) {{
            callback(new CommandBuilder(H.beginCommand(String(name))));
          }},
          unregister(name) {{ H.unregisterCommand(String(name)); }}
        }};
        this.settings = function (titleKey) {{
          return new SettingsPageBuilder(titleKey);
        }};
        this.keybinds = keybindsObject;
        this.brightness = brightnessObject;
        this.files = filesObject;
        this.world = worldSingleton;
        this.system = H.systemObject();
      }}
      onEnable() {{}}
      onDisable() {{}}
      onUpdate() {{}}
      onKeybind() {{}}
    }}

    const Arg = {{
{arg_block}
    }};

    const EquipmentSlot = {{
      MainHand: 'MainHand',
      Offhand: 'Offhand',
      Head: 'Head',
      Chest: 'Chest',
      Legs: 'Legs',
      Feet: 'Feet'
    }};

    root.__stagebMakePlayer = function (id) {{ return new Player(id); }};
    root.__stagebMakeEntity = function (id) {{ return new Entity(id); }};

    const devObject = {{
      memory: {{
        readBytes: function (address, length) {{
          return H.devMemoryReadBytes(Number(address), Number(length));
        }},
        readU32: function (address) {{
          return H.devMemoryReadU32(Number(address));
        }},
        readI64: function (address) {{
          return H.devMemoryReadI64(Number(address));
        }},
        readF32: function (address) {{
          return H.devMemoryReadF32(Number(address));
        }},
        readPtr: function (address) {{
          return H.devMemoryReadPtr(Number(address));
        }},
        writeBytes: function (address, bytes) {{
          return H.devMemoryWriteBytes(Number(address), bytes);
        }},
        scanAob: function (pattern) {{
          return H.devMemoryScanAob(String(pattern));
        }}
      }},
      symbols: {{
        list: function () {{
          return H.devSymbolsList();
        }},
        status: function (name) {{
          return H.devSymbolsStatus(String(name));
        }}
      }},
      module: {{
        info: function () {{
          return H.devModuleInfo();
        }}
      }}
    }};

    const sdkModule = {{
      Mod: Mod,
      Arg: Arg,
      Setting: Setting,
      ItemStack: ItemStack,
      EquipmentSlot: EquipmentSlot,
      brightness: brightnessObject,
      files: filesObject,
      world: worldSingleton,
      system: H.systemObject(),
      dev: devObject
    }};
    root.__sdkModule = sdkModule;
    return sdkModule;
  }}

  if (root.__bedrockHost) {{
    installSdkModule(root.__bedrockHost);
  }}
  if (typeof module !== 'undefined' && module.exports) {{
    module.exports = root.__sdkModule || new Proxy({{}}, {{
      get: function (_target, prop) {{
        if (root.__sdkModule) return root.__sdkModule[prop];
        if (root.__bedrockHost) return installSdkModule(root.__bedrockHost)[prop];
        throw new Error("@bedrock/sdk is a BedrockDynamicSDK host-bound module and cannot be invoked outside ScriptClient");
      }}
    }});
  }}
}})(typeof globalThis !== 'undefined' ? globalThis : this);
"""


def build_cpp_bootstrap_header(runtime_js: str) -> str:
    return (
        "// Generated from exsample/ScriptClient/data/binding-registry.json by tools/generate_ts_sdk.py.\n"
        "// Do not edit by hand.\n"
        "#pragma once\n\n"
        "namespace stageb {\n\n"
        'inline constexpr const char* kGeneratedBootstrap = R"STAGEB_BINDINGS(\n'
        + runtime_js
        + ')STAGEB_BINDINGS";\n\n'
        "} // namespace stageb\n"
    )


def build_sdk_dts(registry: dict[str, Any]) -> str:
    api_version = registry["apiVersion"]
    package_name = registry.get("packageName", "@bedrock/sdk")
    error_codes = registry.get("errorCodes", [])
    error_union = " | ".join(f'"{c}"' for c in error_codes)
    limitations = registry.get("unsupportedLimitations", [])

    members_by_prefix: dict[str, list[dict[str, Any]]] = {}
    for m in registry.get("members", []):
        path = m["path"]
        prefix = path.split(".", 1)[0] if "." in path else path
        members_by_prefix.setdefault(prefix, []).append(m)

    limitation_comments = "\n".join(
        f" * - {item['feature']}: {item['reason']}" for item in limitations
    )

    lines: list[str] = [
        f"// Generated from exsample/ScriptClient/data/binding-registry.json (schema: {registry['schema']}).",
        f"// Package: {package_name} (apiVersion {api_version})",
        "// Do not edit by hand; re-run tools/generate_ts_sdk.py.",
        "/**",
        " * Stage A / Stage B unsupported limitations (intentionally omitted from types or throwing Unavailable):",
        limitation_comments,
        " */",
        "",
        f"export type ErrorCode = {error_union};",
        "",
        "export interface SdkError extends Error {",
        "  readonly code: ErrorCode;",
        "}",
        "",
        "export interface Vector3 {",
        "  x: number;",
        "  y: number;",
        "  z: number;",
        "}",
        "",
        "export interface BlockPosition {",
        "  x: number;",
        "  y: number;",
        "  z: number;",
        "}",
        "",
        "export interface Rotation {",
        "  /** Pitch in degrees. */",
        "  x: number;",
        "  /** Yaw in degrees. */",
        "  y: number;",
        "}",
        "",
        "export type DynamicProperty = boolean | number | string | Vector3;",
        "",
        "export type TaskId = number;",
        "",
        "export interface Subscription {",
        "  remove(): void;",
        "}",
        "",
        "export interface EventSignal<T> {",
        "  subscribe(handler: (event: T) => void): Subscription;",
        "  unsubscribe?(subscription: Subscription): void;",
        "}",
        "",
        "/**",
        " * Immutable item stack snapshot (`bedrock::sdk::ItemStack`).",
        " * Note: Extra metadata (nameTag, lore, enchants, durability, dynamicProperties)",
        " * is not exposed by Stage A and is intentionally absent from this type.",
        " */",
        "export declare class ItemStack {",
        "  constructor(typeId: string, amount?: number);",
        "  readonly typeId: string;",
        "  readonly amount: number;",
        "}",
        "",
        "export const EquipmentSlot: {",
        '  readonly MainHand: "MainHand";',
        '  readonly Offhand: "Offhand";',
        '  readonly Head: "Head";',
        '  readonly Chest: "Chest";',
        '  readonly Legs: "Legs";',
        '  readonly Feet: "Feet";',
        "};",
        "export type EquipmentSlot = (typeof EquipmentSlot)[keyof typeof EquipmentSlot];",
        "",
        "export interface HealthComponent {",
        '  readonly typeId: "minecraft:health";',
        "  readonly currentValue: number;",
        "}",
        "",
        "export interface FoodComponent {",
        '  readonly typeId: "minecraft:food";',
        "  readonly currentValue: number;",
        "}",
        "",
        "export interface MovementComponent {",
        '  readonly typeId: "minecraft:movement";',
        "  readonly velocity: Vector3;",
        "}",
        "",
        "/**",
        " * Read-only inventory container snapshot (`bedrock::sdk::InventoryComponent`).",
        " * Note: Container mutation (setItem / addItem) is not supported by Stage A.",
        " */",
        "export interface InventoryContainer {",
        "  readonly size: number;",
        "  getItem(slot: number): ItemStack | undefined;",
        "}",
        "",
        "export interface InventoryComponent {",
        '  readonly typeId: "minecraft:inventory";',
        "  readonly size: number;",
        "  readonly container: InventoryContainer;",
        "  getItem(slot: number): ItemStack | undefined;",
        "}",
        "",
        "/**",
        " * Read-only equipment snapshot (`bedrock::sdk::EquippableComponent`).",
        " * Note: Equipment mutation (setEquipment) is not supported by Stage A.",
        " */",
        "export interface EquippableComponent {",
        '  readonly typeId: "minecraft:equippable";',
        "  getEquipment(slot: EquipmentSlot | string): ItemStack | undefined;",
        "}",
        "",
        "export interface ItemEntityComponent {",
        '  readonly typeId: "minecraft:item";',
        "  readonly itemStack: ItemStack;",
        "}",
        "",
        "export interface EntityComponentMap {",
        '  "minecraft:health": HealthComponent;',
        '  "health": HealthComponent;',
        '  "minecraft:food": FoodComponent;',
        '  "minecraft:player.hunger": FoodComponent;',
        '  "minecraft:hunger": FoodComponent;',
        '  "food": FoodComponent;',
        '  "hunger": FoodComponent;',
        '  "minecraft:movement": MovementComponent;',
        '  "movement": MovementComponent;',
        '  "minecraft:inventory": InventoryComponent;',
        '  "inventory": InventoryComponent;',
        '  "minecraft:equippable": EquippableComponent;',
        '  "equippable": EquippableComponent;',
        '  "minecraft:item": ItemEntityComponent;',
        '  "item": ItemEntityComponent;',
        "}",
        "",
        "export type EntityComponent = EntityComponentMap[keyof EntityComponentMap];",
        "",
        "export interface EffectOptions {",
        "  /** Effect amplifier (0..255, default 0). */",
        "  amplifier?: number;",
        "  /** Whether to show potion particles (default true). */",
        "  showParticles?: boolean;",
        "}",
        "",
        "export interface Effect {",
        "  readonly typeId: string;",
        "  /** Remaining duration in seconds (`Number.POSITIVE_INFINITY` for infinite effects). */",
        "  readonly duration: number;",
        "  readonly amplifier: number;",
        "  readonly showParticles?: boolean;",
        "}",
        "",
        "export interface Block {",
        "  readonly typeId: string;",
        "  readonly location: BlockPosition;",
        "  /** Client block mutation (`Block::setType`). Throws `SdkError` (`code: \"Unavailable\"`) when unsupported. */",
        "  setType(typeId: string): void;",
        "}",
        "",
        "export interface Dimension {",
        "  readonly id: string;",
        "  readonly typeId: string;",
        "  getBlock(position: BlockPosition): Block | undefined;",
        "  getEntities(): Entity[];",
        "  /** Note: Stage A client builds throw `SdkError` with `code: \"Unavailable\"`. */",
        "  spawnEntity(typeId: string, position: Vector3): Entity;",
        "}",
        "",
        "export interface Entity {",
        "  readonly id: string;",
        "  readonly isValid: boolean;",
        "  readonly typeId: string | undefined;",
        "  readonly location: Vector3 | undefined;",
        "  readonly rotation: Rotation | undefined;",
        "  readonly dimension: Dimension | undefined;",
        "  getRotation(): Rotation | undefined;",
        "  /** Item stack for dropped `minecraft:item` entities (`item-entity-detection`). */",
        "  getItemStack(): ItemStack | undefined;",
        "  /** Note: Stage A client builds throw `SdkError` with `code: \"Unavailable\"`. */",
        "  teleport(position: Vector3): void;",
        "  /** Note: Stage A client builds throw `SdkError` with `code: \"Unavailable\"`. */",
        "  addTag(tag: string): boolean;",
        "  getDynamicProperty(key: string): DynamicProperty | undefined;",
        "  setDynamicProperty(key: string, value?: DynamicProperty | null): void;",
        "  getComponent<K extends keyof EntityComponentMap>(componentId: K): EntityComponentMap[K] | undefined;",
        "  getComponent(componentId: string): EntityComponent | undefined;",
        "}",
        "",
        "export interface Player extends Entity {",
        "  readonly name: string | undefined;",
        "  sendMessage(text: string): void;",
        "  /**",
        "   * Apply a client-local potion effect (`Player::addEffect`).",
        "   * @param typeId Effect identifier (e.g. `\"minecraft:night_vision\"` or `\"night_vision\"`).",
        "   * @param durationSeconds Duration in **seconds** (converted x20 to game ticks; `Infinity` is preserved as +inf).",
        "   * @param options Optional amplifier (0..255) and particle visibility.",
        "   */",
        "  addEffect(typeId: string, durationSeconds: number, options?: EffectOptions): void;",
        "  removeEffect(typeId: string): boolean;",
        "  clearEffects(): void;",
        "  /** Returns active effects with `duration` in **seconds** (converted from game ticks / 20; `Infinity` preserved). */",
        "  getEffects(): Effect[];",
        "}",
        "",
        "export interface PlayerSpawnAfterEvent {",
        "  readonly player: Player;",
        "  readonly initialSpawn: boolean;",
        "}",
        "",
        "export interface WorldAfterEvents {",
        "  readonly playerSpawn: EventSignal<PlayerSpawnAfterEvent>;",
        "}",
        "",
        "export interface World {",
        "  localPlayer(): Player | undefined;",
        "  getAllPlayers(): Player[];",
        "  getPlayers(): Player[];",
        "  readonly currentDimension: Dimension | undefined;",
        "  getDimension(typeId: string): Dimension | undefined;",
        "  sendMessage(text: string): void;",
        "  getDynamicProperty(key: string): DynamicProperty | undefined;",
        "  setDynamicProperty(key: string, value?: DynamicProperty | null): void;",
        "  readonly afterEvents: WorldAfterEvents;",
        "}",
        "",
        "export interface System {",
        "  run(task: () => void): TaskId;",
        "  runTimeout(task: () => void, ticks: number): TaskId;",
        "  runInterval(task: () => void, ticks: number): TaskId;",
        "  clearRun(id: TaskId): boolean;",
        "}",
        "",
        "export interface BrightnessOverride {",
        "  /** Set client gamma override in 0..25, or `undefined` to restore previous/vanilla gamma. */",
        "  setGamma(gamma?: number): void;",
        "}",
        "",
        "/** Metadata returned by `files.stat(path)`. `modifiedMs` is a Unix epoch millisecond timestamp. */",
        "export interface FileStat {",
        "  readonly size: number;",
        "  readonly directory: boolean;",
        "  readonly modifiedMs: number;",
        "}",
        "",
        "/** One entry returned by `files.list(path?)`. */",
        "export interface FileEntry {",
        "  readonly name: string;",
        "  readonly directory: boolean;",
        "}",
        "",
        "export interface RemoveOptions {",
        "  /** Required to remove a directory; ignored for files. */",
        "  readonly recursive?: boolean;",
        "}",
        "",
        "/**",
        " * Synchronous file API for this mod's persistent data root.",
        " * Every path is relative to `dataDir`; absolute paths and paths that",
        " * normalize outside the root throw. Operations throw on failure.",
        " */",
        "export interface Files {",
        "  /** Absolute path of this mod's data root, created lazily on first use. */",
        "  readonly dataDir: string;",
        "  readText(path: string): string;",
        "  /** Writes UTF-8 text and creates missing parent directories. */",
        "  writeText(path: string, text: string): void;",
        "  appendText(path: string, text: string): void;",
        "  readBytes(path: string): Uint8Array;",
        "  /** Writes bytes and creates missing parent directories. */",
        "  writeBytes(path: string, bytes: Uint8Array): void;",
        "  /** `JSON.parse(readText(path))` — throws on malformed JSON. */",
        "  readJson(path: string): unknown;",
        "  /** `writeText(path, JSON.stringify(value))`. */",
        "  writeJson(path: string, value: unknown): void;",
        "  exists(path: string): boolean;",
        "  stat(path: string): FileStat;",
        "  /** Entries in `path` (default `.`); throws if it is not a directory. */",
        "  list(path?: string): FileEntry[];",
        "  /** Creates `path` and all missing parents. */",
        "  mkdir(path: string): void;",
        "  /** Removes a file, or a directory when `{ recursive: true }` is passed. */",
        "  remove(path: string, options?: RemoveOptions): void;",
        "  rename(from: string, to: string): void;",
        "}",
        "",
        "export type SettingValue = boolean | number | string;",
        "export type ChoiceEntry = readonly [id: string, labelKey: string] | { id: string; labelKey?: string; label?: string } | string;",
        "export type LocaleEntries = Readonly<Record<string, string>> | ReadonlyArray<readonly [key: string, value: string] | { key: string; value: string }>;",
        "",
        "export interface SettingDescriptor {",
        '  readonly kind: "toggle" | "slider" | "choice" | "text";',
        "  readonly id: string;",
        "  readonly labelKey: string;",
        "  readonly descriptionKey?: string;",
        "  readonly initial?: SettingValue;",
        "  readonly minimum?: number;",
        "  readonly maximum?: number;",
        "  readonly step?: number;",
        "  readonly choices?: ReadonlyArray<ChoiceEntry>;",
        "}",
        "",
        "export interface SettingFactory {",
        "  toggle(id: string, labelKey: string, initial: boolean, descriptionKey?: string): SettingDescriptor;",
        "  slider(id: string, labelKey: string, initial: number, minimum: number, maximum: number, step?: number, descriptionKey?: string): SettingDescriptor;",
        "  choice(id: string, labelKey: string, initial: string, choices: ReadonlyArray<ChoiceEntry>, descriptionKey?: string): SettingDescriptor;",
        "  text(id: string, labelKey: string, descriptionKey?: string): SettingDescriptor;",
        "}",
        "",
        "export interface SettingsPage {",
        "  add(setting: SettingDescriptor | string): this;",
        "  locale(localeId: string, entries: LocaleEntries): this;",
        "  onChange(callback: (settingId: string, value: SettingValue) => void): this;",
        "  modName(key: string): this;",
        "  registerPage(): this;",
        "  unregisterPage(): this;",
        "  value<T extends SettingValue = SettingValue>(settingId: string): T;",
        "}",
        "",
        "export interface KeybindEvent {",
        "  readonly id: string;",
        "  readonly pressed: boolean;",
        "}",
        "",
        "export interface KeyBindings {",
        "  add(id: string, labelKey: string, defaultKey: string): this;",
        "  locale(localeId: string, entries: LocaleEntries): this;",
        "  key(id: string): string | undefined;",
        "  readonly changed: EventSignal<KeybindEvent>;",
        "}",
        "",
        "export interface BaseArgOptions<T> {",
        "  optional?: boolean;",
        "  default?: T;",
        "  suggestions?: ReadonlyArray<T | string>;",
        "}",
        "",
        "export interface EffectArgOptions extends BaseArgOptions<string> {}",
        "",
        "export interface DurationArgOptions extends BaseArgOptions<number> {",
        "  /** Allow `\"Infinity\"` (parsed as `Number.POSITIVE_INFINITY` seconds). */",
        "  infinity?: boolean;",
        "}",
        "",
        "export interface IntArgOptions extends BaseArgOptions<number> {",
        "  min?: number;",
        "  max?: number;",
        "}",
        "",
        "export interface FloatArgOptions extends BaseArgOptions<number> {",
        "  min?: number;",
        "  max?: number;",
        "}",
        "",
        "export interface BoolArgOptions extends BaseArgOptions<boolean> {}",
        "export interface StringArgOptions extends BaseArgOptions<string> {}",
        "",
        "export interface ArgSpec<K extends string = string> {",
        "  readonly __kind: K;",
        "  readonly optional?: boolean;",
        "  readonly default?: unknown;",
        "  readonly infinity?: boolean;",
        "  readonly min?: number;",
        "  readonly max?: number;",
        "  readonly suggestions?: ReadonlyArray<unknown>;",
        "}",
        "",
        "export interface ArgFactory {",
        '  effect(options?: EffectArgOptions): ArgSpec<"effect">;',
        '  /** Duration argument in **seconds** (supports `Infinity` when `infinity: true`). */',
        '  duration(options?: DurationArgOptions): ArgSpec<"duration">;',
        '  int(options?: IntArgOptions): ArgSpec<"int">;',
        '  bool(options?: BoolArgOptions): ArgSpec<"bool">;',
        '  string(options?: StringArgOptions): ArgSpec<"string">;',
        '  message(options?: StringArgOptions): ArgSpec<"message">;',
        '  float(options?: FloatArgOptions): ArgSpec<"float">;',
        "}",
        "",
        "export interface CommandContext {",
        "  get<T = unknown>(name: string): T;",
        "  getOptional<T = unknown>(name: string): T | undefined;",
        "  rawLine(): string;",
        "  respond(text: string): void;",
        "}",
        "",
        "export interface CommandBuilder {",
        "  literal(value: string): this;",
        "  argument(name: string, spec: ArgSpec): this;",
        "  executes(handler: (ctx: CommandContext) => void): this;",
        "}",
        "",
        "export interface CommandManager {",
        "  register(name: string, builder: (cmd: CommandBuilder) => void): void;",
        "  unregister(name: string): void;",
        "}",
        "",
        "export declare class Mod {",
        "  constructor();",
        "  readonly commands: CommandManager;",
        "  readonly keybinds: KeyBindings;",
        "  readonly brightness: BrightnessOverride;",
        "  readonly files: Files;",
        "  readonly world: World;",
        "  readonly system: System;",
        "  settings(titleKey: string): SettingsPage;",
        "  onEnable(): void;",
        "  onDisable(): void;",
        "  onUpdate(): void;",
        "  onKeybind(id: string, pressed: boolean): void;",
        "}",
        "",
        "export declare const Arg: ArgFactory;",
        "export declare const Setting: SettingFactory;",
        "export declare const brightness: BrightnessOverride;",
        "export declare const files: Files;",
        "export declare const world: World;",
        "export declare const system: System;",
        "export declare const dev: Dev;",
        "",
        "/**",
        " * Guarded raw-memory and profile introspection surface for developer tooling.",
        " * Every native read/write is range-validated through the SDK memory guard;",
        " * invalid addresses throw `SdkError` (`code: \"Invalid\"` / `\"Unavailable\"`)",
        " * instead of crashing the host process.",
        " */",
        "export interface DevMemory {",
        "  /** Read up to 4096 bytes; returns an array of byte numbers. */",
        "  readBytes(address: number, length: number): number[];",
        "  readU32(address: number): number;",
        "  readI64(address: number): number;",
        "  readF32(address: number): number;",
        "  /** Returns the pointer value at `address` as an exact JS number. */",
        "  readPtr(address: number): number;",
        "  /** Guarded write of a `Uint8Array`/`ArrayBuffer`/number[] to memory. */",
        "  writeBytes(address: number, bytes: Uint8Array | ArrayBuffer | number[]): void;",
        "  /** AOB scan over the main module image; returns RVAs (capped at 256). */",
        "  scanAob(pattern: string): number[];",
        "}",
        "",
        "export interface DevSymbolEntry {",
        "  readonly name: string;",
        "  readonly rva: number;",
        "  readonly status: string;",
        "  /** AOB signature pattern with ?? wildcards, empty when absent. */",
        "  readonly signature: string;",
        "  /** Entry-check bytes (hex string without spaces), empty when absent. */",
        "  readonly entryCheck: string;",
        "}",
        "",
        "export interface DevSymbols {",
        "  list(): DevSymbolEntry[];",
        "  status(name: string): DevSymbolEntry | null;",
        "}",
        "",
        "export interface DevModuleInfo {",
        "  readonly base: number;",
        "  readonly size: number;",
        "  readonly version: string;",
        "}",
        "",
        "export interface Dev {",
        "  readonly memory: DevMemory;",
        "  readonly symbols: DevSymbols;",
        "  readonly module: {",
        "    /** Returns {base, size, version} of main module. */",
        "    info(): DevModuleInfo;",
        "  };",
        "}",
        "",
    ]

    # Sanity check that every member prefix in the registry is represented in the generated declaration.
    expected_prefixes = {"world", "system", "files", "player", "entity", "dimension", "block", "new ItemStack", "Mod", "brightness", "dev"}
    unknown = set(members_by_prefix) - expected_prefixes
    if unknown:
        raise ValueError(f"Unmapped member prefixes in binding-registry.json: {sorted(unknown)}")

    return "\n".join(lines)


def build_sdk_package_json(registry: dict[str, Any]) -> str:
    pkg = {
        "name": registry.get("packageName", "@bedrock/sdk"),
        "version": registry["apiVersion"],
        "description": "Generated TypeScript/JavaScript SDK for BedrockDynamicSDK client mods (DESIGN.md §11)",
        "main": "./index.js",
        "types": "./index.d.ts",
        "exports": {
            ".": {
                "types": "./index.d.ts",
                "default": "./index.js",
            }
        },
        "sideEffects": False,
        "license": "MIT",
    }
    return json.dumps(pkg, indent=2) + "\n"


def build_vanilla_package_files(
    game_version: str, source_version: str, catalog: dict[str, Any]
) -> tuple[str, str, str]:
    header = f"// Generated from @minecraft/vanilla-data {source_version} for game {game_version}.\n// Do not edit by hand; re-run tools/generate_ts_sdk.py.\n"

    dts_lines = [
        header.rstrip(),
        f'export declare const dataVersion: "{source_version}";',
        "",
    ]
    js_lines = [
        header.rstrip(),
        '"use strict";',
        "Object.defineProperty(exports, '__esModule', { value: true });",
        f'exports.dataVersion = "{source_version}";',
        "",
    ]

    for kind, name in CATEGORIES.items():
        enum_name = f"Minecraft{name}Types"
        entries = sorted(catalog["ids"][kind].items())
        dts_lines.append(f"export declare const {enum_name}: {{")
        js_lines.append(f"const {enum_name} = Object.freeze({{")
        for constant, identifier in entries:
            if not CONSTANT_RE.fullmatch(f'    {constant} = "{identifier}"'):
                raise ValueError(f"Invalid {kind} entry: {constant!r} = {identifier!r}")
            dts_lines.append(f'  readonly {constant}: "{identifier}";')
            js_lines.append(f'  {constant}: "{identifier}",')
        dts_lines.append("};")
        dts_lines.append(f"export type {name}Id = (typeof {enum_name})[keyof typeof {enum_name}];")
        dts_lines.append(f"export declare function all{name}Ids(): ReadonlyArray<{name}Id>;\n")

        js_lines.append("});")
        js_lines.append(f"exports.{enum_name} = {enum_name};")
        js_lines.append(f"const _all{name}Ids = Object.freeze(Object.values({enum_name}));")
        js_lines.append(f"exports.all{name}Ids = function () {{ return _all{name}Ids; }};\n")

    pkg = {
        "name": "@bedrock/vanilla-data",
        "version": source_version,
        "description": f"Generated vanilla Bedrock identifier catalog ({source_version} for game {game_version}, with minecraft:item supplement)",
        "main": "./index.js",
        "types": "./index.d.ts",
        "exports": {
            ".": {
                "types": "./index.d.ts",
                "default": "./index.js",
            }
        },
        "sideEffects": False,
        "license": "MIT",
    }
    return "\n".join(dts_lines), "\n".join(js_lines), json.dumps(pkg, indent=2) + "\n"


def generate_all(
    root: Path,
    registry_path: Path,
    out_dir: Path,
    cpp_bootstrap_out: Path | None,
    game_version: str = "",
) -> dict[str, Path]:
    registry = load_registry(registry_path)
    runtime_js = build_runtime_core_js(registry)
    sdk_dts = build_sdk_dts(registry)
    sdk_pkg = build_sdk_package_json(registry)

    sdk_dir = out_dir / "node_modules" / "@bedrock" / "sdk"
    write_if_changed(sdk_dir / "index.d.ts", sdk_dts)
    write_if_changed(sdk_dir / "index.js", runtime_js)
    write_if_changed(sdk_dir / "package.json", sdk_pkg)

    resolved_game_ver, source_ver, catalog = load_vanilla_catalog(root, game_version)
    vanilla_dts, vanilla_js, vanilla_pkg = build_vanilla_package_files(
        resolved_game_ver, source_ver, catalog
    )
    vanilla_dir = out_dir / "node_modules" / "@bedrock" / "vanilla-data"
    write_if_changed(vanilla_dir / "index.d.ts", vanilla_dts)
    write_if_changed(vanilla_dir / "index.js", vanilla_js)
    write_if_changed(vanilla_dir / "package.json", vanilla_pkg)

    if cpp_bootstrap_out is not None:
        write_if_changed(cpp_bootstrap_out, build_cpp_bootstrap_header(runtime_js))

    return {
        "sdk_dir": sdk_dir,
        "vanilla_dir": vanilla_dir,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Repository root directory",
    )
    parser.add_argument(
        "--registry",
        type=Path,
        default=None,
        help="Path to binding-registry.json (defaults to <root>/exsample/ScriptClient/data/binding-registry.json)",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="Output root containing node_modules/@bedrock/{sdk,vanilla-data} (defaults to <root>/sdk/ts)",
    )
    parser.add_argument(
        "--cpp-bootstrap",
        type=Path,
        default=None,
        help="Optional C++ header output for Stage B runtime bootstrap (defaults to <root>/exsample/ScriptClient/core/generated_sdk_bootstrap.hpp)",
    )
    parser.add_argument(
        "--no-cpp-bootstrap",
        action="store_true",
        help="Skip writing the C++ bootstrap header",
    )
    parser.add_argument(
        "--version",
        default="",
        help="Game version key in data/manifest.json (defaults to default_version)",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    registry_path = (
        args.registry.resolve()
        if args.registry
        else root / "exsample/ScriptClient/data/binding-registry.json"
    )
    out_dir = args.out_dir.resolve() if args.out_dir else root / "sdk/ts"
    cpp_bootstrap: Path | None = None
    if not args.no_cpp_bootstrap:
        cpp_bootstrap = (
            args.cpp_bootstrap.resolve()
            if args.cpp_bootstrap
            else root / "exsample/ScriptClient/core/generated_sdk_bootstrap.hpp"
        )

    generate_all(root, registry_path, out_dir, cpp_bootstrap, args.version)


if __name__ == "__main__":
    main()

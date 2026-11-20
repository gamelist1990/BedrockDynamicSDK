// Generated from exsample/ScriptClient/data/binding-registry.json by tools/generate_ts_sdk.py.
// Do not edit by hand.
#pragma once

namespace stageb {

inline constexpr const char* kGeneratedBootstrap = R"STAGEB_BINDINGS(
// Generated from exsample/ScriptClient/data/binding-registry.json (apiVersion 1.0.0).
// Do not edit by hand; re-run tools/generate_ts_sdk.py.
(function (root) {
  "use strict";

  function installSdkModule(H) {
    if (!H) {
      throw new Error("@bedrock/sdk requires the BedrockDynamicSDK ScriptClient host runtime");
    }

    class CommandBuilder {
      constructor(id) {
        this.__id = id;
        this.__literals = [];
        this.__args = [];
      }
      literal(value) {
        this.__literals.push(String(value));
        return this;
      }
      argument(name, spec) {
        this.__args.push({ name: String(name), spec: spec });
        return this;
      }
      executes(handler) {
        H.executes(this.__id, this.__literals, this.__args, handler);
        this.__literals = [];
        this.__args = [];
        return this;
      }
    }

    class ItemStack {
      constructor(typeId, amount) {
        if (typeof typeId !== 'string' || typeId.length === 0) {
          const err = new Error('ItemStack requires a non-empty typeId');
          err.code = 'Invalid';
          throw err;
        }
        const amt = amount === undefined ? 1 : Number(amount);
        if (!Number.isFinite(amt) || amt < 0 || Math.trunc(amt) !== amt) {
          const err = new Error('ItemStack amount must be a non-negative integer');
          err.code = 'Invalid';
          throw err;
        }
        Object.defineProperty(this, '__typeId', { value: String(typeId), enumerable: false });
        Object.defineProperty(this, '__amount', { value: amt >>> 0, enumerable: false });
      }
      get typeId() { return this.__typeId; }
      get amount() { return this.__amount; }
    }

    class Block {
      constructor(dimId, pos, typeId) {
        Object.defineProperty(this, '__dimId', { value: String(dimId), enumerable: false });
        Object.defineProperty(this, '__pos', {
          value: { x: Math.trunc(pos.x), y: Math.trunc(pos.y), z: Math.trunc(pos.z) },
          enumerable: false
        });
        Object.defineProperty(this, '__typeId', { value: String(typeId), enumerable: false });
      }
      get typeId() { return this.__typeId; }
      get location() { return { x: this.__pos.x, y: this.__pos.y, z: this.__pos.z }; }
      setType(typeId) {
        return H.blockSetType(this.__dimId, this.__pos.x, this.__pos.y, this.__pos.z, String(typeId));
      }
    }

    class Dimension {
      constructor(dimId) {
        Object.defineProperty(this, '__dimId', { value: String(dimId), enumerable: false });
      }
      get id() { return this.__dimId; }
      get typeId() { return this.__dimId; }
      getBlock(pos) {
        if (!pos || typeof pos !== 'object') {
          const err = new Error('getBlock expects a BlockPosition {x,y,z}');
          err.code = 'Invalid';
          throw err;
        }
        const t = H.dimensionGetBlock(this.__dimId, Math.trunc(pos.x), Math.trunc(pos.y), Math.trunc(pos.z));
        if (t === undefined) return undefined;
        return new Block(this.__dimId, pos, t);
      }
      getEntities() {
        const list = H.dimensionGetEntities(this.__dimId) || [];
        return list.map(function (entry) {
          return entry.isPlayer ? new Player(entry.id) : new Entity(entry.id);
        });
      }
      spawnEntity(typeId, pos) {
        if (!pos || typeof pos !== 'object') {
          const err = new Error('spawnEntity expects a Vector3 {x,y,z}');
          err.code = 'Invalid';
          throw err;
        }
        const id = H.dimensionSpawnEntity(this.__dimId, String(typeId), Number(pos.x), Number(pos.y), Number(pos.z));
        return new Entity(id);
      }
    }

    class Entity {
      constructor(id) {
        Object.defineProperty(this, '__id', { value: Number(id), enumerable: false });
      }
      get id() { return String(this.__id); }
      get isValid() { return Boolean(H.entityIsValid(this.__id)); }
      get typeId() { return H.entityTypeId(this.__id); }
      get location() { return H.entityLocation(this.__id); }
      get rotation() { return H.entityRotation(this.__id); }
      get dimension() {
        const d = H.entityDimension(this.__id);
        return d === undefined ? undefined : new Dimension(d);
      }
      getRotation() { return H.entityRotation(this.__id); }
      getItemStack() {
        const item = H.entityGetItemStack(this.__id);
        return item === undefined ? undefined : new ItemStack(item.typeId, item.amount);
      }
      teleport(pos) {
        if (!pos || typeof pos !== 'object') {
          const err = new Error('teleport expects a Vector3 {x,y,z}');
          err.code = 'Invalid';
          throw err;
        }
        return H.entityTeleport(this.__id, Number(pos.x), Number(pos.y), Number(pos.z));
      }
      addTag(tag) {
        return H.entityAddTag(this.__id, String(tag));
      }
      getDynamicProperty(key) {
        return H.entityGetDynamicProperty(this.__id, String(key));
      }
      setDynamicProperty(key, value) {
        return H.entitySetDynamicProperty(this.__id, String(key), value);
      }
      getComponent(componentId) {
        const raw = H.entityGetComponent(this.__id, String(componentId));
        if (raw === undefined) return undefined;
        if (raw.__kind === 'inventory') {
          const slots = (raw.slots || []).map(function (s) {
            return s ? new ItemStack(s.typeId, s.amount) : undefined;
          });
          const container = {
            size: slots.length,
            getItem: function (slot) {
              const idx = Math.trunc(Number(slot));
              return (idx >= 0 && idx < slots.length) ? slots[idx] : undefined;
            }
          };
          return {
            typeId: 'minecraft:inventory',
            size: slots.length,
            container: container,
            getItem: container.getItem
          };
        }
        if (raw.__kind === 'equippable') {
          const map = raw.slots || {};
          return {
            typeId: 'minecraft:equippable',
            getEquipment: function (slot) {
              const key = String(slot);
              const s = map[key];
              return s ? new ItemStack(s.typeId, s.amount) : undefined;
            }
          };
        }
        if (raw.typeId === 'minecraft:item' && raw.itemStack) {
          return {
            typeId: 'minecraft:item',
            itemStack: new ItemStack(raw.itemStack.typeId, raw.itemStack.amount)
          };
        }
        return raw;
      }
    }

    class Player extends Entity {
      constructor(id) {
        super(id);
      }
      get name() { return H.playerName(this.__id); }
      sendMessage(text) { return H.playerSendMessage(this.__id, String(text)); }
      addEffect(typeId, durationSeconds, options) {
        const opts = options || {};
        const amp = opts.amplifier === undefined ? 0 : Number(opts.amplifier);
        const vis = opts.showParticles === undefined ? true : Boolean(opts.showParticles);
        return H.playerAddEffect(this.__id, String(typeId), Number(durationSeconds), amp, vis);
      }
      removeEffect(typeId) {
        return H.playerRemoveEffect(this.__id, String(typeId));
      }
      clearEffects() {
        return H.playerClearEffects(this.__id);
      }
      getEffects() {
        return H.playerGetEffects(this.__id);
      }
    }

    const Setting = {
      toggle: function (id, labelKey, initial, descriptionKey) {
        return {
          kind: 'toggle',
          id: String(id),
          labelKey: String(labelKey),
          initial: Boolean(initial),
          descriptionKey: descriptionKey === undefined ? '' : String(descriptionKey)
        };
      },
      slider: function (id, labelKey, initial, minimum, maximum, step, descriptionKey) {
        return {
          kind: 'slider',
          id: String(id),
          labelKey: String(labelKey),
          initial: Number(initial),
          minimum: Number(minimum),
          maximum: Number(maximum),
          step: step === undefined ? 1 : Number(step),
          descriptionKey: descriptionKey === undefined ? '' : String(descriptionKey)
        };
      },
      choice: function (id, labelKey, initial, choices, descriptionKey) {
        return {
          kind: 'choice',
          id: String(id),
          labelKey: String(labelKey),
          initial: String(initial),
          choices: Array.isArray(choices) ? choices : [],
          descriptionKey: descriptionKey === undefined ? '' : String(descriptionKey)
        };
      },
      text: function (id, labelKey, descriptionKey) {
        return {
          kind: 'text',
          id: String(id),
          labelKey: String(labelKey),
          descriptionKey: descriptionKey === undefined ? '' : String(descriptionKey)
        };
      }
    };

    class SettingsPageBuilder {
      constructor(titleKey) {
        this.__titleKey = String(titleKey);
      }
      add(setting) {
        H.settingsAdd(this.__titleKey, setting);
        return this;
      }
      locale(localeId, entries) {
        H.settingsLocale(this.__titleKey, String(localeId), entries);
        return this;
      }
      onChange(callback) {
        H.settingsOnChange(this.__titleKey, callback);
        return this;
      }
      modName(key) {
        H.settingsModName(this.__titleKey, String(key));
        return this;
      }
      registerPage() {
        H.settingsRegisterPage(this.__titleKey);
        return this;
      }
      unregisterPage() {
        H.settingsUnregisterPage(this.__titleKey);
        return this;
      }
      value(settingId) {
        return H.settingsValue(this.__titleKey, String(settingId));
      }
    }

    const keybindsObject = {
      add: function (id, labelKey, defaultKey) {
        H.keybindAdd(String(id), String(labelKey), defaultKey);
        return keybindsObject;
      },
      locale: function (localeId, entries) {
        H.keybindLocale(String(localeId), entries);
        return keybindsObject;
      },
      key: function (id) {
        return H.keybindKey(String(id));
      },
      changed: {
        subscribe: function (fn) {
          const subId = H.keybindSubscribe(fn);
          return {
            remove: function () { H.keybindUnsubscribe(subId); }
          };
        }
      }
    };

    const brightnessObject = {
      setGamma: function (gamma) {
        H.brightnessSetGamma(gamma);
      }
    };

    // Synchronous, sandboxed per-Mod file surface. The host owns the data root
    // and rejects absolute/escaping paths; readJson/writeJson are thin JS
    // wrappers over the same readText/writeText primitives.
    const filesObject = {
      get dataDir() { return H.filesDataDir(); },
      readText: function (path) { return H.filesReadText(String(path)); },
      writeText: function (path, text) { return H.filesWriteText(String(path), String(text)); },
      appendText: function (path, text) { return H.filesAppendText(String(path), String(text)); },
      readBytes: function (path) { return H.filesReadBytes(String(path)); },
      writeBytes: function (path, bytes) { return H.filesWriteBytes(String(path), bytes); },
      readJson: function (path) { return JSON.parse(H.filesReadText(String(path))); },
      writeJson: function (path, value) { return H.filesWriteText(String(path), JSON.stringify(value)); },
      exists: function (path) { return H.filesExists(String(path)); },
      stat: function (path) { return H.filesStat(String(path)); },
      list: function (path) { return H.filesList(path === undefined ? '.' : String(path)); },
      mkdir: function (path) { return H.filesMkdir(String(path)); },
      remove: function (path, options) {
        return H.filesRemove(String(path), Boolean(options && options.recursive));
      },
      rename: function (from, to) { return H.filesRename(String(from), String(to)); }
    };

    const worldSingleton = {
      localPlayer: function () {
        const id = H.worldLocalPlayer();
        return id === undefined ? undefined : new Player(id);
      },
      getAllPlayers: function () {
        const ids = H.worldGetAllPlayers() || [];
        return ids.map(function (id) { return new Player(id); });
      },
      getPlayers: function () {
        const ids = H.worldGetAllPlayers() || [];
        return ids.map(function (id) { return new Player(id); });
      },
      get currentDimension() {
        const d = H.worldCurrentDimension();
        return d === undefined ? undefined : new Dimension(d);
      },
      getDimension: function (id) {
        const d = H.worldGetDimension(String(id));
        return d === undefined ? undefined : new Dimension(d);
      },
      sendMessage: function (text) {
        return H.worldSendMessage(String(text));
      },
      getDynamicProperty: function (key) {
        return H.worldGetDynamicProperty(String(key));
      },
      setDynamicProperty: function (key, value) {
        return H.worldSetDynamicProperty(String(key), value);
      },
      afterEvents: {
        playerSpawn: {
          subscribe: function (fn) {
            const subId = H.worldSubscribePlayerSpawn(fn);
            return {
              remove: function () { H.worldUnsubscribePlayerSpawn(subId); }
            };
          },
          unsubscribe: function (handle) {
            if (handle && typeof handle.remove === 'function') handle.remove();
          }
        }
      }
    };

    class Mod {
      constructor() {
        const scope = H.createScope();
        this.__scope = scope;
        this.commands = {
          register(name, callback) {
            callback(new CommandBuilder(H.beginCommand(String(name))));
          },
          unregister(name) { H.unregisterCommand(String(name)); }
        };
        this.settings = function (titleKey) {
          return new SettingsPageBuilder(titleKey);
        };
        this.keybinds = keybindsObject;
        this.brightness = brightnessObject;
        this.files = filesObject;
        this.world = worldSingleton;
        this.system = H.systemObject();
      }
      onEnable() {}
      onDisable() {}
      onUpdate() {}
      onKeybind() {}
    }

    const Arg = {
    effect: function (o) { return Object.assign({ __kind: 'effect' }, o || {}); },
    duration: function (o) { return Object.assign({ __kind: 'duration' }, o || {}); },
    int: function (o) { return Object.assign({ __kind: 'int' }, o || {}); },
    bool: function (o) { return Object.assign({ __kind: 'bool' }, o || {}); },
    string: function (o) { return Object.assign({ __kind: 'string' }, o || {}); },
    message: function (o) { return Object.assign({ __kind: 'message' }, o || {}); },
    float: function (o) { return Object.assign({ __kind: 'float' }, o || {}); }
    };

    const EquipmentSlot = {
      MainHand: 'MainHand',
      Offhand: 'Offhand',
      Head: 'Head',
      Chest: 'Chest',
      Legs: 'Legs',
      Feet: 'Feet'
    };

    root.__stagebMakePlayer = function (id) { return new Player(id); };
    root.__stagebMakeEntity = function (id) { return new Entity(id); };

    const sdkModule = {
      Mod: Mod,
      Arg: Arg,
      Setting: Setting,
      ItemStack: ItemStack,
      EquipmentSlot: EquipmentSlot,
      brightness: brightnessObject,
      files: filesObject,
      world: worldSingleton,
      system: H.systemObject()
    };
    root.__sdkModule = sdkModule;
    return sdkModule;
  }

  if (root.__bedrockHost) {
    installSdkModule(root.__bedrockHost);
  }
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = root.__sdkModule || new Proxy({}, {
      get: function (_target, prop) {
        if (root.__sdkModule) return root.__sdkModule[prop];
        if (root.__bedrockHost) return installSdkModule(root.__bedrockHost)[prop];
        throw new Error("@bedrock/sdk is a BedrockDynamicSDK host-bound module and cannot be invoked outside ScriptClient");
      }
    });
  }
})(typeof globalThis !== 'undefined' ? globalThis : this);
)STAGEB_BINDINGS";

} // namespace stageb

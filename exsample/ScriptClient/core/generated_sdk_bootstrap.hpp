// Embedded from sdk/ts/node_modules/@bedrock/sdk/index.js by tools/generate_ts_sdk.py.
// Do not edit by hand.
#pragma once

namespace stageb {

inline constexpr const char* kGeneratedBootstrap = R"STAGEB_BINDINGS(
// Hand-authored ScriptClient SDK. The packager embeds this file verbatim.
(function (root, H) {
  "use strict";
  if (!H) throw new Error("@bedrock/sdk requires ScriptClient");

  const hostOnly = Symbol("host-only constructor");
  const TicksPerSecond = 20;
  const maxTicks = 0x7fffffff;
  function invalid(message) {
    throw Object.assign(new Error(message), { code: "Invalid" });
  }
  function assertHost(token) {
    if (token !== hostOnly) invalid("This object is created by ScriptClient");
  }
  function typeId(id) {
    if (typeof id !== "string" || !id) invalid("Expected a type identifier");
    return id.includes(":") ? id : `minecraft:${id}`;
  }
  function textOf(message, depth = 0) {
    if (typeof message === "string") return message;
    if (!message || typeof message !== "object" || depth >= 32)
      invalid("Expected a string or text-only RawMessage");
    if ("translate" in message || "score" in message || "with" in message)
      throw Object.assign(new Error("Only text RawMessage tokens are supported"), { code: "Unavailable" });
    if (typeof message.text === "string") return message.text;
    if (Array.isArray(message.rawtext))
      return message.rawtext.map(part => textOf(part, depth + 1)).join("");
    invalid("Expected text or rawtext");
  }
  function tickCount(value, min) {
    if (!Number.isInteger(value) || value < min || value > maxTicks)
      invalid(`Expected an integer tick count in ${min}..${maxTicks}`);
    return value;
  }
  function secondsToTicks(seconds) {
    return Math.round(seconds * TicksPerSecond);
  }
  function effectSeconds(ticks) {
    if (ticks === Infinity) return Infinity;
    const seconds = tickCount(ticks, 1) / TicksPerSecond;
    // The host multiplies by 20 and the game truncates to an integer.
    return seconds * TicksPerSecond < ticks ? seconds * (1 + Number.EPSILON) : seconds;
  }

  class EventSignal {
    #add;
    #remove;
    #subscriptions = new Map();
    constructor(token, add, remove) {
      assertHost(token);
      this.#add = add;
      this.#remove = remove;
    }
    subscribe(callback) {
      if (typeof callback !== "function") invalid("Expected an event callback");
      if (!this.#subscriptions.has(callback))
        this.#subscriptions.set(callback, this.#add(callback));
      return callback;
    }
    unsubscribe(callback) {
      if (!this.#subscriptions.has(callback)) return;
      this.#remove(this.#subscriptions.get(callback));
      this.#subscriptions.delete(callback);
    }
  }

  class ItemStack {
    #typeId;
    #amount;
    constructor(id, amount = 1) {
      this.#typeId = typeId(id);
      if (!Number.isInteger(amount) || amount < 0 || amount > 0xffffffff)
        invalid("Item amount must be an unsigned 32-bit integer");
      this.#amount = amount;
    }
    get typeId() { return this.#typeId; }
    get amount() { return this.#amount; }
  }
  const itemStack = item => item ? new ItemStack(item.typeId, item.amount) : undefined;
  const EquipmentSlot = Object.freeze({
    Mainhand: "Mainhand", Offhand: "Offhand", Head: "Head", Chest: "Chest", Legs: "Legs", Feet: "Feet",
  });

  class Block {
    #dimension;
    #location;
    #typeId;
    constructor(token, dimension, location, id) {
      assertHost(token);
      this.#dimension = dimension;
      this.#location = location;
      this.#typeId = id;
    }
    get dimension() { return this.#dimension; }
    get location() { return { ...this.#location }; }
    get typeId() { return this.#typeId; }
    get x() { return this.#location.x; }
    get y() { return this.#location.y; }
    get z() { return this.#location.z; }
    setType(id) { H.blockSetType(this.dimension.id, this.x, this.y, this.z, typeId(id)); }
  }

  class Dimension {
    #id;
    constructor(token, id) {
      assertHost(token);
      this.#id = id;
    }
    get id() { return this.#id; }
    getBlock(location) {
      const x = Math.floor(location?.x ?? NaN);
      const y = Math.floor(location?.y ?? NaN);
      const z = Math.floor(location?.z ?? NaN);
      if (![x, y, z].every(n => Number.isInteger(n) && n >= -0x80000000 && n <= 0x7fffffff))
        invalid("Expected a finite block location in the signed 32-bit range");
      const id = H.dimensionGetBlock(this.#id, x, y, z);
      return id === undefined ? undefined : new Block(hostOnly, this, { x, y, z }, id);
    }
    getEntities() {
      return H.dimensionGetEntities(this.#id).map(({ id, isPlayer }) =>
        isPlayer ? makePlayer(id) : new Entity(hostOnly, id));
    }
  }
  const dimension = id => id === undefined ? undefined : new Dimension(hostOnly, id);

  class Entity {
    #id;
    constructor(token, id) {
      assertHost(token);
      this.#id = id;
    }
    get id() { return String(this.#id); }
    get isValid() { return H.entityIsValid(this.#id); }
    get typeId() { return H.entityTypeId(this.#id); }
    get location() { return H.entityLocation(this.#id); }
    get dimension() { return dimension(H.entityDimension(this.#id)); }
    getRotation() { return H.entityRotation(this.#id); }
    getDynamicProperty(key) { return H.entityGetDynamicProperty(this.#id, key); }
    setDynamicProperty(key, value) { H.entitySetDynamicProperty(this.#id, key, value); }
    getComponent(id) {
      const component = H.entityGetComponent(this.#id, typeId(id));
      if (!component) return undefined;
      if (component.__kind === "inventory") {
        const slots = component.slots.map(itemStack);
        return { typeId: "minecraft:inventory", container: {
          size: slots.length,
          getItem: slot => Number.isInteger(slot) ? slots[slot] : undefined,
        } };
      }
      if (component.__kind === "equippable") {
        const slots = component.slots;
        return { typeId: "minecraft:equippable", getEquipment(slot) {
          const key = String(slot).toLowerCase();
          return Object.hasOwn(slots, key) ? itemStack(slots[key]) : undefined;
        } };
      }
      if (component.typeId === "minecraft:item")
        return { typeId: component.typeId, itemStack: itemStack(component.itemStack) };
      return component;
    }
  }

  class Effect {
    #snapshot;
    constructor(token, snapshot) {
      assertHost(token);
      this.#snapshot = snapshot;
    }
    get typeId() { return this.#snapshot.typeId; }
    get duration() { return secondsToTicks(this.#snapshot.duration); }
    get amplifier() { return this.#snapshot.amplifier; }
    get showParticles() { return this.#snapshot.showParticles; }
  }
  class Player extends Entity {
    #id;
    constructor(token, id) {
      super(token, id);
      this.#id = id;
    }
    get name() { return H.playerName(this.#id); }
    get fps() { return H.playerFps(this.#id); }
    get ping() { return H.playerPing(this.#id); }
    get cps() {
      const value = H.playerCps(this.#id);
      return Object.freeze({ left: value.left, right: value.right });
    }
    get device() {
      const value = H.playerDevice(this.#id);
      return Object.freeze({
        available: value.available === true,
        platform: value.platform,
        clientVersion: value.clientVersion,
        screenWidth: value.screenWidth,
        screenHeight: value.screenHeight,
      });
    }
    sendMessage(message) { H.playerSendMessage(this.#id, textOf(message)); }
    addEffect(id, duration, { amplifier = 0, showParticles = true } = {}) {
      if (!Number.isInteger(amplifier) || amplifier < 0 || amplifier > 255)
        invalid("Effect amplifier must be 0..255");
      H.playerAddEffect(this.#id, typeId(id), effectSeconds(duration), amplifier, showParticles);
    }
    getEffects() { return H.playerGetEffects(this.#id).map(data => new Effect(hostOnly, data)); }
    getEffect(id) {
      const key = typeId(id);
      return this.getEffects().find(effect => typeId(effect.typeId) === key);
    }
    removeEffect(id) { return H.playerRemoveEffect(this.#id, typeId(id)); }
    clearEffects() { H.playerClearEffects(this.#id); }
  }
  function makePlayer(id) { return id === undefined ? undefined : new Player(hostOnly, id); }
  const allPlayers = () => H.worldGetAllPlayers().map(makePlayer);
  const world = Object.freeze({
    afterEvents: Object.freeze({
      playerSpawn: new EventSignal(hostOnly, H.worldSubscribePlayerSpawn, H.worldUnsubscribePlayerSpawn),
    }),
    getLocalPlayer: () => makePlayer(H.worldLocalPlayer()),
    getAllPlayers: allPlayers,
    getPlayers(options = {}) {
      const { name, excludeNames = [] } = options;
      if (Object.keys(options).some(key => key !== "name" && key !== "excludeNames") ||
          (name !== undefined && typeof name !== "string") || !Array.isArray(excludeNames) ||
          !excludeNames.every(entry => typeof entry === "string"))
        invalid("Only name and excludeNames player queries are supported");
      return allPlayers().filter(player => {
        const playerName = player.name;
        return (name === undefined || playerName === name) && !excludeNames.includes(playerName);
      });
    },
    get currentDimension() { return dimension(H.worldCurrentDimension()); },
    getDimension: id => dimension(H.worldGetDimension(typeId(id))),
    sendMessage: message => H.worldSendMessage(textOf(message)),
    getDynamicProperty: H.worldGetDynamicProperty,
    setDynamicProperty: H.worldSetDynamicProperty,
  });

  const commandKinds = new Set(["effect", "duration", "int", "bool", "string", "message", "float"]);
  function commandBranch({ literals = [], parameters = [], execute }) {
    if (typeof execute !== "function") invalid("Expected a command handler");
    const durations = new Set();
    const args = parameters.map(({ name, kind, ...options }) => {
      if (!commandKinds.has(kind)) invalid(`Unknown command parameter kind: ${kind}`);
      if (kind === "duration") {
        durations.add(name);
        if (options.default !== undefined) {
          if (options.default === Infinity) {
            if (!options.infinity) invalid("An infinite duration default requires infinity: true");
          } else tickCount(options.default, 0);
          // QuickJS's arrow-parameter lookahead can leave a pending exception
          // if it misreads a slash after `.default` as a regexp.
          options.default = (options.default) / TicksPerSecond;
        }
      }
      return { name, spec: { ...options, __kind: kind } };
    });
    const convert = (name, value) =>
      value !== undefined && durations.has(name) ? secondsToTicks(value) : value;
    return {
      literals, args, execute: context => execute(Object.freeze({
        get: name => convert(name, context.get(name)),
        getOptional: name => convert(name, context.getOptional(name)),
        rawLine: () => context.rawLine(),
        respond: text => context.respond(text),
      })),
    };
  }
  const commands = Object.freeze({
    register(name, branches) {
      const compiled = branches.map(commandBranch);
      if (!compiled.length) invalid("Expected at least one command branch");
      const id = H.beginCommand(name);
      for (const branch of compiled) H.executes(id, branch.literals, branch.args, branch.execute);
    },
    unregister: H.unregisterCommand,
  });

  const Setting = Object.freeze({
    toggle: (id, labelKey, initial, descriptionKey) => ({ kind: "toggle", id, labelKey, initial, descriptionKey }),
    slider: (id, labelKey, initial, minimum, maximum, step = 1, descriptionKey) =>
      ({ kind: "slider", id, labelKey, initial, minimum, maximum, step, descriptionKey }),
    choice: (id, labelKey, initial, choices, descriptionKey) =>
      ({ kind: "choice", id, labelKey, initial, choices, descriptionKey }),
    text: (id, labelKey, descriptionKey) => ({ kind: "text", id, labelKey, descriptionKey }),
  });
  function settings(titleKey) {
    const page = Object.freeze({
      add(control) { H.settingsAdd(titleKey, control); return page; },
      locale(id, entries) { H.settingsLocale(titleKey, id, entries); return page; },
      onChange(callback) { H.settingsOnChange(titleKey, callback); return page; },
      modName(key) { H.settingsModName(titleKey, key); return page; },
      registerPage() { H.settingsRegisterPage(titleKey); return page; },
      unregisterPage() { H.settingsUnregisterPage(titleKey); return page; },
      value: id => H.settingsValue(titleKey, id),
    });
    return page;
  }
  const keybinds = Object.freeze({
    add(id, labelKey, defaultKey) { H.keybindAdd(id, labelKey, defaultKey); return keybinds; },
    locale(id, entries) { H.keybindLocale(id, entries); return keybinds; },
    key: H.keybindKey,
  });
  const brightness = Object.freeze({ setGamma: H.brightnessSetGamma });

  // Native tasks are dispatched in ascending task ID order. This private,
  // realm-lifetime interval runs before any public task on each scheduler pulse.
  let currentTick = 0;
  const clock = H.systemRunInterval(() => { ++currentTick; }, 1);
  const system = Object.freeze({
    get currentTick() { return currentTick; },
    afterEvents: Object.freeze({
      keybindChanged: new EventSignal(hostOnly, H.keybindSubscribe, H.keybindUnsubscribe),
    }),
    run: H.systemRun,
    runTimeout: (callback, delay = 0) => H.systemRunTimeout(callback, tickCount(delay, 0)),
    runInterval: (callback, interval = 1) => H.systemRunInterval(callback, tickCount(interval, 1)),
    clearRun(id) {
      if (!Number.isSafeInteger(id) || id < 0) invalid("Expected a valid run ID");
      return id !== clock && H.systemClear(id);
    },
    commands, settings, keybinds,
  });

  const files = Object.freeze({
    get dataDir() { return H.filesDataDir(); },
    readText: H.filesReadText, writeText: H.filesWriteText, appendText: H.filesAppendText,
    readBytes: H.filesReadBytes, writeBytes: H.filesWriteBytes, appendBytes: H.filesAppendBytes,
    readJson: path => JSON.parse(H.filesReadText(path)),
    writeJson(path, value) {
      const json = JSON.stringify(value);
      if (json === undefined) invalid("Value is not JSON-serializable");
      H.filesWriteText(path, json);
    },
    exists: H.filesExists, stat: H.filesStat, list: (path = ".") => H.filesList(path),
    mkdir: H.filesMkdir, remove: (path, options) => H.filesRemove(path, options?.recursive ?? false),
    rename: H.filesRename, copy: (from, to, options) => H.filesCopy(from, to, options?.exclusive ?? false),
  });
  const dev = Object.freeze({
    memory: Object.freeze({
      readBytes: H.devMemoryReadBytes, readU32: H.devMemoryReadU32, readI64: H.devMemoryReadI64,
      readF32: H.devMemoryReadF32, readPtr: H.devMemoryReadPtr,
      writeBytes: H.devMemoryWriteBytes, scanAob: H.devMemoryScanAob,
    }),
    symbols: Object.freeze({ list: H.devSymbolsList, status: H.devSymbolsStatus }),
    module: Object.freeze({ info: H.devModuleInfo }),
  });

  const sdk = Object.freeze({
    world, system, Entity, Player, Dimension, Block, Effect, ItemStack, EventSignal,
    EquipmentSlot, TicksPerSecond, Setting, brightness, files, dev,
  });
  root.__stagebMakePlayer = makePlayer;
  root.__sdkModule = sdk;
  if (typeof module !== "undefined") module.exports = sdk;
})(globalThis, globalThis.__bedrockHost);
)STAGEB_BINDINGS";

} // namespace stageb

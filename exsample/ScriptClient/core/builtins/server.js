(function (bindings) {
  'use strict';
  const { sdk, vanillaEffects } = bindings;
  const internal = Symbol();
  const ticksPerSecond = 20;
  const EquipmentSlot = Object.freeze(Object.fromEntries(
    ['Mainhand', 'Offhand', 'Head', 'Chest', 'Legs', 'Feet'].map(slot => [slot, slot])));
  function fail(code, message) { throw Object.assign(new Error(message), { code }); }
  function check(key) { if (key !== internal) fail('Invalid', 'cannot construct this host object'); }
  function name(id) {
    if (typeof id !== 'string' || !id) fail('Invalid', 'expected a type identifier');
    return id.includes(':') ? id : 'minecraft:' + id;
  }
  function rawText(message, depth = 0) {
    if (typeof message === 'string') return message;
    if (!message || typeof message !== 'object' || depth >= 32) fail('Invalid', 'invalid message');
    if (Array.isArray(message)) return message.map(item => rawText(item, depth + 1)).join('');
    if (message.translate !== undefined || message.score !== undefined || message.with !== undefined)
      fail('Unavailable', 'translation and score tokens are not available in client messages');
    return typeof message.text === 'string' ? message.text :
      Array.isArray(message.rawtext) ? rawText(message.rawtext, depth + 1) : '';
  }
  function pos(location) {
    if (!location || !['x', 'y', 'z'].every(k =>
      Number.isFinite(location[k]) && Math.abs(location[k]) <= 2147483647))
      fail('Invalid', 'expected a finite block position');
    const { x, y, z } = location;
    return { x: Math.floor(x), y: Math.floor(y), z: Math.floor(z) };
  }
  class EffectType {
    #id;
    constructor(key, id) { check(key); this.#id = id; }
    getName() { return this.#id; }
  }
  const allEffectTypes = new Map(vanillaEffects.map(id => [id, new EffectType(internal, id)]));
  class EffectTypes {
    constructor() { check(); }
    static get(id) { return allEffectTypes.get(name(id)); }
    static getAll() { return [...allEffectTypes.values()]; }
  }
  function effectId(effectType) {
    return name(effectType instanceof EffectType ? effectType.getName() : effectType);
  }
  class Effect {
    #data;
    constructor(key, data) { check(key); this.#data = data; }
    get amplifier() { return this.#data.amplifier; }
    get duration() { return Math.round(this.#data.duration * ticksPerSecond); }
    get typeId() { return this.#data.typeId; }
  }
  class ItemStack {
    #item;
    constructor(itemType, amount) {
      if (itemType === internal) this.#item = amount;
      else this.#item = new sdk.ItemStack(name(itemType), amount ?? 1);
    }
    get typeId() { return this.#item.typeId; }
    get amount() { return this.#item.amount; }
  }
  const wrapItem = item => item === undefined ? undefined : new ItemStack(internal, item);
  class Block {
    #block;
    #dim;
    constructor(key, block, dim) { check(key); this.#block = block; this.#dim = dim; }
    get typeId() { return this.#block.typeId; }
    get location() { return this.#block.location; }
    get dimension() { return this.#dim; }
    get x() { return this.location.x; }
    get y() { return this.location.y; }
    get z() { return this.location.z; }
  }
  function queryPlayers(players, options) {
    if (options === undefined) return players;
    if (!options || typeof options !== 'object') fail('Invalid', 'expected player query options');
    if (Object.keys(options).some(key => key !== 'name' && key !== 'excludeNames'))
      fail('Unavailable', 'only name and excludeNames player queries are supported');
    const { name: playerName, excludeNames } = options;
    if ((playerName !== undefined && typeof playerName !== 'string') ||
        (excludeNames !== undefined && (!Array.isArray(excludeNames) ||
                                        excludeNames.some(n => typeof n !== 'string'))))
      fail('Invalid', 'invalid player name query');
    return players.filter(p => (playerName === undefined || p.name === playerName) &&
                               !excludeNames?.includes(p.name));
  }
  class Dimension {
    #dim;
    constructor(key, dim) { check(key); this.#dim = dim; }
    get id() { return this.#dim.id; }
    getBlock(location) {
      const block = this.#dim.getBlock(pos(location));
      return block === undefined ? undefined : new Block(internal, block, this);
    }
    getEntities(options) {
      if (options !== undefined && (!options || typeof options !== 'object' || Object.keys(options).length))
        fail('Unavailable', 'dimension entity queries are not supported');
      return this.#dim.getEntities().map(wrapEntity);
    }
    getPlayers(options) {
      return queryPlayers(world.getAllPlayers().filter(p => p.dimension?.id === this.id), options);
    }
  }
  const wrapDim = dim => dim === undefined ? undefined : new Dimension(internal, dim);
  class Entity {
    #entity;
    constructor(key, entity) { check(key); this.#entity = entity; }
    get id() { return this.#entity.id; }
    get isValid() { return this.#entity.isValid; }
    get typeId() { return this.#entity.typeId; }
    get location() { return this.#entity.location; }
    get dimension() { return wrapDim(this.#entity.dimension); }
    getRotation() { return this.#entity.getRotation(); }
    getDynamicProperty(id) { return this.#entity.getDynamicProperty(id); }
    setDynamicProperty(id, value) { this.#entity.setDynamicProperty(id, value); }
    getComponent(componentId) {
      const typeId = name(componentId);
      const component = this.#entity.getComponent(typeId);
      if (!component) return undefined;
      switch (typeId) {
        case 'minecraft:health':
          return { typeId, currentValue: component.currentValue };
        case 'minecraft:inventory':
          return { typeId, container: {
            get size() { return component.container.size; },
            getItem: slot => wrapItem(component.container.getItem(slot)),
          } };
        case 'minecraft:equippable':
          return { typeId, getEquipment: slot =>
            wrapItem(component.getEquipment(slot === EquipmentSlot.Mainhand ? 'MainHand' : slot)) };
        case 'minecraft:item':
          return { typeId, itemStack: wrapItem(component.itemStack) };
        default: return undefined;
      }
    }
  }
  class Player extends Entity {
    #player;
    constructor(key, player) { super(key, player); this.#player = player; }
    get name() { return this.#player.name; }
    sendMessage(message) { this.#player.sendMessage(rawText(message)); }
    addEffect(effectType, duration, options) {
      if (!Number.isInteger(duration) || duration < 1 || duration > 20000000)
        fail('Invalid', 'effect duration must be 1..20000000 ticks');
      const amplifier = options?.amplifier ?? 0;
      if (!Number.isInteger(amplifier) || amplifier < 0 || amplifier > 255)
        fail('Invalid', 'effect amplifier must be 0..255');
      const id = effectId(effectType);
      this.#player.addEffect(id, duration / ticksPerSecond, options);
      return this.getEffect(id);
    }
    removeEffect(effectType) { return this.#player.removeEffect(effectId(effectType)); }
    getEffect(effectType) {
      const id = effectId(effectType);
      return this.getEffects().find(effect => effect.typeId === id);
    }
    getEffects() { return this.#player.getEffects().map(effect => new Effect(internal, effect)); }
  }
  function wrapEntity(entity) {
    return typeof entity.sendMessage === 'function' ? wrapPlayer(entity) : new Entity(internal, entity);
  }
  const wrapPlayer = player => new Player(internal, player);
  const subscriptions = new Map();
  const playerSpawn = {
    subscribe(callback) {
      if (typeof callback !== 'function') fail('Invalid', 'expected a callback');
      if (!subscriptions.has(callback)) {
        const subscription = sdk.world.afterEvents.playerSpawn.subscribe(event => callback({
          player: wrapPlayer(event.player), initialSpawn: event.initialSpawn,
        }));
        subscriptions.set(callback, subscription);
      }
      return callback;
    },
    unsubscribe(callback) {
      const subscription = subscriptions.get(callback);
      if (!subscription) return;
      subscription.remove();
      subscriptions.delete(callback);
    },
  };
  const world = {
    getAllPlayers: () => sdk.world.getAllPlayers().map(wrapPlayer),
    getPlayers: options => queryPlayers(world.getAllPlayers(), options),
    getDimension(dimensionId) {
      const dim = sdk.world.getDimension(name(dimensionId));
      if (!dim) fail('Unavailable', 'only the active client dimension can be accessed');
      return wrapDim(dim);
    },
    sendMessage: message => sdk.world.sendMessage(rawText(message)),
    getDynamicProperty: id => sdk.world.getDynamicProperty(id),
    setDynamicProperty(id, value) { sdk.world.setDynamicProperty(id, value); },
    afterEvents: { playerSpawn },
    beforeEvents: Object.freeze({}),
  };
  function tickDelay(delay) {
    if (!Number.isInteger(delay) || delay < 0 || delay > 2147483647)
      fail('Invalid', 'expected a non-negative integer tick delay');
    return delay;
  }
  const system = {
    run: callback => sdk.system.run(callback),
    runTimeout: (callback, delay = 0) => sdk.system.runTimeout(callback, tickDelay(delay)),
    runInterval: (callback, interval = 1) => sdk.system.runInterval(callback, tickDelay(interval)),
    clearRun(id) {
      if (!Number.isSafeInteger(id) || id < 0) fail('Invalid', 'invalid run id');
      sdk.system.clearRun(id);
    },
  };
  return { world, system, Effect, EffectType, EffectTypes, Entity, Player, Dimension, Block,
    ItemStack, EquipmentSlot, TicksPerSecond: ticksPerSecond };
})

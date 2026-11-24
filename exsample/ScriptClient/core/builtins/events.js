(function () {
  'use strict';
  const state = new WeakMap();
  function listeners(emitter) {
    if (!state.has(emitter)) state.set(emitter, new Map());
    return state.get(emitter);
  }
  function check(listener) {
    if (typeof listener !== 'function') throw new TypeError('listener must be a function');
  }
  function add(emitter, name, listener, prepend) {
    check(listener);
    if (emitter.listenerCount('newListener'))
      emitter.emit('newListener', name, listener.listener || listener);
    const map = listeners(emitter);
    const list = map.get(name) || [];
    if (prepend) list.unshift(listener);
    else list.push(listener);
    map.set(name, list);
    return emitter;
  }
  function onceWrapper(emitter, name, listener) {
    check(listener);
    let fired = false;
    function wrapped(...args) {
      if (fired) return;
      fired = true;
      emitter.removeListener(name, wrapped);
      return listener.apply(emitter, args);
    }
    wrapped.listener = listener;
    return wrapped;
  }
  class EventEmitter {
    addListener(name, listener) { return add(this, name, listener, false); }
    prependListener(name, listener) { return add(this, name, listener, true); }
    once(name, listener) { return this.on(name, onceWrapper(this, name, listener)); }
    prependOnceListener(name, listener) {
      return this.prependListener(name, onceWrapper(this, name, listener));
    }
    emit(name, ...args) {
      const list = listeners(this).get(name);
      if (!list) {
        if (name === 'error') {
          if (args[0] instanceof Error) throw args[0];
          const error = new Error('Unhandled error event');
          error.code = 'ERR_UNHANDLED_ERROR';
          error.context = args[0];
          throw error;
        }
        return false;
      }
      for (const listener of list.slice()) listener.apply(this, args);
      return true;
    }
    removeListener(name, listener) {
      check(listener);
      const map = listeners(this);
      const list = map.get(name) || [];
      for (let i = list.length - 1; i >= 0; --i) {
        const target = list[i];
        if (target !== listener && target.listener !== listener) continue;
        list.splice(i, 1);
        if (!list.length) map.delete(name);
        if (this.listenerCount('removeListener'))
          this.emit('removeListener', name, target.listener || target);
        break;
      }
      return this;
    }
    removeAllListeners(name) {
      const map = listeners(this);
      if (arguments.length) {
        for (const listener of this.rawListeners(name).reverse()) this.removeListener(name, listener);
      } else if (!this.listenerCount('removeListener')) {
        map.clear();
      } else {
        for (const key of map.keys()) if (key !== 'removeListener') this.removeAllListeners(key);
        this.removeAllListeners('removeListener');
      }
      return this;
    }
    eventNames() { return [...listeners(this).keys()]; }
    rawListeners(name) { return (listeners(this).get(name) || []).slice(); }
    listeners(name) { return this.rawListeners(name).map(fn => fn.listener || fn); }
    listenerCount(name, listener) {
      const list = this.rawListeners(name);
      return listener === undefined ? list.length :
        list.filter(fn => fn === listener || fn.listener === listener).length;
    }
    static listenerCount(emitter, name) { return emitter.listenerCount(name); }
    static once(emitter, name) {
      return new Promise((resolve, reject) => {
        const success = (...args) => {
          if (name !== 'error') emitter.off('error', failure);
          resolve(args);
        };
        const failure = error => { emitter.off(name, success); reject(error); };
        emitter.once(name, success);
        if (name !== 'error') emitter.once('error', failure);
      });
    }
  }
  EventEmitter.prototype.on = EventEmitter.prototype.addListener;
  EventEmitter.prototype.off = EventEmitter.prototype.removeListener;
  EventEmitter.EventEmitter = EventEmitter;
  return EventEmitter;
})

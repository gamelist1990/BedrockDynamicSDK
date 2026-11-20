// Runs in the live 1.26.65.1 OreUI document. Keep the game's React tree,
// router and native settings renderers; extend their query/command driver.
(() => {
  "use strict";
  const engine = window.engine;
  if (!engine || typeof engine.trigger !== "function" || window.__bdModSettingsAdapter) return;
  const prefix = "/hbui/__bedrock_mod_settings/__BDMODTOKEN__/";
  const group = "bedrockdynamic-mod-menu";
  const keyGroup = "bedrockdynamic-mod-keybind";
  const query = "vanilla.menus.settings";
  const subscribe = "query:subscribe/";
  const subscribed = "query:subscribed/";
  const updated = "query:updated/";
  const rootQuery = query + "GroupQuery";
  const rootArgument = "settings-tabs-groups";
  const types = { number: 0, boolean: 1, string: 2, option: 3, action: 4, text: 5, image: 5 };
  const nativeOn = engine.on.bind(engine);
  const nativeOff = engine.off.bind(engine);
  const nativeTrigger = engine.trigger.bind(engine);
  const listeners = new Map();
  const local = new Map();
  const roots = new Map();
  const drafts = new Map();
  let menus = new Map();
  let pages = new Map();
  let rows = new Map();
  let keyPages = new Map();
  let keyRows = new Map();
  let keyText = {};
  let capture = null;
  let language = "en_US";
  let menuLabel = "Mod Menu";
  let actionLabel = "Run";
  let menuRequest = 0;
  let sequence = 0;
  let writes = Promise.resolve();

  function request(path, receive) {
    const xhr = new XMLHttpRequest();
    xhr.open("GET", prefix + path + ".json");
    if (receive) {
      xhr.onload = () => {
        let data;
        try { data = JSON.parse(xhr.responseText); } catch (_) {}
        receive(data, xhr.status);
      };
      xhr.onerror = () => receive(null, 0);
    }
    xhr.send();
  }
  const log = phase => request("boot/" + phase);
  const item = (id, type = 6) => ({id, type});
  const info = (name, description = "") => ({name, description, state: 0});
  const rootGroup = id => menus.has(id) || pages.has(id) || id === group ||
    id === keyGroup || keyPages.has(id);
  const ownId = id => rootGroup(id) || rows.has(id) || keyRows.has(id);

  function keyName(code) {
    if (code === 9) return "Tab";
    if (code === 32) return keyText["keybind.space"];
    if (code >= 112 && code <= 123) return "F" + (code - 111);
    return String.fromCharCode(code);
  }

  function dataFor(name, id) {
    const menu = menus.get(id) || pages.get(id);
    if (name === query + "GroupQuery") {
      if (id === group) return {settings: Array.from(menus.keys(), id => item(id))};
      if (id === keyGroup) return {settings: Array.from(keyPages.keys(), id => item(id))};
      if (keyPages.has(id)) return {settings: keyPages.get(id).rows.map(row =>
        item(row.id, types.action))};
      if (menu) return {settings: menu.page ? [item(menu.page)] :
        menu.rows.map(row => item(row.id, types[row.kind]))};
      return {settings: []};
    }
    if (name === query + "GroupInfoQuery")
      return info(id === keyGroup ? keyText["keybind.title"] : id === group ? menuLabel :
        keyPages.has(id) ? keyPages.get(id).title : menu ? (menu.modName ?? menu.title) : "");
    const binding = keyRows.get(id);
    if (binding) {
      const label = binding.label + ": " + keyName(binding.value);
      const prompt = capture === id ? keyText["keybind.waiting"] : "";
      return {...info(label, prompt), actionType: 1, actionLabel: keyText["keybind.capture"]};
    }
    const row = rows.get(id);
    if (!row) return {name: "", state: 2};
    const common = {...info(row.label, row.description), value: row.value};
    switch (row.kind) {
      case "number": return {...common, min: row.minimum, max: row.maximum, step: row.step,
        valueText: String(row.value)};
      case "option": return {...common, value: row.choices.findIndex(choice => choice.id === row.value),
        options: row.choices.map((choice, i) => ({label: choice.label, value: i, disabled: false}))};
      case "string": return {...common, placeholder: "", maxLength: 2048, isValid: true,
        fallbackValue: row.value};
      case "action": return {...common, actionLabel, actionType: 1};
      default: return common;
    }
  }

  function rootKind(id, data) {
    if (id === rootArgument) return "sidebar";
    if (id === "keyboard-and-mouse" || (Array.isArray(data?.settings) &&
        data.settings.some(entry => typeof entry.id === "string" &&
          entry.id.startsWith("keyboardAndMouse.inputGroup.")))) return "keyboard";
    return null;
  }
  function extendRoot(data, id) {
    if (!data || !Array.isArray(data.settings)) return data;
    const kind = rootKind(id, data);
    if (kind === "sidebar" && menus.size)
      return {...data, settings: [item(group), ...data.settings.filter(entry => entry.id !== group)]};
    if (kind === "keyboard" && keyPages.size)
      return {...data, settings: [...data.settings.filter(entry => entry.id !== keyGroup), item(keyGroup)]};
    return data;
  }
  function deliver(event, value) {
    for (const key of listeners.get(event) || []) key.callback(value);
  }
  function publish(id) {
    for (const [key, entry] of local)
      if (!id || entry.id === id) deliver(updated + key, dataFor(entry.name, entry.id));
  }

  function loadMenus() {
    const ticket = ++menuRequest;
    writes.then(() => {
      if (ticket !== menuRequest) return;
      request("menus/" + ++sequence + "/" + language, data => {
        if (ticket !== menuRequest) return;
        if (!data || !data.ok || !Array.isArray(data.menus)) {
          log("menus-read-failed");
          return;
        }
        menuLabel = data.menuLabel;
        actionLabel = data.actionLabel;
        pages = new Map();
        for (const menu of data.menus) {
          if (menu.modName === undefined) continue;
          // The native sidebar and top-level section share GroupInfo. If the
          // page title differs, render it as a native subsection under the mod.
          menu.page = menu.id + ".page";
          pages.set(menu.page, {title: menu.title, rows: menu.rows});
        }
        menus = new Map(data.menus.map(menu => [menu.id, menu]));
        rows = new Map(data.menus.flatMap(menu => menu.rows.map(row => [row.id, row])));
        const keys = data.keyGroups || [];
        keyText = data.keyTranslations || {};
        keyPages = new Map(keys.map(menu => [menu.id, menu]));
        keyRows = new Map(keys.flatMap(menu => menu.rows.map(row => [row.id, row])));
        log("menus-loaded-" + menus.size + "-" + language);
        for (const [key, root] of roots)
          if (root.received && rootKind(root.id, root.result))
            deliver(updated + key, extendRoot(root.result, root.id));
        publish();
      });
    });
  }

  function onLocale(data) {
    if (!data || typeof data.locale !== "string") return;
    const next = data.locale.replace(/^([a-z]{2})[-_]?([a-z]{2})$/i,
      (_, lang, region) => lang.toLowerCase() + "_" + region.toUpperCase());
    if (!/^[a-z]{2}_[A-Z]{2}$/.test(next) || next === language) return;
    language = next;
    loadMenus();
  }

  function on(event, callback, ...rest) {
    const match = /^(?:query:subscribed|query:updated)\/(.+)$/.exec(event);
    if (!match || typeof callback !== "function") return nativeOn(event, callback, ...rest);
    const key = match[1];
    let set = listeners.get(event);
    if (!set) listeners.set(event, set = new Set());
    const wrapper = function(data, ...args) {
      const root = roots.get(key);
      if (root) {
        root.received = true;
        root.result = data;
        data = extendRoot(data, root.id);
      }
      return callback.call(this, data, ...args);
    };
    set.add({callback, wrapper});
    return nativeOn(event, wrapper, ...rest);
  }
  function off(event, callback, ...rest) {
    const set = listeners.get(event);
    if (set) for (const entry of set) {
      if (entry.callback !== callback) continue;
      set.delete(entry);
      if (!set.size) listeners.delete(event);
      return nativeOff(event, entry.wrapper, ...rest);
    }
    return nativeOff(event, callback, ...rest);
  }
  function trigger(event, ...args) {
    if (event === "query:unsubscribe") {
      roots.delete(String(args[0]));
      const entry = local.get(String(args[0]));
      if (capture && entry?.id === capture) finishCapture();
      if (local.delete(String(args[0]))) return;
    }
    if (typeof event === "string" && event.startsWith(subscribe)) {
      const [key, id] = args;
      const name = event.slice(subscribe.length);
      if (name.startsWith(query) && ownId(id)) {
        local.set(String(key), {name, id});
        // Query listeners are already registered by the shipped driver.
        deliver(subscribed + key, dataFor(name, id));
        return;
      }
      if (name === rootQuery) {
        roots.set(String(key), {id, received: false});
        if (id === rootArgument) log("sidebar-query");
      }
    }
    return nativeTrigger(event, ...args);
  }

  function write(id, kind, value) {
    const hex = Array.from(encodeURIComponent(JSON.stringify({id, kind, value})).match(/%[0-9A-F]{2}|./g),
      x => x.length === 3 ? x.slice(1).toLowerCase() : x.charCodeAt(0).toString(16).padStart(2, "0")).join("");
    writes = writes.then(() => new Promise(resolve => {
      request("write/" + ++sequence + "/" + hex, data => {
        if (data && data.ok) {
          const row = rows.get(id) || keyRows.get(id);
          if (row && kind !== "action") {
            row.value = value;
            publish(id);
          }
        }
        log((data && data.ok ? "write-ok-" : "write-failed-") + kind);
        resolve();
      });
    })).catch(() => log("write-bridge-exception"));
    return undefined;
  }

  function finishCapture() {
    const id = capture;
    capture = null;
    if (id) publish(id);
  }

  function captureKey(event) {
    if (!capture || event.repeat) return;
    event.preventDefault();
    event.stopImmediatePropagation();
    const id = capture;
    const code = event.code || "";
    const letter = /^Key([A-Z])$/.exec(code);
    const digit = /^Digit([0-9])$/.exec(code);
    const fn = /^F([1-9]|1[0-2])$/.exec(code);
    const key = letter ? letter[1].charCodeAt(0) : digit ? digit[1].charCodeAt(0) :
      fn ? 111 + Number(fn[1]) : code === "Tab" ? 9 : code === "Space" ? 32 : 0;
    if (code === "Escape" || event.key === "Escape") {
      const binding = keyRows.get(id);
      finishCapture();
      if (binding) write(id, "key", binding.defaultKey);
    } else if (key) {
      finishCapture();
      write(id, "key", key);
    }
  }
  window.addEventListener("keydown", captureKey, true);

  function patchCommand(groups, name, method, kind) {
    const descriptor = groups[name] && groups[name][method];
    if (!descriptor || typeof descriptor.callable !== "function") return false;
    const original = descriptor.callable;
    descriptor.callable = function(...args) {
      let [id, value] = args;
      const binding = kind === "action" && keyRows.get(id);
      if (binding) {
        finishCapture();
        capture = id;
        publish(id);
        return;
      }
      const row = rows.get(id);
      if (!row || row.kind !== kind) return original.apply(this, args);
      if (method === "updateString") { drafts.set(id, value); return; }
      if (method === "commitString") {
        if (!drafts.has(id)) return;
        value = drafts.get(id);
        drafts.delete(id);
      }
      if (kind === "option") {
        const choice = row.choices[value];
        if (!choice) return;
        value = choice.id;
      }
      return write(id, kind, kind === "action" ? null : value);
    };
    return true;
  }
  function installCommands() {
    const groups = window.__commands__;
    if (!groups) return false;
    const update = "vanilla_menus_update_settings";
    const action = "vanilla_menus_invoke_action_settings";
    const specs = [[update, "updateBoolean", "boolean"], [update, "updateNumber", "number"],
      [update, "updateOption", "option"], [update, "updateString", "string"], [update, "commitString", "string"],
      [action, "invokeAction", "action"]];
    if (!specs.every(([name, method]) => groups[name] && groups[name][method] &&
        typeof groups[name][method].callable === "function")) return false;
    specs.forEach(spec => patchCommand(groups, ...spec));
    return true;
  }

  // 1.26.65.1 has six native data renderers but none for an arbitrary image.
  // The model creates the Image caption as a native Text row; add only its
  // decorative PNG to that row. Never replace or overlay the settings tree.
  function paintImages() {
    if (!document.querySelector(".d338e4afd1aa1a084129")) return;
    const slots = document.querySelectorAll(".d338e4afd1aa1a084129 .acf4d3cf60729bb0f2ba");
    for (const row of rows.values()) {
      if (row.kind !== "image") continue;
      for (const title of slots) {
        if (title.textContent !== row.label && title.getAttribute("text") !== row.label) continue;
        const panel = title.parentElement;
        if (!panel || panel.querySelector("[data-bd-image]")) continue;
        const image = document.createElement("img");
        image.setAttribute("data-bd-image", row.id);
        image.src = row.image;
        image.width = row.width;
        image.height = row.height;
        image.alt = row.label;
        image.style.display = "block";
        panel.appendChild(image);
        log("content-image-mounted");
      }
    }
  }

  engine.on = on;
  engine.off = off;
  engine.trigger = trigger;
  window.__bdModSettingsAdapter = true;
  log("settings-adapter-installed");
  if (installCommands()) log("settings-commands-installed");
  else {
    let tries = 0;
    const timer = setInterval(() => {
      if (installCommands()) { clearInterval(timer); log("settings-commands-installed"); }
      else if (++tries >= 60) { clearInterval(timer); log("settings-commands-unavailable"); }
    }, 100);
  }
  // Reuse the shipped driver's hash of the no-arg query name; the engine
  // doesn't deliver core.locale to an arbitrary subscription ID. Observe
  // its updates without unsubscribing the game's localization provider.
  const localeQuery = "core.locale";
  let localeKey = 0;
  for (const c of localeQuery) localeKey = (localeKey * 31 + c.charCodeAt(0)) >>> 0;
  nativeOn(subscribed + localeKey, onLocale);
  nativeOn(updated + localeKey, onLocale);
  nativeTrigger(subscribe + localeQuery, localeKey);
  loadMenus();
  setInterval(paintImages, 350);
})();

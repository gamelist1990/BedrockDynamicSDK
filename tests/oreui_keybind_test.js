const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");

const groupQuery = "vanilla.menus.settingsGroupQuery";
const actionQuery = "vanilla.menus.settingsActionQuery";
const sidebar = "settings-tabs-groups";
const menuGroup = "bedrockdynamic-mod-menu";
const group = "bedrockdynamic-mod-keybind";
const mod = "bedrockdynamic-mod-test-keys";
const binding = "bedrockdynamic.mod.test-keys.zoom";
const asset = {
  ok: true, menus: [{id: "test-menu", title: "Test menu", rows: []}],
  keyGroups: [{id: mod, title: "Test", rows: [{id: binding, label: "Zoom", value: 88, defaultKey: 67}]}],
  keyTranslations: {"keybind.title": "Mod KeyBind", "keybind.capture": "Change key",
    "keybind.waiting": "Press a key (Esc restores the default)"},
};

const listeners = new Map();
const writes = [];
const engine = {
  on(name, fn) { listeners.set(name, fn); },
  off(name) { listeners.delete(name); },
  trigger(event, key, id) {
    if (event === `query:subscribe/${groupQuery}` && (id === "keyboard-and-mouse" || id === sidebar)) {
      listeners.get(`query:subscribed/${key}`)({settings: [{id: "vanilla.last", type: 4}]});
    }
  },
};
const command = () => ({callable() {}});
const window = {
  engine,
  __commands__: {
    vanilla_menus_update_settings: {
      updateBoolean: command(), updateNumber: command(), updateOption: command(),
      updateString: command(), commitString: command(),
    },
    vanilla_menus_invoke_action_settings: {invokeAction: command()},
  },
  addEventListener(name, fn) { this[name] = fn; },
};
class Request {
  open(_method, url) { this.url = url; }
  send() {
    let response = {};
    if (this.url.includes("/menus/")) response = asset;
    if (this.url.includes("/write/")) {
      const hex = this.url.split("/").at(-1).slice(0, -5);
      writes.push(JSON.parse(Buffer.from(hex, "hex").toString("utf8")));
      response = {ok: true};
    }
    this.responseText = JSON.stringify(response);
    this.status = 200;
    this.onload?.();
  }
}
vm.runInNewContext(fs.readFileSync(process.argv[2], "utf8"), {
  window, XMLHttpRequest: Request, setInterval() {},
});

const result = (query, id) => {
  let value;
  engine.on("query:subscribed/1", data => { value = data; });
  engine.trigger(`query:subscribe/${query}`, 1, id);
  engine.trigger("query:unsubscribe", 1);
  return value;
};
const flush = () => new Promise(resolve => setImmediate(resolve));
const key = code => {
  let consumed = false;
  window.keydown({code, key: code, preventDefault() { consumed = true; }, stopImmediatePropagation() {}});
  return consumed;
};
const capture = () => window.__commands__.vanilla_menus_invoke_action_settings.invokeAction.callable(binding);

(async () => {
  // The game bundle may subscribe before the asynchronous Mod catalog arrives.
  let lateSidebar, lateKeyboard;
  engine.on("query:updated/2", data => { lateSidebar = data; });
  engine.on("query:subscribed/2", data => assert.equal(data.settings[0].id, "vanilla.last"));
  engine.trigger(`query:subscribe/${groupQuery}`, 2, sidebar);
  engine.on("query:updated/3", data => { lateKeyboard = data; });
  engine.on("query:subscribed/3", data => assert.equal(data.settings[0].id, "vanilla.last"));
  engine.trigger(`query:subscribe/${groupQuery}`, 3, "keyboard-and-mouse");
  await flush();
  assert.equal(lateSidebar.settings[0].id, menuGroup);
  assert.equal(lateKeyboard.settings.at(-1).id, group);
  assert.equal(result(groupQuery, sidebar).settings[0].id, menuGroup);
  const keyboard = result(groupQuery, "keyboard-and-mouse");
  assert.equal(keyboard.settings.at(-1).id, group);
  const actions = result(groupQuery, mod);
  assert.equal(actions.settings.length, 1);
  assert.equal(actions.settings[0].id, binding);
  assert.equal(key("Escape"), false); // Never reset outside capture.
  assert.equal(writes.length, 0);
  capture();
  assert.equal(key("Escape"), true);
  await flush();
  assert.deepEqual(writes[0], {id: binding, kind: "key", value: 67});
  assert.equal(result(actionQuery, binding).name, "Zoom: C");
  capture();
  assert.equal(key("KeyX"), true);
  await flush();
  assert.deepEqual(writes[1], {id: binding, kind: "key", value: 88});
  assert.equal(result(actionQuery, binding).name, "Zoom: X");
})().catch(error => { console.error(error); process.exitCode = 1; });

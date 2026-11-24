const { http, HttpRequest, HttpRequestMethod } = require("@minecraft/server-net");
const { system, world } = require("@minecraft/server");
const base = "http://127.0.0.1:18765";

const spawned = world.afterEvents.playerSpawn.subscribe(() => {
  world.afterEvents.playerSpawn.unsubscribe(spawned);
  system.run(smoke);
});

async function smoke() {
  try {
    const request = new HttpRequest(base + "/echo").setMethod(HttpRequestMethod.POST)
      .setBody("fixture-echo").addHeader("X-Fixture-Request", "bol");
    const echo = await http.request(request);
    console.log(`[NetworkSmoke] echo status=${echo.status} body=${echo.body} header=${
      echo.headers.find(header => header.key.toLowerCase() === "x-echo")?.value}`);
    const id = system.runInterval(() => console.log("[NetworkSmoke] slow in-flight tick"), 2);
    try {
      const slow = await fetch(base + "/slow");
      console.log(`[NetworkSmoke] fetch slow status=${slow.status} text=${await slow.text()}`);
    } finally {
      system.clearRun(id);
    }
  } catch (error) {
    console.error(`[NetworkSmoke] local error ${error.code}: ${error.message}`);
  }
  try {
    const external = await http.get("https://gamelist1990.github.io/BedrockDynamicSDK/");
    console.log(`[NetworkSmoke] external HTTPS status=${external.status}`);
  } catch (error) {
    console.error(`[NetworkSmoke] external error ${error.code}: ${error.message}`);
  }
}

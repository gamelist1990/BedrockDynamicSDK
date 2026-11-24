import { system, TicksPerSecond, world } from "@bedrock/sdk";

world.afterEvents.playerSpawn.subscribe(({ player, initialSpawn }) => {
  console.log(`[OfficialStyle] spawn initial=${initialSpawn} player=${player.name}`);
  system.run(() => {
    if (!player.isValid) return;
    player.sendMessage({ text: "Hello from the Script API style mod!" });
    player.addEffect("minecraft:speed", 2 * TicksPerSecond, { showParticles: false });
    const effect = player.getEffect("minecraft:speed");
    console.log(`[OfficialStyle] addEffect ${effect?.typeId} ticks=${effect?.duration} currentTick=${system.currentTick}`);
  });
});

const timer = system.runInterval(() => {
  const players = world.getAllPlayers();
  if (players.length > 0) {
    console.log(`[OfficialStyle] interval players=${players.length}`);
    system.clearRun(timer);
  }
}, TicksPerSecond);

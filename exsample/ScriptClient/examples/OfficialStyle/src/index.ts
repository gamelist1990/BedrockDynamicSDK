import { EffectTypes, system, TicksPerSecond, world } from "@minecraft/server";

world.afterEvents.playerSpawn.subscribe(({ player, initialSpawn }) => {
  console.log(`[OfficialStyle] spawn initial=${initialSpawn} player=${player.name}`);
  system.run(() => {
    if (!player.isValid) return;
    player.sendMessage({ text: "Hello from the Script API style mod!" });
    const speed = EffectTypes.get("speed");
    if (speed) {
      const effect = player.addEffect(speed, 2 * TicksPerSecond, { showParticles: false });
      console.log(`[OfficialStyle] addEffect ${effect?.typeId} ticks=${effect?.duration}`);
    }
  });
});

const timer = system.runInterval(() => {
  const players = world.getAllPlayers();
  if (players.length > 0) {
    console.log(`[OfficialStyle] interval players=${players.length}`);
    system.clearRun(timer);
  }
}, TicksPerSecond);

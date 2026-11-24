// Client-local potion effects. Chat arguments are in seconds; SDK values are ticks.
import { system, TicksPerSecond, world } from "@bedrock/sdk";
import type { CommandContext } from "@bedrock/sdk";

system.commands.register("effectfake", [
  {
    literals: ["give"],
    parameters: [
      { name: "effect", kind: "effect" },
      { name: "duration", kind: "duration", optional: true, default: 30 * TicksPerSecond, infinity: true },
      { name: "amplifier", kind: "int", optional: true, default: 0, min: 0, max: 255, suggestions: [0, 1, 2, 3, 4, 9] },
      { name: "visible", kind: "bool", optional: true, default: true },
    ],
    execute: give,
  },
  {
    literals: ["clear"],
    parameters: [{ name: "effect", kind: "effect", optional: true }],
    execute: clear,
  },
]);

function give(ctx: CommandContext): void {
  const player = world.getLocalPlayer();
  if (!player) return;
  const effect = ctx.get<string>("effect");
  const duration = ctx.get<number>("duration");
  const amplifier = ctx.get<number>("amplifier");
  player.addEffect(effect, duration, { amplifier, showParticles: ctx.get<boolean>("visible") });

  const length = duration === Infinity ? "∞" : `${duration / TicksPerSecond}秒`;
  player.sendMessage(`§a[EffectFake] ${effect} を付けた（${length} / 強さ ${amplifier}）`);
}

function clear(ctx: CommandContext): void {
  const player = world.getLocalPlayer();
  if (!player) return;
  const effect = ctx.getOptional<string>("effect");
  if (effect) {
    player.removeEffect(effect);
    player.sendMessage(`§a[EffectFake] ${effect} を消した`);
  } else {
    player.clearEffects();
    player.sendMessage("§a[EffectFake] 全ての効果を消した");
  }
}

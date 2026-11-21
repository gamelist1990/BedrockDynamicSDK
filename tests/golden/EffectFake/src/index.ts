// EffectFake — クライアント側だけでポーション効果を付ける Mod。
//
// サーバーから付けられた効果ではなく、この Mod がローカルプレイヤーに
// 直接付けるので、サーバー側には一切影響しない（見た目と自分への効果のみ）。
//
// コマンド（バニラの /effect と同じ形）:
//   /effectfake give <effect> [duration] [amplifier] [visible]
//   /effectfake clear [effect]
//
//   effect    : エフェクト ID（補完あり、vanilla-data から）
//   duration  : 秒数。"Infinity" で無限（補完あり）
//   amplifier : 強さ 0-255（補完あり）
//   visible   : パーティクルを表示するか true/false（補完あり）

import { Arg, Mod, world } from "@bedrock/sdk";
import type { CommandContext } from "@bedrock/sdk";

const INFINITY_SECONDS = Number.POSITIVE_INFINITY;

export default class EffectFake extends Mod {
  onEnable(): void {
    this.commands.register("effectfake", (cmd) => {
      cmd
        .literal("give")
        .argument("effect", Arg.effect())
        .argument(
          "duration",
          Arg.duration({ optional: true, default: 30, infinity: true }),
        )
        .argument(
          "amplifier",
          Arg.int({
            optional: true,
            default: 0,
            min: 0,
            max: 255,
            suggestions: [0, 1, 2, 3, 4, 9],
          }),
        )
        .argument("visible", Arg.bool({ optional: true, default: true }))
        .executes((ctx) => this.give(ctx));

      cmd
        .literal("clear")
        .argument("effect", Arg.effect({ optional: true }))
        .executes((ctx) => this.clear(ctx));
    });
  }

  private give(ctx: CommandContext): void {
    const player = world.localPlayer();
    if (!player) return;

    const effect = ctx.get<string>("effect");
    const duration = ctx.get<number>("duration"); // Infinity はそのまま渡る
    const amplifier = ctx.get<number>("amplifier");
    const visible = ctx.get<boolean>("visible");

    // addEffect はクライアント側のみに適用される（サーバーへは送られない）。
    // duration に Infinity を渡すと無限扱いになる。
    player.addEffect(effect, duration, {
      amplifier,
      showParticles: visible,
    });

    const length =
      duration === INFINITY_SECONDS ? "∞" : `${duration}秒`;
    player.sendMessage(
      `§a[EffectFake] ${effect} を付けた（${length} / 強さ ${amplifier}）`,
    );
  }

  private clear(ctx: CommandContext): void {
    const player = world.localPlayer();
    if (!player) return;

    const effect = ctx.getOptional<string>("effect");
    if (effect) {
      player.removeEffect(effect);
      player.sendMessage(`§a[EffectFake] ${effect} を消した`);
    } else {
      // 引数なしは全消し。
      for (const active of player.getEffects()) {
        player.removeEffect(active.typeId);
      }
      player.sendMessage("§a[EffectFake] 全ての効果を消した");
    }
  }
}

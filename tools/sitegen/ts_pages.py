"""TS pages: title, summary, source declaration selectors, example, note."""

FIRST = """\
import { world } from "@bedrock/sdk";

world.afterEvents.playerSpawn.subscribe(({ player }) => {
  player.sendMessage("Hello!");
});
"""

PAGES = {
    "world": (
        "ワールド",
        "ローカルプレイヤーや、クライアントから見えるプレイヤーを取得します。",
        "world World:getLocalPlayer,getAllPlayers,getPlayers,currentDimension,getDimension,sendMessage,getDynamicProperty,setDynamicProperty "
        "PlayerQueryOptions RawMessage DynamicProperty Vector3",
        """\
import { world } from "@bedrock/sdk";

world.afterEvents.playerSpawn.subscribe(() => {
  world.getLocalPlayer()?.sendMessage(`表示中: ${world.getAllPlayers().length} 人`);
});
""",
        "ディメンションは現在のものだけ取得できます。動的プロパティは Mod ごと・ワールドセッション内だけに残ります。",
    ),
    "dimension": (
        "ディメンションとブロック",
        "現在のディメンションからブロックやエンティティを読み取ります。",
        "Dimension Block BlockPosition Vector3",
        """\
import { world } from "@bedrock/sdk";

world.afterEvents.playerSpawn.subscribe(({ player }) => {
  const { location, dimension } = player;
  if (!location || !dimension) return;
  const block = dimension.getBlock({ ...location, y: location.y - 1 });
  if (block) player.sendMessage(block.typeId);
});
""",
        "<code>getBlock</code> は座標を切り下げます。<code>setType</code> はクライアント側のみで、変更後はブロックを取り直してください。",
    ),
    "system": (
        "スケジューラー",
        "ゲームのティックに合わせて処理を予約、反復、取り消しします。",
        "system System:currentTick,run,runTimeout,runInterval,clearRun TaskId TicksPerSecond",
        """\
import { system, TicksPerSecond, world } from "@bedrock/sdk";

world.afterEvents.playerSpawn.subscribe(() => {
  system.runTimeout(() => {
    world.getLocalPlayer()?.sendMessage("20 ティック経過");
  }, TicksPerSecond);
});
""",
        "遅延・間隔は整数ティック（既定は遅延 0、間隔 1）。遅延 0 も同期実行ではありません。<code>currentTick</code> は Mod ごとの値です。",
    ),
    "commands": (
        "コマンド",
        "チャットから実行できるクライアントコマンドと、その分岐・引数を登録します。",
        "System:commands CommandRegistry CommandParameter CommandBranch CommandContext",
        """\
import { system, world } from "@bedrock/sdk";

system.commands.register("hello", [{
  execute() {
    world.getLocalPlayer()?.sendMessage("Hello!");
  },
}]);
// ワールド内で /hello を実行
""",
        "エントリまたは有効化時に登録し、名前に <code>/</code> は付けません。<code>duration</code> は入力が秒、既定値とハンドラーで受け取る値はティックです。",
    ),
    "keybinds": (
        "キー割り当て",
        "Mod の操作にキーを割り当て、押下・解放をイベントで受け取ります。",
        "System:keybinds KeyBindings LocaleEntries SystemAfterEvents KeybindEvent",
        """\
import { system, world } from "@bedrock/sdk";

system.keybinds.add("greet", "key.greet", "C")
  .locale("ja_JP", { "key.greet": "あいさつ" });
system.afterEvents.keybindChanged.subscribe(({ id, pressed }) => {
  if (id === "greet" && pressed) world.getLocalPlayer()?.sendMessage("Hello!");
});
""",
        "キーには英字・数字・Tab・Space・F1〜F12 を指定できます。",
    ),
    "settings": (
        "設定ページ",
        "Mod Menu にトグルなどの設定項目を追加し、値を読み取ります。",
        "System:settings Setting SettingsPage SettingDescriptor SettingValue LocaleEntries ChoiceEntry",
        """\
import { Setting, system } from "@bedrock/sdk";

system.settings("demo.title")
  .add(Setting.toggle("enabled", "demo.enabled", true))
  .locale("ja_JP", { "demo.title": "デモ", "demo.enabled": "有効" })
  .registerPage();
""",
        "表示には対応するクライアントの設定画面が必要です。",
    ),
    "events": (
        "イベント（afterEvents）",
        "ローカルプレイヤーのスポーンと、登録したキーの状態変化を購読します。",
        "World:afterEvents System:afterEvents WorldAfterEvents SystemAfterEvents "
        "PlayerSpawnAfterEvent KeybindEvent EventSignal",
        """\
import { world } from "@bedrock/sdk";

const spawn = world.afterEvents.playerSpawn;
const greet = spawn.subscribe(({ player }) => {
  spawn.unsubscribe(greet);
  player.sendMessage("初回スポーン");
});
""",
        "<code>subscribe</code> は渡した関数そのものを返します。<code>initialSpawn</code> はその Mod への初回通知を示します。",
    ),
    "player": (
        "プレイヤー",
        "ローカル HUD にメッセージを表示し、プレイヤーのステータス効果を操作します。",
        "Player:name,sendMessage,addEffect,getEffect,getEffects,removeEffect,clearEffects RawMessage EffectOptions Effect TicksPerSecond",
        """\
import { TicksPerSecond, world } from "@bedrock/sdk";

world.afterEvents.playerSpawn.subscribe(({ player }) => {
  player.addEffect("minecraft:speed", 2 * TicksPerSecond);
  player.sendMessage("移動速度アップ");
});
""",
        "効果操作はローカルプレイヤーのティック内で実行します。効果時間は正の整数ティック（最大 2147483647）または <code>Infinity</code> です。",
    ),
    "entity": (
        "エンティティとコンポーネント",
        "座標・向きや、インベントリ・装備などのコンポーネントを読み取ります。",
        "Entity EntityComponentMap EntityComponent Vector3 Rotation DynamicProperty HealthComponent FoodComponent "
        "MovementComponent InventoryComponent InventoryContainer EquippableComponent ItemEntityComponent ItemStack EquipmentSlot",
        """\
import { EquipmentSlot, world } from "@bedrock/sdk";

world.afterEvents.playerSpawn.subscribe(({ player }) => {
  const item = player.getComponent("minecraft:equippable")
    ?.getEquipment(EquipmentSlot.Mainhand);
  if (item) player.sendMessage(`${item.typeId} × ${item.amount}`);
});
""",
        "取得できないコンポーネントは <code>undefined</code>。回転の単位は度です。インベントリと装備は読み取り専用です。",
    ),
    "client-info": (
        "クライアント診断",
        "ローカルプレイヤーの FPS、ping（ミリ秒の中央値）、左右 CPS、デバイス情報を読み取ります。",
        "Player:fps,ping,cps,device Cps DeviceInfo",
        """\
import { system, TicksPerSecond, world } from "@bedrock/sdk";

system.runInterval(() => {
  const player = world.getLocalPlayer();
  if (!player) return;
  const { fps, ping, cps, device } = player;
  console.log(fps, ping, cps.left, cps.right, device.clientVersion);
}, TicksPerSecond);
""",
        "FPS と CPS は直近 1 秒の値です。未接続時の ping は 0、取得できない値は 0 または空です。",
    ),
    "files": (
        "ファイル",
        "Mod ごとの <code>mod-data/</code> 領域に、テキスト・バイト列・JSON を保存します。",
        "files Files FileStat FileEntry RemoveOptions CopyOptions",
        """\
import { files } from "@bedrock/sdk";

files.writeJson("options.json", { enabled: true });
console.log(JSON.stringify(files.readJson("options.json")));
""",
        "パスは <code>dataDir</code> からの相対パスです。処理は同期で、ディレクトリの削除には <code>{ recursive: true }</code> が必要です。",
    ),
    "brightness": (
        "明るさ",
        "ゲームの設定を書き換えずに、クライアントのガンマ値を上書きします。",
        "brightness BrightnessOverride",
        """\
import { brightness, system, TicksPerSecond, world } from "@bedrock/sdk";

world.afterEvents.playerSpawn.subscribe(() => {
  brightness.setGamma(12);
  system.runTimeout(() => brightness.setGamma(), 5 * TicksPerSecond);
});
""",
        "範囲は 0〜25。引数なしでこの Mod の上書きを解除します。",
    ),
}

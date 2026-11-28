"""C++ pages: title, summary, public signatures, example, note."""

from pathlib import Path

HELLO = Path(__file__).resolve().parents[2] / "docs/site/quickstart/hello.cpp"

PAGES = {
    "index": (
        "C++ SDK とホスト",
        "<code>&lt;bedrock/sdk.hpp&gt;</code> をインクルードし、<a href=\"../index.html#cpp-build\"><code>bedrock::sdk</code> をリンク</a>します。<code>ClientHost</code> で Mod を起動・終了します。",
        """\
// bedrock::sdk::Mod
explicit Mod(std::string id);
virtual void onEnable();
virtual void onDisable();
virtual void onUpdate();
virtual void onPlayerSpawn(Player&);
virtual void onKeybind(std::string_view, bool);

// bedrock::sdk::ClientHost
using ErrorHandler = std::function<void(const Error&)>;
Result<void> start(Mod& mod, ErrorHandler onError = {});
Result<void> stop();
Result<void> attach(Mod& mod);
Result<void> detach(Mod& mod);
Result<void> synchronize(const std::function<void()>& operation);
""",
        HELLO.read_text(encoding="utf-8"),
        "ローダースレッドから DLL のローダーロック・コールバックの外で起動・終了します。Mod はホストより長く保持します。",
    ),
    "world": (
        "ワールドとディメンション",
        "<code>Mod::world()</code> から、クライアントが読み込んだプレイヤー・ディメンション・ブロックにアクセスします。",
        """\
// Mod
World& world() noexcept;

// World
std::optional<Player> localPlayer() const;
std::vector<Player> getAllPlayers() const;
std::vector<Player> getPlayers() const;
std::optional<Dimension> currentDimension() const;
std::optional<Dimension> getDimension(std::string_view typeId) const;
Result<void> sendMessage(std::string_view text) const;
std::optional<DynamicProperty> getDynamicProperty(std::string_view key) const;
Result<void> setDynamicProperty(std::string_view key, std::optional<DynamicProperty> value) const;

// Dimension, Block
std::optional<Block> getBlock(BlockPosition position) const; // Dimension
std::vector<Entity> getEntities() const;                     // Dimension
const std::string& typeId() const noexcept;                 // Block
BlockPosition location() const noexcept;                    // Block

struct BlockPosition { int x{}, y{}, z{}; };
using DynamicProperty = std::variant<bool, double, std::string, Vector3>;
""",
        """\
// Mod のメンバーとして追加
void onPlayerSpawn(bedrock::sdk::Player&) override {
    bedrock::sdk::Logger log("World");
    log.info("Players: ", world().getAllPlayers().size());
}
""",
        "ワールドの読み取りはゲームの所有ティック内で行います。動的プロパティはセッション限りで、<code>std::nullopt</code> で削除できます。",
    ),
    "entity": (
        "プレイヤーとエンティティ",
        "エンティティの状態を読み取り、ローカルプレイヤーにメッセージを表示します。",
        """\
// Entity
bool isValid() const;
std::optional<std::string> typeId() const;
std::optional<Vector3> location() const;
std::optional<Rotation> getRotation() const;
std::optional<Dimension> dimension() const;
std::optional<ItemStack> getItemStack() const;
std::optional<DynamicProperty> getDynamicProperty(std::string_view key) const;
Result<void> setDynamicProperty(std::string_view key, std::optional<DynamicProperty> value) const;
template<class T> std::optional<T> getComponent() const;

// Player : Entity
std::optional<std::string> name() const;
Result<void> sendMessage(std::string_view text) const;
Result<void> addEffect(std::string_view typeId, double duration, EffectOptions options = {}) const;
Result<bool> removeEffect(std::string_view typeId) const;
Result<std::vector<Effect>> getEffects() const;
Result<void> clearEffects() const;

struct Vector3 { float x{}, y{}, z{}; };
struct Rotation { float x{}, y{}; };
struct EffectOptions { std::uint8_t amplifier{}; bool showParticles{true}; };
struct Effect { std::string typeId; double duration{}; EffectOptions options; };

// MovementComponent, InventoryComponent, EquippableComponent
Vector3 velocity() const noexcept;                           // MovementComponent
std::size_t size() const noexcept;                          // InventoryComponent
std::optional<ItemStack> getItem(std::size_t slot) const;    // InventoryComponent
std::optional<ItemStack> getEquipment(EquipmentSlot slot) const; // EquippableComponent
enum class EquipmentSlot { MainHand, Offhand, Head, Chest, Legs, Feet };

// ItemStack
explicit ItemStack(std::string typeId, unsigned amount = 1);
const std::string& typeId() const noexcept;
unsigned amount() const noexcept;
""",
        """\
// Mod のメンバーとして追加
void onPlayerSpawn(bedrock::sdk::Player& player) override {
    if (const auto position = player.location()) {
        const auto sent = player.sendMessage("Y: " + std::to_string(position->y));
        if (!sent) bedrock::sdk::Logger("Hello").warn(sent.error().message);
    }
}
""",
        "取得できない値は <code>std::nullopt</code>。効果操作はローカルの所有ティック内のみで、時間はティックです。",
    ),
    "events": (
        "イベント購読",
        "イベントにコールバックを登録し、<code>Subscription</code> で購読を管理します。",
        """\
// EventSignal<Args...>
using Handler = std::function<void(Args...)>;
Subscription operator+=(Handler handler);
Subscription subscribe(Handler handler);
void publish(Args... args);

template<class Event>
using Events = EventSignal<const Event&>;

// Subscription
Subscription() = default;
Subscription(Subscription&& other) noexcept;
Subscription& operator=(Subscription&& other) noexcept;
void reset() noexcept;
""",
        """\
#include <bedrock/sdk.hpp>

void greet() {
    bedrock::sdk::EventSignal<int> event;
    auto sub = event.subscribe([](int count) {
        bedrock::sdk::Logger("Event").info("Count: ", count);
    });
    event.publish(1);
}
""",
        "<code>Subscription</code> はコピーできません。破棄または <code>reset()</code> で次回の通知から解除されます。",
    ),
    "system": (
        "ティックスケジューラー",
        "Mod の処理をゲームティックに合わせて予約・反復します。",
        """\
// Mod
System& system() noexcept;

// System
using Task = std::function<void()>;
using TaskId = std::uint64_t;
TaskId run(Task task);
TaskId runTimeout(Task task, unsigned ticks);
TaskId runInterval(Task task, unsigned ticks);
bool clearRun(TaskId id);
""",
        """\
// Mod のメンバーとして追加
void onEnable() override {
    system().runTimeout([] {
        bedrock::sdk::Logger("Task").info("20 ticks elapsed");
    }, 20);
}
""",
        "遅延は実時間ではなく、Mod に配信されたゲームティックで数えます。",
    ),
    "keybinds": (
        "キー割り当て",
        "操作 ID にキーを割り当て、押下・解放を受け取ります。",
        """\
// Mod
KeyBindings& keybinds() noexcept;
virtual void onKeybind(std::string_view, bool);

// KeyBindings
Result<void> add(std::string id, std::string labelKey, Key defaultKey);
void locale(std::string id, std::vector<Translation> entries);
std::optional<Key> key(std::string_view id) const;
Events<KeybindEvent> changed;
struct KeybindEvent { std::string_view id; bool pressed; };
struct Translation { std::string key; std::string value; };
""",
        """\
// Mod のメンバーとして追加（<stdexcept> もインクルード）
void onEnable() override {
    auto& keys = keybinds();
    const auto added = keys.add("greet", "key.greet", bedrock::sdk::Key::C);
    if (!added) throw std::runtime_error(added.error().message);
    keys.locale("ja_JP", {{"key.greet", "あいさつ"}});
}

void onKeybind(std::string_view id, bool pressed) override {
    if (id == "greet" && pressed) bedrock::sdk::Logger("Hello").info("Hello!");
}
""",
        "キーは英字・数字・Tab・Space・F1〜F12。イベントの <code>id</code> はコールバック内だけ有効です。",
    ),
    "settings": (
        "設定ページ",
        "Mod Menu に設定項目を登録し、その値を読み取ります。",
        """\
// Mod
SettingsPage settings(std::string titleKey) const;

// Setting
static Setting toggle(std::string id, std::string labelKey, bool initial);
static Setting sessionToggle(std::string id, std::string labelKey, bool initial);
static Setting action(std::string id, std::string labelKey, std::string descriptionKey = {});
static Setting slider(std::string id, std::string labelKey, double initial,
                      double minimum, double maximum, double step);
static Setting choice(std::string id, std::string labelKey, std::string initial,
                      std::vector<std::pair<std::string, std::string>> choices);
static Setting text(std::string id, std::string labelKey, std::string descriptionKey = {});

// SettingsPage
SettingsPage& add(Setting setting);
SettingsPage& locale(std::string id, std::vector<Translation> entries);
SettingsPage& onChange(SettingChanged callback);
SettingsPage& modName(std::string key);
Result<void> registerPage() const noexcept;
Result<void> registerPage(const Profile& profile) const noexcept;
Result<void> unregisterPage() const noexcept;
Result<SettingValue> value(std::string_view settingId) const noexcept;
using SettingValue = std::variant<bool, double, std::string>;
using SettingChanged = std::function<void(std::string_view, const SettingValue&)>;
struct Translation { std::string key; std::string value; };
""",
        """\
// Mod のメンバーとして追加（<stdexcept> もインクルード）
void onEnable() override {
    const auto result = settings("demo.title")
        .add(bedrock::sdk::Setting::toggle("enabled", "demo.enabled", true))
        .locale("ja_JP", {{"demo.title", "デモ"}, {"demo.enabled", "有効"}})
        .registerPage();
    if (!result) throw std::runtime_error(result.error().message);
}

void onDisable() override {
    const auto result = settings("demo.title").unregisterPage();
    if (!result) throw std::runtime_error(result.error().message);
}
""",
        "<code>BEDROCK_SDK_ENABLE_MOD_SETTINGS</code> を有効にし、<code>BEDROCK_GAME_VERSION</code> に対象ビルドを指定します。<code>sessionToggle</code> の永続化は呼び出し側が管理します。",
    ),
    "packets": (
        "パケット受信",
        "受信したタイトルパケットを購読したり、プレイヤー一覧をスナップショットとして取得したりします。",
        """\
enum class TitleType { Clear, Reset, SetTitle, SetSubtitle, ActionBar, Times,
                       SetTitleJson, SetSubtitleJson, Unknown };
struct TitleEvent { TitleType type{TitleType::Unknown}; std::string text; };

// TitleObserver
using Handler = std::function<void(const TitleEvent&)>;
bool install(std::string_view selectedProfileJson, std::string_view embeddedProfileJson,
             Handler handler, std::string& error);
void stop() noexcept;

// PacketRoster
bool install(std::string_view selectedProfileJson, std::string_view embeddedProfileJson,
             std::string& error);
bool install(const bedrock::sdk::SymbolResolver& resolver, std::string_view profileJson,
             std::string& error);
std::shared_ptr<const std::vector<RosterPlayerEntry>> snapshotShared() const noexcept;
void stop() noexcept;

struct RosterPlayerEntry {
    std::string name;
    std::array<std::byte, 16> uuid{};
    std::int64_t uniqueId{};
    std::array<std::uint32_t, 64> face{};
    bool hasFace{};
};
""",
        """\
#include <bedrock/sdk.hpp>

namespace sdk = bedrock::sdk;
sdk::TitleObserver titles;

bool start(const sdk::Profile& profile, std::string& error) {
    return titles.install(profile.json(), profile.json(), [](const sdk::TitleEvent& e) {
        if (e.type == sdk::TitleType::SetTitle) sdk::Logger("Title").info(e.text);
    }, error);
}

void stop() { titles.stop(); }
""",
        "<a href=\"profiles.html\">ビルドに合うプロファイル</a>が必要です。終了時は <code>stop()</code> を呼びます。",
    ),
    "world-render": (
        "ワールド描画",
        "ワールド描画の後に、線や四角形を描画します。",
        """\
// WorldRender
bool install(const SymbolResolver& resolver, std::string_view profileJson, std::string& error);
bool install(const Profile& profile, std::string& error);
bool install(std::string_view selectedProfileJson, std::string_view embeddedProfileJson,
             std::string& error);
WorldRenderSubscription onPostRender(std::function<void(WorldRenderer&)> callback);

// WorldRenderer
Vector3 origin() const noexcept;
bool drawLines(std::span<const WorldLineSegment> segments,
               std::array<float, 4> rgba, float thickness = 1.0f) noexcept;
bool drawNameQuad(std::array<WorldNameVertex, 4> vertices,
                  std::array<float, 4> rgba = {1.0f, 1.0f, 1.0f, 1.0f}) noexcept;

struct WorldLineSegment { Vector3 from{}; Vector3 to{}; };
struct WorldNameVertex { Vector3 position{}; };
""",
        """\
#include <bedrock/sdk.hpp>
namespace sdk = bedrock::sdk;
sdk::WorldRender render;
sdk::WorldRenderSubscription subscription;
bool start(const sdk::Profile& profile, std::string& error) {
    if (!render.install(profile, error)) return false;
    subscription = render.onPostRender([](sdk::WorldRenderer& renderer) {
        const auto o = renderer.origin();
        const sdk::WorldLineSegment line{{-o.x, 64.f - o.y, -o.z},
                                         {1.f - o.x, 64.f - o.y, -o.z}};
        renderer.drawLines(std::span{&line, 1}, {1.f, 0.f, 0.f, 1.f});
    });
    return true;
}
void stop() { subscription = {}; }
""",
        "描画はコールバック内でのみ行い、座標は <code>origin()</code> からの相対値で渡します。",
    ),
    "overlay": (
        "オーバーレイ",
        "ゲーム画面に Dear ImGui でオーバーレイを描画します。",
        """\
struct OverlayOptions {
    unsigned toggleVirtualKey = 0x79;
    bool initiallyVisible = true;
    bool captureInput = true;
    bool nativeCursor = true;
};

// Overlay
bool install(void (*drawCallback)(void* userdata), void* userdata,
             const OverlayOptions& options, std::string& error);
void uninstall() noexcept;
""",
        """\
#include <bedrock/sdk.hpp>
#include <imgui.h>

bedrock::sdk::Overlay overlay;

bool start(std::string& error) {
    bedrock::sdk::OverlayOptions options;
    options.captureInput = false;
    return overlay.install([](void*) {
        if (ImGui::Begin("Hello")) ImGui::TextUnformatted("Hello!");
        ImGui::End();
    }, nullptr, options, error);
}

void stop() { overlay.uninstall(); }
""",
        "<code>BEDROCK_SDK_ENABLE_GUI</code> を有効にし、<code>bedrock::gui_overlay</code> もリンクします。破棄前に <code>uninstall()</code> を呼びます（既定の表示切替は F10）。",
    ),
    "commands": (
        "クライアントコマンド",
        "コマンド名・引数・ハンドラーを登録し、コマンド行を受け取ります。",
        """\
enum class CommandParameterType { Integer, Message, CustomEnum, PacketEnum, String };
struct CommandParameter {
    std::string name;
    CommandParameterType type{CommandParameterType::Message};
    std::string enumName;
    bool optional{};
};
struct CommandDefinition {
    std::string name;
    std::string description;
    std::vector<std::vector<CommandParameter>> overloads;
    std::function<void(const std::string&)> handler;
};
struct CommandEnum {
    std::string name;
    std::vector<std::string> values;
    std::string mergedFrom;
    bool merged{};
};

// Commands
bool load(const std::string& profileJson, const std::vector<CommandEnum>& enums,
          const std::vector<CommandDefinition>& commands, std::string& error);
bool load(const Profile& profile, const std::vector<CommandEnum>& enums,
          const std::vector<CommandDefinition>& commands, std::string& error);
bool replace(const std::vector<CommandEnum>& enums,
             const std::vector<CommandDefinition>& commands, std::string& error);
bool isLoaded() const noexcept;
""",
        """\
#include <bedrock/sdk.hpp>

namespace sdk = bedrock::sdk;
sdk::Commands commands;

bool start(const sdk::Profile& profile, std::string& error) {
    return commands.load(profile, {}, {{"hello", "Say hello", {{}},
        [](const std::string&) { sdk::Logger("Hello").info("Hello!"); }}}, error);
}
// 接続前に start を呼び、接続後に /hello を実行
""",
        "この例はサーバーのコマンド一覧を受信する接続向けです。コマンド名に <code>/</code> を付けず、利用中は <code>Commands</code> を保持します。",
    ),
    "profiles": (
        "ゲームプロファイル",
        "実行中のゲームに一致するプロファイルを開き、対応する公開 API に渡します。",
        """\
enum class ProfileUpdatePolicy { EmbeddedOnly, RefreshIfNeeded, ForceRemote };
enum class ProfileCandidatePolicy { VerifiedOnly, AllowCandidates };

// Profiles
bool open(ProfileUpdatePolicy updatePolicy, ProfileCandidatePolicy candidatePolicy, std::string& error);
bool open(ProfileUpdatePolicy updatePolicy, std::string& error);
Profile snapshot() const;

// Profile
const std::string& json() const noexcept;
std::string version() const noexcept;
""",
        """\
#include <bedrock/sdk.hpp>

namespace sdk = bedrock::sdk;

bool showVersion(std::string& error) {
    sdk::Profiles profiles;
    if (!profiles.open(sdk::ProfileUpdatePolicy::EmbeddedOnly, error)) return false;
    sdk::Logger("Profile").info(profiles.snapshot().version());
    return true;
}
""",
        "プロファイルはゲームのビルドと一致するものが必要です。必要な API を導入する前に取得します。",
    ),
    "client-info": (
        "クライアント診断",
        "FPS、ping（ミリ秒）、左右 CPS、デバイス情報を読み取ります。",
        """\
// Player
double fps() const;
PingSnapshot ping() const;
CpsSnapshot cps() const;
DeviceInfo device() const;

// ClientInfo
static bool install(std::string_view selectedProfileJson, std::string_view embeddedProfileJson,
                    std::string& error);
static void stop() noexcept;
static bool installed() noexcept;
static ClientMetricsSnapshot snapshot() noexcept;
static double fps() noexcept;
static PingSnapshot ping() noexcept;
static CpsSnapshot cps() noexcept;
static DeviceInfo device() noexcept;

struct PingSnapshot {
    unsigned lastMs{};
    unsigned medianMs{};
    unsigned samples{};
    bool connected{};
};
struct CpsSnapshot { unsigned left{}; unsigned right{}; };
struct DeviceInfo {
    bool available{};
    std::string platform;
    std::string clientVersion;
    unsigned screenWidth{};
    unsigned screenHeight{};
};
struct ClientMetricsSnapshot {
    bool fpsAvailable{};
    double fps{};
    double frameTimeMs{};
    PingSnapshot ping{};
    CpsSnapshot cps{};
    DeviceInfo device{};
};
""",
        """\
// Mod のメンバーとして追加
void onEnable() override {
    system().runInterval([this] {
        if (const auto player = world().localPlayer()) {
            const auto ping = player->ping();
            const auto cps = player->cps();
            bedrock::sdk::Logger("Client").info(
                player->fps(), " FPS, ", ping.medianMs, " ms, ",
                cps.left, "/", cps.right, " CPS, ", player->device().platform);
        }
    }, 20);
}
""",
        "<code>ClientHost</code> 使用時は手動導入不要です。FPS/CPS は直近 1 秒。ローカル以外・取得不可の値は 0 または空です。",
    ),
    "content_log": (
        "Creator Content Log",
        "ゲーム標準の Creator Content Log にだけメッセージを出力します。",
        """\
enum class LogLevel : unsigned char { Debug=0, Info=1, Warn=2, Error=3, Off=4 };

// ContentLog
static bool available() noexcept;
static void write(LogLevel level, std::string_view tag, std::string_view message) noexcept;
""",
        """\
// Mod のメンバーとして追加
void onEnable() override {
    bedrock::sdk::ContentLog::write(bedrock::sdk::LogLevel::Info, "Hello", "Ready");
}
""",
        "<code>available()</code> は SDK 側の出力先の有無だけを示します。ゲーム側の Content Log 設定や実際の表示は保証しません。",
    ),
    "logging": (
        "ロガー",
        "レベル付きログを SDK の出力先に書き込みます。",
        """\
enum class LogLevel : unsigned char { Debug=0, Info=1, Warn=2, Error=3, Off=4 };

// Logger
explicit Logger(std::string tag = "Bedrock", LogLevel minimum = LogLevel::Info);
void setLevel(LogLevel level) noexcept;
LogLevel level() const noexcept;
void debug(std::string_view message);
void info(std::string_view message);
void warn(std::string_view message);
void error(std::string_view message);
template<class... Parts> void debug(Parts&&... parts);
template<class... Parts> void info(Parts&&... parts);
template<class... Parts> void warn(Parts&&... parts);
template<class... Parts> void error(Parts&&... parts);

// FileLogSink
enum class OpenMode { ClearOnOpen, Append };
bool open(const std::filesystem::path& path, OpenMode mode = OpenMode::ClearOnOpen);
void attach(Logger& logger) noexcept;
void close() noexcept;
bool isOpen() const noexcept;
""",
        """\
#include <bedrock/sdk.hpp>

void writeLog() {
    bedrock::sdk::FileLogSink file;
    bedrock::sdk::Logger log("Hello");
    if (file.open("hello.log")) file.attach(log);
    log.info("Ready");
}
""",
        "<code>FileLogSink</code> は使用中の <code>Logger</code> より長く保持します。ゲーム内だけに出す場合は <a href=\"content_log.html\">Creator Content Log</a> を使います。",
    ),
}

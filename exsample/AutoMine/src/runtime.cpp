#include "auto_mine.hpp"
#include "diagnostics.hpp"

namespace automine::client {

void AutoMineMod::signalStop() {
    stopped_ = true;
}

void AutoMineMod::stop() {
    signalStop();
    controls_.stop();
    renderSub_ = {};
    craft_.stop();
    gui_.stop();
    releaseInputs();
}

void AutoMineMod::run() {
    status = 1;
    try {
        const auto dir = directory();
        std::filesystem::create_directories(dir);
        crashrec::install(dir);
        loadConfig(dir);
        initialize();
        status = 2;
        std::ofstream out(dir / L"AutoMine.log", std::ios::trunc);
        while (!stopped_.load()) {
            flush(out);
            if (crashTestPending_.exchange(false)) {
                // Deliberate unguarded fault OUTSIDE the tick guards, to
                // verify the crash reporter end to end (/mine crashtest).
                crashrec::setSection("crashtest");
                crashrec::ringPush("crashtest: deliberate fault");
                volatile int *bad = nullptr;
                *bad = 42;
            }
            std::this_thread::sleep_for(100ms);
        }
        flush(out);
        status = 3;
    } catch (const std::exception &error) {
        status = 4;
        try {
            const auto dir = directory();
            std::filesystem::create_directories(dir);
            std::ofstream out(dir / L"AutoMine.log", std::ios::app);
            flush(out);
            out << "startup failed: " << error.what() << '\n';
        } catch (...) {
        }
    }
    stop();
}

void AutoMineMod::note(std::string text) {
    crashrec::ringPush(text.c_str());
    std::lock_guard lock(logMutex_);
    if (logs_.size() < 256)
        logs_.push_back(std::move(text));
}

void AutoMineMod::flush(std::ostream &out) {
    std::deque<std::string> batch;
    {
        std::lock_guard lock(logMutex_);
        batch.swap(logs_);
    }
    for (const auto &line : batch)
        out << line << '\n';
    if (!batch.empty())
        out.flush();
}

void AutoMineMod::reply(const std::string &text) {
    note(text);
    if (!gui_.message("[AutoMine] " + text))
        note("game message unavailable; see AutoMine.log");
}

std::filesystem::path AutoMineMod::directory() {
    PWSTR path{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &path)))
        throw std::runtime_error("Documents unavailable");
    const auto documents = std::filesystem::path(path);
    CoTaskMemFree(path);
    return documents / L"PEXData" / L"AutoMine";
}

void AutoMineMod::loadConfig(const std::filesystem::path &dir) {
    const auto path = dir / L"config.json";
    nlohmann::json config;
    if (std::filesystem::exists(path)) {
        std::ifstream file(path);
        file >> config;
    } else {
        config = {{"enabled", true},
                  {"radius", 24},
                  {"show_path", true},
                  {"skip_underwater", true},
                  {"independent_control", true},
                  {"continue_in_chat", true}};
        std::ofstream file(path);
        file << config.dump(2) << '\n';
    }
    enabled_ = config.value("enabled", true);
    radius_ = std::clamp(config.value("radius", 24), 4, 48);
    showPath_ = config.value("show_path", true);
    skipUnderwater_ = config.value("skip_underwater", true);
    independent_ = config.value("independent_control", true);
    continueInChat_ = config.value("continue_in_chat", true);
    backgroundCraft_ = independent_ && config.value("background_crafting", true);
    autoDiscard_ = config.value("auto_discard", true);
    for (const auto &id : config.value(
             "discard_items",
             std::vector<std::string>{"wheat_seeds", "beetroot_seeds", "poppy", "dandelion",
                                      "azure_bluet", "oxeye_daisy", "short_grass", "fern"}))
        discardItems_.insert(localPart(id));
}

namespace {
// Inert camera-visual policy for AutoMine: it reads the player proxy only
// and never drives the camera visuals, so every visual feature stays off
// (the same state freecam's policy reports while freecam is inactive).
bool camPolicyInactive() noexcept { return false; }
std::uint64_t camPolicyZero() noexcept { return 0; }
} // namespace

void AutoMineMod::initialize() {
    // /mine is registered through the public Commands facade: accumulate
    // the enum + definition, then load them with the selected profile.
    commandEnums_.push_back(bedrock::sdk::CommandEnum{
        "MineBlock", {"status", "stop", "path", "mobs", "food"}, "Block", true});
    bedrock::sdk::CommandDefinition mineDef;
    mineDef.name = "mine";
    mineDef.description = "AutoMine: find and mine nearby blocks";
    mineDef.overloads = {
        {bedrock::sdk::CommandParameter{"what", bedrock::sdk::CommandParameterType::CustomEnum,
                                        "MineBlock", false},
         bedrock::sdk::CommandParameter{"count", bedrock::sdk::CommandParameterType::Integer,
                                        "", true}},
        {}};
    mineDef.handler = [this](const std::string &line) {
        std::lock_guard lock(nativeMutex_);
        if (nativeLines_.size() < 16)
            nativeLines_.push_back(line);
    };
    commandDefs_.push_back(std::move(mineDef));
    craft_.onDiagnostic([this](const std::string &text) { note("craft: " + text); });

    bool accessReady = false;
    bedrock::sdk::Profiles profiles;
    std::string error;
    if (!profiles.open(bedrock::sdk::ProfileUpdatePolicy::RefreshIfNeeded,
                       bedrock::sdk::ProfileCandidatePolicy::AllowCandidates, error))
        throw std::runtime_error(error);
    const bedrock::sdk::Profile profile = profiles.snapshot();
    if (!resolver_.open(profile, error))
        throw std::runtime_error(error);
    {
        std::string failure;
        if (independent_) {
            const auto root = nlohmann::json::parse(profile.json());
            const auto &layouts = root.at("layouts");
            if (!layouts.contains("AutomationControls") || !layouts.contains("BlockActions") ||
                layouts.at("AutomationControls").value("revision", 0) < 2 ||
                layouts.at("BlockActions").value("revision", 0) < 2 ||
                (backgroundCraft_ && !layouts.contains("BackgroundContainers")))
                throw std::runtime_error("profile predates independent automation controls");
        }
        if (!gui_.load(profile, failure))
            throw std::runtime_error(failure);
        if (!injected_.load(profile, commandEnums_, commandDefs_, failure))
            throw std::runtime_error("/mine injected command definition rejected: " + failure);
        bedrock::sdk::CameraVisualPolicy camPolicy;
        camPolicy.active = &camPolicyInactive;
        camPolicy.stopped = &camPolicyInactive;
        camPolicy.spectatorMode = &camPolicyInactive;
        camPolicy.diagnosticsEnabled = &camPolicyInactive;
        camPolicy.epoch = &camPolicyZero;
        camPolicy.flightSerial = &camPolicyZero;
        std::vector<std::string> camWarnings;
        if (!camera_.configure(profile, camPolicy, failure, camWarnings))
            throw std::runtime_error(failure);
        for (const auto &warning : camWarnings)
            note("camera: " + warning);
        if (!loadWorld(profile, failure))
            throw std::runtime_error(failure);
        // AutoCraft (real ItemStackRequest crafting) is
        // optional: mining keeps working without it.
        std::string craftError;
        craftReady_ = craft_.load(profile, craftError);
        if (!craftReady_)
            note("craft adapter unavailable: " + craftError);
        else
            craftProfile_ = profile;
        // Path display is cosmetic: never fail startup over it.
        std::string outlineError;
        outlineReady_ = outline_.install(profile, outlineError);
        if (!outlineReady_)
            note("path renderer unavailable: " + outlineError);
        // Actor access (combat) is optional too.
        std::string actorError;
        actorsReady_ = loadActors(profile, resolver_, actorError);
        if (!actorsReady_)
            note("actor access unavailable: " + actorError);
        loadScreen(profile, resolver_); // verified screen state for item moves
        std::string accessError;
        accessReady = access_.configure(profile, resolver_, accessError);
        if (!accessReady)
            note("game access unavailable: " + accessError);
    }
    // Bind the hook dispatch to the game access BEFORE any subscription or
    // install, so the access' LevelTick scope opens inside the handlers
    // (the native pattern of a HookDispatch constructed with the access).
    if (accessReady) {
        std::string bindError;
        if (!game_.useGameAccess(access_, bindError))
            note("hook binding unavailable: " + bindError);
    }

    game_.onGameplay([this](const bedrock::sdk::GameplayEventValue &event) {
        if (event.clientInstance)
            client_.store(event.clientInstance);
    });
    game_.onWorldChange([this](const bedrock::sdk::WorldChangeEventValue &event) {
        if (event.clientInstance)
            client_.store(event.clientInstance);
        ++epoch_;
        pendingReset_ = true; // applied on the tick thread
    });
    game_.onLevelTick([this](const bedrock::sdk::LevelTickEventValue &event) {
        if (stopped_.load())
            return;
        try {
            level_.store(event.level);
            auto scope = gui_.tick(client_.load(), event.level, epoch_.load());
            if (!scope)
                return;
            (void)camera_.tick(client_.load(), epoch_.load());
            snap_ = camera_.readPlayerProxy();
            if (craftReady_)
                craftUpdateGuarded(event.level);
            tickGuarded();
            flushIndependent();
        } catch (const std::exception &e) {
            releaseInputs();
            flushIndependent();
            note(std::string("tick failed: ") + e.what());
        }
    });
    if (craftReady_) {
        std::string packetError;
        if (!craft_.installPackets(*craftProfile_, packetError)) {
            craftReady_ = false;
            note("craft packets unavailable: " + packetError);
        }
    }
    if (!game_.installGameplay(resolver_, error) ||
        !game_.installLevelTick(resolver_, error) ||
        !game_.installWorldEvents(resolver_, profile.json(), error) ||
        !game_.installNativeKeyboard(resolver_, profile.json(), error) ||
        !game_.installInput(resolver_, error))
        throw std::runtime_error(error);
    if (independent_) {
        std::string controlError;
        independentReady_ = blockActions_.load(profile, world_, controlError) &&
                            controls_.load(profile, controlError);
        if (!independentReady_) {
            controls_.stop();
            note("independent control unavailable: " + controlError);
        } else
            note("independent control: scoped movement + targeted GameMode mining; camera "
                 "untouched [runtime unverified]");
        game_.onNativeKey([this](const bedrock::sdk::NativeKeyEventValue &key) {
            if (anyScreenUp()) {
                controls_.clearManualInput();
                return; // neither typing nor its key-up events are movement
            }
            constexpr unsigned keys[]{kKeyW, kKeyA, kKeyS, kKeyD, kKeySpace, kKeySneak, kKeySprint};
            for (unsigned i = 0; i < std::size(keys); ++i)
                if (key.inputKey == keys[i])
                    controls_.manualInput(i, key.pressed);
            if (key.inputKey == 0x10)
                controls_.manualInput(11, key.pressed);
            if (key.inputKey == 0xA1)
                controls_.manualInput(12, key.pressed);
            if (key.inputKey == 0x11)
                controls_.manualInput(13, key.pressed);
            if (key.inputKey == 0xA3)
                controls_.manualInput(14, key.pressed);
            if (controls_.manualActive())
                blockActions_.cancelMiningOwnership();
        });
        game_.onMouse([this](const bedrock::sdk::MouseEventValue &event) {
            if (anyScreenUp()) {
                controls_.clearManualInput();
                return;
            }
            if (!event.synthetic && (event.action == 1 || event.action == 2) &&
                (!anyScreenUp() || event.state == 0)) {
                controls_.manualInput(8 + static_cast<unsigned>(event.action), event.state != 0);
                blockActions_.cancelMiningOwnership();
            }
        });
    }
    if (outlineReady_) {
        renderSub_ = outline_.onPostRender(
            [this](bedrock::sdk::WorldRenderer &renderer) { drawPathGuarded(renderer); });
        note("path renderer: subscribed");
    }
    note("AutoMine v2: /mine [block] registered in CommandRegistry (autocomplete)");
    note("AutoMine build: profile " + profile.version() +
         (backgroundCraft_ ? "; background crafting enabled [runtime unverified]"
                           : "; foreground crafting"));
    note(bedrock::sdk::hookRuntimeDiagnostic());
}

bool AutoMineMod::loadWorld(const bedrock::sdk::Profile &profile, std::string &failure) {
    try {
        world_.load(profile.json());
    } catch (const std::exception &e) {
        failure = e.what();
        return false;
    }
    return true;
}

} // namespace automine::client

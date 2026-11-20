#include "auto_mine.hpp"
#include "diagnostics.hpp"

namespace automine::client {

void AutoMineMod::signalStop() {
    stopped_ = true;
}

void AutoMineMod::stop() {
    signalStop();
    controls_.stop();
    if (renderToken_) {
        BDS_HookUnsubscribe(renderToken_);
        renderToken_ = nullptr;
    }
    if (mouseToken_) {
        BDS_HookUnsubscribe(mouseToken_);
        mouseToken_ = nullptr;
    }
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

void AutoMineMod::initialize() {
    injected_.setDiagnostic([this](const std::string &text) { note("cmd: " + text); });
    std::string defError;
    if (!injected_.addMergedEnum("MineBlock", "Block", {"status", "stop", "path", "mobs", "food"},
                                 defError) ||
        !injected_.add(
            "mine", "AutoMine: find and mine nearby blocks",
            {{{"what", bedrock::InjectedType::customEnum("MineBlock"), false, 0},
              {"count", bedrock::InjectedType::integer(), true, 0}},
             {}},
            [this](const std::string &line) {
                std::lock_guard lock(nativeMutex_);
                if (nativeLines_.size() < 16)
                    nativeLines_.push_back(line);
            },
            defError))
        throw std::runtime_error("/mine injected command definition rejected: " + defError);
    craft_.onDiagnostic([this](const std::string &text) { note("craft: " + text); });
    bedrock::ProfileManager profiles;
    const bedrock::ProfileRequirements required{bedrock::Feature::Gameplay,
                                                bedrock::Feature::LevelTick,
                                                bedrock::Feature::World,
                                                bedrock::Feature::Motion,
                                                bedrock::Feature::Gui,
                                                bedrock::Feature::WorldHeight,
                                                bedrock::Feature::ClientCommands,
                                                bedrock::Feature::WorldEvents,
                                                bedrock::Feature::Actors,
                                                bedrock::Feature::Camera};
    const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,
                                          bedrock::CandidatePolicy::AllowCandidates};
    std::string error;
    if (!profiles.prepare(
            required, options,
            [this](const bedrock::RuntimeProfile &profile, std::string &failure) {
                if (independent_) {
                    const auto root = bedrock::parseNativeProfile(profile.json());
                    if (!root.at("layouts").contains("AutomationControls") ||
                        !root.at("layouts").contains("BlockActions") ||
                        root.at("layouts").at("AutomationControls").value("revision", 0) < 2 ||
                        root.at("layouts").at("BlockActions").value("revision", 0) < 2 ||
                        (backgroundCraft_ &&
                         !root.at("layouts").contains("BackgroundContainers"))) {
                        failure = "profile predates independent automation controls";
                        return false;
                    }
                }
                if (!gui_.load(profile, failure) || !injected_.load(profile, failure) ||
                    !camera_.load(profile, failure) || !loadWorld(profile, failure))
                    return false;
                // AutoCraft (real ItemStackRequest crafting) is
                // optional: mining keeps working without it.
                std::string craftError;
                craftReady_ = craft_.load(profile, craftError);
                if (!craftReady_)
                    note("craft adapter unavailable: " + craftError);
                else
                    craftProfileJson_ = profile.json();
                // Path display is cosmetic: never fail startup over it.
                std::string outlineError;
                outlineReady_ = outline_.load(profile, outlineError);
                if (!outlineReady_)
                    note("path renderer unavailable: " + outlineError);
                // Actor access (combat) is optional too.
                std::string actorError;
                actorsReady_ = loadActors(profile, actorError);
                if (!actorsReady_)
                    note("actor access unavailable: " + actorError);
                loadScreen(profile); // verified screen state for item moves
                std::string accessError;
                if (!access_.load(profile, accessError))
                    note("game access unavailable: " + accessError);
                return true;
            },
            error))
        throw std::runtime_error(error);

    game_.on<bedrock::GameplayEvent>([this](const bedrock::GameplayEvent &event) {
        if (event.clientInstance)
            client_.store(event.clientInstance);
    });
    game_.on<bedrock::WorldChangeEvent>([this](const bedrock::WorldChangeEvent &event) {
        if (event.clientInstance)
            client_.store(event.clientInstance);
        ++epoch_;
        pendingReset_ = true; // applied on the tick thread
    });
    game_.on<bedrock::LevelTickEvent>([this](const bedrock::LevelTickEvent &event) {
        if (stopped_.load())
            return;
        try {
            level_.store(event.level);
            auto scope = gui_.tick(client_.load(), event.level, epoch_.load());
            if (!scope)
                return;
            auto camScope = camera_.tick(client_.load(), bedrock::WorldEpoch{epoch_.load()});
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
        if (!craft_.installPackets(craftProfileJson_, packetError)) {
            craftReady_ = false;
            note("craft packets unavailable: " + packetError);
        }
    }
    const auto selected = profiles.snapshot();
    hooks_ = std::make_unique<bedrock::GameHooks>(game_);
    if (!hooks_->installGameplay(selected->resolver(), error) ||
        !hooks_->installLevelTick(selected->resolver(), error) ||
        !hooks_->installWorldEvents(selected->resolver(), selected->json(), error) ||
        !hooks_->installNativeKeyboard(selected->resolver(), selected->json(), error) ||
        !hooks_->installInput(selected->resolver(), error))
        throw std::runtime_error(error);
    if (independent_) {
        std::string controlError;
        independentReady_ = blockActions_.load(*selected, world_, controlError) &&
                            controls_.load(*selected, controlError);
        if (!independentReady_) {
            controls_.stop();
            note("independent control unavailable: " + controlError);
        } else
            note("independent control: scoped movement + targeted GameMode mining; camera "
                 "untouched [runtime unverified]");
        game_.on<bedrock::NativeKeyEvent>([this](const bedrock::NativeKeyEvent &key) {
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
        game_.on<bedrock::MouseEvent>([this](const bedrock::MouseEvent &event) {
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
    } else
        installMouseCapture(selected->resolver());
    if (outlineReady_) {
        if (BDS_HookSubscribe(BDS_HOOK_RENDER_LEVEL, outline_.renderTarget(), &onRenderLevel, this,
                              &renderToken_))
            note("path renderer: subscribed");
        else {
            outlineReady_ = false;
            note("path renderer: subscribe failed");
        }
    }
    note("AutoMine v2: /mine [block] registered in CommandRegistry (autocomplete)");
    note(std::string("AutoMine build: ") + BEDROCK_SDK_BUILD_ID +
         (backgroundCraft_ ? "; background crafting enabled [runtime unverified]"
                           : "; foreground crafting"));
    note(BDS_HookRuntimeDiagnostic());
}

bool AutoMineMod::loadWorld(const bedrock::RuntimeProfile &profile, std::string &failure) {
    try {
        world_.load(profile.json());
    } catch (const std::exception &e) {
        failure = e.what();
        return false;
    }
    return true;
}

} // namespace automine::client

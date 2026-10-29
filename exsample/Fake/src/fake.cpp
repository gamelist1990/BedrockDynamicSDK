#include <bedrock/runtime/game_hooks.hpp>
#include <bedrock/native/command_requests.hpp>
#include <bedrock/native/message_commands.hpp>
#include <fake_command.hpp>
#include <knownfolders.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
class FakeMod {
public:
    std::atomic<unsigned> status{};
    void signalStop() noexcept { stopped_=true; }
    void run() noexcept {
        status=1;
        try {
            PWSTR path{};
            if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents,KF_FLAG_CREATE,nullptr,&path)))
                throw std::runtime_error("Documents directory unavailable");
            const auto directory=std::filesystem::path(path)/L"PEXData"/L"Fake";
            CoTaskMemFree(path);
            std::filesystem::create_directories(directory);
            std::ofstream log(directory/L"Fake.log",std::ios::trunc);
            if (!log) throw std::runtime_error("could not open Fake.log");
            initialize();
            while (!stopped_) { flush(log); std::this_thread::sleep_for(std::chrono::milliseconds{100}); }
            hooks_.reset(); transport_.stop(); flush(log); status=3;
        } catch (const std::exception& error) {
            note(std::string("startup failed: ")+error.what()); status=4;
            hooks_.reset(); transport_.stop();
            try {
                const auto* home=_wgetenv(L"USERPROFILE");
                if (home) {
                    auto directory=std::filesystem::path(home)/L"Documents"/L"PEXData"/L"Fake";
                    std::filesystem::create_directories(directory);
                    std::ofstream log(directory/L"Fake.log",std::ios::app); flush(log);
                }
            } catch (...) {}
        } catch (...) { status=4; }
    }
private:
    void note(std::string value) {
        OutputDebugStringA(("[Fake] "+value+"\n").c_str());
        std::lock_guard lock(logMutex_);
        if (logs_.size()<256) logs_.push_back(std::move(value));
    }
    void flush(std::ostream& log) {
        std::deque<std::string> batch;
        { std::lock_guard lock(logMutex_); batch.swap(logs_); }
        for (const auto& value:batch) log<<value<<'\n';
        if (!batch.empty()) log.flush();
    }
    void execute(std::string_view line, const bedrock::NativeMessageInvocation* context=nullptr) {
        const auto invocation=fake_mod::parse(line);
        if (!invocation) { note(invocation.error); return; }
        bedrock::CommandRequestOptions options;
        options.origin=invocation.origin;
        if (context) {
            if (!context->contextAvailable || context->version<=0) {
                note(nlohmann::json({{"event","request-blocked"},{"command",std::string(line)},
                    {"version",context->version},{"origin_context_available",context->contextAvailable}}).dump());
                return;
            }
            options.version=context->version; options.uuid=context->origin.uuid; options.playerId=context->origin.playerId;
        }
        note(nlohmann::json({{"event","fake-command"},{"input",std::string(line)},{"command","/me "+invocation.message},
            {"origin",options.origin},{"version",options.version}}).dump());
        transport_.commands().request("/me "+invocation.message,options).then([this](const bedrock::CommandResponse& response) {
            nlohmann::json result={{"status",bedrock::commandResponseStatusName(response.status)},
                {"request_id",response.origin.requestId},{"origin",response.origin.type},
                {"success_count",response.successCount},{"output_type",response.outputType},
                {"succeeded",response.succeeded()},{"has_data_set",response.hasDataSet},
                {"messages",nlohmann::json::array()},{"detail",response.detail}};
            for (const auto& message:response.messages)
                result["messages"].push_back({{"successful",message.successful},{"id",message.id},{"parameters",message.parameters}});
            note(result.dump(-1,' ',false,nlohmann::json::error_handler_t::replace));
        });
    }
    void initialize() {
        transport_.onDiagnostic([this](const std::string& value) { note(value); });
        if (!transport_.addLocalCommand("fake",[this](std::string_view line) { execute(line); }))
            throw std::runtime_error("/fake local registration failed");
        if (!registration_.add("fake","Send /me with an explicit CommandOriginData type",[this](bedrock::NativeMessageInvocation invocation) {
            std::lock_guard lock(commandMutex_);
            if (pendingCommands_.size()<64) pendingCommands_.push_back(std::move(invocation));
            else note("/fake queue full; command not sent");
        }))
            throw std::runtime_error("/fake native definition could not be added");
        bedrock::ProfileManager profiles;
        std::string error;
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,bedrock::CandidatePolicy::AllowCandidates};
        if (!profiles.prepare({bedrock::Feature::Gameplay,bedrock::Feature::LevelTick},options,
            [this](const bedrock::RuntimeProfile& profile,std::string& failure) { return registration_.load(profile,failure) && transport_.load(profile,failure); },error))
            throw std::runtime_error(error);
        game_.Event.Enum.Gameplay.then([this](const bedrock::GameplayEvent& event) {
            if (event.clientInstance && client_.exchange(event.clientInstance)!=event.clientInstance)
                note("ClientInstance observed; waiting for owning tick to register /fake");
        });
        game_.Event.Enum.LevelTick.then([this](const bedrock::LevelTickEvent& event) {
            if (stopped_) return;
            try {
                const auto client=client_.load();
                std::deque<bedrock::NativeMessageInvocation> commands;
                { std::lock_guard lock(commandMutex_); commands.swap(pendingCommands_); }
                for (const auto& invocation:commands) execute(invocation.line,&invocation);
                transport_.update(client,event.level);
                if (registrationClient_!=client || registrationLevel_!=event.level) {
                    registrationClient_=client; registrationLevel_=event.level;
                    registered_=false; registrationTicks_=0;
                }
                if (registrationTicks_++%20==0) {
                    const auto state=registration_.update(client);
                    registered_=state==bedrock::NativeMessageCommands::State::Registered;
                    if (!registrationState_ || *registrationState_!=state) {
                        registrationState_=state;
                        if (registered_) note("/fake registered in Minecraft CommandRegistry [message argument + command suggestions]");
                        else if (state==bedrock::NativeMessageCommands::State::WaitingClient) note("registration waiting: ClientInstance");
                        else if (state==bedrock::NativeMessageCommands::State::WaitingRegistry) note("registration waiting: Minecraft CommandRegistry");
                        else if (state==bedrock::NativeMessageCommands::State::WaitingTemplate) note("registration waiting: CommandMessage parser type");
                        else note("registration blocked: "+registration_.error());
                    }
                }
                const bool nowReady=registered_ && transport_.senderReady();
                if (ready_!=nowReady) {
                    ready_=nowReady; status=nowReady ? 2u : 1u;
                    note(nowReady ? "Fake ready; /fake message | /fake --origin 0..255 message" : "Fake waiting: registration or request sender unavailable");
                }
            }
            catch (const std::exception& error) { note(std::string("tick: ")+error.what()); }
        });
        const auto profile=profiles.snapshot();
        hooks_=std::make_unique<bedrock::GameHooks>(game_);
        if (!hooks_->installGameplay(profile->resolver(),error) || !hooks_->installLevelTick(profile->resolver(),error))
            throw std::runtime_error(error);
        note("Fake attached; waiting for Minecraft CommandRegistry registration and request sender");
        note("Open Esc then resume gameplay once to observe ClientInstance and activate the sender.");
        note("Responses appear in Fake.log; 10 second timeout, callbacks resume on the next game tick.");
        note(BDS_HookRuntimeDiagnostic());
    }
    bedrock::GameApi game_;
    bedrock::NativeCommandRequests transport_;
    bedrock::NativeMessageCommands registration_;
    std::optional<bedrock::NativeMessageCommands::State> registrationState_;
    void* registrationClient_{};
    void* registrationLevel_{};
    unsigned registrationTicks_{};
    bool registered_{},ready_{};
    std::unique_ptr<bedrock::GameHooks> hooks_;
    std::atomic<void*> client_{};
    std::atomic<bool> stopped_{};
    std::mutex commandMutex_;
    std::deque<bedrock::NativeMessageInvocation> pendingCommands_;
    std::mutex logMutex_;
    std::deque<std::string> logs_;
};
// Process lifetime; pinned worker prevents callbacks entering an unloaded DLL.
std::atomic<FakeMod*> mod{};
std::atomic<unsigned> initialStatus{};
std::atomic<bool> stopRequested{};
DWORD WINAPI start(void*) {
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&start),&pinned)) { initialStatus=4; return 1; }
    try { auto* instance=new FakeMod; mod=instance; if (stopRequested) instance->signalStop(); instance->run(); }
    catch (...) { initialStatus=4; }
    return 0;
}
}
extern "C" __declspec(dllexport) unsigned Fake_GetStatus() { auto* instance=mod.load(); return instance ? instance->status.load() : initialStatus.load(); }
extern "C" __declspec(dllexport) void Fake_Stop() { stopRequested=true; if (auto* instance=mod.load()) instance->signalStop(); }
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread=CreateThread(nullptr,0,start,nullptr,0,nullptr)) CloseHandle(thread);
        else initialStatus=4;
    } else if (reason==DLL_PROCESS_DETACH) stopRequested=true;
    return TRUE;
}

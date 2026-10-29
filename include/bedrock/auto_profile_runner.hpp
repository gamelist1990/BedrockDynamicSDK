#pragma once
#include <bedrock/detail/auto_profile_x64.hpp>
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace bedrock::auto_profile {
// User control and diagnostic output share status.json. Control is reread
// before every status replacement, so the output writer preserves user edits.
class Developer {
public:
    ~Developer(){cancel_.store(true);if(worker_.joinable())worker_.join();}
    Developer()=default;Developer(const Developer&)=delete;Developer& operator=(const Developer&)=delete;
    void requestStop() noexcept{cancel_.store(true);}
    void shutdown(){requestStop();if(worker_.joinable())worker_.join();}
    bool needsSources(const std::filesystem::path& status){
        readControl(status);
        return enabled_&&(!worker_.joinable()||done_.load())&&(!started_||lastRevision_!=revision_||reenabled_);
    }
    void poll(const std::filesystem::path& directory,const GameIdentity& identity,std::vector<Json> sources,const Json& catalog){
        readControl(directory/L"status.json");
        if(!enabled_){cancel_.store(true);if(done_.load()&&worker_.joinable())worker_.join();return;}
        if(done_.load()&&worker_.joinable())worker_.join();
        if(worker_.joinable())return;
        if(started_&&lastRevision_==revision_&&!reenabled_){monitor(directory);return;}
        if(!sourcePath_.empty()){
            const auto path=std::filesystem::u8path(sourcePath_);std::ifstream file(path.is_absolute()?path:directory/path,std::ios::binary|std::ios::ate);
            if(!file||file.tellg()<0||file.tellg()>16*1024*1024){set({{"state","source-error"},{"error","auto-dev-source is unreadable or exceeds16MiB"}});return;}
            std::string text(static_cast<std::size_t>(file.tellg()),'\0');file.seekg(0);file.read(text.data(),static_cast<std::streamsize>(text.size()));
            try{sources.insert(sources.begin(),Json::parse(text));}catch(const std::exception& e){set({{"state","source-error"},{"error",e.what()}});return;}
        }
        reenabled_=false;started_=true;lastRevision_=revision_;cancel_.store(false);done_.store(false);
        const auto requestedVersion=version_;
        set({{"state","snapshotting"},{"run",revision_},{"game_memory_writes",false},{"auto_apply",false}});
        worker_=std::thread([this,directory,identity,sources=std::move(sources),catalog,requestedVersion]{
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
            try{
                auto image=captureImage(identity,cancel_);if(!requestedVersion.empty())image.version=requestedVersion;
                Engine engine;auto result=engine.run(image,sources,catalog,instructionSignature,cancel_,[this](const Json& progress){set({{"state","analysing"},{"progress",progress}});},instructionFacts);
                if(cancel_.load())throw std::runtime_error("auto-dev cancelled");
                result.report["identity"]={{"file_sha256",image.identity.fileSha256},{"text_sha256",image.identity.textSha256}};
                result.report["profile_has_functions"]=!result.profile["symbols"].empty();
                result.report["run"]=lastRevision_;
                const auto dir=directory/L"auto-dev";std::filesystem::create_directories(dir);
                save(dir/L"windows-x64.json",result.profile.dump(2)+"\n");
                save(dir/L"report.json",result.report.dump(2)+"\n");
                std::ostringstream text;text<<"CheckSig auto-dev (experimental)\n"<<result.report["status"].get<std::string>()<<"\nGenerated functions: "<<result.profile["symbols"].size()<<"\n";
                for(const auto& [name,item]:result.report["functions"].items())if(item.value("status",std::string{})=="unresolved"||item.value("status",std::string{})=="conflict")text<<name<<": "<<item.value("reasons",Json::array()).dump()<<"\n";
                for(const auto& item:result.report["unresolved"])text<<item.value("path",std::string{})<<": "<<item.value("reason",std::string{})<<"\n";
                text<<"No candidate was called, hooked, applied, or written to game memory. Review generated definitions and native layouts before use.\n";
                save(dir/L"report.txt",text.str());
                {std::lock_guard lock(mutex_);generated_=result.profile;}
                set({{"state",result.report["status"]},{"run",lastRevision_},{"generated_functions",result.profile["symbols"].size()},
                    {"unresolved",result.report["unresolved"].size()},{"vtables",result.report["vtables"].size()},
                    {"profile",utf8(dir/L"windows-x64.json")},{"report",utf8(dir/L"report.json")},{"auto_apply",false},{"game_memory_writes",false}});
            }catch(const std::exception& e){set({{"state",cancel_.load()?"cancelled":"error"},{"error",e.what()},{"auto_apply",false}});}
            done_.store(true);
        });
    }
    bool decorate(Json& status,const std::filesystem::path& path){
        if(!readControl(path))return false;
        status["auto-dev-algorithm"]=1;status["auto-dev-supported"]=true;
        status["auto-dev"]=enabled_;status["auto-dev-run"]=revision_;
        if(!sourcePath_.empty())status["auto-dev-source"]=sourcePath_;
        if(!version_.empty())status["auto-dev-version"]=version_;
        {std::lock_guard lock(mutex_);status["auto-dev-result"]=summary_;}
        return true;
    }
private:
    bool readControl(const std::filesystem::path& path){
        std::error_code error;if(!std::filesystem::exists(path,error))return true;
        const auto size=std::filesystem::file_size(path,error);if(error||size>4*1024*1024)return false;
        try{std::ifstream file(path,std::ios::binary);Json control;file>>control;
            const bool enabled=control.value("auto-dev",false);const auto revision=control.value("auto-dev-run",std::uint64_t{});
            const auto source=control.value("auto-dev-source",std::string{}),version=control.value("auto-dev-version",std::string{});
            if(source.size()>32768||version.size()>64)return false;
            if((!enabled_&&enabled)||source!=sourcePath_||version!=version_)reenabled_=true;
            if(source!=sourcePath_||version!=version_)cancel_.store(true);
            if(!enabled||revision!=revision_)cancel_.store(true);
            enabled_=enabled;revision_=revision;sourcePath_=source;version_=version;return true;
        }catch(...){return false;} // Do not overwrite a partially edited file.
    }
    void monitor(const std::filesystem::path& directory){
        const auto now=std::chrono::steady_clock::now();if(now<nextMonitor_)return;nextMonitor_=now+std::chrono::seconds(5);
        Json profile;{std::lock_guard lock(mutex_);profile=generated_;}if(profile.empty())return;
        try{auto live=inspectLive(profile);save(directory/L"auto-dev"/L"live.json",live.dump(2)+"\n");
            std::lock_guard lock(mutex_);summary_["live"]=std::move(live);
        }catch(const std::exception& e){std::lock_guard lock(mutex_);summary_["live_error"]=e.what();}
    }
    static std::string utf8(const std::filesystem::path& path){const auto text=path.u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};}
    static void save(const std::filesystem::path& path,const std::string& value){
        const auto temp=std::filesystem::path(path.wstring()+L".tmp");
        {std::ofstream file(temp,std::ios::binary|std::ios::trunc);file.write(value.data(),static_cast<std::streamsize>(value.size()));file.flush();if(!file)throw std::runtime_error("auto-dev output write failed");}
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("auto-dev output replace failed");
    }
    void set(Json value){std::lock_guard lock(mutex_);summary_=std::move(value);}
    bool enabled_{},reenabled_{},started_{};std::uint64_t revision_{},lastRevision_{};
    std::string sourcePath_,version_;std::thread worker_;std::atomic<bool> cancel_{false},done_{true};
    std::chrono::steady_clock::time_point nextMonitor_{};Json generated_;
    std::mutex mutex_;Json summary_={{"state","disabled"}};
};
} // namespace bedrock::auto_profile

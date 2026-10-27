#pragma once
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace bedrock {
// Text dispatch only. For native suggestions/metadata registration use
// <bedrock/native_client_commands.hpp> and NativeClientCommands.
// Register before native callbacks start. Dispatch on the owning game thread.
// Unknown names are not consumed; the adapter forwards them to vanilla.
class ClientCommands {
public:
    using Handler = std::function<void(std::string_view)>;
    bool add(std::string name, Handler handler) {
        if(name.empty()||name.find_first_of(" /\t\r\n")!=name.npos||!handler)return false;
        return commands_.emplace(std::move(name),std::move(handler)).second;
    }
    bool contains(std::string_view line) const {return find(line)!=commands_.end();}
    bool dispatch(std::string_view line) const {
        const auto it=find(line);if(it==commands_.end())return false;
        auto end=line.find_first_of(" \t\r\n",1);
        it->second(end==line.npos?std::string_view{}:line.substr(end+1));return true;
    }
private:
    auto find(std::string_view line)const -> std::map<std::string,Handler,std::less<>>::const_iterator {
        if(line.empty()||line.front()!='/')return commands_.end();
        const auto end=line.find_first_of(" \t\r\n",1);
        return commands_.find(line.substr(1,end==line.npos?line.size()-1:end-1));
    }
    std::map<std::string,Handler,std::less<>> commands_;
};
}

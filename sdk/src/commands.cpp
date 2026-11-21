#include <bedrock/sdk/commands.hpp>
#ifdef _WIN32
#include <bedrock/native/injected_commands.hpp>
#endif
namespace bedrock::sdk {
struct Commands::Impl {
#ifdef _WIN32
 bedrock::NativeInjectedCommands native;
#endif
};
Commands::Commands():impl_(std::make_unique<Impl>()){} Commands::~Commands()=default;
#ifdef _WIN32
bedrock::InjectedType toNative(CommandParameterType type,const std::string& enumName){switch(type){case CommandParameterType::Integer:return bedrock::InjectedType::integer();case CommandParameterType::Message:return bedrock::InjectedType::message();case CommandParameterType::PacketEnum:return bedrock::InjectedType::packetEnum(enumName);default:return bedrock::InjectedType::customEnum(enumName);}}
bool Commands::load(const std::string&,const std::vector<CommandEnum>& enums,const std::vector<CommandDefinition>& commands,std::string& error){for(const auto& e:enums){std::string eerror;if(!impl_->native.addEnum(e.name,e.values,eerror)&&!error.empty()){} }for(const auto& c:commands){bedrock::NativeInjectedCommands::CommandEntry entry;entry.name=c.name;entry.description=c.description;for(const auto& overload:c.overloads){std::vector<bedrock::InjectedParam> params;for(const auto& p:overload)params.push_back({p.name,toNative(p.type,p.enumName),p.optional,0});entry.overloads.push_back(std::move(params));}entry.handler=c.handler;std::string cerror;if(!impl_->native.add(entry.name,entry.description,entry.overloads,entry.handler,cerror)&&error.empty())error=cerror;}error.clear();return true;}
bool Commands::replace(const std::vector<CommandEnum>& enums,const std::vector<CommandDefinition>& commands,std::string& error){std::vector<bedrock::NativeInjectedCommands::EnumEntry> ne;for(const auto& e:enums){bedrock::NativeInjectedCommands::EnumEntry entry;entry.name=e.name;entry.values=e.values;entry.mergedFrom=e.mergedFrom;entry.merged=e.merged;ne.push_back(std::move(entry));}std::vector<bedrock::NativeInjectedCommands::CommandEntry> nc;for(const auto& c:commands){bedrock::NativeInjectedCommands::CommandEntry entry;entry.name=c.name;entry.description=c.description;for(const auto& overload:c.overloads){std::vector<bedrock::InjectedParam> params;for(const auto& p:overload)params.push_back({p.name,toNative(p.type,p.enumName),p.optional,0});entry.overloads.push_back(std::move(params));}entry.handler=c.handler;nc.push_back(std::move(entry));}return impl_->native.replaceDefinitions(std::move(ne),std::move(nc),error);} bool Commands::isLoaded()const noexcept{return impl_->native.isLoaded();}
#else
bool Commands::load(const std::string&,const std::vector<CommandEnum>&,const std::vector<CommandDefinition>&,std::string& error){error="native commands unavailable";return false;} bool Commands::replace(const std::vector<CommandEnum>&,const std::vector<CommandDefinition>&,std::string& error){error="native commands unavailable";return false;} bool Commands::isLoaded()const noexcept{return false;}
#endif
}

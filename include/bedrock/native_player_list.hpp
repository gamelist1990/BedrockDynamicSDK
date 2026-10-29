#pragma once
#include <bedrock/profile_json.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <array>
#include <optional>
#include <unordered_map>
#include <vector>

namespace bedrock {
struct PlayerUuid {std::uint64_t low{},high{};bool operator==(const PlayerUuid&) const=default;};
struct PlayerRosterEntry {PlayerUuid uuid;std::uint64_t uniqueId{};std::string name;};
// All returned values are owned copies. Call only on the owning game tick;
// native containers, strings, skin objects and offsets stay inside this adapter.
class NativePlayerList {
public:
    void load(std::string_view profile) {loadRoot(parseNativeProfile(profile));}
    void load(std::string_view selected,std::string_view embedded) {
        const auto root=parseNativeProfile(selected);
        try{loadRoot(root);return;}catch(const std::exception&) {
            const auto fallback=parseNativeProfile(embedded);
            for(const auto* field:{"sha256","text_sha256","size_of_image","text_rva","text_size"})
                if(root.at("target").at(field)!=fallback.at("target").at(field))throw;
            loadRoot(fallback);
        }
    }
    bool ready() const noexcept{return ready_;}
    std::optional<std::vector<PlayerRosterEntry>> snapshot(void* level) const {
        std::vector<PlayerRosterEntry> rows;
        if(!ready_||!capture(level,&rows))return std::nullopt;
        return rows;
    }
    bool face(void* level,PlayerUuid id,std::array<std::uint32_t,64>& output) const noexcept {
        if(!ready_||!skinReady_)return false;
        __try{return faceUnsafe(level,id,output);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
private:
    struct Hash {std::size_t operator()(PlayerUuid id) const noexcept{return static_cast<std::size_t>(id.low^id.high);}};
    // Release MSVC STL map ABI. Iteration uses the UUID key and value address
    // only: game entry fields and extent are described by the profile, never
    // by C++ members here. No find(), insert(), erase(), allocator or destructor
    // is invoked on the game's map. Value alignment must remain eight bytes.
    struct alignas(8) NativeEntry {std::byte storage[0xA8];};
    using Map=std::unordered_map<PlayerUuid,NativeEntry,Hash>;
    void loadRoot(const nlohmann::json& root) {
        ready_=skinReady_=false;getter_=nullptr;
        const auto& layout=root.at("native_player_identity");
        if(layout.at("schema")!=1||layout.at("pointer_size")!=8||layout.value("revision",0)<2||
           layout.at("map_abi")!="msvc_release_unordered_map_uuid_value_align8")
            throw std::runtime_error("unsupported PlayerList ABI/revision; refresh exact-build definition");
        const auto field=[&](const char* key){return static_cast<std::size_t>(native_profile::number(layout.at(key)));};
        levelOffset_=field("level_player_list_offset");entrySize_=field("entry_size");
        uniqueOffset_=field("actor_unique_id_offset");uuidOffset_=field("uuid_offset");nameOffset_=field("name_offset");
        limit_=field("maximum_entries");nameLimit_=field("maximum_name_bytes");
        if(!levelOffset_||levelOffset_>0x10000||entrySize_>0x10000||entrySize_<32||
           uniqueOffset_>entrySize_-8||uuidOffset_>entrySize_-16||nameOffset_>entrySize_-32||
           !limit_||limit_>4096||!nameLimit_||nameLimit_>4096)throw std::runtime_error("invalid PlayerList bounds");
        if(layout.contains("get_list"))getter_=native_profile::checked(root,layout.at("get_list"));
        if(layout.contains("skin")){
            const auto& skin=layout.at("skin");
            const auto n=[&](const char* key){return static_cast<std::size_t>(native_profile::number(skin.at(key)));};
            skinOffset_=n("entry_pointer_offset");imageOffset_=n("image_offset");
            widthOffset_=n("width_offset");heightOffset_=n("height_offset");dataOffset_=n("data_offset");sizeOffset_=n("size_offset");
            skinReady_=skinOffset_<=entrySize_-8&&imageOffset_<=0x10000&&widthOffset_<=256&&heightOffset_<=256&&dataOffset_<=256&&sizeOffset_<=256;
            if(!skinReady_)throw std::runtime_error("invalid PlayerList skin bounds");
        }
        ready_=true;
    }
    Map* list(void* level) const noexcept {
        if(!level)return nullptr;
        void* value{};
        if(getter_){__try{value=reinterpret_cast<void*(__fastcall*)(void*)>(getter_)(level);}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}}
        else if(!native_profile::read(level,levelOffset_,value))return nullptr;
        return static_cast<Map*>(value);
    }
    bool text(const void* source,std::string& output) const {
        std::array<std::uintptr_t,4> abi{};
        if(!native_profile::read(source,0,abi)||abi[2]>nameLimit_||abi[2]>abi[3])return false;
        const auto* bytes=abi[3]<16?static_cast<const char*>(source):reinterpret_cast<const char*>(abi[0]);
        if(!bytes)return false;
        output.resize(abi[2]);return copy(bytes,output.data(),output.size());
    }
    static bool copy(const void* source,void* output,std::size_t size) noexcept {
        __try{std::memcpy(output,source,size);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool capture(void* level,std::vector<PlayerRosterEntry>* rows) const noexcept {
        __try{return captureUnsafe(level,*rows);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool captureUnsafe(void* level,std::vector<PlayerRosterEntry>& rows) const {
        const auto* players=list(level);if(!players||players->size()>limit_)return false;
        rows.reserve(players->size());std::size_t visited{};
        for(const auto& item:*players){
            if(++visited>limit_)return false;
            PlayerRosterEntry row;row.uuid=item.first;
            const void* entry=&item.second;PlayerUuid valueId{};
            if(!native_profile::read(entry,uniqueOffset_,row.uniqueId)||!native_profile::read(entry,uuidOffset_,valueId)||
               valueId!=row.uuid||!text(static_cast<const std::byte*>(entry)+nameOffset_,row.name))continue;
            if(!row.name.empty())rows.emplace_back(std::move(row));
        }
        return true;
    }
    bool faceUnsafe(void* level,PlayerUuid id,std::array<std::uint32_t,64>& output) const {
        const auto* players=list(level);if(!players||players->size()>limit_)return false;
        const void* skin{};std::size_t visited{};
        for(const auto& entry:*players){if(++visited>limit_)return false;
            if(entry.first==id){if(!native_profile::read(&entry.second,skinOffset_,skin))return false;break;}}
        if(!skin)return false;
        const auto* image=static_cast<const std::byte*>(skin)+imageOffset_;
        std::uint32_t width{},height{};const std::uint8_t* data{};std::size_t size{};
        if(!native_profile::read(image,widthOffset_,width)||!native_profile::read(image,heightOffset_,height)||
           !native_profile::read(image,dataOffset_,data)||!native_profile::read(image,sizeOffset_,size)||!data||
           width<64||height<16||width>4096||height>4096||width%8||size<std::size_t(width)*height*4)return false;
        const auto unit=width/8;if(unit*6>width||unit*2>height)return false;
        std::array<std::uint32_t,64> pixels{};
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
            std::array<std::uint8_t,4> base{},over{};
            const auto sy=unit+y*unit/8,sx=x*unit/8;
            if(!copy(data+(std::size_t(sy)*width+unit+sx)*4,base.data(),4)||
               !copy(data+(std::size_t(sy)*width+unit*5+sx)*4,over.data(),4))return false;
            const auto blend=[&](unsigned c){return (unsigned(over[c])*over[3]+unsigned(base[c])*(255-over[3]))/255;};
            pixels[y*8+x]=(blend(0)<<24)|(blend(1)<<16)|(blend(2)<<8)|255;
        }
        output=pixels;return true;
    }
    void* getter_{};bool ready_{},skinReady_{};
    std::size_t levelOffset_{},entrySize_{},uniqueOffset_{},uuidOffset_{},nameOffset_{},limit_{},nameLimit_{};
    std::size_t skinOffset_{},imageOffset_{},widthOffset_{},heightOffset_{},dataOffset_{},sizeOffset_{};
};
} // namespace bedrock

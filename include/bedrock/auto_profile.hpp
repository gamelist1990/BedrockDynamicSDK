#pragma once
#include <nlohmann/json.hpp>
#include <bedrock/remote_catalog.hpp>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <cctype>
#include <optional>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace bedrock::auto_profile {
using Json=nlohmann::json;
struct Function {std::uint32_t begin{},end{};};
struct Section {std::uint32_t rva{};std::vector<std::byte> bytes;};
struct Image {
    GameIdentity identity;std::uint64_t pointerBase{};std::string provenance,version;
    Section text,rdata,data;std::vector<Function> functions;
    const Function* function(std::uint32_t rva) const {
        const auto i=std::upper_bound(functions.begin(),functions.end(),rva,[](auto value,const auto& f){return value<f.begin;});
        if(i==functions.begin())return nullptr;const auto& f=*std::prev(i);return rva<f.end?&f:nullptr;
    }
    const std::byte* read(std::uint32_t rva,std::size_t size) const {
        for(const auto* section:{&text,&rdata,&data})if(rva>=section->rva&&std::uint64_t(rva-section->rva)+size<=section->bytes.size())return section->bytes.data()+(rva-section->rva);
        return nullptr;
    }
};
struct Result {Json profile,report;};
// Decoder returns an instruction-aligned wildcard mask for one <=96-byte
// window. Relocations, branch destinations, immediates and displacements must
// be masked by the SDK decoder, never guessed by Mod code.
using Signature=std::function<std::string(std::span<const std::byte>)>;
using Facts=std::function<Json(std::span<const std::byte>)>;
using Progress=std::function<void(const Json&)>;
inline std::uint32_t number(const Json& value){const auto n=value.is_string()?std::stoull(value.get<std::string>(),nullptr,0):value.get<std::uint64_t>();if(n>UINT32_MAX)throw std::runtime_error("auto-dev integer overflow");return static_cast<std::uint32_t>(n);}
inline std::string hex(std::uint64_t value){std::ostringstream out;out<<"0x"<<std::hex<<value;return out.str();}
inline std::vector<int> pattern(std::string_view value){
    std::istringstream in{std::string(value)};std::string token;std::vector<int> result;
    while(in>>token){if(token=="?"||token=="??")result.push_back(-1);else{
        if(token.size()!=2||!std::isxdigit(static_cast<unsigned char>(token[0]))||!std::isxdigit(static_cast<unsigned char>(token[1])))throw std::runtime_error("invalid auto-dev anchor");
        result.push_back(static_cast<int>(std::stoul(token,nullptr,16)));}if(result.size()>128)throw std::runtime_error("auto-dev anchor exceeds 128 bytes");}
    return result;
}
inline std::vector<std::uint32_t> matches(const Image& image,const std::vector<int>& p,const std::atomic<bool>& stop,std::size_t cap=512){
    std::size_t at{},length{};
    for(std::size_t i=0;i<p.size();){if(p[i]<0){++i;continue;}auto end=i;while(end<p.size()&&p[end]>=0)++end;if(end-i>length){at=i;length=end-i;}i=end;}
    if(length<4||p.size()>image.text.bytes.size())return {};
    std::vector<std::uint32_t> result;const auto& data=image.text.bytes;
    // Search in bounded slices, with overlap for anchors crossing a slice.
    constexpr std::size_t slice=256*1024;
    std::vector<std::byte> fixed;for(std::size_t i=0;i<length;++i)fixed.push_back(static_cast<std::byte>(p[at+i]));
    for(std::size_t begin=0;begin<data.size();begin+=slice){
        if(stop.load())throw std::runtime_error("auto-dev cancelled");
        const auto last=std::min(data.size(),begin+slice+p.size());
        auto seek=data.begin()+begin;
        while(seek<data.begin()+last){
            const auto found=std::search(seek,data.begin()+last,fixed.begin(),fixed.end());if(found==data.begin()+last)break;
            const auto pos=static_cast<std::size_t>(found-data.begin());seek=found+1;
            if(pos<at)continue;const auto start=pos-at;
            if(start<begin||start>=begin+slice||start+p.size()>data.size())continue;
            bool equal=true;for(std::size_t i=0;i<p.size();++i)if(p[i]>=0&&std::to_integer<unsigned>(data[start+i])!=static_cast<unsigned>(p[i])){equal=false;break;}
            if(equal){result.push_back(image.text.rva+static_cast<std::uint32_t>(start));if(result.size()>cap)return result;}
        }
    }return result;
}
inline std::string bytes(const Image& image,std::uint32_t rva,std::size_t size){
    const auto* p=image.read(rva,size);if(!p)return {};const char* digits="0123456789ABCDEF";std::string result;
    for(std::size_t i=0;i<size;++i){if(i)result+=' ';const auto n=std::to_integer<unsigned>(p[i]);result+=digits[n>>4];result+=digits[n&15];}return result;
}
class Engine {
public:
    Result run(const Image& image,const std::vector<Json>& sources,const Json& catalog,const Signature& decoder,
               const std::atomic<bool>& stop,const Progress& progress={},const Facts& facts={}) const {
        Result result;result.profile={{"schema",1},{"target",{{"product","minecraft-bedrock"},{"kind","client"},{"platform","windows"},{"arch","x64"},
            {"version",image.version.empty()?"unknown":image.version},{"sha256",image.identity.fileSha256},{"text_sha256",image.identity.textSha256},
            {"image_base",hex(image.pointerBase)},{"size_of_image",image.identity.imageSize},{"text_rva",hex(image.text.rva)},{"text_size",image.text.bytes.size()}}},
            {"symbols",Json::object()}};
        result.report={{"schema",1},{"experimental",true},{"provenance",image.provenance},{"policy","unique independent anchors; exact-build guards; no guessed layouts; no automatic application"},
            {"functions",Json::object()},{"derived_fields",Json::array()},{"vtables",Json::array()},{"unresolved",Json::array()}};
        result.report["invariants"]={{{"value",8},{"meaning","pointer_size"},{"basis","validated AMD64 PE target"},{"scope","Windows x64 architecture; not a guessed game object layout"}}};
        result.report["sources"]=Json::array();
        for(const auto& source:sources)if(source.contains("target"))result.report["sources"].push_back({{"target",source.at("target")},{"symbols",source.value("symbols",Json::object()).size()}});
        const Json* exact{};
        for(const auto& source:sources){if(!source.contains("target")||!source.contains("symbols"))continue;
            const auto& t=source.at("target");if(t.value("sha256",std::string{})==image.identity.fileSha256&&number(t.at("size_of_image"))==image.identity.imageSize&&
                number(t.at("text_rva"))==image.text.rva&&number(t.at("text_size"))==image.text.bytes.size()) {exact=&source;break;}}
        // Exact fingerprint is existing build knowledge, not cross-build proof.
        if(exact){result.profile=*exact;result.report["mode"]="exact-build-revalidation";}
        else result.report["mode"]="cross-build-discovery";
        struct Definition {std::string name;Json value;std::string source;};std::vector<Definition> definitions;
        const auto append=[&](const Json& source){
            const auto version=source.value("target",Json::object()).value("version",std::string("unknown"));
            for(const auto& [name,value]:source.at("symbols").items())if(value.is_object()&&value.contains("rva"))definitions.push_back({name,value,version});
            const auto walk=[&](const auto& self,const Json& node,const std::string& path,unsigned depth)->void{
                if(depth>32||!node.is_object())return;
                if(node.contains("rva")&&node.contains("entry_check")){
                    // Canonical identifiers win over duplicated layout entries.
                    bool canonical=false;for(const auto& [name,value]:source.at("symbols").items())if(value.contains("rva")&&number(value.at("rva"))==number(node.at("rva"))){canonical=true;break;}
                    if(!canonical)definitions.push_back({path,node,version});return;
                }
                for(const auto& [key,value]:node.items())self(self,value,path+"."+key,depth+1);
            };
            if(source.contains("layouts"))walk(walk,source.at("layouts"),"layouts",0);
        };
        if(exact)append(*exact);else for(const auto& source:sources)if(source.contains("symbols"))append(source);
        if(definitions.size()>2048)throw std::runtime_error("auto-dev source exceeds 2048 function definitions");
        std::map<std::string,Json> accepted;std::set<std::string> conflicts;
        std::size_t done{};
        for(const auto& def:definitions){
            if(stop.load())throw std::runtime_error("auto-dev cancelled");
            if(progress)progress({{"phase","functions"},{"done",done++},{"total",definitions.size()},{"symbol",def.name}});
            Json evidence={{"source_version",def.source},{"status","unresolved"},{"reasons",Json::array()}};
            std::optional<std::uint32_t> chosen;const Function* function{};
            if(exact){
                const auto rva=number(def.value.at("rva"));const auto p=pattern(def.value.at("entry_check").get<std::string>());const auto* entry=image.read(rva,p.size());bool equal=entry&&!p.empty();
                for(std::size_t i=0;equal&&i<p.size();++i)equal=p[i]<0||std::to_integer<unsigned>(entry[i])==static_cast<unsigned>(p[i]);
                if(equal){chosen=rva;function=image.function(rva);evidence["method"]="exact-fingerprint-and-entry-guard";}
                else evidence["reasons"].push_back("exact-build entry is unreadable or patched/mismatched; not regenerated from a jump stub");
            }else{
                auto anchors=def.value.value("signature_anchors",Json::array());
                // A single legacy signature is diagnostic evidence, never enough
                // to assign a new function identity automatically.
                if(anchors.size()<2)evidence["reasons"].push_back("fewer than two independent source anchors; manual semantic/ABI analysis required");
                else {
                    std::map<std::uint32_t,std::vector<std::array<std::size_t,3>>> votes;
                    Json observations=Json::array();
                    for(const auto& anchor:anchors){
                        if(observations.size()>=16){evidence["reasons"].push_back("anchor limit16; remaining anchors not examined");break;}
                        const auto p=pattern(anchor.value("pattern",std::string{}));const auto fixed=std::count_if(p.begin(),p.end(),[](int v){return v>=0;});
                        if(p.size()<24||fixed<16)continue;
                        const auto hits=matches(image,p,stop);std::set<std::uint32_t> owners;
                        for(auto rva:hits)if(const auto* f=image.function(rva);f&&std::uint64_t(rva)+p.size()<=f->end)owners.insert(f->begin);
                        Json locations=Json::array();for(std::size_t i=0;i<std::min<std::size_t>(16,hits.size());++i)locations.push_back(hex(hits[i]));
                        observations.push_back({{"source_offset",anchor.value("offset",0u)},{"pattern",anchor.at("pattern")},{"fixed_bytes",fixed},{"hits",hits.size()},{"locations",locations},{"functions",owners.size()},{"capped",hits.size()>512}});
                        if(hits.size()!=1||owners.size()!=1)continue;
                        votes[*owners.begin()].push_back({anchor.value("offset",0u),p.size(),hits[0]});
                    }
                    evidence["anchors"]=observations;std::set<std::uint32_t> supported;
                    for(auto& [rva,ranges]:votes){std::sort(ranges.begin(),ranges.end());std::size_t count{},end{};std::vector<std::pair<std::size_t,std::size_t>> targetRanges;
                        for(auto [offset,size,target]:ranges)if(!count||offset>=end){
                            bool overlaps=false;for(auto [a,z]:targetRanges)if(target<z&&a<target+size)overlaps=true;
                            if(!overlaps){++count;end=offset+size;targetRanges.emplace_back(target,target+size);}}
                        if(count>=2)supported.insert(rva);}
                    if(supported.size()==1){chosen=*supported.begin();function=image.function(*chosen);evidence["method"]="independent-unique-nonoverlapping-anchor-consensus";}
                    else evidence["reasons"].push_back(supported.empty()?"no function has two independent unique anchor matches":"multiple functions satisfy independent anchor consensus; identity ambiguous");
                }
            }
            if(chosen){
                const auto* entry=image.read(*chosen,1);
                if(!entry||(!exact&&(entry[0]==std::byte{0xE9}||entry[0]==std::byte{0xEB}||entry[0]==std::byte{0xCC}))){evidence["reasons"].push_back("candidate entry is a jump/padding stub; requires explicit thunk analysis");chosen.reset();}
            }
            if(chosen){
                if(facts&&function&&function->begin==*chosen){
                    const auto count=std::min<std::size_t>(4096,function->end-function->begin);const auto* code=image.read(*chosen,count);
                    if(code){evidence["instruction_facts"]=facts({code,count});
                        Json references=Json::array(),calls=Json::array();
                        for(const auto& operand:evidence["instruction_facts"].value("operands",Json::array())){
                            const auto next=std::int64_t(*chosen)+operand.at("offset").get<std::int64_t>()+operand.at("length").get<std::int64_t>();
                            if(operand.value("rip_relative",false)){
                                const auto target=next+operand.at("displacement").get<std::int64_t>();
                                if(target>=0&&target<=UINT32_MAX){Json ref={{"instruction_rva",hex(*chosen+operand.at("offset").get<std::uint32_t>())},{"target_rva",hex(target)}};
                                    if(operand.at("opcode")=="0x8d"){
                                        std::string text;bool terminated=false;for(unsigned n=0;n<256;++n){const auto* byte=image.read(static_cast<std::uint32_t>(target)+n,1);if(!byte)break;const auto c=std::to_integer<unsigned>(*byte);if(!c){terminated=true;break;}if(c<32||c>126)break;text+=static_cast<char>(c);}
                                        if(terminated&&text.size()>=4)ref["ascii_string"]=text;
                                    }references.push_back(std::move(ref));
                                }
                            }
                            if(operand.value("relative",false)&&operand.at("opcode")=="0xe8"){
                                const auto target=next+static_cast<std::int32_t>(operand.at("immediate").get<std::uint32_t>());
                                if(target>=0&&target<=UINT32_MAX&&image.function(static_cast<std::uint32_t>(target)))calls.push_back(hex(target));
                            }
                        }
                        evidence["rip_references"]=references;evidence["direct_calls"]=calls;
                        if(!exact&&def.value.contains("auto_dev_constraints"))for(const auto& required:def.value.at("auto_dev_constraints").value("required_strings",Json::array())){
                            bool found=false;for(const auto& ref:references)if(ref.value("ascii_string",std::string{})==required.get<std::string>())found=true;
                            if(!found){evidence["reasons"].push_back("required source string context not observed in decoded prefix; manual analysis required");chosen.reset();}
                        }
                    }
                }
                if(!chosen){result.report["functions"][def.name]=std::move(evidence);continue;}
                // A named field is derived only from an identified getter with
                // an exact decoder shape. No general "this offset is stable" rule.
                if(facts&&def.name=="Level::getPlayerList"){
                    const auto* prefix=image.read(*chosen,8);if(prefix){const auto decoded=facts({prefix,8});
                        if(decoded.contains("pointer_getter")&&decoded.at("pointer_getter").at("displacement").get<std::uint32_t>()>0&&decoded.at("pointer_getter").at("displacement").get<std::uint32_t>()<=0x10000){
                            const auto offset=decoded.at("pointer_getter").at("displacement");
                            result.report["derived_fields"].push_back({{"path","/layouts/LocalPlayer/level_player_list_offset"},{"value",offset},
                                {"function",def.name},{"rva",hex(*chosen)},{"evidence",decoded.at("pointer_getter")},{"runtime_verified",false},{"immutable",false}});
                            if(exact&&result.profile.contains("layouts")&&result.profile["layouts"].contains("LocalPlayer"))
                                result.profile["layouts"]["LocalPlayer"]["level_player_list_offset"]=offset;
                            else result.profile["diagnostic_candidates"]["player_list_offset"]={{"value",offset},{"source",def.name},{"status","candidate; container layout still required"}};
                        }
                    }
                }
                Json generated=exact?def.value:Json{{"status","candidate"},{"abi_verified",false},{"runtime_verified",false},{"source_abi",def.value.value("abi",def.value.value("source_abi",std::string{}))}};
                generated["rva"]=hex(*chosen);generated["entry_check"]=bytes(image,*chosen,function&&function->begin==*chosen?std::min<std::size_t>(24,function->end-*chosen):8);
                generated["signature"]="";generated["signature_length"]=0;generated["cross_version_signature_available"]=false;
                generated["auto_dev_evidence"]=evidence;generated["source"]=exact?def.value.value("source",std::string("exact-build source")):"CheckSig auto-dev; candidate identity; ABI and behavior unverified";
                if(function&&function->begin==*chosen&&decoder){
                    Json fresh=Json::array();std::size_t lastEnd{};
                    for(std::size_t offset=0;offset<function->end-function->begin&&fresh.size()<3;){
                        const auto size=std::min<std::size_t>(96,function->end-function->begin-offset);if(size<32)break;
                        const auto* input=image.read(function->begin+static_cast<std::uint32_t>(offset),size);if(!input)break;
                        const auto signature=decoder({input,size});const auto p=pattern(signature);
                        if(p.size()>=32&&std::count_if(p.begin(),p.end(),[](int v){return v>=0;})>=24&&offset>=lastEnd){
                            const auto hits=matches(image,p,stop,1);
                            if(hits.size()==1&&hits[0]==function->begin+offset){fresh.push_back({{"offset",offset},{"pattern",signature},{"kind",offset?"body":"entry"},{"uniqueness","unique-in-target-snapshot"}});lastEnd=offset+p.size();}
                        }
                        // Decoder windows must begin at function entry or at the
                        // end of an already decoded window, never arbitrary bytes.
                        if(p.empty()||!facts||facts({input,p.size()}).value("terminal",false))break;offset+=p.size();if(offset>512)break;
                    }
                    if(!fresh.empty()){generated["signature_anchors"]=fresh;generated["signature"]=fresh[0]["pattern"];generated["signature_length"]=pattern(fresh[0]["pattern"].get<std::string>()).size();generated["cross_version_signature_available"]=true;}
                }
                if(accepted.contains(def.name)&&number(accepted.at(def.name).at("rva"))!=*chosen){conflicts.insert(def.name);evidence["status"]="conflict";evidence["reasons"].push_back("reference versions disagree on the target function");}
                else{accepted[def.name]=std::move(generated);evidence["status"]=exact?"exact-build-reused":"candidate-function-found";evidence["rva"]=hex(*chosen);}
            }
            evidence["abi_review_required"]=!exact||!def.value.value("abi_verified",false);
            evidence["runtime_review_required"]=!exact||!def.value.value("runtime_verified",false);
            auto& recorded=result.report["functions"][def.name];
            if(recorded.is_object()&&recorded.contains("status")&&!chosen&&accepted.contains(def.name))recorded["other_source_attempts"].push_back(std::move(evidence));
            else recorded=std::move(evidence);
        }
        for(const auto& name:conflicts){accepted.erase(name);result.report["functions"][name]["status"]="conflict";}
        // Distinct source identities converging on one target need manual review.
        if(!exact){std::map<std::uint32_t,std::vector<std::string>> owners;for(const auto& [name,value]:accepted)owners[number(value.at("rva"))].push_back(name);
            for(const auto& [rva,names]:owners)if(names.size()>1)for(const auto& name:names){accepted.erase(name);result.report["functions"][name]["status"]="target-collision";result.report["functions"][name]["reasons"].push_back("different identifiers converged on one target; alias/inlining must be analysed");}}
        for(auto& [name,value]:accepted)result.profile["symbols"][name]=std::move(value);
        if(exact){
            // Preserve only definitions whose executable entry was rechecked.
            for(auto it=result.profile["symbols"].begin();it!=result.profile["symbols"].end();)if(!accepted.contains(it.key()))it=result.profile["symbols"].erase(it);else ++it;
        }
        if(exact&&result.profile.contains("layouts")){
            const auto prune=[&](const auto& self,Json& node,const std::string& path)->void{
                if(!node.is_object())return;
                for(auto it=node.begin();it!=node.end();){bool remove=false;
                    if(it.value().is_object()&&it.value().contains("symbol")&&!it.value().contains("rva"))remove=!result.profile["symbols"].contains(it.value().at("symbol").get<std::string>());
                    else if(it.value().is_object()&&it.value().contains("rva")&&it.value().contains("entry_check")){
                        const auto p=pattern(it.value().at("entry_check").get<std::string>());const auto* data=image.read(number(it.value().at("rva")),p.size());remove=!data||p.empty();
                        for(std::size_t n=0;!remove&&n<p.size();++n)remove=p[n]>=0&&std::to_integer<unsigned>(data[n])!=static_cast<unsigned>(p[n]);
                    }
                    if(remove){result.report["unresolved"].push_back({{"path",path+"/"+it.key()},{"reason","exact-build function guard failed or referenced function unresolved"}});it=node.erase(it);}
                    else{self(self,it.value(),path+"/"+it.key());++it;}
                }
            };prune(prune,result.profile["layouts"],"/layouts");
        }
        discoverVtables(image,result,stop,progress);
        auditRequirements(image,catalog,result,exact!=nullptr);
        result.profile["generation"]={{"generator","CheckSig auto-dev"},{"experimental",true},{"auto_apply",false},{"candidate_only",!exact},
            {"requires_review",true},{"report","report.json"},{"provenance",image.provenance}};
        result.report["resolved_functions"]=result.profile["symbols"].size();
        result.report["status"]=result.report["unresolved"].empty()?"existing-definition-revalidated-runtime-review-required":"partial-manual-analysis-required";
        result.report["limitations"]={"Function identity confidence is not an ABI or full Mod verification.","No offset is assumed immutable across game builds.","Cross-build native fields and weak/ambiguous functions are omitted.","No hooks, calls into candidates, game memory writes or automatic profile activation are performed."};
        return result;
    }
private:
    static void discoverVtables(const Image& image,Result& result,const std::atomic<bool>& stop,const Progress& progress){
        std::map<std::uint64_t,std::string> known;for(const auto& [name,value]:result.profile["symbols"].items())known[image.pointerBase+number(value.at("rva"))]=name;
        const auto word=[&](std::uint32_t rva,std::uint64_t& v){const auto* p=image.read(rva,8);if(!p)return false;std::memcpy(&v,p,8);return true;};
        const auto dword=[&](std::uint32_t rva,std::uint32_t& v){const auto* p=image.read(rva,4);if(!p)return false;std::memcpy(&v,p,4);return true;};
        std::map<std::string,std::set<unsigned>> slots;std::set<std::uint32_t> emitted;
        for(std::size_t offset=8;offset+8<=image.rdata.bytes.size();offset+=8){
            if((offset&0xffff)==0){if(stop.load())throw std::runtime_error("auto-dev cancelled");if(progress)progress({{"phase","vtables"},{"bytes",offset},{"total",image.rdata.bytes.size()}});}
            std::uint64_t pointer{};const auto rva=image.rdata.rva+static_cast<std::uint32_t>(offset);if(!word(rva,pointer)||!known.contains(pointer))continue;
            // A callable std::function table is not automatically a class vtable.
            auto begin=rva;for(unsigned back=0;back<512&&begin>=image.rdata.rva+8;++back){std::uint64_t previous{};if(!word(begin-8,previous)||previous<image.pointerBase+image.text.rva||previous>=image.pointerBase+image.text.rva+image.text.bytes.size())break;begin-=8;}
            if(begin<image.rdata.rva+8||emitted.contains(begin))continue;
            std::uint64_t col{};if(!word(begin-8,col)||col<image.pointerBase||col-image.pointerBase>UINT32_MAX)continue;
            const auto colRva=static_cast<std::uint32_t>(col-image.pointerBase);std::uint32_t signature{},type{},self{};
            if(!dword(colRva,signature)||signature!=1||!dword(colRva+12,type)||!dword(colRva+20,self)||self!=colRva||type>image.identity.imageSize-17)continue;
            std::string name;bool terminated=false;for(unsigned i=0;i<1024;++i){const auto* c=image.read(type+16+i,1);if(!c)break;const auto value=std::to_integer<unsigned>(*c);if(!value){terminated=true;break;}if(value<32||value>126)break;name+=static_cast<char>(*c);}
            if(!terminated)continue;
            if(!name.starts_with(".?AV")&&!name.starts_with(".?AU"))continue;
            Json methods=Json::array();for(unsigned slot=0;slot<512;++slot){std::uint64_t target{};if(!word(begin+slot*8,target)||target<image.pointerBase+image.text.rva||target>=image.pointerBase+image.text.rva+image.text.bytes.size())break;
                if(known.contains(target)){methods.push_back({{"slot",slot},{"symbol",known.at(target)},{"rva",hex(target-image.pointerBase)}});slots[known.at(target)].insert(slot);}}
            emitted.insert(begin);result.report["vtables"].push_back({{"rva",hex(begin)},{"rtti_name",name},{"methods",methods},{"status","RTTI-and-code-pointer-observation; object ownership and ABI unverified"}});
            if(emitted.size()>=2048){result.report["unresolved"].push_back({{"path","/virtual_methods"},{"reason","vtable limit2048 reached; enumeration incomplete"}});break;}
        }
        // Slot evidence is emitted only when all observed tables agree.
        for(const auto& [name,values]:slots)if(values.size()==1&&!result.profile.value("virtual_methods",Json::object()).contains(name))
            result.profile["virtual_methods"][name]={{"slot",*values.begin()},{"status","candidate"},{"runtime_verified",false},{"source","auto-dev target RTTI tables agree; ownership/ABI need review"}};
    }
    static void auditRequirements(const Image& image,const Json& catalog,Result& result,bool exact){
        for(const auto& req:catalog.value("requirements",Json::array())){
            const auto path=req.value("path",std::string{});const auto symbol=req.value("symbol",std::string{});bool found=false;
            try{const auto pointer=Json::json_pointer(path);found=exact&&!path.empty()&&result.profile.contains(pointer);
                if(req.value("kind",std::string{})=="function"){
                    const Json* definition=nullptr;
                    if(!symbol.empty()&&result.profile["symbols"].contains(symbol))definition=&result.profile["symbols"].at(symbol);
                    else if(found)definition=&result.profile.at(pointer);
                    if(definition&&definition->contains("symbol")&&!definition->contains("rva"))definition=&result.profile["symbols"].at(definition->at("symbol").get<std::string>());
                    found=definition&&definition->contains("rva")&&definition->contains("entry_check");
                    if(found&&!definition->value("abi_verified",false))result.report["unresolved"].push_back({{"path",path},{"symbol",symbol},{"kind","native-ABI-review"},{"reason","function identity/guard is available but calling convention, argument types and hidden return are not independently verified"}});
                    if(found){const auto p=pattern(definition->at("entry_check").get<std::string>());const auto* entry=image.read(number(definition->at("rva")),p.size());found=entry&&!p.empty();
                        for(std::size_t i=0;found&&i<p.size();++i)found=p[i]<0||std::to_integer<unsigned>(entry[i])==static_cast<unsigned>(p[i]);}
                }
            }catch(...){found=false;}
            if(found&&req.value("kind",std::string{})!="function")result.report["unresolved"].push_back({{"path",path},{"kind",req.value("kind",std::string{})},{"reason","same-executable definition reused; field meaning, runtime value and lifetime are not independently re-verified by auto-dev"}});
            if(!found)result.report["unresolved"].push_back({{"path",path},{"symbol",symbol},{"kind",req.value("kind",std::string{})},
                {"reason",req.value("kind",std::string{})=="function"?"identity/entry evidence missing or ambiguous; inspect calls, RTTI and ABI with REA/Ghidra":"native object offset, field meaning, lifetime or gate has no cross-build proof; old values are not copied"}});
        }
        if(!exact&&image.version.empty())result.report["unresolved"].push_back({{"path","/target/version"},{"reason","new game version label is unknown; set auto-dev-version explicitly; executable identity is SHA256, not this label"}});
        if(!exact)result.report["unresolved"].push_back({{"path","/layouts"},{"reason","new-build object layouts require instruction/data-flow and lifetime evidence; stable names and matching code anchors alone cannot prove them"}});
    }
};
#ifdef _WIN32
Image captureImage(const GameIdentity& identity,const std::atomic<bool>& stop);
Json inspectLive(const Json& profile);
#endif
} // namespace bedrock::auto_profile

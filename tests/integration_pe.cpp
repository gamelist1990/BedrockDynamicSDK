#include "bedrock/profiles/resolver.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

static std::uint16_t le16(const std::vector<std::byte>& v, std::size_t p) {
    if (p+2>v.size()) throw std::runtime_error("PE truncated");
    return static_cast<std::uint16_t>(v[p]) | (static_cast<std::uint16_t>(v[p+1]) << 8);
}
static std::uint32_t le32(const std::vector<std::byte>& v, std::size_t p) {
    if (p+4>v.size()) throw std::runtime_error("PE truncated");
    return static_cast<std::uint32_t>(le16(v,p)) | (static_cast<std::uint32_t>(le16(v,p+2)) << 16);
}
int main(int argc, char** argv) {
    if (argc < 4 || argc > 5) {
        std::cerr<<"usage: integration_pe <valid-PE> <generated-db.json> <verified-PE-SHA256> [--allow-candidates]\n";
        return 2;
    }
    try {
        std::ifstream pefile(argv[1],std::ios::binary);
        std::ifstream jsonfile(argv[2],std::ios::binary);
        if (!pefile || !jsonfile) throw std::runtime_error("missing input");
        std::vector<char> bytes(std::istreambuf_iterator<char>{pefile}, {});
        std::vector<std::byte> raw(bytes.size());
        std::memcpy(raw.data(), bytes.data(), bytes.size());
        const std::size_t pe=le32(raw,0x3c);
        if (le16(raw,0) != 0x5a4d || le32(raw,pe)!=0x00004550 ||
            le16(raw,pe+4)!=0x8664) throw std::runtime_error("not AMD64 PE");
        const std::uint16_t count=le16(raw,pe+6), optionalLen=le16(raw,pe+20);
        const std::size_t optional=pe+24;
        const std::uint32_t mappedSize=le32(raw,optional+56);
        if (!mappedSize || mappedSize > (512U<<20)) throw std::runtime_error("image size unsafe");
        std::vector<std::byte> image(mappedSize);
        std::uint32_t textRva=0, textSize=0;
        const auto imageBase = std::uint64_t{le32(raw, optional+24)} |
                               (std::uint64_t{le32(raw, optional+28)} << 32);
        std::size_t sections=optional+optionalLen;
        for (int i=0; i<count; ++i) {
            std::size_t sh=sections+40*i;
            if (sh+40 > raw.size()) throw std::runtime_error("bad section headers");
            const auto sectionRva=le32(raw,sh+12), sectionSize=le32(raw,sh+8);
            const auto fileSize=le32(raw,sh+16), filePtr=le32(raw,sh+20);
            if (sectionRva >= image.size() || sectionSize > image.size()-sectionRva ||
                filePtr > raw.size() || fileSize > raw.size()-filePtr)
                throw std::runtime_error("bad section mapping");
            std::memcpy(image.data()+sectionRva,raw.data()+filePtr,
                        std::min<std::size_t>(fileSize,sectionSize));
            if (std::memcmp(raw.data()+sh, ".text", 5)==0) {
                textRva=le32(raw,sh+12);
                textSize=le32(raw,sh+8);
            }
        }
        if (!textSize) throw std::runtime_error("no .text");
        std::string body(std::istreambuf_iterator<char>{jsonfile}, {});
        auto j=nlohmann::json::parse(body);
        bedrock::Resolver resolver;
        std::string err;
        if (!resolver.loadJson(body, err)) throw std::runtime_error(err);
        if (!resolver.attachImage(image.data(),image.size(),argv[3],textRva,textSize,err))
            throw std::runtime_error(err);
        const bool candidates = argc == 5 && std::string(argv[4]) == "--allow-candidates";
        int checked=0;
        for (auto it=j["symbols"].begin();it!=j["symbols"].end();++it) {
            if (!candidates && it.value().value("status","candidate") != "verified")
                continue;
            auto resolved=resolver.resolve(it.key(), candidates);
            if (!resolved || resolved->method!="exact-rva" || !resolved->exact_build)
                throw std::runtime_error("failed exact resolution: "+it.key());
            ++checked;
        }
        if (!checked) throw std::runtime_error("no matching symbols in DB");
        // Both native GameMode classes are valid. Verify the adapter accepts
        // the actual function in every supported class's virtual slot, so a
        // base-only binding cannot silently reject the other class at runtime.
        if (candidates && j.contains("layouts") && j["layouts"].contains("BlockActions")) {
            const auto& actions=j["layouts"]["BlockActions"];
            for (const auto& table : j["layouts"]["BlockSource"]["game_mode_vtables"]) {
                const auto rva=std::stoull(table.get<std::string>(),nullptr,0);
                for (const char* name : {"start_destroy","continue_destroy","stop_destroy",
                                        "use_item","release_item","attack_actor"}) {
                    const auto& action=actions.at(name);
                    const auto at=rva+action.at("slot").get<unsigned>()*8;
                    const auto function=std::uint64_t{le32(image,at)} |
                                        (std::uint64_t{le32(image,at+4)} << 32);
                    const auto matches=[&](const nlohmann::json& ref) {
                        const auto& spec=j.at("symbols").at(ref.at("symbol").get<std::string>());
                        return function==imageBase+std::stoull(spec.at("rva").get<std::string>(),nullptr,0);
                    };
                    bool supported=matches(action.at("function"));
                    for (const auto& alt : action.value("alternate_functions",nlohmann::json::array()))
                        supported=supported||matches(alt);
                    if (!supported) throw std::runtime_error(std::string("BlockActions rejects supported GameMode: ")+name);
                }
            }
            std::cout<<"PASS: both GameMode vtables covered by BlockActions bindings\n";
        }
        std::cout<<"PASS: "<<checked<<" exact-hash RVAs resolved in mapped PE image"
                 <<(candidates?" (candidates are NOT ABI verified)":"")<<"\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr<<ex.what()<<"\n";
        return 1;
    }
}

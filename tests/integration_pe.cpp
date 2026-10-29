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
        std::size_t sections=optional+optionalLen;
        for (int i=0; i<count; ++i) {
            std::size_t sh=sections+40*i;
            if (sh+40 > raw.size()) throw std::runtime_error("bad section headers");
            if (std::memcmp(raw.data()+sh, ".text", 5)==0) {
                textRva=le32(raw,sh+12);
                textSize=le32(raw,sh+8);
                auto fileSize=le32(raw,sh+16), filePtr=le32(raw,sh+20);
                if (textRva >= image.size() || textSize > image.size()-textRva ||
                    filePtr > raw.size() || fileSize > raw.size()-filePtr)
                    throw std::runtime_error("bad .text mapping");
                std::memcpy(image.data()+textRva,raw.data()+filePtr,
                            std::min<std::size_t>(fileSize,textSize));
                break;
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
        std::cout<<"PASS: "<<checked<<" exact-hash RVAs resolved in mapped PE image"
                 <<(candidates?" (candidates are NOT ABI verified)":"")<<"\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr<<ex.what()<<"\n";
        return 1;
    }
}

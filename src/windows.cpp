#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include "bedrock/resolver.hpp"
#include "bedrock/remote_catalog.hpp"
#include <bedrock/auto_profile.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace bedrock {
namespace {
bool hashFile(const std::filesystem::path& path, std::string& out) {
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE hash{};
    std::array<unsigned char, 32> digest{};
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) goto done;
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) goto done;
        std::vector<char> buf(1 << 20);
        while (file) {
            file.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            if (file.gcount() > 0 &&
                BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()),
                               static_cast<ULONG>(file.gcount()), 0) < 0)
                goto done;
        }
        if (!file.eof()) goto done;
    }
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) goto done;
    {
        std::ostringstream s;
        for (auto byte : digest) s << std::hex << std::setfill('0') << std::setw(2) << +byte;
        out = s.str();
    }
    ok = true;
done:
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
bool hashMemory(const std::byte* memory, std::size_t size, std::string& out) {
    if (!memory || !size) return false;
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE hash{};
    std::array<unsigned char, 32> digest{};
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) goto done;
    {
        auto* ptr = memory;
        std::size_t remaining = size;
        while (remaining) {
            const ULONG count = static_cast<ULONG>(std::min<std::size_t>(remaining, 1 << 20));
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(
                const_cast<std::byte*>(ptr)), count, 0) < 0) goto done;
            ptr += count;
            remaining -= count;
        }
    }
    if (BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0) < 0) goto done;
    {
        std::ostringstream s;
        for (auto byte : digest) s << std::hex << std::setfill('0') << std::setw(2) << +byte;
        out = s.str();
    }
    ok = true;
done:
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
} // namespace

bool inspectLoadedGame(GameIdentity& identity, std::string& error) {
    identity = {};
    const HMODULE handle = GetModuleHandleW(nullptr);
    if (!handle) { error = "GetModuleHandleW failed"; return false; }
    const auto* base = reinterpret_cast<const std::byte*>(handle);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 0x100000) {
        error = "no DOS header"; return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = "main module is not AMD64 PE"; return false;
    }
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i=0; i<nt->FileHeader.NumberOfSections; ++i) {
        if (std::string(reinterpret_cast<const char*>(section[i].Name), 5) == ".text") {
            identity.imageSize = nt->OptionalHeader.SizeOfImage;
            identity.textRva = section[i].VirtualAddress;
            identity.textSize = section[i].SizeOfRawData;
            break;
        }
    }
    if (!identity.textSize || identity.textRva >= identity.imageSize ||
        identity.textSize > identity.imageSize - identity.textRva) {
        error = "invalid mapped .text"; return false;
    }
    if (!hashMemory(base + identity.textRva, identity.textSize, identity.textSha256)) {
        error = "failed to SHA-256 game code"; return false;
    }
    std::array<wchar_t, 32768> filename{};
    const auto length = GetModuleFileNameW(handle,filename.data(),
        static_cast<DWORD>(filename.size()));
    if (length && length < filename.size())
        hashFile(std::filesystem::path(filename.data()), identity.fileSha256);
    error.clear();
    return true;
}

bool Resolver::attachWindowsMainModule(std::string& error) {
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) { error = "GetModuleHandleW failed"; return false; }
    auto* base = reinterpret_cast<const std::byte*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) {
        error = "invalid DOS header"; return false;
    }
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = "not a Windows x64 PE"; return false;
    }
    auto* sec = IMAGE_FIRST_SECTION(nt);
    std::uint32_t textRva = 0, textSize = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::string(reinterpret_cast<const char*>(sec[i].Name), 5) == ".text") {
            textRva = sec[i].VirtualAddress;
            textSize = sec[i].SizeOfRawData; // data generator hashes raw .text including alignment padding
            break;
        }
    }
    if (!textSize) { error = "missing .text section"; return false; }
    std::array<wchar_t, 32768> path{};
    DWORD len = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!len || len >= path.size()) { error = "GetModuleFileNameW failed"; return false; }
    std::string digest;
    if (hashFile(std::filesystem::path(path.data()), digest)) {
        if (attachImage(base, nt->OptionalHeader.SizeOfImage,
                        digest, textRva, textSize, error) && exactBuild()) return true;
    }
    // GDK encrypted disk images: require SHA-256 equality on the mapped
    // full raw .text section, never bypass a hash mismatch by signature alone.
    std::string textDigest;
    if (!hashMemory(base + textRva, textSize, textDigest)) {
        error = "failed to hash mapped .text"; return false;
    }
    return attachImageByTextHash(base, nt->OptionalHeader.SizeOfImage,
                                 textDigest, textRva, textSize, error);
}

bool Resolver::attachWindowsMainModule(const GameIdentity& identity,
                                       std::string& error) {
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) { error = "GetModuleHandleW failed"; return false; }
    auto* base = reinterpret_cast<const std::byte*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) {
        error = "invalid DOS header"; return false;
    }
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = "not a Windows x64 PE"; return false;
    }
    std::uint32_t textRva = 0, textSize = 0;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::string(reinterpret_cast<const char*>(sections[i].Name), 5) == ".text") {
            textRva = sections[i].VirtualAddress;
            textSize = sections[i].SizeOfRawData;
            break;
        }
    }
    if (identity.textSha256.size() != 64 || !identity.imageSize ||
        !identity.textSize || nt->OptionalHeader.SizeOfImage != identity.imageSize ||
        textRva != identity.textRva || textSize != identity.textSize) {
        error = "loaded module changed since profile selection";
        return false;
    }
    return attachImageByTextHash(base, identity.imageSize, identity.textSha256,
                                 identity.textRva, identity.textSize, error);
}

namespace auto_profile {
namespace {
bool copyLive(const void* source,void* output,std::size_t count) noexcept {
    __try{std::memcpy(output,source,count);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
Image captureImage(const GameIdentity& identity,const std::atomic<bool>& stop) {
    Image result;result.identity=identity;
    const auto module=GetModuleHandleW(nullptr);const auto loaded=reinterpret_cast<std::uintptr_t>(module);
    std::array<wchar_t,32768> path{};const auto pathLength=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if(!pathLength||pathLength>=path.size())throw std::runtime_error("auto-dev cannot locate loaded EXE");
    const std::filesystem::path filePath(path.data());std::error_code ec;
    const auto fileSize=std::filesystem::file_size(filePath,ec);
    std::ifstream file;std::string digest;
    bool disk=!ec&&fileSize<=1024ull*1024*1024&&hashFile(filePath,digest)&&digest==identity.fileSha256;
    if(disk)file.open(filePath,std::ios::binary);
    disk=disk&&file.good();
    const auto read=[&](bool fromDisk,std::uint64_t offset,void* output,std::size_t count){
        if(stop.load())throw std::runtime_error("auto-dev cancelled");
        if(fromDisk){if(offset>fileSize||count>fileSize-offset)return false;
            file.clear();file.seekg(static_cast<std::streamoff>(offset));file.read(static_cast<char*>(output),static_cast<std::streamsize>(count));return file.good();}
        return offset<=identity.imageSize&&count<=identity.imageSize-offset&&copyLive(reinterpret_cast<const void*>(loaded+offset),output,count);
    };
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    const auto headers=[&](bool fromDisk){
        return read(fromDisk,0,&dos,sizeof(dos))&&dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_lfanew>0&&dos.e_lfanew<0x100000&&
            read(fromDisk,static_cast<unsigned>(dos.e_lfanew),&nt,sizeof(nt))&&nt.Signature==IMAGE_NT_SIGNATURE&&
            nt.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64&&nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC&&
            nt.OptionalHeader.SizeOfImage==identity.imageSize&&nt.FileHeader.NumberOfSections>0&&nt.FileHeader.NumberOfSections<=96;
    };
    if(disk&&!headers(true))disk=false;
    if(!disk&&!headers(false))throw std::runtime_error("auto-dev cannot validate loaded PE geometry");
    const auto table=std::uint64_t(dos.e_lfanew)+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;
    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    if(!read(disk,table,sections.data(),sections.size()*sizeof(IMAGE_SECTION_HEADER)))throw std::runtime_error("auto-dev section table unreadable");
    Section pdata;
    for(const auto& section:sections){
        char name[9]{};std::memcpy(name,section.Name,8);Section* destination{};
        if(std::string_view(name)==".text")destination=&result.text;
        else if(std::string_view(name)==".rdata")destination=&result.rdata;
        else if(std::string_view(name)==".data")destination=&result.data;
        else if(std::string_view(name)==".pdata")destination=&pdata;
        if(!destination)continue;
        const auto size=section.SizeOfRawData;
        const auto limit=destination==&result.text?512u*1024*1024:128u*1024*1024;
        if(section.VirtualAddress>=identity.imageSize||size>identity.imageSize-section.VirtualAddress||size>limit)throw std::runtime_error("auto-dev section exceeds bounds");
        destination->rva=section.VirtualAddress;destination->bytes.resize(size);
        for(std::size_t offset=0;offset<size;offset+=1024*1024){
            const auto count=std::min<std::size_t>(1024*1024,size-offset);
            const auto source=std::uint64_t(disk?section.PointerToRawData:section.VirtualAddress)+offset;
            if(!read(disk,source,destination->bytes.data()+offset,count))throw std::runtime_error("auto-dev section snapshot failed; retry after input/executable stabilizes");
        }
    }
    if(result.text.rva!=identity.textRva||result.text.bytes.size()!=identity.textSize||pdata.bytes.empty())throw std::runtime_error("auto-dev section metadata differs from loaded module");
    if(disk&&(!hashFile(filePath,digest)||digest!=identity.fileSha256))throw std::runtime_error("EXE changed during auto-dev snapshot");
    if(!hashMemory(result.text.bytes.data(),result.text.bytes.size(),result.identity.textSha256))throw std::runtime_error("auto-dev text hashing failed");
    result.pointerBase=disk?nt.OptionalHeader.ImageBase:loaded;
    result.provenance=disk?"loaded EXE path; file SHA256 checked before and after snapshot; clean file code":"guarded live module snapshot; disk code inaccessible/encrypted; patched entries are not accepted as new guards";
    if(pdata.bytes.size()/12>1000000)throw std::runtime_error("auto-dev pdata exceeds function limit");
    for(std::size_t i=0;i+12<=pdata.bytes.size();i+=12){
        std::uint32_t begin{},end{};std::memcpy(&begin,pdata.bytes.data()+i,4);std::memcpy(&end,pdata.bytes.data()+i+4,4);
        if(begin>=result.text.rva&&end>begin&&std::uint64_t(end)<=std::uint64_t(result.text.rva)+result.text.bytes.size())result.functions.push_back({begin,end});
    }
    std::sort(result.functions.begin(),result.functions.end(),[](const auto& a,const auto& b){return a.begin<b.begin;});
    if(result.functions.empty())throw std::runtime_error("auto-dev has no validated pdata functions");
    return result;
}

Json inspectLive(const Json& profile){
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));const auto size=number(profile.at("target").at("size_of_image"));
    Json report={{"scope","read-only live entry guards; patches are observations, not newly inferred functions"},{"tick_ms",GetTickCount64()},
        {"matched",0},{"changed",0},{"unreadable",0},{"differences",Json::array()}};
    std::size_t processed{};
    for(const auto& [name,definition]:profile.at("symbols").items()){
        if(++processed>2048)break;
        const auto rva=number(definition.at("rva"));const auto p=pattern(definition.at("entry_check").get<std::string>());
        std::vector<std::byte> data(p.size());
        if(p.empty()||rva>=size||p.size()>size-rva||!copyLive(reinterpret_cast<const void*>(base+rva),data.data(),data.size())){report["unreadable"]=report["unreadable"].get<unsigned>()+1;continue;}
        bool equal=true;for(std::size_t i=0;i<p.size();++i)if(p[i]>=0&&std::to_integer<unsigned>(data[i])!=static_cast<unsigned>(p[i]))equal=false;
        const char* key=equal?"matched":"changed";report[key]=report[key].get<unsigned>()+1;
        if(!equal&&report["differences"].size()<64)report["differences"].push_back({{"symbol",name},{"rva",hex(rva)},
            {"jump_stub",data[0]==std::byte{0xE9}||data[0]==std::byte{0xEB}},{"reason","entry differs; an SDK/third-party hook may be installed; no automatic rebinding"}});
    }
    return report;
}
} // namespace auto_profile
} // namespace bedrock
#endif

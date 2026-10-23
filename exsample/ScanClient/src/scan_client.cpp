#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winver.h>
#include <bcrypt.h>
#include <bedrock/remote_catalog.hpp>
#include <nlohmann/json.hpp>
#include "ghidra_import_script.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
constexpr SIZE_T kChunk = 64 * 1024;
constexpr size_t kMaxFindings = 1500;
std::atomic<bool> g_pause{false}, g_stop{false};
std::atomic<uint64_t> g_done{0}, g_total{0}, g_failures{0}, g_regionsDone{0};
std::mutex g_console;
fs::path g_out;
std::string g_version="unknown";
bedrock::GameIdentity g_identity;
uintptr_t g_moduleBase=0;
uintptr_t g_preferredBase=0;
uint32_t g_timestamp=0;

bool safeRead(uintptr_t address,void* output,SIZE_T size) noexcept {
    if(!address||!output||!size||address>UINTPTR_MAX-size) return false;
    SIZE_T received=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),output,size,&received)&&received==size;
}
std::string digestHex(const UCHAR* bytes,size_t size) {
    std::ostringstream out; for(size_t i=0;i<size;++i) out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(bytes[i]); return out.str();
}
bool hashMappedRange(uintptr_t address,uint64_t size,std::string& result) {
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; DWORD objectSize=0,returned=0; bool ok=false;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return false;
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&returned,0)<0) goto done;
    {
        std::vector<UCHAR> object(objectSize); std::array<UCHAR,32> digest{}; std::vector<UCHAR> chunk(1024*1024);
        if(BCryptCreateHash(algorithm,&hash,object.data(),objectSize,nullptr,0,0)<0) goto done;
        for(uint64_t offset=0;offset<size;) {
            const SIZE_T amount=static_cast<SIZE_T>(std::min<uint64_t>(chunk.size(),size-offset));
            if(address>UINTPTR_MAX-offset||!safeRead(address+static_cast<uintptr_t>(offset),chunk.data(),amount)||
               BCryptHashData(hash,chunk.data(),static_cast<ULONG>(amount),0)<0) goto done;
            offset+=amount;
        }
        if(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0) goto done;
        result=digestHex(digest.data(),digest.size()); ok=true;
    }
done:
    if(hash) BCryptDestroyHash(hash); if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0); return ok;
}
bool hashFilePath(const wchar_t* path,std::string& result) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; DWORD objectSize=0,returned=0; bool ok=false;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) { CloseHandle(file); return false; }
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&returned,0)<0) goto done;
    {
        std::vector<UCHAR> object(objectSize); std::array<UCHAR,32> digest{}; std::vector<UCHAR> chunk(1024*1024);
        if(BCryptCreateHash(algorithm,&hash,object.data(),objectSize,nullptr,0,0)<0) goto done;
        for(;;) {
            DWORD received=0; if(!ReadFile(file,chunk.data(),static_cast<DWORD>(chunk.size()),&received,nullptr)) goto done;
            if(!received) break;
            if(BCryptHashData(hash,chunk.data(),received,0)<0) goto done;
        }
        if(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0) goto done;
        result=digestHex(digest.data(),digest.size()); ok=true;
    }
done:
    if(hash) BCryptDestroyHash(hash); if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0); CloseHandle(file); return ok;
}

void log(const std::string& s) { std::lock_guard lock(g_console); std::printf("%s\n",s.c_str()); std::fflush(stdout); }
std::string hex(uint64_t v) { std::ostringstream o; o<<"0x"<<std::hex<<std::uppercase<<v; return o.str(); }
void atomicWrite(const fs::path& p,const std::string& data) {
    std::error_code ec; fs::create_directories(p.parent_path(),ec);
    auto tmp=p; tmp += L".part";
    { std::ofstream f(tmp,std::ios::binary|std::ios::trunc); if(!f) return; f.write(data.data(),static_cast<std::streamsize>(data.size())); f.flush(); if(!f) return; }
    fs::rename(tmp,p,ec); if(ec) { fs::remove(p,ec); ec.clear(); fs::rename(tmp,p,ec); }
}
std::string detectVersion() {
    std::array<wchar_t,32768> path{}; DWORD n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!n||n>=path.size()) return "unknown";
    DWORD ignored=0, size=GetFileVersionInfoSizeW(path.data(),&ignored);
    if(size>1024*1024) size=0;
    std::vector<std::byte> buf(size);
    if(size&&GetFileVersionInfoW(path.data(),0,size,buf.data())) {
        VS_FIXEDFILEINFO* info=nullptr; UINT len=0;
        if(VerQueryValueW(buf.data(),L"\\",reinterpret_cast<void**>(&info),&len)&&info&&len>=sizeof(*info)) {
            return std::to_string(HIWORD(info->dwProductVersionMS))+"."+
                std::to_string(LOWORD(info->dwProductVersionMS))+"."+
                std::to_string(HIWORD(info->dwProductVersionLS))+"."+
                std::to_string(LOWORD(info->dwProductVersionLS));
        }
    }
    return "unknown-"+(g_identity.textSha256.empty()?std::string("unfingerprinted"):g_identity.textSha256.substr(0,12));
}
fs::path documentsRoot() {
    std::array<wchar_t,32768> path{};
    const DWORD n=GetEnvironmentVariableW(L"USERPROFILE",path.data(),static_cast<DWORD>(path.size()));
    if(n&&n<path.size()) return fs::path(path.data())/L"Documents";
    return fs::current_path()/L"Documents";
}
struct Region { uintptr_t base{}; SIZE_T size{}; DWORD protect{}; DWORD type{}; };
struct Section { std::string name; uint32_t rva{},virtualSize{},rawSize{},rawOffset{},characteristics{}; };
std::vector<Section> readSections() {
    std::vector<Section> out;
    HMODULE module=GetModuleHandleW(nullptr); if(!module) return out;
    g_moduleBase=reinterpret_cast<uintptr_t>(module);
    IMAGE_DOS_HEADER dos{}; if(!safeRead(g_moduleBase,&dos,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||
        dos.e_lfanew<=0||dos.e_lfanew>0x100000) return out;
    const uintptr_t ntAddress=g_moduleBase+static_cast<uintptr_t>(dos.e_lfanew);
    if(ntAddress<g_moduleBase) return out;
    IMAGE_NT_HEADERS64 nt{}; if(!safeRead(ntAddress,&nt,sizeof(nt))||nt.Signature!=IMAGE_NT_SIGNATURE||
        nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||
        nt.FileHeader.NumberOfSections==0||nt.FileHeader.NumberOfSections>96||
        nt.OptionalHeader.SizeOfImage==0||nt.OptionalHeader.SizeOfImage>0x80000000u||
        g_moduleBase>UINTPTR_MAX-nt.OptionalHeader.SizeOfImage) return out;
    g_identity.imageSize=nt.OptionalHeader.SizeOfImage; g_preferredBase=nt.OptionalHeader.ImageBase;
    g_timestamp=nt.FileHeader.TimeDateStamp;
    const uintptr_t firstSection=ntAddress+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;
    if(firstSection<ntAddress||firstSection-g_moduleBase>g_identity.imageSize||
        nt.FileHeader.NumberOfSections>(g_identity.imageSize-(firstSection-g_moduleBase))/sizeof(IMAGE_SECTION_HEADER)) return {};
    bool foundText=false;
    for(WORD i=0;i<nt.FileHeader.NumberOfSections;++i) {
        IMAGE_SECTION_HEADER header{};
        if(!safeRead(firstSection+static_cast<uintptr_t>(i)*sizeof(header),&header,sizeof(header))) return {};
        char name[9]{}; memcpy(name,header.Name,8);
        Section section{name,header.VirtualAddress,header.Misc.VirtualSize,header.SizeOfRawData,
            header.PointerToRawData,header.Characteristics};
        const uint64_t extent=section.virtualSize?section.virtualSize:section.rawSize;
        if(section.rva>=g_identity.imageSize||extent>g_identity.imageSize-section.rva) return {};
        if(section.name==".text") {
            if(foundText||section.rawSize==0||section.rawSize>g_identity.imageSize-section.rva) return {};
            g_identity.textRva=section.rva; g_identity.textSize=section.rawSize; foundText=true;
        }
        out.push_back(std::move(section));
    }
    if(!foundText||!hashMappedRange(g_moduleBase+g_identity.textRva,g_identity.textSize,g_identity.textSha256)) return {};
    std::array<wchar_t,32768> filePath{};
    const DWORD pathLength=GetModuleFileNameW(module,filePath.data(),static_cast<DWORD>(filePath.size()));
    if(pathLength&&pathLength<filePath.size()) hashFilePath(filePath.data(),g_identity.fileSha256);
    return out;
}
const char* protectionName(DWORD p) {
    if(p&PAGE_GUARD) return "guard";
    switch(p&0xff) { case PAGE_NOACCESS:return "no-access"; case PAGE_READONLY:return "read-only";
      case PAGE_READWRITE:return "read-write"; case PAGE_WRITECOPY:return "copy-on-write";
      case PAGE_EXECUTE:return "execute-only"; case PAGE_EXECUTE_READ:return "execute-read";
      case PAGE_EXECUTE_READWRITE:return "execute-read-write"; case PAGE_EXECUTE_WRITECOPY:return "execute-copy-on-write"; default:return "unknown"; }
}
const char* memoryTypeName(DWORD t) { switch(t) { case MEM_IMAGE:return "image"; case MEM_MAPPED:return "mapped"; case MEM_PRIVATE:return "private"; default:return "unknown"; } }
bool readable(DWORD p) {
    if(p&(PAGE_GUARD|PAGE_NOACCESS)) return false;
    switch(p&0xff) { case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
      case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true; default:return false; }
}
bool executable(DWORD p) {
    switch(p&0xff) { case PAGE_EXECUTE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:return true; default:return false; }
}
std::vector<Region> enumerateRegions() {
    std::vector<Region> out; SYSTEM_INFO si{}; GetSystemInfo(&si);
    uintptr_t p=reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress), end=reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    while(p<end) {
        MEMORY_BASIC_INFORMATION m{}; if(!VirtualQuery(reinterpret_cast<void*>(p),&m,sizeof(m))) { p+=0x10000; continue; }
        uintptr_t b=reinterpret_cast<uintptr_t>(m.BaseAddress); if(m.RegionSize==0||b>UINTPTR_MAX-m.RegionSize) break;
        if(m.State==MEM_COMMIT&&readable(m.Protect)) out.push_back({b,m.RegionSize,m.Protect,m.Type});
        p=b+m.RegionSize;
    }
    return out;
}
void exportMemoryMap(const std::vector<Region>& regions) {
    json map={{"schema",1},{"process_id",GetCurrentProcessId()},{"module_base",hex(g_moduleBase)},
        {"module_text_sha256",g_identity.textSha256},{"regions",json::array()}};
    for(const auto& r:regions) {
        json item={{"base",hex(r.base)},{"end",hex(r.base+r.size)},{"size",r.size},
            {"protection",protectionName(r.protect)},{"protection_value",hex(r.protect)},
            {"type",memoryTypeName(r.type)},{"type_value",hex(r.type)}};
        const uintptr_t moduleEnd=g_moduleBase+g_identity.imageSize;
        if(r.base<moduleEnd&&r.base+r.size>g_moduleBase) {
            const uintptr_t lo=std::max(r.base,g_moduleBase),hi=std::min(r.base+r.size,moduleEnd);
            item["module_rva_start"]=hex(lo-g_moduleBase); item["module_rva_end"]=hex(hi-g_moduleBase);
        }
        map["regions"].push_back(std::move(item));
    }
    atomicWrite(g_out/"memory_regions.json",map.dump(2));
}
json exportModuleLayout(const std::vector<Section>& sections) {
    json layout={{"schema",1},{"module",{{"name","Minecraft.Windows.exe"},
        {"loaded_base",hex(g_moduleBase)},{"preferred_image_base",hex(g_preferredBase)},
        {"image_size",g_identity.imageSize},{"pe_timestamp",hex(g_timestamp)},
        {"machine","x64"},{"file_sha256",g_identity.fileSha256},{"text_sha256",g_identity.textSha256}}},
        {"sections",json::array()}};
    for(const auto& s:sections) layout["sections"].push_back({{"name",s.name},{"rva",hex(s.rva)},
        {"loaded_va",hex(g_moduleBase+s.rva)},{"virtual_size",s.virtualSize},
        {"raw_file_offset",hex(s.rawOffset)},{"raw_file_size",s.rawSize},
        {"characteristics",hex(s.characteristics)},{"readable_extents",json::array()}});
    return layout;
}
void dumpSectionMemory(json& layout,const std::vector<Section>& sections) {
    const fs::path dir=g_out/"module_sections"; std::error_code ec; fs::create_directories(dir,ec);
    std::vector<uint8_t> buffer(kChunk);
    for(size_t si=0;si<sections.size();++si) {
        const auto& s=sections[si]; const uint64_t sectionSize=s.virtualSize?s.virtualSize:s.rawSize;
        if(s.rva>=g_identity.imageSize||sectionSize==0) continue;
        const uint64_t bounded=std::min<uint64_t>(sectionSize,g_identity.imageSize-s.rva);
        uintptr_t cursor=g_moduleBase+s.rva,end=cursor+static_cast<uintptr_t>(bounded); unsigned part=0;
        json extents=json::array();
        while(cursor<end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if(!VirtualQuery(reinterpret_cast<const void*>(cursor),&mbi,sizeof(mbi))) { cursor+=0x1000; continue; }
            const uintptr_t regionStart=reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t regionEnd=regionStart+mbi.RegionSize;
            if(!mbi.RegionSize||regionEnd<=cursor) break;
            const uintptr_t next=std::min(end,regionEnd);
            if(mbi.State==MEM_COMMIT&&readable(mbi.Protect)) {
                const uintptr_t extentStart=cursor; const SIZE_T extentSize=static_cast<SIZE_T>(next-cursor);
                std::ostringstream filename; filename<<s.name<<"-rva-"<<std::hex<<std::uppercase<<(extentStart-g_moduleBase)<<"-part-"<<std::dec<<part++<<".bin";
                const fs::path finalPath=dir/filename.str(); fs::path tempPath=finalPath; tempPath+=L".part";
                std::ofstream out(tempPath,std::ios::binary|std::ios::trunc); bool ok=static_cast<bool>(out); SIZE_T done=0;
                while(ok&&done<extentSize) {
                    const SIZE_T want=std::min<SIZE_T>(buffer.size(),extentSize-done); SIZE_T received=0;
                    const BOOL copied=ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(extentStart+done),buffer.data(),want,&received);
                    if(!copied||received!=want) { ok=false; break; }
                    out.write(reinterpret_cast<const char*>(buffer.data()),static_cast<std::streamsize>(received));
                    if(!out) { ok=false; break; }
                    done+=received; Sleep(1);
                }
                out.flush(); if(!out) ok=false; out.close();
                if(ok) {
                    fs::rename(tempPath,finalPath,ec); if(ec) { fs::remove(finalPath,ec); ec.clear(); fs::rename(tempPath,finalPath,ec); }
                    ok=!ec;
                }
                if(!ok) fs::remove(tempPath,ec);
                extents.push_back({{"rva",hex(extentStart-g_moduleBase)},{"loaded_va",hex(extentStart)},
                    {"size",extentSize},{"protection",protectionName(mbi.Protect)},
                    {"dump_file",ok?(fs::path("module_sections")/filename.str()).string():std::string{}},
                    {"dumped",ok}});
            }
            cursor=next;
        }
        layout["sections"][si]["readable_extents"]=std::move(extents);
    }
    atomicWrite(g_out/"module_layout.json",layout.dump(2));
}
void dumpMappedImage(json& layout) {
    const fs::path finalPath=g_out/"mapped_image.bin";
    const fs::path tempPath=g_out/"mapped_image.bin.part";
    std::ofstream out(tempPath,std::ios::binary|std::ios::trunc);
    if(!out) { log("[ScanClient] Could not create mapped_image.bin."); return; }
    std::vector<uint8_t> buffer(kChunk,0);
    uint64_t copied=0, unreadable=0;
    bool ok=true;
    while(copied<g_identity.imageSize) {
        const SIZE_T amount=static_cast<SIZE_T>(std::min<uint64_t>(buffer.size(),g_identity.imageSize-copied));
        for(SIZE_T page=0;page<amount;) {
            const SIZE_T pageBytes=std::min<SIZE_T>(0x1000,amount-page);
            if(!safeRead(g_moduleBase+static_cast<uintptr_t>(copied+page),buffer.data()+page,pageBytes)) {
                std::fill(buffer.begin()+page,buffer.begin()+page+pageBytes,0);
                unreadable+=pageBytes;
            }
            page+=pageBytes;
        }
        out.write(reinterpret_cast<const char*>(buffer.data()),static_cast<std::streamsize>(amount));
        if(!out) { ok=false; break; }
        copied+=amount;
        if((copied%(16*1024*1024))<amount) {
            log("[ScanClient] Mapped image: "+std::to_string(copied/1024/1024)+" / "+std::to_string(g_identity.imageSize/1024/1024)+" MiB");
        }
        Sleep(1);
    }
    out.flush(); if(!out) ok=false; out.close();
    std::error_code ec;
    if(ok) { fs::rename(tempPath,finalPath,ec); if(ec) { fs::remove(finalPath,ec); ec.clear(); fs::rename(tempPath,finalPath,ec); } ok=!ec; }
    if(!ok) { fs::remove(tempPath,ec); log("[ScanClient] Mapped image output incomplete; see output disk space/permissions."); }
    layout["mapped_image"]={{"file","mapped_image.bin"},{"size",g_identity.imageSize},
        {"layout","file offset equals RVA; unreadable ranges are zero-filled"},
        {"unreadable_bytes_zero_filled",unreadable},{"complete",ok}};
    atomicWrite(g_out/"module_layout.json",layout.dump(2));
    if(ok) log("[ScanClient] Saved RVA-mapped image ("+std::to_string(unreadable)+" unreadable bytes zero-filled).");
}
void writeGhidraImportScript() {
    atomicWrite(g_out/"ghidra_import.py",kGhidraImportScript);
}
void saveProgress(const char* state,const std::vector<Region>& regions,size_t ri,SIZE_T off) {
    json p={{"schema",1},{"state",state},{"version",g_version},{"pid",GetCurrentProcessId()},
        {"module_text_sha256",g_identity.textSha256},{"bytes_scanned",g_done.load()},
        {"bytes_total",g_total.load()},{"regions_total",regions.size()},{"regions_done",g_regionsDone.load()},
        {"read_failures",g_failures.load()},{"current_region_index",ri},
        {"current_region_base",ri<regions.size()?hex(regions[ri].base):"0x0"},
        {"current_region_offset",off}};
    atomicWrite(g_out/"progress.json",p.dump(2));
}
void saveCheckpoint(const std::vector<Region>& rs,size_t ri,SIZE_T off) {
    json c={{"schema",1},{"pid",GetCurrentProcessId()},{"module_text_sha256",g_identity.textSha256},
        {"version",g_version},{"region_index",ri},{"region_base",ri<rs.size()?hex(rs[ri].base):"0x0"},
        {"region_size",ri<rs.size()?rs[ri].size:0},{"offset",off},
        {"bytes_scanned",g_done.load()},{"note","Resume is accepted only in this same process with a matching game fingerprint."}};
    atomicWrite(g_out/"checkpoint.json",c.dump(2));
}
bool loadCheckpoint(const std::vector<Region>& rs,size_t& ri,SIZE_T& off) {
    std::ifstream f(g_out/"checkpoint.json"); if(!f) return false;
    try { json c=json::parse(f); if(c.value("pid",0u)!=GetCurrentProcessId()||c.value("module_text_sha256",std::string{})!=g_identity.textSha256) return false;
        const size_t idx=c.value("region_index",size_t{}); if(idx>=rs.size()) return false;
        if(c.value("region_base",std::string{})!=hex(rs[idx].base)||c.value("region_size",SIZE_T{})!=rs[idx].size) return false;
        const auto cursor=c.value("offset",SIZE_T{}); if(cursor>rs[idx].size) return false;
        ri=idx; off=cursor; g_done.store(c.value("bytes_scanned",uint64_t{})); return true;
    } catch(...) { return false; }
}
std::string bytePattern(const uint8_t* p,size_t n) { std::ostringstream o; for(size_t i=0;i<n;++i){if(i)o<<' ';o<<std::hex<<std::uppercase<<std::setw(2)<<std::setfill('0')<<unsigned(p[i]);}return o.str(); }
std::string sha256(std::string_view data) {
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    DWORD objectSize=0,returned=0; std::string digest;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return digest;
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&returned,0)<0) goto done;
    { std::vector<UCHAR> object(objectSize); std::array<UCHAR,32> bytes{};
      if(BCryptCreateHash(algorithm,&hash,object.data(),objectSize,nullptr,0,0)<0) goto done;
      if(BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),static_cast<ULONG>(data.size()),0)<0) goto done;
      if(BCryptFinishHash(hash,bytes.data(),static_cast<ULONG>(bytes.size()),0)<0) goto done;
      std::ostringstream out; for(auto b:bytes) out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b); digest=out.str(); }
done:
    if(hash) BCryptDestroyHash(hash); if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    return digest;
}
json loadItems(const fs::path& path,const char* field) {
    try { std::ifstream f(path); if(!f) return json::array(); auto doc=json::parse(f);
        if(doc.value("module_text_sha256",std::string{})!=g_identity.textSha256||!doc.contains(field)||!doc[field].is_array()) return json::array();
        return doc[field];
    } catch(...) { return json::array(); }
}
void saveFindings(const json& signatures,const json& strings,const json& vtables) {
    const auto provenance=json{{"schema",1},{"module_text_sha256",g_identity.textSha256},{"version",g_version}};
    auto sig=provenance; sig["status"]="candidate-only"; sig["items"]=signatures;
    auto str=provenance; str["status"]="observations"; str["items"]=strings;
    auto vt=provenance; vt["status"]="heuristic-candidates"; vt["items"]=vtables;
    atomicWrite(g_out/"signatures.json",sig.dump(2));
    atomicWrite(g_out/"strings.json",str.dump(2));
    atomicWrite(g_out/"vtable_candidates.json",vt.dump(2));
}

void scan() {
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    g_identity={};
    const auto sections=readSections();
    if(sections.empty()||g_identity.textSha256.empty()) { log("[ScanClient] Safe PE inspection failed; scan cancelled."); return; }
    g_version=detectVersion();
    g_out=documentsRoot()/L"PEXData"/L"ScanClient"/fs::path(g_version)/L"windows-x64";
    std::error_code ec; fs::create_directories(g_out,ec);
    log("[ScanClient] Minecraft "+g_version+" | fingerprint "+g_identity.textSha256.substr(0,16));
    log("[ScanClient] Output: "+g_out.string());
    auto moduleLayout=exportModuleLayout(sections);
    dumpSectionMemory(moduleLayout,sections);
    dumpMappedImage(moduleLayout);
    writeGhidraImportScript();
    auto regions=enumerateRegions(); uint64_t total=0;
    exportMemoryMap(regions);
    for(const auto& r:regions) if(UINT64_MAX-total>=r.size) total+=r.size;
    g_total.store(total); if(regions.empty()) { log("[ScanClient] No readable committed memory regions."); return; }
    size_t ri=0; SIZE_T offset=0; bool resumed=loadCheckpoint(regions,ri,offset);
    if(resumed) log("[ScanClient] Resuming saved cursor."); else log("[ScanClient] Starting a fresh scan (checkpoint identity/layout did not match).");

    json signatures=json::array(), strings=json::array(), paths=json::array(), rtti=json::array(), vtables=json::array();
    if(resumed) {
        signatures=loadItems(g_out/"signatures.json","items");
        strings=loadItems(g_out/"strings.json","items");
        paths=loadItems(g_out/"paths.json","items");
        rtti=loadItems(g_out/"rtti_candidates.json","items");
        vtables=loadItems(g_out/"vtable_candidates.json","items");
    }
    std::vector<std::pair<uintptr_t,uintptr_t>> execRanges;
    for(const auto& r:regions) if(executable(r.protect)) execRanges.emplace_back(r.base,r.base+r.size);
    auto pointsIntoCode=[&](uintptr_t address) {
        auto it=std::upper_bound(execRanges.begin(),execRanges.end(),address,
            [](uintptr_t v,const auto& range){return v<range.first;});
        if(it==execRanges.begin()) return false;
        --it; return address>=it->first&&address<it->second;
    };
    uint64_t lastCheckpoint=g_done.load(), lastProgress=0; auto start=std::chrono::steady_clock::now();
    std::vector<uint8_t> buffer(kChunk);
    for(;ri<regions.size()&&!g_stop.load();++ri,offset=0) {
        const auto& r=regions[ri];
        while(offset<r.size&&!g_stop.load()) {
            if(g_pause.load()) saveProgress("paused",regions,ri,offset);
            while(g_pause.load()&&!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if(g_stop.load()) break;
            const SIZE_T want=std::min<SIZE_T>(buffer.size(),r.size-offset); SIZE_T got=0;
            BOOL ok=ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(r.base+offset),buffer.data(),want,&got);
            if(!ok||got==0) { ++g_failures; offset+=want; g_done.fetch_add(want); continue; }
            const auto* data=buffer.data();
            // Executable byte windows are useful signature seeds, not unique or ABI-verified signatures.
            if(executable(r.protect)) {
                for(SIZE_T at=0;at+24<=got&&signatures.size()<kMaxFindings;at+=4096) {
                    signatures.push_back({{"address",hex(r.base+offset+at)},{"bytes",bytePattern(data+at,24)},
                        {"length",24},{"status","candidate"},{"source","readable executable memory"}});
                }
            }
            // Collect bounded printable UTF-8/ASCII observations with provenance.
            for(SIZE_T i=0;i<got&&strings.size()<kMaxFindings;) {
                if(data[i]>=0x20&&data[i]<=0x7e) { SIZE_T j=i; while(j<got&&data[j]>=0x20&&data[j]<=0x7e&&j-i<512) ++j;
                    if(j-i>=10) { std::string s(reinterpret_cast<const char*>(data+i),j-i);
                        const auto item=json{{"address",hex(r.base+offset+i)},{"value",s},{"source","readable process memory"}};
                        strings.push_back(item);
                        if(paths.size()<kMaxFindings&&(s.find('/')!=std::string::npos||s.find('\\')!=std::string::npos||s.find(".dll")!=std::string::npos||s.find(".exe")!=std::string::npos||s.find(".json")!=std::string::npos)) paths.push_back(item);
                        if(rtti.size()<kMaxFindings&&(s.find(".?AV")!=std::string::npos||s.find(".?AU")!=std::string::npos||s.find("class ")!=std::string::npos||s.find("struct ")!=std::string::npos)) rtti.push_back(item);
                    }
                    i=std::max(j,i+1);
                } else ++i;
            }
            // Heuristic vtable candidates: 4+ consecutive pointers into executable pages.
            const DWORD prot=r.protect&0xff;
            if(prot==PAGE_READONLY||prot==PAGE_READWRITE||prot==PAGE_WRITECOPY) {
                const SIZE_T aligned=(8-(offset&7))&7;
                for(SIZE_T i=aligned;i+32<=got&&vtables.size()<kMaxFindings;i+=8) {
                    unsigned run=0; for(;run<8&&i+(run+1)*8<=got;++run) {
                        uintptr_t value=0; memcpy(&value,data+i+run*8,8);
                        if(!pointsIntoCode(value)) break;
                    }
                    if(run>=4) { vtables.push_back({{"address",hex(r.base+offset+i)},{"entries",run},
                        {"status","candidate"},{"evidence","consecutive pointers target executable committed pages"}}); i+=(run-1)*8; }
                }
            }
            offset+=got; g_done.fetch_add(got); g_regionsDone.store(ri);
            const uint64_t done=g_done.load();
            if(done-lastCheckpoint>=16*1024*1024) { saveFindings(signatures,strings,vtables);
                atomicWrite(g_out/"paths.json",json({{"schema",1},{"module_text_sha256",g_identity.textSha256},{"status","observations"},{"items",paths}}).dump(2));
                atomicWrite(g_out/"rtti_candidates.json",json({{"schema",1},{"module_text_sha256",g_identity.textSha256},{"status","heuristic-candidates"},{"items",rtti}}).dump(2));
                saveCheckpoint(regions,ri,offset); lastCheckpoint=done; }
            if(done-lastProgress>=4*1024*1024) {
                saveProgress("scanning",regions,ri,offset); lastProgress=done;
                const double pct=total?100.0*double(done)/double(total):100.0;
                const auto secs=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
                log("[ScanClient] "+std::to_string(int(pct*100)/100.0)+"% | "+std::to_string(done/1024/1024)+" / "+std::to_string(total/1024/1024)+" MiB | region "+std::to_string(ri+1)+"/"+std::to_string(regions.size())+" | read failures "+std::to_string(g_failures.load())+" | "+std::to_string(secs>0?uint64_t(done/secs/1024/1024):0)+" MiB/s");
            }
            if(!ok||got<want) { ++g_failures; }
            Sleep(1);
        }
    }
    saveFindings(signatures,strings,vtables);
    atomicWrite(g_out/"paths.json",json({{"schema",1},{"module_text_sha256",g_identity.textSha256},{"status","observations"},{"items",paths}}).dump(2));
    atomicWrite(g_out/"rtti_candidates.json",json({{"schema",1},{"module_text_sha256",g_identity.textSha256},{"status","heuristic-candidates"},{"items",rtti}}).dump(2));
    saveCheckpoint(regions,ri,offset);
    const bool complete=!g_stop.load()&&ri>=regions.size();
    json target={{"product","minecraft-bedrock"},{"kind","client"},{"platform","windows"},{"arch","x64"},
        {"version",g_version},{"sha256",g_identity.fileSha256},{"text_sha256",g_identity.textSha256},
        {"image_base",hex(g_preferredBase)},{"size_of_image",g_identity.imageSize},
        {"text_rva",hex(g_identity.textRva)},{"text_size",g_identity.textSize}};
    json profile={{"schema",1},{"target",target},{"symbols",json::object()},
        {"analysis",{{"scan_client",{{"status","candidate-only"},{"complete",complete},{"memory_regions",regions.size()},
            {"bytes_scanned",g_done.load()},{"read_failures",g_failures.load()},
            {"candidate_files",{{"signatures","signatures.json"},{"strings","strings.json"},{"paths","paths.json"},
                {"rtti","rtti_candidates.json"},{"vtables","vtable_candidates.json"},
                {"module_layout","module_layout.json"},{"memory_regions","memory_regions.json"},
                {"module_section_dumps","module_sections/"},{"mapped_image","mapped_image.bin"},
                {"ghidra_import","ghidra_import.py"}}},
            {"note","Automated memory observations are not ABI verification."}}}}}};
    const std::string profileText=profile.dump(2);
    atomicWrite(g_out/"windows-x64.json",profileText);
    const std::string profileHash=sha256(profileText);
    json entry={{"path","windows-x64.json"},{"sha256",profileHash},
        {"module_sha256",g_identity.fileSha256},{"symbols",0},
        {"module_text_sha256",g_identity.textSha256},{"size_of_image",g_identity.imageSize},
        {"text_rva",hex(g_identity.textRva)},{"text_size",g_identity.textSize},
        {"rtti_candidates",rtti.size()},{"status",complete?"complete":"partial"}};
    json versionProfiles={{"windows-x64",entry}};
    json versions={{g_version,versionProfiles}};
    json files={{"signatures","signatures.json"},{"strings","strings.json"},{"paths","paths.json"},
        {"rtti_candidates","rtti_candidates.json"},{"vtable_candidates","vtable_candidates.json"},
        {"module_layout","module_layout.json"},{"memory_regions","memory_regions.json"},
        {"module_section_dumps","module_sections/"},{"mapped_image","mapped_image.bin"},
        {"ghidra_import","ghidra_import.py"}};
    json manifest={{"schema",1},{"tool","ScanClient"},{"versions",versions},{"files",files}};
    atomicWrite(g_out/"manifest.json",manifest.dump(2));
    saveProgress(complete?"complete":"stopped",regions,ri,offset);
    log(std::string("[ScanClient] ")+(complete?"Scan complete.":"Scan stopped; checkpoint saved.")+" Findings: signatures="+std::to_string(signatures.size())+", strings="+std::to_string(strings.size())+", vtable candidates="+std::to_string(vtables.size()));
}

DWORD WINAPI boot(void*) {
    if(!GetConsoleWindow()) AllocConsole();
    std::freopen("CONOUT$","w",stdout); std::freopen("CONIN$","r",stdin);
    SetConsoleTitleW(L"Bedrock ScanClient");
    auto scanEntry=[] {
        try { scan(); }
        catch(const std::exception& e) { log(std::string("[ScanClient] Scan failed safely: ")+e.what()); }
        catch(...) { log("[ScanClient] Scan failed safely with an unknown error."); }
    };
    std::thread worker;
    try { worker=std::thread(scanEntry); }
    catch(const std::exception& e) { log(std::string("[ScanClient] Could not start scan thread: ")+e.what()); return 0; }
    catch(...) { log("[ScanClient] Could not start scan thread."); return 0; }
    log("[ScanClient] Commands: pause | resume | status | stop | restart | exit");
    std::string command;
    while(std::getline(std::cin,command)) {
        if(command=="pause") { g_pause.store(true); log("[ScanClient] Pause requested."); }
        else if(command=="resume") { g_pause.store(false); log("[ScanClient] Resuming."); }
        else if(command=="stop") { g_stop.store(true); g_pause.store(false); log("[ScanClient] Stop requested; checkpoint will be saved."); }
        else if(command=="status") { log("[ScanClient] "+std::to_string(g_done.load()/1024/1024)+" / "+std::to_string(g_total.load()/1024/1024)+" MiB | read failures="+std::to_string(g_failures.load())); }
        else if(command=="restart") {
            g_stop.store(true); g_pause.store(false);
            if(worker.joinable()) worker.join();
            g_done.store(0); g_total.store(0); g_failures.store(0); g_regionsDone.store(0);
            g_stop.store(false);
            try { worker=std::thread(scanEntry); log("[ScanClient] Restarting from the last compatible checkpoint."); }
            catch(const std::exception& e) { log(std::string("[ScanClient] Could not restart scan: ")+e.what()); }
            catch(...) { log("[ScanClient] Could not restart scan."); }
        }
        else if(command=="exit") { log("[ScanClient] Console closing; scan continues in the background."); break; }
        else if(!command.empty()) log("[ScanClient] Unknown command.");
    }
    if(worker.joinable()) worker.detach();
    FreeConsole(); return 0;
}
}

BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(module); HANDLE t=CreateThread(nullptr,0,boot,nullptr,0,nullptr); if(t) CloseHandle(t); }
    return TRUE;
}
#endif

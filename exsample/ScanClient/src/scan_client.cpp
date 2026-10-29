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
#include <bedrock/profiles/remote_catalog.hpp>
#include <nlohmann/json.hpp>
#include "ghidra_import_script.hpp"
#define hde64_disasm scanclient_hde64_disasm
#include "hde64.h"
#undef hde64_disasm
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
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
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
constexpr SIZE_T kChunk = 64 * 1024;
constexpr uint64_t kMaxAnalysisSection = 256ull * 1024 * 1024;
constexpr uint64_t kMaxDynamicMonitorBytes = 128ull * 1024 * 1024;
constexpr uint64_t kMaxDynamicLogBytes = 64ull * 1024 * 1024;
constexpr size_t kMonitorPageSize = 4096;
constexpr size_t kMaxFindings = 4096;
constexpr size_t kMaxTableEntries = 256;
constexpr size_t kMaxRuntimeCandidatePages = 2048;
constexpr uint64_t kMaxCandidateScanBytes = 1024ull * 1024;
struct ScanCandidate { std::string id; uint32_t beginRva{},endRva{}; json details; };
struct DynamicCandidateStats { std::string section; uint64_t changes{},changedBytes{},firstPass{},lastPass{}; };
std::atomic<bool> g_pause{false}, g_stop{false};
std::atomic<bool> g_outputReady{false};
std::atomic<uint64_t> g_done{0}, g_saved{0}, g_skipped{0}, g_total{0}, g_failures{0};
std::atomic<uint64_t> g_monitorPasses{0}, g_monitorPagesChanged{0}, g_monitorBytesChanged{0}, g_monitorReadFailures{0};
std::atomic<unsigned> g_monitorState{0}; // 0=not started, 1=running, 2=stopped
std::mutex g_console;
std::mutex g_candidateMutex;
std::vector<ScanCandidate> g_staticCandidates;
std::unordered_map<uint32_t,DynamicCandidateStats> g_dynamicCandidates;
fs::path g_out;
std::string g_version="unknown";
bedrock::GameIdentity g_identity;
uintptr_t g_moduleBase=0;
uintptr_t g_preferredBase=0;
uint32_t g_timestamp=0;
std::string sha256(std::string_view data);

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
std::string utcTimestamp() {
    SYSTEMTIME now{}; GetSystemTime(&now);
    char value[32]{};
    std::snprintf(value,sizeof(value),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds);
    return value;
}
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
bool readable(DWORD p) {
    if(p&(PAGE_GUARD|PAGE_NOACCESS)) return false;
    switch(p&0xff) { case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
      case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true; default:return false; }
}
bool focusedSection(const std::string& name) {
    return name==".text"||name==".rdata"||name==".pdata";
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
bool waitWhilePaused() {
    while(g_pause.load()&&!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return !g_stop.load();
}
uint64_t focusedCaptureSize(const std::vector<Section>& sections) {
    uint64_t total=0;
    for(const auto& s:sections) if(focusedSection(s.name)) {
        const uint64_t size=s.virtualSize?s.virtualSize:s.rawSize;
        if(UINT64_MAX-total>=size) total+=size;
    }
    return total;
}
bool dumpSectionMemory(json& layout,const std::vector<Section>& sections) {
    const fs::path dir=g_out/"module_sections"; std::error_code ec; fs::create_directories(dir,ec);
    if(ec) { ++g_failures; return false; }
    std::vector<uint8_t> buffer(kChunk);
    uint64_t lastLog=0;
    for(size_t si=0;si<sections.size();++si) {
        const auto& s=sections[si];
        if(!focusedSection(s.name)) continue;
        const uint64_t sectionSize=s.virtualSize?s.virtualSize:s.rawSize;
        if(s.rva>=g_identity.imageSize||sectionSize==0) continue;
        const uint64_t bounded=std::min<uint64_t>(sectionSize,g_identity.imageSize-s.rva);
        uintptr_t cursor=g_moduleBase+s.rva,end=cursor+static_cast<uintptr_t>(bounded); unsigned part=0;
        json extents=json::array();
        while(cursor<end&&!g_stop.load()) {
            MEMORY_BASIC_INFORMATION mbi{};
            if(!VirtualQuery(reinterpret_cast<const void*>(cursor),&mbi,sizeof(mbi))) {
                ++g_failures;
                const uint64_t skipped=std::min<uint64_t>(0x1000,end-cursor);
                g_skipped.fetch_add(skipped); cursor+=static_cast<uintptr_t>(skipped); continue;
            }
            const uintptr_t regionStart=reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            if(!mbi.RegionSize||regionStart>UINTPTR_MAX-mbi.RegionSize) { ++g_failures; break; }
            const uintptr_t regionEnd=regionStart+mbi.RegionSize;
            if(regionEnd<=cursor) { ++g_failures; break; }
            const uintptr_t next=std::min(end,regionEnd);
            if(mbi.State==MEM_COMMIT&&readable(mbi.Protect)) {
                const uintptr_t extentStart=cursor; const SIZE_T extentSize=static_cast<SIZE_T>(next-cursor);
                std::ostringstream filename; filename<<s.name<<"-rva-"<<std::hex<<std::uppercase<<(extentStart-g_moduleBase)<<"-part-"<<std::dec<<part++<<".bin";
                const fs::path finalPath=dir/filename.str(); fs::path tempPath=finalPath; tempPath+=L".part";
                std::ofstream out(tempPath,std::ios::binary|std::ios::trunc); bool ok=static_cast<bool>(out); SIZE_T done=0;
                while(ok&&done<extentSize) {
                    if(!waitWhilePaused()) { ok=false; break; }
                    const SIZE_T want=std::min<SIZE_T>(buffer.size(),extentSize-done); SIZE_T received=0;
                    const BOOL copied=ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(extentStart+done),buffer.data(),want,&received);
                    if(!copied||received!=want) { ++g_failures; ok=false; break; }
                    out.write(reinterpret_cast<const char*>(buffer.data()),static_cast<std::streamsize>(received));
                    if(!out) { ++g_failures; ok=false; break; }
                    done+=received; g_done.fetch_add(received);
                    if(g_done.load()-lastLog>=16*1024*1024) {
                        lastLog=g_done.load();
                        const uint64_t total=g_total.load();
                        const double pct=total?100.0*double(lastLog)/double(total):100.0;
                        log("[ScanClient] Focused module capture "+std::to_string(int(pct*100)/100.0)+"% | "+
                            std::to_string(lastLog/1024/1024)+" / "+std::to_string(total/1024/1024)+" MiB");
                    }
                    Sleep(1);
                }
                out.flush(); if(!out) ok=false; out.close();
                if(ok&&done==extentSize) {
                    fs::rename(tempPath,finalPath,ec); if(ec) { fs::remove(finalPath,ec); ec.clear(); fs::rename(tempPath,finalPath,ec); }
                    ok=!ec;
                }
                if(!ok&&!g_stop.load()) {
                    ++g_failures;
                    g_skipped.fetch_add(extentSize);
                }
                if(!ok) fs::remove(tempPath,ec);
                if(ok) g_saved.fetch_add(extentSize);
                extents.push_back({{"rva",hex(extentStart-g_moduleBase)},{"loaded_va",hex(extentStart)},
                    {"size",extentSize},{"protection",protectionName(mbi.Protect)},
                    {"dump_file",ok?(fs::path("module_sections")/filename.str()).string():std::string{}},
                    {"dumped",ok}});
                if(g_stop.load()) break;
            } else {
                g_skipped.fetch_add(next-cursor);
            }
            cursor=next;
        }
        layout["sections"][si]["readable_extents"]=std::move(extents);
        atomicWrite(g_out/"module_layout.json",layout.dump(2));
        if(g_stop.load()) break;
    }
    atomicWrite(g_out/"module_layout.json",layout.dump(2));
    return !g_stop.load();
}
void writeGhidraImportScript() {
    atomicWrite(g_out/"ghidra_import.py",kGhidraImportScript);
}
void saveProgress(const char* state) {
    const unsigned monitorState=g_monitorState.load();
    const char* monitorText=monitorState==1?"running":monitorState==2?"stopped":"not-started";
    size_t runtimeCandidatePages=0,staticCandidateCount=0;
    {
        std::lock_guard lock(g_candidateMutex);
        runtimeCandidatePages=g_dynamicCandidates.size();
        staticCandidateCount=g_staticCandidates.size();
    }
    json p={{"schema",3},{"scope","Minecraft.Windows.exe PE sections only"},
        {"state",state},{"version",g_version},{"pid",GetCurrentProcessId()},
        {"module_sha256",g_identity.fileSha256},{"module_text_sha256",g_identity.textSha256},
        {"bytes_read",g_done.load()},{"bytes_saved",g_saved.load()},{"bytes_skipped",g_skipped.load()},
        {"bytes_total",g_total.load()},
        {"read_failures",g_failures.load()},
        {"dynamic_monitor",{{"state",monitorText},{"passes",g_monitorPasses.load()},
            {"changed_pages",g_monitorPagesChanged.load()},{"changed_bytes",g_monitorBytesChanged.load()},
            {"read_failures",g_monitorReadFailures.load()},
            {"runtime_candidate_pages",runtimeCandidatePages}}},
        {"ranked_scan_candidates",staticCandidateCount}};
    atomicWrite(g_out/"progress.json",p.dump(2));
}
std::string bytePattern(const uint8_t* p,size_t n) {
    std::ostringstream o;
    for(size_t i=0;i<n;++i) { if(i) o<<' '; o<<std::hex<<std::uppercase<<std::setw(2)<<std::setfill('0')<<unsigned(p[i]); }
    return o.str();
}
uint32_t parseHex32(const std::string& value) {
    try { return static_cast<uint32_t>(std::stoul(value,nullptr,0)); } catch(...) { return 0; }
}
std::string candidateRvaId(const char* prefix,uint32_t rva) {
    std::ostringstream out; out<<prefix<<std::hex<<std::nouppercase<<rva; return out.str();
}
void recordDynamicCandidate(uint32_t rva,const std::string& section,uint64_t pass,size_t changedBytes) {
    std::lock_guard lock(g_candidateMutex);
    auto it=g_dynamicCandidates.find(rva);
    if(it==g_dynamicCandidates.end()) {
        if(g_dynamicCandidates.size()>=kMaxRuntimeCandidatePages) return;
        it=g_dynamicCandidates.emplace(rva,DynamicCandidateStats{}).first;
        it->second.section=section;
        it->second.firstPass=pass;
    }
    ++it->second.changes;
    it->second.changedBytes+=changedBytes;
    it->second.lastPass=pass;
}
struct MonitorSection {
    Section section;
    std::vector<uint8_t> previous;
    std::vector<uint8_t> validPages;
};
void recordMonitorPage(MonitorSection& monitored,size_t pageIndex,size_t pageOffset,
    const uint8_t* current,size_t length,uint64_t pass,std::ofstream& output,
    uint64_t& outputBytes,bool& outputEnabled) {
    if(pageIndex>=monitored.validPages.size()||pageOffset>monitored.previous.size()||
       length>monitored.previous.size()-pageOffset) return;
    auto* previous=monitored.previous.data()+pageOffset;
    if(!monitored.validPages[pageIndex]) {
        memcpy(previous,current,length); monitored.validPages[pageIndex]=1; return;
    }
    std::vector<uint16_t> changedOffsets; changedOffsets.reserve(128);
    size_t changed=0;
    for(size_t i=0;i<length;++i) if(previous[i]!=current[i]) {
        ++changed;
        if(changedOffsets.size()<128) changedOffsets.push_back(static_cast<uint16_t>(i));
    }
    if(changed) {
        const uint32_t pageRva=monitored.section.rva+static_cast<uint32_t>(pageOffset);
        recordDynamicCandidate(pageRva,monitored.section.name,pass,changed);
        json offsets=changedOffsets;
        json event={{"type","changed-page"},{"utc",utcTimestamp()},{"pass",pass},
            {"section",monitored.section.name},{"rva",hex(pageRva)},
            {"candidate_id",candidateRvaId("data-",pageRva)},
            {"candidate_kind","runtime-mutating-page; semantic role unclassified"},
            {"address",hex(g_moduleBase+pageRva)},{"page_size",length},
            {"changed_bytes",changed},{"changed_offsets_first_128",std::move(offsets)},
            {"offset_list_truncated",changed>changedOffsets.size()},
            {"before_prefix_hex",bytePattern(previous,std::min<size_t>(64,length))},
            {"after_prefix_hex",bytePattern(current,std::min<size_t>(64,length))},
            {"status","writable-main-module memory changed; semantic meaning unclassified"}};
        const std::string line=event.dump()+"\n";
        if(outputEnabled&&outputBytes+line.size()<=kMaxDynamicLogBytes) {
            output.write(line.data(),static_cast<std::streamsize>(line.size()));
            if(output) outputBytes+=line.size();
            else outputEnabled=false;
        } else if(outputEnabled) {
            outputEnabled=false;
            log("[ScanClient] Dynamic change log reached its 64 MiB cap; monitoring continues without further file records.");
        }
        g_monitorPagesChanged.fetch_add(1);
        g_monitorBytesChanged.fetch_add(changed);
    }
    memcpy(previous,current,length);
}
bool waitWhileMonitoringPaused(std::stop_token stopToken) {
    while(g_pause.load()&&!g_stop.load()&&!stopToken.stop_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return !g_stop.load()&&!stopToken.stop_requested();
}
void monitorWritableSections(const std::vector<Section>& sections,std::stop_token stopToken) {
    std::vector<MonitorSection> monitored;
    uint64_t selectedBytes=0;
    for(const auto& section:sections) {
        if((section.characteristics&IMAGE_SCN_MEM_WRITE)==0) continue;
        const uint64_t size=section.virtualSize?section.virtualSize:section.rawSize;
        if(!size||size>g_identity.imageSize-section.rva||size>kMaxDynamicMonitorBytes-selectedBytes) {
            log("[ScanClient] Dynamic monitor skipped oversized writable section: "+section.name);
            continue;
        }
        MonitorSection entry; entry.section=section;
        try {
            entry.previous.resize(static_cast<size_t>(size));
            entry.validPages.resize(static_cast<size_t>((size+kMonitorPageSize-1)/kMonitorPageSize));
        } catch(...) {
            log("[ScanClient] Dynamic monitor could not allocate a bounded page baseline for: "+section.name);
            continue;
        }
        selectedBytes+=size; monitored.push_back(std::move(entry));
    }
    const fs::path logPath=g_out/"dynamic_changes.jsonl";
    std::error_code ec;
    uint64_t outputBytes=fs::exists(logPath,ec)?fs::file_size(logPath,ec):0;
    bool outputEnabled=!ec&&outputBytes<kMaxDynamicLogBytes;
    std::ofstream output;
    if(outputEnabled) {
        output.open(logPath,std::ios::binary|std::ios::app);
        outputEnabled=static_cast<bool>(output);
    }
    if(!outputEnabled) log("[ScanClient] Dynamic event file is unavailable or reached its 64 MiB cap; monitoring continues without further file records.");
    if(outputEnabled) {
        json start={{"type","monitor-start"},{"utc",utcTimestamp()},{"version",g_version},
            {"module_text_sha256",g_identity.textSha256},{"scope","writable sections of Minecraft.Windows.exe only"},
            {"selected_bytes",selectedBytes},{"poll_interval_after_each_pass_ms",250},
            {"sections",json::array()}};
        for(const auto& entry:monitored) start["sections"].push_back(entry.section.name);
        const std::string line=start.dump()+"\n";
        if(outputBytes+line.size()<=kMaxDynamicLogBytes) {
            output.write(line.data(),static_cast<std::streamsize>(line.size())); outputBytes+=line.size();
        } else outputEnabled=false;
    }
    g_monitorState.store(1);
    log("[ScanClient] Dynamic monitor running: writable Minecraft image sections="+
        std::to_string(monitored.size())+", baseline bytes="+std::to_string(selectedBytes/1024/1024)+
        " MiB; polling repeats until stop. Private heaps and other modules are excluded.");
    std::array<uint8_t,kChunk> buffer{};
    while(!g_stop.load()&&!stopToken.stop_requested()) {
        if(!waitWhileMonitoringPaused(stopToken)) break;
        const uint64_t pass=g_monitorPasses.load()+1;
        const uint64_t changedPagesBefore=g_monitorPagesChanged.load();
        const uint64_t changedBytesBefore=g_monitorBytesChanged.load();
        for(auto& entry:monitored) {
            const uint64_t size=entry.previous.size();
            for(uint64_t offset=0;offset<size&&!g_stop.load()&&!stopToken.stop_requested();) {
                if(!waitWhileMonitoringPaused(stopToken)) break;
                const size_t amount=static_cast<size_t>(std::min<uint64_t>(buffer.size(),size-offset));
                const uintptr_t address=g_moduleBase+entry.section.rva+static_cast<uintptr_t>(offset);
                if(safeRead(address,buffer.data(),amount)) {
                    for(size_t local=0;local<amount;local+=kMonitorPageSize) {
                        const size_t pageBytes=std::min(kMonitorPageSize,amount-local);
                        const size_t pageOffset=static_cast<size_t>(offset)+local;
                        recordMonitorPage(entry,pageOffset/kMonitorPageSize,pageOffset,
                            buffer.data()+local,pageBytes,pass,output,outputBytes,outputEnabled);
                    }
                } else {
                    ++g_monitorReadFailures;
                    for(size_t local=0;local<amount;local+=kMonitorPageSize) {
                        const size_t pageBytes=std::min(kMonitorPageSize,amount-local);
                        const size_t pageOffset=static_cast<size_t>(offset)+local;
                        if(safeRead(address+local,buffer.data()+local,pageBytes)) {
                            recordMonitorPage(entry,pageOffset/kMonitorPageSize,pageOffset,
                                buffer.data()+local,pageBytes,pass,output,outputBytes,outputEnabled);
                        } else {
                            ++g_monitorReadFailures;
                            const size_t page=pageOffset/kMonitorPageSize;
                            if(page<entry.validPages.size()) entry.validPages[page]=0;
                        }
                    }
                }
                offset+=amount;
                Sleep(0);
            }
            if(g_stop.load()) break;
        }
        if(outputEnabled) output.flush();
        g_monitorPasses.store(pass);
        const uint64_t changedPages=g_monitorPagesChanged.load()-changedPagesBefore;
        const uint64_t changedBytes=g_monitorBytesChanged.load()-changedBytesBefore;
        if(changedPages) log("[ScanClient] Dynamic changes: pass="+std::to_string(pass)+
            " changed_pages="+std::to_string(changedPages)+" changed_bytes="+std::to_string(changedBytes));
        for(unsigned waited=0;waited<250&&!g_stop.load()&&!g_pause.load()&&!stopToken.stop_requested();waited+=25)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if(outputEnabled) {
        const std::string line=json{{"type","monitor-stop"},{"utc",utcTimestamp()},
            {"passes",g_monitorPasses.load()},{"changed_pages",g_monitorPagesChanged.load()},
            {"changed_bytes",g_monitorBytesChanged.load()}}.dump()+"\n";
        if(outputBytes+line.size()<=kMaxDynamicLogBytes) output.write(line.data(),static_cast<std::streamsize>(line.size()));
        output.flush();
    }
    g_monitorState.store(2);
    log("[ScanClient] Dynamic monitor stopped: passes="+std::to_string(g_monitorPasses.load())+
        ", changed pages="+std::to_string(g_monitorPagesChanged.load())+
        ", changed bytes="+std::to_string(g_monitorBytesChanged.load()));
}
std::vector<uint8_t> readCapturedSection(const Section& section,const json& layout) {
    const uint64_t size=section.virtualSize?section.virtualSize:section.rawSize;
    if(size==0||size>g_identity.imageSize-section.rva||size>kMaxAnalysisSection) {
        ++g_failures;
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size),0);
    if(!layout.contains("sections")||!layout["sections"].is_array()) return {};
    for(const auto& item:layout["sections"]) {
        if(item.value("name",std::string{})!=section.name||!item.contains("readable_extents")) continue;
        for(const auto& extent:item["readable_extents"]) {
            if(!extent.value("dumped",false)) continue;
            const uint32_t rva=parseHex32(extent.value("rva",std::string{}));
            const uint64_t length=extent.value("size",uint64_t{});
            if(rva<section.rva||length>size||uint64_t(rva-section.rva)>size-length) continue;
            const fs::path path=g_out/extent.value("dump_file",std::string{});
            std::ifstream file(path,std::ios::binary);
            if(!file) { ++g_failures; continue; }
            file.read(reinterpret_cast<char*>(bytes.data()+(rva-section.rva)),static_cast<std::streamsize>(length));
            if(static_cast<uint64_t>(file.gcount())!=length) ++g_failures;
        }
        return bytes;
    }
    return {};
}
struct RuntimeFunction { uint32_t begin{},end{},unwind{}; };
std::vector<RuntimeFunction> parseRuntimeFunctions(const std::vector<uint8_t>& pdata,const Section& text) {
    std::vector<RuntimeFunction> functions;
    const uint64_t textEnd=uint64_t(text.rva)+(text.rawSize?text.rawSize:text.virtualSize);
    for(size_t offset=0;offset+sizeof(uint32_t)*3<=pdata.size()&&!g_stop.load();offset+=sizeof(uint32_t)*3) {
        RuntimeFunction f{}; memcpy(&f.begin,pdata.data()+offset,4);
        memcpy(&f.end,pdata.data()+offset+4,4); memcpy(&f.unwind,pdata.data()+offset+8,4);
        if(f.begin>=text.rva&&f.end>f.begin&&f.end<=textEnd&&f.unwind<g_identity.imageSize)
            functions.push_back(f);
    }
    std::sort(functions.begin(),functions.end(),[](const auto& a,const auto& b){return a.begin<b.begin;});
    functions.erase(std::unique(functions.begin(),functions.end(),[](const auto& a,const auto& b){return a.begin==b.begin;}),functions.end());
    return functions;
}
const RuntimeFunction* findRuntimeFunction(const std::vector<RuntimeFunction>& functions,uint32_t begin) {
    const auto it=std::lower_bound(functions.begin(),functions.end(),begin,
        [](const RuntimeFunction& f,uint32_t value){return f.begin<value;});
    return it!=functions.end()&&it->begin==begin?&*it:nullptr;
}
std::vector<uint8_t> readCapturedTextPrefix(const Section& text,const json& layout,uint32_t rva,size_t length) {
    std::vector<uint8_t> out;
    if(!layout.contains("sections")||!layout["sections"].is_array()) return out;
    for(const auto& item:layout["sections"]) {
        if(item.value("name",std::string{})!=text.name||!item.contains("readable_extents")) continue;
        for(const auto& extent:item["readable_extents"]) {
            if(!extent.value("dumped",false)) continue;
            const uint32_t extentRva=parseHex32(extent.value("rva",std::string{}));
            const uint64_t extentSize=extent.value("size",uint64_t{});
            if(rva<extentRva||uint64_t(rva-extentRva)>=extentSize) continue;
            const size_t amount=static_cast<size_t>(std::min<uint64_t>(length,extentSize-(rva-extentRva)));
            const fs::path path=g_out/extent.value("dump_file",std::string{});
            std::ifstream file(path,std::ios::binary);
            if(!file) return {};
            file.seekg(static_cast<std::streamoff>(rva-extentRva));
            out.resize(amount);
            file.read(reinterpret_cast<char*>(out.data()),static_cast<std::streamsize>(amount));
            if(static_cast<size_t>(file.gcount())!=amount) out.clear();
            return out;
        }
        return out;
    }
    return out;
}
std::vector<std::string> matchingWorldTerms(const std::string& value) {
    // Deliberately match semantic names, not broad words such as "world",
    // "level", "packet", or "session" which flooded the previous output.
    static constexpr std::array<std::string_view,3> terms={
        "minecraft::registerlevellistener","player changedimension","dimensionchangeprogresshandler"};
    std::string lower=value;
    std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    std::vector<std::string> matches;
    for(const auto term:terms) if(lower.find(term)!=std::string::npos) matches.emplace_back(term);
    return matches;
}
std::vector<std::string> matchingClientSendTerms(const std::string& value) {
    std::string lower=value;
    std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    std::vector<std::string> matches;
    if(lower.find("packetsendbeforeevent")!=std::string::npos)
        matches.emplace_back("packetsendbeforeevent");
    if(lower.find("serverbound")!=std::string::npos&&lower.find("packet")!=std::string::npos)
        matches.emplace_back("serverbound-packet-type");
    return matches;
}
const RuntimeFunction* findContainingRuntimeFunction(const std::vector<RuntimeFunction>& functions,uint32_t rva) {
    const auto it=std::upper_bound(functions.begin(),functions.end(),rva,
        [](uint32_t value,const RuntimeFunction& f){return value<f.begin;});
    if(it==functions.begin()) return nullptr;
    const auto& candidate=*std::prev(it);
    return rva<candidate.end?&candidate:nullptr;
}
void addFilteredString(const std::string& value,uint32_t rva,const char* encoding,
    json& worldStrings,json& clientSendStrings,json& rtti) {
    if(value.size()<5||value.size()>512) return;
    const auto worldMatches=matchingWorldTerms(value);
    const auto sendMatches=matchingClientSendTerms(value);
    auto append=[&](json& output,const std::vector<std::string>& matches,const char* domain) {
        if(matches.empty()||output.size()>=kMaxFindings) return;
        output.push_back({{"rva",hex(rva)},{"address",hex(g_moduleBase+rva)},
            {"encoding",encoding},{"value",value},{"matched_terms",matches},
            {"scan_domain",domain},{"status","observed-string"}});
    };
    append(worldStrings,worldMatches,"world-transition");
    append(clientSendStrings,sendMatches,"client-to-server-packet");
    if(worldMatches.empty()&&sendMatches.empty()) return;
    if(value.find(".?AV")!=std::string::npos||value.find(".?AU")!=std::string::npos||value.find(".?AT")!=std::string::npos) {
        if(rtti.size()<kMaxFindings) rtti.push_back({{"rva",hex(rva)},{"address",hex(g_moduleBase+rva)},
            {"value",value},{"status","RTTI-name-string-candidate; descriptor relationship unverified"}});
    }
}
void collectFilteredStrings(const std::vector<uint8_t>& data,const Section& section,
    json& worldStrings,json& clientSendStrings,json& rtti) {
    for(size_t i=0;i<data.size()&&worldStrings.size()+clientSendStrings.size()<kMaxFindings&&!g_stop.load();) {
        if(data[i]>=0x20&&data[i]<=0x7e&&i+1<data.size()&&data[i+1]==0) {
            size_t j=i; std::string value;
            while(j+1<data.size()&&data[j]>=0x20&&data[j]<=0x7e&&data[j+1]==0&&value.size()<512) {
                value.push_back(static_cast<char>(data[j])); j+=2;
            }
            if(value.size()>=5) addFilteredString(value,section.rva+static_cast<uint32_t>(i),"utf-16le",
                worldStrings,clientSendStrings,rtti);
            i=std::max(j,i+1); continue;
        }
        if(data[i]>=0x20&&data[i]<=0x7e) {
            size_t j=i; while(j<data.size()&&data[j]>=0x20&&data[j]<=0x7e&&j-i<512) ++j;
            if(j-i>=5) addFilteredString(std::string(reinterpret_cast<const char*>(data.data()+i),j-i),
                section.rva+static_cast<uint32_t>(i),"ascii",worldStrings,clientSendStrings,rtti);
            i=std::max(j,i+1); continue;
        }
        ++i;
    }
}
std::string nearbyAsciiContext(const std::vector<uint8_t>& data,size_t offset) {
    const size_t begin=offset>128?offset-128:0;
    std::vector<std::string> runs;
    for(size_t i=begin;i<offset;) {
        if(data[i]>=0x20&&data[i]<=0x7e) {
            size_t j=i; while(j<offset&&data[j]>=0x20&&data[j]<=0x7e&&j-i<128) ++j;
            if(j-i>=4) runs.emplace_back(reinterpret_cast<const char*>(data.data()+i),j-i);
            i=std::max(j,i+1);
        } else ++i;
    }
    std::string context;
    for(const auto& run:runs) { if(!context.empty()) context+=" | "; context+=run; }
    return context;
}
void collectFunctionPointerTables(const std::vector<uint8_t>& data,const Section& rdata,const Section& text,
    const std::vector<RuntimeFunction>& functions,const json& layout,json& tables,json& signatures,
    std::unordered_set<uint32_t>& emittedFunctions) {
    const uintptr_t textStart=g_moduleBase+text.rva;
    const uintptr_t textEnd=textStart+(text.rawSize?text.rawSize:text.virtualSize);
    const size_t alignment=(8-(rdata.rva&7))&7;
    for(size_t i=alignment;i+4*sizeof(uint64_t)<=data.size()&&tables.size()<kMaxFindings&&!g_stop.load();) {
        uint64_t first=0; memcpy(&first,data.data()+i,sizeof(first));
        if(first<textStart||first>=textEnd) { i+=8; continue; }
        const size_t tableStart=i; std::vector<uint32_t> targets; size_t functionStarts=0;
        while(i+sizeof(uint64_t)<=data.size()&&targets.size()<kMaxTableEntries) {
            uint64_t value=0; memcpy(&value,data.data()+i,sizeof(value));
            if(value<textStart||value>=textEnd) break;
            const uint32_t target=static_cast<uint32_t>(value-g_moduleBase);
            targets.push_back(target);
            if(findRuntimeFunction(functions,target)) ++functionStarts;
            i+=sizeof(uint64_t);
        }
        const size_t minimumStarts=std::max<size_t>(3,(targets.size()+1)/2);
        if(targets.size()<4||functionStarts<minimumStarts) { i=tableStart+sizeof(uint64_t); continue; }
        const uint32_t tableRva=rdata.rva+static_cast<uint32_t>(tableStart);
        const double ratio=targets.empty()?0.0:double(functionStarts)/double(targets.size());
        json entries=json::array();
        for(size_t entry=0;entry<targets.size();++entry) {
            const auto* fn=findRuntimeFunction(functions,targets[entry]);
            entries.push_back({{"index",entry},{"function_rva",hex(targets[entry])},
                {"function_range_verified_by_pdata",fn!=nullptr},
                {"end_rva",fn?hex(fn->end):std::string{}}});
            if(!fn||!emittedFunctions.insert(fn->begin).second||signatures.size()>=kMaxFindings) continue;
            const size_t sampleSize=std::min<size_t>(48,fn->end-fn->begin);
            const auto sample=readCapturedTextPrefix(text,layout,fn->begin,sampleSize);
            if(sample.empty()) continue;
            signatures.push_back({{"rva",hex(fn->begin)},{"end_rva",hex(fn->end)},
                {"function_size",fn->end-fn->begin},{"sample_length",sample.size()},
                {"byte_prefix",bytePattern(sample.data(),sample.size())},
                {"status","exact-build function prefix; uniqueness and ABI unverified"}});
        }
        tables.push_back({{"rva",hex(tableRva)},{"address",hex(g_moduleBase+tableRva)},
            {"entry_count",targets.size()},{"pdata_function_start_count",functionStarts},
            {"pdata_start_ratio",ratio},{"classification","function-pointer-table-candidate"},
            {"vtable_status","unverified; class/type metadata not established"},
            {"nearby_ascii_context",nearbyAsciiContext(data,tableStart)},{"entries",std::move(entries)}});
    }
}
json collectValidatedStringReferences(const Section& text,
    const std::vector<RuntimeFunction>& functions,const json& layout,const json& strings,
    json& signatures,std::unordered_set<uint32_t>& emittedFunctions,json& decodeStats) {
    std::unordered_map<uint32_t,size_t> stringByRva;
    for(size_t i=0;i<strings.size();++i)
        stringByRva.emplace(parseHex32(strings[i].value("rva",std::string{})),i);
    json references=json::array();
    decodeStats={{"decoder","HDE64"},{"function_boundary",".pdata runtime-function start"},
        {"functions_decoded",0},{"functions_too_large",0},{"decode_errors",0},
        {"function_read_errors",0},{"instructions_decoded",0}};
    if(stringByRva.empty()||!layout.contains("sections")||!layout["sections"].is_array()) return references;

    uint64_t functionsDecoded=0,functionsTooLarge=0,decodeErrors=0,functionReadErrors=0,instructionsDecoded=0;
    std::unordered_set<uint64_t> emittedReferences;
    constexpr uint64_t maxLinearDecodeBytes=kMaxCandidateScanBytes;
    constexpr size_t maxInstructionLength=15;
    std::vector<uint8_t> code;
    for(const auto& sectionLayout:layout["sections"]) {
        if(sectionLayout.value("name",std::string{})!=".text"||!sectionLayout.contains("readable_extents")) continue;
        for(const auto& extent:sectionLayout["readable_extents"]) {
            if(!extent.value("dumped",false)) continue;
            const uint32_t extentRva=parseHex32(extent.value("rva",std::string{}));
            const uint64_t extentSize=extent.value("size",uint64_t{});
            if(!extentSize||extentSize>UINT32_MAX||uint64_t(extentRva)+extentSize>UINT32_MAX) continue;
            std::ifstream file(g_out/extent.value("dump_file",std::string{}),std::ios::binary);
            if(!file) { ++g_failures; ++functionReadErrors; continue; }
            const uint64_t extentEnd=uint64_t(extentRva)+extentSize;
            for(const auto& fn:functions) {
                if(g_stop.load()||references.size()>=kMaxFindings) break;
                if(fn.begin<extentRva||fn.end>extentEnd) continue;
                if(!waitWhilePaused()) break;
                const uint64_t functionSize=uint64_t(fn.end)-fn.begin;
                if(!functionSize||functionSize>maxLinearDecodeBytes) {
                    ++functionsTooLarge;
                    continue;
                }
                code.assign(static_cast<size_t>(functionSize)+maxInstructionLength,0);
                file.clear();
                file.seekg(static_cast<std::streamoff>(uint64_t(fn.begin)-extentRva));
                file.read(reinterpret_cast<char*>(code.data()),static_cast<std::streamsize>(functionSize));
                if(static_cast<uint64_t>(file.gcount())!=functionSize) {
                    ++g_failures; ++functionReadErrors; continue;
                }
                ++functionsDecoded;
                for(size_t offset=0;offset<functionSize;) {
                    hde64s decoded{};
                    const unsigned int decodedLength=scanclient_hde64_disasm(code.data()+offset,&decoded);
                    const uint32_t invalidFlags=F_ERROR|F_ERROR_OPCODE|F_ERROR_LENGTH|F_ERROR_LOCK|F_ERROR_OPERAND;
                    if(!decodedLength||decodedLength>maxInstructionLength||
                       (decoded.flags&invalidFlags)||offset+decodedLength>functionSize) {
                        ++decodeErrors;
                        break;
                    }
                    ++instructionsDecoded;
                    const bool ripRelative=(decoded.flags&F_MODRM)&&(decoded.flags&F_DISP32)&&
                        !(decoded.flags&F_PREFIX_67)&&decoded.modrm_mod==0&&decoded.modrm_rm==5;
                    if(ripRelative) {
                        const uint64_t nextRva=uint64_t(fn.begin)+offset+decodedLength;
                        const int64_t targetSigned=static_cast<int64_t>(nextRva)+
                            static_cast<int32_t>(decoded.disp.disp32);
                        if(targetSigned>=0&&targetSigned<=UINT32_MAX) {
                            const uint32_t targetRva=static_cast<uint32_t>(targetSigned);
                            const auto stringIt=stringByRva.find(targetRva);
                            if(stringIt!=stringByRva.end()) {
                                const uint32_t instructionRva=fn.begin+static_cast<uint32_t>(offset);
                                const uint64_t referenceKey=(uint64_t(instructionRva)<<32)|targetRva;
                                if(emittedReferences.insert(referenceKey).second) {
                                    const auto& sourceString=strings[stringIt->second];
                                    json item={{"instruction_rva",hex(instructionRva)},
                                        {"instruction_address",hex(g_moduleBase+instructionRva)},
                                        {"instruction_bytes",bytePattern(code.data()+offset,decodedLength)},
                                        {"instruction_length",decodedLength},
                                        {"target_string_rva",hex(targetRva)},
                                        {"target_string",sourceString.value("value",std::string{})},
                                        {"matched_terms",sourceString.value("matched_terms",json::array())},
                                        {"scan_domain",sourceString.value("scan_domain",std::string("unclassified"))},
                                        {"encoding",sourceString.value("encoding",std::string{})},
                                        {"function_rva",hex(fn.begin)},{"function_end_rva",hex(fn.end)},
                                        {"function_boundary_source",".pdata runtime-function start"},
                                        {"reference_kind","HDE64-decoded RIP-relative memory operand; instruction boundary verified from function start"},
                                        {"status","instruction-boundary-verified static reference; runtime call path and semantics unverified"}};
                                    references.push_back(std::move(item));
                                    if(emittedFunctions.insert(fn.begin).second&&signatures.size()<kMaxFindings) {
                                        const size_t sampleSize=std::min<size_t>(48,static_cast<size_t>(functionSize));
                                        signatures.push_back({{"rva",hex(fn.begin)},{"end_rva",hex(fn.end)},
                                            {"function_size",functionSize},{"sample_length",sampleSize},
                                            {"byte_prefix",bytePattern(code.data(),sampleSize)},
                                            {"source","HDE64-validated .pdata function string reference"},
                                            {"status","exact-build function prefix; uniqueness and ABI unverified"}});
                                    }
                                }
                            }
                        }
                    }
                    offset+=decodedLength;
                }
            }
        }
        break;
    }
    decodeStats["functions_decoded"]=functionsDecoded;
    decodeStats["functions_too_large"]=functionsTooLarge;
    decodeStats["decode_errors"]=decodeErrors;
    decodeStats["function_read_errors"]=functionReadErrors;
    decodeStats["instructions_decoded"]=instructionsDecoded;
    return references;
}
struct CandidateRule {
    const char* idPrefix;
    const char* category;
    const char* priority;
    const char* confidence;
    const char* assessment;
};
const CandidateRule* classifyCandidateString(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    static constexpr CandidateRule rules[]={
        {"level-listener","world-transition","high","medium",
            "Exact Minecraft::registerLevelListener signature string is referenced by this .pdata function; callback firing and lifetime are not verified."},
        {"player-dimension-change","world-transition","high","medium",
            "The function references the exact player changeDimension trace label; this is a strong transition candidate, but runtime invocation is not observed here."},
        {"dimension-progress-type","transition-related-type","low","low",
            "The function constructs a string containing DimensionChangeProgressHandler; this is type/name evidence, not proof of a progress callback."},
        {"packet-send-event","client-send-event-api","medium","low",
            "PacketSendBeforeEvent is referenced by this .pdata function. This is a send-event API clue, not proof of a client-usable native hook, raw packet access, or a network dispatch boundary."},
        {"serverbound-packet","client-to-server-packet-type","medium","low",
            "A Serverbound packet or payload type name is referenced by this .pdata function. This can locate serialization/dispatch code, but type-name evidence alone does not prove an actual send path or packet contents."}
    };
    if(value.find("minecraft::registerlevellistener")!=std::string::npos) return &rules[0];
    if(value.find("player changedimension")!=std::string::npos) return &rules[1];
    if(value.find("dimensionchangeprogresshandler")!=std::string::npos) return &rules[2];
    if(value.find("packetsendbeforeevent")!=std::string::npos) return &rules[3];
    if(value.find("serverbound")!=std::string::npos&&value.find("packet")!=std::string::npos) return &rules[4];
    return nullptr;
}
json buildScanCandidates(const json& strings,json& references,json& exclusions) {
    std::unordered_map<uint32_t,std::string> stringsByRva;
    for(const auto& item:strings)
        stringsByRva.emplace(parseHex32(item.value("rva",std::string{})),item.value("value",std::string{}));
    std::vector<ScanCandidate> candidates;
    std::unordered_map<std::string,size_t> candidateIndex;
    json acceptedReferences=json::array();
    for(auto& ref:references) {
        const uint32_t stringRva=parseHex32(ref.value("target_string_rva",std::string{}));
        const auto stringIt=stringsByRva.find(stringRva);
        if(stringIt==stringsByRva.end()) continue;
        const CandidateRule* rule=classifyCandidateString(stringIt->second);
        if(!rule) {
            ref["candidate_status"]="excluded";
            ref["candidate_exclusion_reason"]="specific string was found, but no allowlisted candidate class matched";
            exclusions.push_back(ref);
            continue;
        }
        const uint32_t functionRva=parseHex32(ref.value("function_rva",std::string{}));
        const uint32_t functionEnd=parseHex32(ref.value("function_end_rva",std::string{}));
        const uint32_t instructionRva=parseHex32(ref.value("instruction_rva",std::string{}));
        const bool hasFunction=functionRva&&functionEnd>functionRva;
        if(!hasFunction) {
            ref["candidate_status"]="excluded";
            ref["candidate_exclusion_reason"]="no usable .pdata runtime-function range; no selectable scan range emitted";
            exclusions.push_back(ref);
            continue;
        }
        const uint32_t begin=functionRva;
        const uint32_t end=functionEnd;
        const uint32_t identityRva=functionRva;
        const std::string id=candidateRvaId(rule->idPrefix,identityRva);
        json evidence={{"target_string_rva",hex(stringRva)},{"target_string",stringIt->second},
            {"instruction_rva",hex(instructionRva)},{"instruction_bytes",ref.value("instruction_bytes",std::string{})},
            {"function_rva",hex(functionRva)},{"function_end_rva",hex(functionEnd)},
            {"scan_domain",ref.value("scan_domain",std::string("unclassified"))},
            {"instruction_boundary_verified",true},
            {"function_boundary_source",".pdata runtime-function start + HDE64 linear decode"}};
        auto found=candidateIndex.find(id);
        if(found==candidateIndex.end()) {
            const std::string scanScope="containing .pdata runtime-function range";
            json details={{"id",id},{"category",rule->category},{"priority",rule->priority},
                {"confidence",rule->confidence},{"rva",hex(begin)},{"end_rva",hex(end)},
                {"scan_scope",scanScope},{"scan_bytes",uint64_t(end)-begin},
                {"scan_eligible",end>begin&&uint64_t(end)-begin<=kMaxCandidateScanBytes},
                {"assessment",rule->assessment},{"evidence",json::array()}};
            details["evidence"].push_back(evidence);
            candidateIndex.emplace(id,candidates.size());
            candidates.push_back({id,begin,end,std::move(details)});
        } else {
            candidates[found->second].details["evidence"].push_back(std::move(evidence));
        }
        ref["candidate_id"]=id;
        ref["candidate_category"]=rule->category;
        ref["candidate_priority"]=rule->priority;
        ref["candidate_status"]="selectable-candidate";
        acceptedReferences.push_back(ref);
    }
    references=std::move(acceptedReferences);
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b) {
        const std::string pa=a.details.value("priority",std::string{});
        const std::string pb=b.details.value("priority",std::string{});
        if(pa!=pb) return pa=="high";
        return a.beginRva<b.beginRva;
    });
    {
        std::lock_guard lock(g_candidateMutex);
        g_staticCandidates=std::move(candidates);
    }
    json items=json::array();
    {
        std::lock_guard lock(g_candidateMutex);
        for(const auto& candidate:g_staticCandidates) items.push_back(candidate.details);
    }
    return items;
}
json candidateIndexDocument() {
    json items=json::array();
    size_t staticCount=0,dynamicCount=0;
    {
        std::lock_guard lock(g_candidateMutex);
        staticCount=g_staticCandidates.size();
        for(const auto& candidate:g_staticCandidates) items.push_back(candidate.details);
        std::vector<std::pair<uint32_t,DynamicCandidateStats>> dynamic;
        dynamic.reserve(g_dynamicCandidates.size());
        for(const auto& entry:g_dynamicCandidates) dynamic.push_back(entry);
        std::sort(dynamic.begin(),dynamic.end(),[](const auto& a,const auto& b) {
            if(a.second.changes!=b.second.changes) return a.second.changes>b.second.changes;
            return a.first<b.first;
        });
        dynamicCount=dynamic.size();
        for(const auto& [rva,stats]:dynamic) {
            const uint32_t end=static_cast<uint32_t>(std::min<uint64_t>(g_identity.imageSize,
                uint64_t(rva)+kMonitorPageSize));
            items.push_back({{"id",candidateRvaId("data-",rva)},{"category","runtime-mutating-page"},
                {"priority","unclassified"},{"confidence","observed-change-only"},
                {"section",stats.section},
                {"rva",hex(rva)},{"end_rva",hex(end)},{"scan_scope","one observed writable-module page"},
                {"scan_bytes",end-rva},{"scan_eligible",end>rva&&uint64_t(end)-rva<=kMaxCandidateScanBytes},
                {"change_observations",stats.changes},{"changed_bytes_total",stats.changedBytes},
                {"first_pass",stats.firstPass},{"last_pass",stats.lastPass},
                {"assessment","This page changed during monitoring. Its semantic role and relation to a world transition are unknown."}});
        }
    }
    return {{"schema",1},{"tool","ScanClient"},{"version",g_version},
        {"module_sha256",g_identity.fileSha256},{"module_text_sha256",g_identity.textSha256},
        {"static_candidate_count",staticCount},{"runtime_page_candidate_count",dynamicCount},
        {"candidates",std::move(items)},
        {"note","Static world-transition and client-to-server packet candidates are ranked from HDE64-validated instruction references to specific strings. Serverbound type names do not prove a send path. Runtime pages are only observed changes; no candidate proves runtime semantics or ABI."}};
}
void saveCandidateIndex() {
    if(g_out.empty()) return;
    atomicWrite(g_out/"scan_candidates.json",candidateIndexDocument().dump(2));
}
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
void listScanCandidates(bool runtimeOnly=false) {
    std::vector<ScanCandidate> statics;
    std::vector<std::pair<uint32_t,DynamicCandidateStats>> dynamic;
    {
        std::lock_guard lock(g_candidateMutex);
        statics=g_staticCandidates;
        dynamic.reserve(g_dynamicCandidates.size());
        for(const auto& entry:g_dynamicCandidates) dynamic.push_back(entry);
    }
    std::sort(dynamic.begin(),dynamic.end(),[](const auto& a,const auto& b) {
        if(a.second.changes!=b.second.changes) return a.second.changes>b.second.changes;
        return a.first<b.first;
    });
    std::ostringstream out;
    out<<"[ScanClient] Static candidates="<<statics.size()<<", runtime-changing pages="<<dynamic.size()<<".";
    if(!runtimeOnly) for(const auto& candidate:statics) {
        out<<"\n  "<<candidate.id<<" | "<<candidate.details.value("priority",std::string("?"))
           <<" | "<<candidate.details.value("category",std::string("candidate"))
           <<" | RVA "<<hex(candidate.beginRva)<<"-"<<hex(candidate.endRva);
    }
    if(runtimeOnly) out<<"\n[ScanClient] Dynamic pages are ranked by change count; their meaning is unknown.";
    const size_t limit=std::min<size_t>(dynamic.size(),runtimeOnly?50:10);
    for(size_t i=0;i<limit;++i) {
        const auto& [rva,stats]=dynamic[i];
        out<<"\n  "<<candidateRvaId("data-",rva)<<" | runtime-change-only"
           <<" | changes="<<stats.changes<<" | changed_bytes="<<stats.changedBytes;
    }
    if(dynamic.size()>limit) out<<"\n  ... "<<(dynamic.size()-limit)<<" more runtime pages; use 'candidates dynamic' to list them.";
    out<<"\n[ScanClient] Capture one candidate with: scan candidate <id>";
    log(out.str());
}
void scanCandidate(const std::string& id) {
    if(!g_outputReady.load(std::memory_order_acquire)||id.empty()) {
        log("[ScanClient] Candidate scan unavailable until the output folder is ready."); return;
    }
    ScanCandidate selected; bool found=false;
    {
        std::lock_guard lock(g_candidateMutex);
        const auto it=std::find_if(g_staticCandidates.begin(),g_staticCandidates.end(),[&](const auto& c){return c.id==id;});
        if(it!=g_staticCandidates.end()) { selected=*it; found=true; }
        if(!found) for(const auto& [rva,stats]:g_dynamicCandidates) {
            if(candidateRvaId("data-",rva)!=id) continue;
            const uint32_t end=static_cast<uint32_t>(std::min<uint64_t>(g_identity.imageSize,
                uint64_t(rva)+kMonitorPageSize));
            selected={id,rva,end,{{"id",id},{"category","runtime-mutating-page"},
                {"priority","unclassified"},{"confidence","observed-change-only"},
                {"section",stats.section},
                {"change_observations",stats.changes},{"changed_bytes_total",stats.changedBytes},
                {"first_pass",stats.firstPass},{"last_pass",stats.lastPass},
                {"assessment","This page changed during monitoring; its relation to a world transition is unknown."}}};
            found=true; break;
        }
    }
    if(!found) { log("[ScanClient] Unknown candidate ID. Use 'candidates' or 'candidates dynamic'."); return; }
    if(selected.endRva<=selected.beginRva||selected.endRva>g_identity.imageSize||
       uint64_t(selected.endRva)-selected.beginRva>kMaxCandidateScanBytes) {
        log("[ScanClient] Candidate range failed bounds checks; no bytes were read."); return;
    }
    const size_t length=selected.endRva-selected.beginRva;
    std::vector<uint8_t> bytes(length);
    for(size_t offset=0;offset<length;) {
        const size_t amount=std::min<size_t>(kChunk,length-offset);
        const uintptr_t address=g_moduleBase+selected.beginRva+offset;
        if(address<g_moduleBase||!safeRead(address,bytes.data()+offset,amount)) {
            log("[ScanClient] Candidate read failed safely; partial bytes were discarded."); return;
        }
        offset+=amount;
    }
    std::string stamp=utcTimestamp();
    stamp.erase(std::remove_if(stamp.begin(),stamp.end(),[](char c){return c=='-'||c==':'||c=='.';}),stamp.end());
    const fs::path directory=g_out/"candidate_scans"/selected.id;
    std::error_code ec; fs::create_directories(directory,ec);
    if(ec) { log("[ScanClient] Could not create the candidate output folder."); return; }
    const std::string basename=stamp+"-rva-"+hex(selected.beginRva).substr(2);
    const fs::path binaryPath=directory/(basename+".bin");
    const fs::path jsonPath=directory/(basename+".json");
    const std::string binary(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    const std::string binaryHash=sha256(binary);
    atomicWrite(binaryPath,binary);
    if(!fs::exists(binaryPath,ec)||ec) { log("[ScanClient] Candidate snapshot could not be saved."); return; }
    json snapshot={{"schema",1},{"type","targeted-read-only-candidate-snapshot"},
        {"utc",utcTimestamp()},{"version",g_version},{"module_sha256",g_identity.fileSha256},
        {"module_text_sha256",g_identity.textSha256},{"loaded_base",hex(g_moduleBase)},
        {"candidate",selected.details},{"rva",hex(selected.beginRva)},
        {"address",hex(g_moduleBase+selected.beginRva)},{"end_rva",hex(selected.endRva)},
        {"bytes",bytes.size()},{"sha256",binaryHash},
        {"first_64_bytes_hex",bytePattern(bytes.data(),std::min<size_t>(bytes.size(),64))},
        {"binary_file",binaryPath.lexically_relative(g_out).generic_string()},
        {"status","guarded read completed; bytes are a point-in-time sample, not proof of event behavior"}};
    atomicWrite(jsonPath,snapshot.dump(2));
    log("[ScanClient] Candidate snapshot saved: "+jsonPath.lexically_relative(g_out).generic_string()+
        " | bytes="+std::to_string(bytes.size())+" | sha256="+binaryHash.substr(0,16));
}
void scan() {
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    g_outputReady.store(false,std::memory_order_release);
    g_done.store(0); g_saved.store(0); g_skipped.store(0); g_failures.store(0);
    g_monitorPasses.store(0); g_monitorPagesChanged.store(0); g_monitorBytesChanged.store(0);
    g_monitorReadFailures.store(0); g_monitorState.store(0);
    {
        std::lock_guard lock(g_candidateMutex);
        g_staticCandidates.clear();
        g_dynamicCandidates.clear();
    }
    g_identity={};
    log("[ScanClient] Fingerprinting Minecraft headers and .text; unrelated process memory is excluded.");
    const auto sections=readSections();
    if(sections.empty()||g_identity.textSha256.empty()) { log("[ScanClient] Safe PE inspection failed; scan cancelled."); return; }
    const Section* textSection=nullptr; const Section* rdataSection=nullptr; const Section* pdataSection=nullptr;
    for(const auto& section:sections) {
        if(section.name==".text") textSection=&section;
        else if(section.name==".rdata") rdataSection=&section;
        else if(section.name==".pdata") pdataSection=&section;
    }
    if(!textSection||!rdataSection||!pdataSection) {
        log("[ScanClient] Required PE sections were not present; scan cancelled."); return;
    }
    g_version=detectVersion();
    g_out=documentsRoot()/L"PEXData"/L"ScanClient"/fs::path(g_version)/L"windows-x64";
    std::error_code ec; fs::create_directories(g_out,ec);
    if(ec) { log("[ScanClient] Could not create the output folder; scan cancelled."); return; }
    g_outputReady.store(true,std::memory_order_release);
    log("[ScanClient] Minecraft "+g_version+" | fingerprint "+g_identity.textSha256.substr(0,16));
    log("[ScanClient] Output: "+g_out.string());
    g_total.store(focusedCaptureSize(sections));
    log("[ScanClient] Scope: Minecraft .text/.rdata/.pdata only; private and unrelated process memory is excluded.");
    std::jthread monitorThread([&sections](std::stop_token stopToken) {
        try { monitorWritableSections(sections,stopToken); }
        catch(const std::exception& e) {
            g_monitorState.store(2);
            log(std::string("[ScanClient] Dynamic monitor stopped safely: ")+e.what());
        } catch(...) {
            g_monitorState.store(2);
            log("[ScanClient] Dynamic monitor stopped safely after an unknown error.");
        }
    });
    saveProgress("capturing");
    auto moduleLayout=exportModuleLayout(sections);
    atomicWrite(g_out/"module_layout.json",moduleLayout.dump(2));
    const bool captureFinished=dumpSectionMemory(moduleLayout,sections);
    atomicWrite(g_out/"module_layout.json",moduleLayout.dump(2));
    writeGhidraImportScript();
    if(!captureFinished||g_stop.load()) {
        monitorThread.join();
        saveProgress("stopped");
        log("[ScanClient] Focused capture stopped. Use restart to begin it again from the start.");
        return;
    }
    log("[ScanClient] Indexing .pdata function ranges and filtering .rdata strings/pointer tables.");
    auto pdataBytes=readCapturedSection(*pdataSection,moduleLayout);
    const auto runtimeFunctions=parseRuntimeFunctions(pdataBytes,*textSection);
    std::vector<uint8_t>().swap(pdataBytes);
    const auto rdataBytes=readCapturedSection(*rdataSection,moduleLayout);
    json worldStrings=json::array(), clientSendStrings=json::array();
    json rtti=json::array(), tables=json::array(), signatures=json::array();
    collectFilteredStrings(rdataBytes,*rdataSection,worldStrings,clientSendStrings,rtti);
    json allStrings=worldStrings;
    for(const auto& item:clientSendStrings) allStrings.push_back(item);
    std::unordered_set<uint32_t> emittedFunctions;
    json decoderStats;
    auto references=collectValidatedStringReferences(*textSection,runtimeFunctions,moduleLayout,
        allStrings,signatures,emittedFunctions,decoderStats);
    json candidateExclusions=json::array();
    const json scanCandidates=buildScanCandidates(allStrings,references,candidateExclusions);
    json worldReferences=json::array(), clientSendReferences=json::array();
    for(const auto& ref:references) {
        const std::string domain=ref.value("scan_domain",std::string{});
        if(domain=="world-transition") worldReferences.push_back(ref);
        else if(domain=="client-to-server-packet") clientSendReferences.push_back(ref);
    }
    collectFunctionPointerTables(rdataBytes,*rdataSection,*textSection,runtimeFunctions,moduleLayout,
        tables,signatures,emittedFunctions);
    if(g_stop.load()) {
        monitorThread.join(); saveProgress("stopped");
        log("[ScanClient] Focused analysis stopped."); return;
    }

    const json provenance={{"schema",2},{"scope","Minecraft.Windows.exe .rdata/.pdata only"},
        {"version",g_version},{"module_sha256",g_identity.fileSha256},
        {"module_text_sha256",g_identity.textSha256}};
    auto stringsDoc=provenance; stringsDoc["status"]="filtered-world-transition-observations"; stringsDoc["items"]=worldStrings;
    auto clientSendStringsDoc=provenance;
    clientSendStringsDoc["status"]="serverbound-packet-and-send-event-name-observations";
    clientSendStringsDoc["items"]=clientSendStrings;
    auto rttiDoc=provenance; rttiDoc["status"]="name-candidates-only"; rttiDoc["items"]=rtti;
    auto tablesDoc=provenance; tablesDoc["status"]="function-pointer-table-candidates"; tablesDoc["items"]=tables;
    auto referencesDoc=provenance; referencesDoc["status"]="instruction-boundary-verified-world-transition-references";
    referencesDoc["items"]=worldReferences;
    auto clientSendReferencesDoc=provenance;
    clientSendReferencesDoc["status"]="instruction-boundary-verified-client-send-candidates";
    clientSendReferencesDoc["items"]=clientSendReferences;
    auto exclusionsDoc=provenance; exclusionsDoc["status"]="excluded-low-confidence-references";
    exclusionsDoc["items"]=candidateExclusions;
    auto signaturesDoc=provenance; signaturesDoc["status"]="exact-build-prefix-candidates"; signaturesDoc["items"]=signatures;
    atomicWrite(g_out/"world_transition_strings.json",stringsDoc.dump(2));
    atomicWrite(g_out/"world_transition_references.json",referencesDoc.dump(2));
    atomicWrite(g_out/"client_send_packet_strings.json",clientSendStringsDoc.dump(2));
    atomicWrite(g_out/"client_send_packet_references.json",clientSendReferencesDoc.dump(2));
    atomicWrite(g_out/"scan_candidates.json",candidateIndexDocument().dump(2));
    atomicWrite(g_out/"candidate_exclusions.json",exclusionsDoc.dump(2));
    atomicWrite(g_out/"rtti_name_candidates.json",rttiDoc.dump(2));
    atomicWrite(g_out/"function_pointer_tables.json",tablesDoc.dump(2));
    atomicWrite(g_out/"signatures.json",signaturesDoc.dump(2));

    const bool complete=!g_stop.load()&&g_failures.load()==0&&g_skipped.load()==0&&g_saved.load()==g_total.load();
    json target={{"product","minecraft-bedrock"},{"kind","client"},{"platform","windows"},{"arch","x64"},
        {"version",g_version},{"sha256",g_identity.fileSha256},{"text_sha256",g_identity.textSha256},
        {"image_base",hex(g_preferredBase)},{"size_of_image",g_identity.imageSize},
        {"text_rva",hex(g_identity.textRva)},{"text_size",g_identity.textSize}};
    json profile={{"schema",1},{"target",target},{"symbols",json::object()},
        {"analysis",{{"scan_client",{{"status","candidate-only"},{"complete",complete},
            {"scope","Minecraft.Windows.exe PE sections .text, .rdata, .pdata only"},
            {"bytes_read",g_done.load()},{"bytes_total",g_total.load()},{"read_failures",g_failures.load()},
            {"bytes_saved",g_saved.load()},{"bytes_skipped",g_skipped.load()},
            {"runtime_functions_from_pdata",runtimeFunctions.size()},
            {"candidate_counts",{{"world_transition_strings",worldStrings.size()},
                {"client_send_packet_strings",clientSendStrings.size()},
                {"rtti_name_candidates",rtti.size()},
                {"world_transition_references",worldReferences.size()},
                {"client_send_packet_references",clientSendReferences.size()},
                {"function_pointer_tables",tables.size()},{"function_prefixes",signatures.size()},
                {"ranked_scan_candidates",scanCandidates.size()},
                {"excluded_candidate_references",candidateExclusions.size()}}},
            {"reference_decoder",decoderStats},
            {"candidate_files",{{"signatures","signatures.json"},{"world_transition_strings","world_transition_strings.json"},
                {"world_transition_references","world_transition_references.json"},
                {"client_send_packet_strings","client_send_packet_strings.json"},
                {"client_send_packet_references","client_send_packet_references.json"},
                {"scan_candidates","scan_candidates.json"},{"candidate_scans","candidate_scans/"},
                {"candidate_exclusions","candidate_exclusions.json"},
                {"rtti_name_candidates","rtti_name_candidates.json"},{"function_pointer_tables","function_pointer_tables.json"},
                {"dynamic_changes","dynamic_changes.jsonl"},
                {"module_layout","module_layout.json"},{"module_section_dumps","module_sections/"},
                {"ghidra_import","ghidra_import.py"}}},
            {"dynamic_monitor",{{"scope","writable PE sections of Minecraft.Windows.exe only"},
                {"poll_interval_after_each_pass_ms",250},{"max_baseline_bytes",kMaxDynamicMonitorBytes},
                {"private_heaps_and_other_modules_scanned",false}}},
            {"note","World-transition and client-to-server packet clues are written separately. Static references are accepted only when HDE64 reaches the RIP-relative instruction by linear decoding from a .pdata function start. Serverbound names and PacketSendBeforeEvent remain clues: they do not prove a native send boundary, packet contents, runtime execution, or ABI. Functions larger than 1 MiB and functions with undecodable bytes are reported in reference_decoder and may have missing references."}}}}}};
    const std::string profileText=profile.dump(2);
    atomicWrite(g_out/"windows-x64.json",profileText);
    const std::string profileHash=sha256(profileText);
    json entry={{"path","windows-x64.json"},{"sha256",profileHash},
        {"module_sha256",g_identity.fileSha256},{"symbols",0},
        {"module_text_sha256",g_identity.textSha256},{"size_of_image",g_identity.imageSize},
        {"text_rva",hex(g_identity.textRva)},{"text_size",g_identity.textSize},
        {"rtti_name_candidates",rtti.size()},{"status",complete?"complete":"partial"}};
    json versionProfiles={{"windows-x64",entry}};
    json versions={{g_version,versionProfiles}};
    json files={{"signatures","signatures.json"},{"world_transition_strings","world_transition_strings.json"},
        {"world_transition_references","world_transition_references.json"},
        {"client_send_packet_strings","client_send_packet_strings.json"},
        {"client_send_packet_references","client_send_packet_references.json"},
        {"scan_candidates","scan_candidates.json"},{"candidate_scans","candidate_scans/"},
        {"candidate_exclusions","candidate_exclusions.json"},
        {"rtti_name_candidates","rtti_name_candidates.json"},{"function_pointer_tables","function_pointer_tables.json"},
        {"dynamic_changes","dynamic_changes.jsonl"},
        {"module_layout","module_layout.json"},{"module_section_dumps","module_sections/"},
        {"ghidra_import","ghidra_import.py"}};
    json manifest={{"schema",2},{"tool","ScanClient"},{"versions",versions},{"files",files}};
    atomicWrite(g_out/"manifest.json",manifest.dump(2));
    saveProgress(complete?"complete":"partial");
    log(std::string("[ScanClient] Static focused scan ")+(complete?"complete.":"partial.")+" Findings: world strings="+
        std::to_string(worldStrings.size())+", serverbound strings="+std::to_string(clientSendStrings.size())+
        ", world references="+std::to_string(worldReferences.size())+
        ", client-send references="+std::to_string(clientSendReferences.size())+
        ", HDE64 decode errors="+std::to_string(decoderStats.value("decode_errors",uint64_t{}))+
        ", function prefixes="+std::to_string(signatures.size()));
    log("[ScanClient] Static capture finished. Dynamic monitoring continues until the stop command.");
    monitorThread.join();
    saveCandidateIndex();
    saveProgress(complete?"complete":"partial");
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
    log("[ScanClient] Commands: pause | resume | status | candidates | candidates dynamic | scan candidate <id> | stop | restart | exit");
    std::string command;
    while(std::getline(std::cin,command)) {
        if(command=="pause") { g_pause.store(true); log("[ScanClient] Pause requested."); }
        else if(command=="resume") { g_pause.store(false); log("[ScanClient] Resuming."); }
        else if(command=="stop") { g_stop.store(true); g_pause.store(false); log("[ScanClient] Stop requested; completed extents are retained in the output bundle."); }
        else if(command=="status") { log("[ScanClient] read="+std::to_string(g_done.load()/1024/1024)+" MiB | saved="+
            std::to_string(g_saved.load()/1024/1024)+" / "+std::to_string(g_total.load()/1024/1024)+
            " MiB | skipped="+std::to_string(g_skipped.load()/1024/1024)+" MiB | failures="+std::to_string(g_failures.load())+
            " | monitor passes="+std::to_string(g_monitorPasses.load())+
            " changed pages="+std::to_string(g_monitorPagesChanged.load())+
            " dynamic read failures="+std::to_string(g_monitorReadFailures.load())); }
        else if(command=="candidates"||command=="candidates dynamic"||command.rfind("scan candidate ",0)==0) {
            try {
                if(command=="candidates") listScanCandidates(false);
                else if(command=="candidates dynamic") listScanCandidates(true);
                else scanCandidate(command.substr(15));
            } catch(const std::exception& e) {
                log(std::string("[ScanClient] Candidate operation failed safely: ")+e.what());
            } catch(...) {
                log("[ScanClient] Candidate operation failed safely.");
            }
        }
        else if(command=="restart") {
            g_outputReady.store(false,std::memory_order_release);
            g_stop.store(true); g_pause.store(false);
            if(worker.joinable()) worker.join();
            g_done.store(0); g_saved.store(0); g_skipped.store(0); g_total.store(0); g_failures.store(0);
            g_stop.store(false);
            try { worker=std::thread(scanEntry); log("[ScanClient] Restarting the focused capture from the beginning."); }
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

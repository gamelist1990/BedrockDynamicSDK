#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <bedrock/hook_runtime.h>
#include <bedrock/remote_catalog.hpp>
#include <bedrock/resolver.hpp>
#include <nlohmann/json.hpp>
#include <hde64.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::size_t kMaxProfileBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxTextBytes = 512 * 1024 * 1024;
constexpr std::size_t kMaxPatternBytes = 512;
constexpr std::size_t kMaxPatternHits = 512;
constexpr std::size_t kMaxReadBytes = 64;
constexpr std::size_t kMaxSymbols = 20000;
constexpr std::string_view k126605ExeSha256 =
    "988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a";
constexpr std::string_view k126605ActorContextEvidence =
    "ev_41859599452e24fd9afcb71522d132dc84b59b14ab622ad287a3f398a511501f";

struct Section {
    std::string name;
    std::uint32_t rva{};
    std::uint32_t virtualSize{};
    std::uint32_t rawSize{};
    std::uint32_t characteristics{};
    std::uint32_t size() const { return std::max(virtualSize, rawSize); }
};

struct Function {
    std::uint32_t begin{};
    std::uint32_t end{};
};

struct PeImage {
    HMODULE module{};
    std::uintptr_t base{};
    std::uint32_t imageSize{};
    std::uint32_t timestamp{};
    std::vector<Section> sections;
    std::vector<Function> functions;
    Section text;
    Section pdata;
    Section rdata;
};

struct HookSpec {
    const char* symbol;
    BDS_HookPoint point;
};

constexpr HookSpec kHookSpecs[] = {
    {"Keyboard::feed", BDS_HOOK_KEYBOARD_FEED},
    {"MouseDevice::feed", BDS_HOOK_MOUSE_FEED},
    {"ClientInstance::grabCursor", BDS_HOOK_GRAB_CURSOR},
    {"ClientInstance::releaseCursor", BDS_HOOK_RELEASE_CURSOR},
    {"LevelRendererPlayer::getFov", BDS_HOOK_GET_FOV},
    {"MultiPlayerLevel::_subTick", BDS_HOOK_LEVEL_SUB_TICK},
    {"Level::getRuntimeActorList", BDS_HOOK_LEVEL_RUNTIME_ACTOR_LIST},
    {"Actor::getNameTag", BDS_HOOK_ACTOR_GET_NAME_TAG},
    {"GameCore::keyboardEvent", BDS_HOOK_GAMECORE_KEYBOARD_EVENT},
    {"ClientInstance::onPlayerLoaded", BDS_HOOK_CLIENT_PLAYER_LOADED},
    {"ClientInstance::onLevelExit", BDS_HOOK_CLIENT_LEVEL_EXIT},
    {"ClientInstance::onDimensionChangedEvent", BDS_HOOK_CLIENT_DIMENSION_CHANGED},
    {"ClientInstance::onCancelJoinGame", BDS_HOOK_CLIENT_JOIN_CANCELLED},
    {"native_world_render.functions.render_level", BDS_HOOK_RENDER_LEVEL},
};

DWORD WINAPI worker(void*);

bool readableProtection(DWORD protection) {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protection & 0xff) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool safeRead(std::uintptr_t address, void* output, std::size_t size) {
    if (!address || !output || !size || size > kMaxTextBytes ||
        address > UINTPTR_MAX - size) return false;
    auto* destination = static_cast<std::byte*>(output);
    std::size_t copied = 0;
    while (copied < size) {
        const auto current = address + copied;
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || !readableProtection(info.Protect)) return false;
        const auto regionStart = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (!info.RegionSize || regionStart > UINTPTR_MAX - info.RegionSize) return false;
        const auto regionEnd = regionStart + info.RegionSize;
        if (current < regionStart || current >= regionEnd) return false;
        const auto amount = std::min<std::size_t>(
            {size - copied, regionEnd - current, std::size_t{1} << 20});
        SIZE_T received = 0;
        if (!ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const void*>(current), destination + copied,
                amount, &received) || received != amount) return false;
        copied += amount;
    }
    return true;
}

bool safeReadAt(std::uintptr_t base, std::size_t offset, void* output,
                std::size_t size) {
    if (!base || base > UINTPTR_MAX - offset) return false;
    return safeRead(base + offset, output, size);
}

bool copyRva(const PeImage& image, std::uint32_t rva, void* output, std::size_t size) {
    if (rva >= image.imageSize || size > image.imageSize - rva) return false;
    return safeRead(image.base + rva, output, size);
}

std::optional<PeImage> inspectPe() {
    PeImage image;
    image.module = GetModuleHandleW(nullptr);
    if (!image.module) return std::nullopt;
    image.base = reinterpret_cast<std::uintptr_t>(image.module);

    IMAGE_DOS_HEADER dos{};
    if (!safeRead(image.base, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        dos.e_lfanew > 0x100000) return std::nullopt;
    const auto ntAddress = image.base + static_cast<std::uintptr_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    if (ntAddress < image.base || !safeRead(ntAddress, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage > kMaxTextBytes ||
        image.base > UINTPTR_MAX - nt.OptionalHeader.SizeOfImage ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96)
        return std::nullopt;
    image.imageSize = nt.OptionalHeader.SizeOfImage;
    image.timestamp = nt.FileHeader.TimeDateStamp;

    const auto sectionAddress = ntAddress + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                                nt.FileHeader.SizeOfOptionalHeader;
    if (sectionAddress < ntAddress || sectionAddress - image.base > image.imageSize ||
        nt.FileHeader.NumberOfSections >
            (image.imageSize - (sectionAddress - image.base)) / sizeof(IMAGE_SECTION_HEADER))
        return std::nullopt;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER header{};
        if (!safeRead(sectionAddress + static_cast<std::uintptr_t>(i) * sizeof(header),
                      &header, sizeof(header))) return std::nullopt;
        char name[9]{};
        std::memcpy(name, header.Name, 8);
        Section section{name, header.VirtualAddress, header.Misc.VirtualSize,
                        header.SizeOfRawData, header.Characteristics};
        if (section.rva >= image.imageSize || section.size() > image.imageSize - section.rva)
            return std::nullopt;
        image.sections.push_back(section);
        if (section.name == ".text") image.text = section;
        else if (section.name == ".pdata") image.pdata = section;
        else if (section.name == ".rdata") image.rdata = section;
    }
    if (!image.text.rawSize || !image.pdata.rawSize ||
        !(image.text.characteristics & IMAGE_SCN_MEM_EXECUTE) ||
        image.text.rawSize > image.imageSize - image.text.rva) return std::nullopt;

    struct RuntimeFunctionEntry {
        DWORD begin;
        DWORD end;
        DWORD unwind;
    };
    const auto count = image.pdata.rawSize / sizeof(RuntimeFunctionEntry);
    if (!count || count > 1000000) return std::nullopt;
    for (std::size_t i = 0; i < count; ++i) {
        RuntimeFunctionEntry row{};
        if (!copyRva(image, image.pdata.rva +
                static_cast<std::uint32_t>(i * sizeof(row)), &row, sizeof(row))) break;
        if (row.begin < image.text.rva ||
            row.begin >= image.text.rva + image.text.rawSize ||
            row.end <= row.begin ||
            row.end > image.text.rva + image.text.rawSize) continue;
        image.functions.push_back({row.begin, row.end});
    }
    std::sort(image.functions.begin(), image.functions.end(),
        [](const Function& a, const Function& b) { return a.begin < b.begin; });
    image.functions.erase(std::unique(image.functions.begin(), image.functions.end(),
        [](const Function& a, const Function& b) {
            return a.begin == b.begin && a.end == b.end;
        }), image.functions.end());
    if (image.functions.empty()) return std::nullopt;
    return image;
}

const Function* containingFunction(const PeImage& image, std::uint32_t rva) {
    auto it = std::upper_bound(image.functions.begin(), image.functions.end(), rva,
        [](std::uint32_t value, const Function& function) { return value < function.begin; });
    if (it == image.functions.begin()) return nullptr;
    --it;
    return it->begin <= rva && rva < it->end ? &*it : nullptr;
}

std::string hex(std::uint64_t value, unsigned width = 0) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setfill('0');
    if (width) out << std::setw(static_cast<int>(width));
    out << value;
    return out.str();
}

std::string bytesHex(const std::byte* data, std::size_t size) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        if (i) out << ' ';
        out << std::setw(2) << std::to_integer<unsigned>(data[i]);
    }
    return out.str();
}

std::string pathUtf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string timestampUtc() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm value{};
    gmtime_s(&value, &now);
    std::ostringstream out;
    out << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

fs::path fromUtf8(std::string_view value) {
    std::u8string encoded;
    encoded.resize(value.size());
    if (!value.empty()) std::memcpy(encoded.data(), value.data(), value.size());
    return fs::path(encoded);
}

std::uint64_t parseUnsigned(std::string_view token, int base = 0) {
    if (token.empty() || token.front() == '-')
        throw std::invalid_argument("expected an unsigned integer");
    std::size_t consumed = 0;
    const auto value = std::stoull(std::string(token), &consumed, base);
    if (consumed != token.size()) throw std::invalid_argument("invalid integer token");
    return value;
}

bool readFile(const fs::path& path, std::string& result, std::size_t limit,
              std::string& error) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > limit) {
        error = ec ? "file is missing or inaccessible" : "file exceeds size limit";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "file could not be opened"; return false; }
    result.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad() || result.size() != size) {
        error = "file read was incomplete";
        return false;
    }
    return true;
}

void atomicWrite(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".part";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out) throw std::runtime_error("could not write output file");
    }
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temporary, path, ec);
    }
    if (ec) throw std::runtime_error("could not replace output file");
}

std::optional<std::vector<int>> parsePattern(std::string_view pattern) {
    std::istringstream in{std::string(pattern)};
    std::vector<int> bytes;
    std::string token;
    while (in >> token) {
        if (token == "?" || token == "??") bytes.push_back(-1);
        else if (token.size() == 2 &&
                 std::isxdigit(static_cast<unsigned char>(token[0])) &&
                 std::isxdigit(static_cast<unsigned char>(token[1]))) {
            unsigned value{};
            std::istringstream hexIn(token);
            hexIn >> std::hex >> value;
            if (!hexIn || value > 255) return std::nullopt;
            bytes.push_back(static_cast<int>(value));
        } else return std::nullopt;
        if (bytes.size() > kMaxPatternBytes) return std::nullopt;
    }
    if (bytes.empty()) return std::nullopt;
    return bytes;
}

struct MatchResult {
    std::vector<std::size_t> offsets;
    bool capped{};
};

MatchResult findMatches(const std::vector<std::byte>& haystack, std::string_view pattern) {
    MatchResult result;
    const auto parsed = parsePattern(pattern);
    if (!parsed) return result;
    std::size_t anchorAt = 0, anchorSize = 0;
    for (std::size_t i = 0; i < parsed->size();) {
        if ((*parsed)[i] < 0) { ++i; continue; }
        std::size_t j = i;
        while (j < parsed->size() && (*parsed)[j] >= 0) ++j;
        if (j - i > anchorSize) { anchorAt = i; anchorSize = j - i; }
        i = j;
    }
    if (anchorSize < 4 || parsed->size() > haystack.size()) return result;
    const auto* begin = reinterpret_cast<const char*>(haystack.data());
    const auto* end = begin + haystack.size();
    const auto* anchor = begin + anchorAt;
    std::size_t seekFrom = 0;
    while (seekFrom + anchorSize <= haystack.size()) {
        const auto* found = std::search(begin + seekFrom, end, anchor, anchor + anchorSize);
        if (found == end) break;
        const auto position = static_cast<std::size_t>(found - begin);
        seekFrom = position + 1;
        if (position < anchorAt) continue;
        const auto start = position - anchorAt;
        if (start + parsed->size() > haystack.size()) continue;
        bool matches = true;
        for (std::size_t i = 0; i < parsed->size(); ++i) {
            if ((*parsed)[i] >= 0 &&
                std::to_integer<unsigned>(haystack[start + i]) !=
                    static_cast<unsigned>((*parsed)[i])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            result.offsets.push_back(start);
            if (result.offsets.size() > kMaxPatternHits) {
                result.offsets.clear();
                result.capped = true;
                return result;
            }
        }
    }
    return result;
}

std::uint32_t jsonU32(const json& value) {
    if (value.is_string())
        return static_cast<std::uint32_t>(std::stoull(value.get<std::string>(), nullptr, 0));
    return value.get<std::uint32_t>();
}

json actorTypeComponentCandidate() {
    return {
        {"rva", "0xB94CA0"},
        {"signature", "55 56 57 53 48 81 EC ?? ?? ?? ?? 48 8D AC 24 ?? ?? ?? ?? 48 C7 45 00 FE FF FF FF 48 8B 81 90 02 00 00"},
        {"entry_check", "55 56 57 53 48 81 EC 88 00 00 00 48"},
        {"cross_version_signature_available", false},
        {"status", "candidate"},
        {"source", "static candidate from exact 1.26.60.5 REA/Ghidra analysis"},
        {"function_boundary", true},
        {"signature_length", 34},
        {"abi_verified", false},
        {"runtime_verified", false},
        {"abi", "unknown; probe-only"},
        {"candidate_evidence", "REA/Ghidra " + std::string(k126605ActorContextEvidence) + "; RVA 0xB94CA0 references ActorTypeComponent hash 0x4F6BA419 and compares a 32-bit component value at offset 0 with 319. Function identity and owner relationship remain inferred; do not call or hook this function."}
    };
}

std::string hexBytesAt(const PeImage& image, std::uint32_t rva, std::size_t size) {
    std::vector<std::byte> bytes(size);
    if (!size || !copyRva(image, rva, bytes.data(), bytes.size())) return {};
    return bytesHex(bytes.data(), bytes.size());
}

bool suspectedJumpStub(const PeImage& image, std::uint32_t rva) {
    std::array<std::byte, 12> bytes{};
    if (!copyRva(image, rva, bytes.data(), bytes.size())) return false;
    const auto u = [&](std::size_t i) { return std::to_integer<unsigned>(bytes[i]); };
    return u(0) == 0xe9 || u(0) == 0xeb ||
        (u(0) == 0xff && u(1) == 0x25) ||
        (u(0) == 0x48 && u(1) == 0xb8 && u(10) == 0xff && u(11) == 0xe0);
}

const Section* sectionAt(const PeImage& image, std::uint32_t rva) {
    for (const auto& section : image.sections) {
        if (section.rva <= rva && rva - section.rva < section.size()) return &section;
    }
    return nullptr;
}

std::size_t hdeImmediateSize(std::uint32_t flags) {
    std::size_t size = 0;
    if (flags & F_IMM8) size += 1;
    if (flags & F_IMM16) size += 2;
    if (flags & F_IMM32) size += 4;
    if (flags & F_IMM64) size += 8;
    return size;
}

std::string asciiStringAt(const PeImage& image, std::uint32_t rva) {
    if (!image.rdata.size() || rva < image.rdata.rva ||
        rva - image.rdata.rva >= image.rdata.size()) return {};
    const auto count = std::min<std::size_t>(
        128, image.rdata.size() - (rva - image.rdata.rva));
    std::array<std::byte, 128> bytes{};
    if (!copyRva(image, rva, bytes.data(), count)) return {};
    std::string value;
    for (std::size_t i = 0; i < count; ++i) {
        const auto c = std::to_integer<unsigned char>(bytes[i]);
        if (!c) return value.size() >= 4 ? value : std::string{};
        if (c < 0x20 || c > 0x7e) return {};
        value.push_back(static_cast<char>(c));
    }
    return {};
}

json referencesFor(const PeImage& image, const Function& function) {
    constexpr std::size_t maxDecodedBytes = 64 * 1024;
    constexpr std::size_t maxReferences = 128;
    const auto functionSize = static_cast<std::size_t>(function.end - function.begin);
    const auto size = std::min(functionSize, maxDecodedBytes);
    std::vector<std::byte> code(size + 16);
    json result = {{"decoder", "HDE64 length decoder; references are heuristic"},
                   {"references", json::array()},
                   {"decoded_bytes", size},
                   {"function_truncated", functionSize > size}};
    if (!size || !copyRva(image, function.begin, code.data(), code.size())) {
        result["readable"] = false;
        return result;
    }
    result["readable"] = true;
    std::size_t offset = 0;
    while (offset < code.size() && result["references"].size() < maxReferences) {
        hde64s decoded{};
        const auto* instruction = code.data() + offset;
        const auto length = hde64_disasm(instruction, &decoded);
        if (!length || (decoded.flags & F_ERROR)) {
            ++offset;
            continue;
        }
        if (length > size - offset) break;
        std::optional<std::int64_t> target;
        std::string kind;
        const auto instructionRva = static_cast<std::int64_t>(function.begin + offset);
        const auto immediateSize = hdeImmediateSize(decoded.flags);
        if (decoded.modrm_mod == 0 && decoded.modrm_rm == 5 &&
            !decoded.p_67 && (decoded.flags & F_DISP32) &&
            length >= immediateSize + sizeof(std::int32_t)) {
            std::int32_t displacement{};
            std::memcpy(&displacement,
                instruction + length - immediateSize - sizeof(displacement), sizeof(displacement));
            target = instructionRva + length + displacement;
            kind = "rip-relative-memory";
        } else {
            const auto opcode = decoded.opcode;
            const bool rel32 = opcode == 0xe8 || opcode == 0xe9 ||
                (opcode == 0x0f && decoded.opcode2 >= 0x80 && decoded.opcode2 <= 0x8f);
            const bool rel8 = opcode == 0xeb || (opcode >= 0x70 && opcode <= 0x7f);
            if (rel32 && immediateSize >= sizeof(std::int32_t) &&
                length >= immediateSize) {
                std::int32_t displacement{};
                std::memcpy(&displacement, instruction + length - immediateSize,
                            sizeof(displacement));
                target = instructionRva + length + displacement;
                kind = opcode == 0xe8 ? "relative-call" : "relative-branch";
            } else if (rel8 && immediateSize >= sizeof(std::int8_t) &&
                       length >= immediateSize) {
                std::int8_t displacement{};
                std::memcpy(&displacement, instruction + length - immediateSize,
                            sizeof(displacement));
                target = instructionRva + length + displacement;
                kind = "short-branch";
            }
        }
        if (target && *target >= 0 &&
            static_cast<std::uint64_t>(*target) < image.imageSize) {
            const auto targetRva = static_cast<std::uint32_t>(*target);
            const auto* section = sectionAt(image, targetRva);
            json reference = {
                {"instruction_rva", hex(function.begin + static_cast<std::uint32_t>(offset))},
                {"target_rva", hex(targetRva)}, {"kind", kind},
                {"target_section", section ? section->name : std::string("outside-section")}
            };
            if (section && section->name == ".text") {
                if (const auto* callee = containingFunction(image, targetRva))
                    reference["target_function"] = {
                        {"begin_rva", hex(callee->begin)}, {"end_rva", hex(callee->end)},
                        {"entry_bytes", hexBytesAt(image, callee->begin,
                            std::min<std::size_t>(16, callee->end - callee->begin))}
                    };
            } else if (section && section->name == ".rdata") {
                const auto value = asciiStringAt(image, targetRva);
                if (!value.empty()) reference["ascii_string"] = value;
            }
            result["references"].push_back(std::move(reference));
        }
        offset += length;
    }
    result["references_capped"] = result["references"].size() >= maxReferences;
    return result;
}

json contextFor(const PeImage& image, const Function& function,
                const std::vector<std::pair<std::size_t, std::string>>& hits) {
    json result = {
        {"function", {{"begin_rva", hex(function.begin)},
                      {"end_rva", hex(function.end)},
                      {"size", function.end - function.begin},
                      {"entry_bytes", hexBytesAt(image, function.begin,
                          std::min<std::size_t>(32, function.end - function.begin))}}},
        {"anchor_hits", json::array()},
        {"nearby_functions", json::array()},
        {"code_references", referencesFor(image, function)}
    };
    for (const auto& [offset, pattern] : hits) {
        const auto rva = static_cast<std::uint32_t>(function.begin + offset);
        const auto before = std::min<std::uint32_t>(8, rva - function.begin);
        const auto after = std::min<std::uint32_t>(24, function.end - rva);
        result["anchor_hits"].push_back({
            {"rva", hex(rva)}, {"function_offset", offset}, {"pattern", pattern},
            {"bytes_before", hexBytesAt(image, rva - before, before)},
            {"bytes_after", hexBytesAt(image, rva, after)}
        });
    }
    const auto index = static_cast<std::size_t>(&function - image.functions.data());
    const auto first = index > 2 ? index - 2 : 0;
    const auto last = std::min(image.functions.size(), index + 3);
    for (auto i = first; i < last; ++i) {
        if (i == index) continue;
        const auto& nearby = image.functions[i];
        result["nearby_functions"].push_back({
            {"begin_rva", hex(nearby.begin)}, {"end_rva", hex(nearby.end)},
            {"distance_from_candidate", nearby.begin > function.begin
                ? static_cast<std::int64_t>(nearby.begin - function.begin)
                : -static_cast<std::int64_t>(function.begin - nearby.begin)},
            {"entry_bytes", hexBytesAt(image, nearby.begin,
                std::min<std::size_t>(16, nearby.end - nearby.begin))}
        });
    }
    return result;
}

using PortDefinition = std::pair<std::string, json>;

void collectNativeDefinitions(const json& node, const std::string& path,
                              std::vector<PortDefinition>& output,
                              std::size_t depth = 0) {
    if (!node.is_object() || depth > 64 || output.size() >= kMaxSymbols) return;
    const bool hasAnchors = node.contains("signature_anchors") &&
        node["signature_anchors"].is_array() && !node["signature_anchors"].empty();
    const bool hasSignature = !node.value("signature", std::string{}).empty();
    const bool hasEntryCheck = !node.value("entry_check", std::string{}).empty();
    if (!path.empty() && node.contains("rva") &&
        (hasAnchors || hasSignature || hasEntryCheck)) {
        output.emplace_back(path, node);
        return;
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (output.size() >= kMaxSymbols) return;
        const auto childPath = path.empty() ? it.key() : path + "." + it.key();
        collectNativeDefinitions(it.value(), childPath, output, depth + 1);
    }
}

std::vector<PortDefinition> sourceDefinitions(const json& profile) {
    std::vector<PortDefinition> definitions;
    if (profile.contains("symbols") && profile["symbols"].is_object()) {
        for (auto it = profile["symbols"].begin(); it != profile["symbols"].end(); ++it) {
            if (it.value().is_object()) definitions.emplace_back(it.key(), it.value());
            if (definitions.size() >= kMaxSymbols) return definitions;
        }
    }
    for (auto it = profile.begin(); it != profile.end(); ++it) {
        if (!it.key().starts_with("native_")) continue;
        collectNativeDefinitions(it.value(), it.key(), definitions);
        if (definitions.size() >= kMaxSymbols) break;
    }
    return definitions;
}

const HookSpec* hookSpec(std::string_view symbol) {
    const auto it = std::find_if(std::begin(kHookSpecs), std::end(kHookSpecs),
        [&](const HookSpec& spec) { return symbol == spec.symbol; });
    return it == std::end(kHookSpecs) ? nullptr : it;
}

json moduleIdentityJson(const PeImage& image, const bedrock::GameIdentity& identity) {
    return {
        {"module", "Minecraft.Windows.exe"}, {"loaded_base", hex(image.base)},
        {"image_size", image.imageSize}, {"pe_timestamp", hex(image.timestamp)},
        {"text_rva", hex(identity.textRva)}, {"text_size", identity.textSize},
        {"file_sha256", identity.fileSha256}, {"text_sha256", identity.textSha256},
        {"pdata_functions", image.functions.size()}
    };
}

struct HookState {
    BDS_HookToken token{};
    BDS_HookPoint point{};
    std::string symbol;
    std::atomic<std::uint64_t> calls{};
    std::atomic<std::int64_t> lastSampleNs{};
    bool actorContextCandidate{};
    std::uint32_t actorRegistryOffset{};
    std::uint32_t actorEntityIdOffset{};
    std::string actorContextEvidence;
    std::mutex mutex;
    json latest = json::object();
    bool dirty{};
};

class CheckSigMod {
public:
    void run() noexcept {
        try {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            if (!pinModule()) throw std::runtime_error("failed to pin CheckSig while active");
            const auto documents = documentsDirectory();
            output_ = documents / L"PEXData" / L"CheckSig";
            commands_ = output_ / L"commands";
            fs::create_directories(commands_);
            std::string identityError;
            if (!bedrock::inspectLoadedGame(identity_, identityError))
                throw std::runtime_error("Minecraft module identity failed: " + identityError);
            image_ = inspectPe();
            if (!image_) throw std::runtime_error("could not safely inspect the loaded x64 PE image");
            if (identity_.textRva != image_->text.rva ||
                identity_.textSize != image_->text.rawSize)
                throw std::runtime_error("loaded PE .text metadata disagrees with the SDK fingerprint");
            writeEvent("started", {
                {"identity", moduleIdentityJson(*image_, identity_)},
                {"hook_runtime", BDS_HookRuntimeVersion()},
                {"hook_runtime_abi", BDS_HookRuntimeAbi()}
            });
            writeStatus("running");

            auto lastStatus = std::chrono::steady_clock::now();
            while (!stopping_.load()) {
                processCommands();
                const auto now = std::chrono::steady_clock::now();
                if (now - lastStatus >= std::chrono::seconds(1)) {
                    flushHookState();
                    writeStatus("running");
                    lastStatus = now;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            unsubscribeAll();
            flushHookState();
            writeStatus("stopped");
        } catch (const std::exception& error) {
            try {
                if (output_.empty()) output_ = documentsDirectory() / L"PEXData" / L"CheckSig";
                fs::create_directories(output_);
                writeEvent("startup-error", {{"message", error.what()}});
                writeStatus("error", error.what());
            } catch (...) {}
        } catch (...) {
            try {
                writeEvent("startup-error", {{"message", "unknown error"}});
                writeStatus("error", "unknown error");
            } catch (...) {}
        }
    }

    void requestStop() noexcept { stopping_.store(true); }

private:
    fs::path output_;
    fs::path commands_;
    std::optional<PeImage> image_;
    bedrock::GameIdentity identity_;
    std::string sourcePath_;
    json sourceProfile_;
    std::string targetPath_;
    json targetProfile_;
    std::unique_ptr<bedrock::Resolver> resolver_;
    std::map<std::string, std::unique_ptr<HookState>> hooks_;
    std::map<fs::path, std::pair<fs::file_time_type, std::uintmax_t>> commandFileState_;
    std::atomic<bool> stopping_{};
    std::mutex fileMutex_;
    std::mutex hookMapMutex_;

    static fs::path documentsDirectory() {
        PWSTR path{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &path)))
            throw std::runtime_error("Windows Documents directory is unavailable");
        fs::path result(path);
        CoTaskMemFree(path);
        return result;
    }

    bool pinModule() {
        HMODULE current{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&worker), &current)) return false;
        return true;
    }

    void writeEvent(const std::string& type, json data) {
        std::lock_guard lock(fileMutex_);
        fs::create_directories(output_);
        data["time_utc"] = timestampUtc();
        data["type"] = type;
        std::ofstream out(output_ / L"events.jsonl", std::ios::binary | std::ios::app);
        out << data.dump() << '\n';
        if (!out) throw std::runtime_error("could not append events.jsonl");
    }

    void writeStatus(std::string_view state, std::string_view error = {}) {
        json status = {
            {"schema", 1}, {"tool", "CheckSig"}, {"state", state},
            {"pid", GetCurrentProcessId()}, {"time_utc", timestampUtc()}
        };
        if (image_) status["identity"] = moduleIdentityJson(*image_, identity_);
        if (!error.empty()) status["error"] = error;
        status["hooks"] = json::array();
        {
            std::lock_guard lock(hookMapMutex_);
            for (const auto& [name, holder] : hooks_) {
                json entry = {{"symbol", name}, {"installed", holder->token != nullptr},
                              {"calls", holder->calls.load(std::memory_order_relaxed)}};
                std::lock_guard stateLock(holder->mutex);
                if (!holder->latest.empty()) entry["latest_values"] = holder->latest;
                status["hooks"].push_back(std::move(entry));
            }
        }
        std::lock_guard lock(fileMutex_);
        atomicWrite(output_ / L"status.json", status.dump(2) + "\n");
    }

    static std::string pointerString(const void* value) {
        return hex(reinterpret_cast<std::uintptr_t>(value));
    }

    static void setLatest(HookState& state, json values) noexcept {
        try {
            std::lock_guard lock(state.mutex);
            state.latest = std::move(values);
            state.dirty = true;
        } catch (...) {}
    }

    static void BDS_HOOK_CALL observeHook(void* user, void* raw,
                                           BDS_HookNext next, void* frame) noexcept {
        auto* state = static_cast<HookState*>(user);
        if (next) next(frame, raw);
        if (!state) return;
        state->calls.fetch_add(1, std::memory_order_relaxed);
        const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto previous = state->lastSampleNs.load(std::memory_order_relaxed);
        if (nowNs - previous < 250'000'000 ||
            !state->lastSampleNs.compare_exchange_strong(
                previous, nowNs, std::memory_order_relaxed)) return;
        try {
            json values = json::object();
            switch (state->point) {
            case BDS_HOOK_KEYBOARD_FEED: {
                const auto* a = static_cast<const BDS_KeyboardArgs*>(raw);
                if (a) values = {{"key", a->key}, {"down", a->down}, {"result", a->result}};
                break;
            }
            case BDS_HOOK_MOUSE_FEED: {
                const auto* a = static_cast<const BDS_MouseArgs*>(raw);
                if (a) values = {{"self", pointerString(a->self)}, {"action", a->action},
                    {"state", a->state}, {"x", a->x}, {"y", a->y}, {"dx", a->dx},
                    {"dy", a->dy}, {"force_motionless_pointer", a->force_motionless_pointer}};
                break;
            }
            case BDS_HOOK_GRAB_CURSOR:
            case BDS_HOOK_RELEASE_CURSOR: {
                const auto* a = static_cast<const BDS_CursorArgs*>(raw);
                if (a) values = {{"self", pointerString(a->self)}};
                break;
            }
            case BDS_HOOK_GET_FOV: {
                const auto* a = static_cast<const BDS_FovArgs*>(raw);
                if (a) values = {{"self", pointerString(a->self)},
                    {"input_fov", a->input_fov}, {"result", a->result}};
                break;
            }
            case BDS_HOOK_LEVEL_SUB_TICK: {
                const auto* a = static_cast<const BDS_LevelArgs*>(raw);
                if (a) values = {{"level", pointerString(a->level)}};
                break;
            }
            case BDS_HOOK_LEVEL_RUNTIME_ACTOR_LIST: {
                const auto* a = static_cast<const BDS_RuntimeActorListArgs*>(raw);
                if (!a) break;
                values = {{"level", pointerString(a->level)},
                          {"output_buffer", pointerString(a->output)}};
                std::array<std::uintptr_t, 3> vector{};
                const bool readable = safeRead(reinterpret_cast<std::uintptr_t>(a->output),
                                               vector.data(), sizeof(vector));
                values["vector_header_readable"] = readable;
                if (!readable) break;
                const auto begin = vector[0];
                const auto end = vector[1];
                const auto capacity = vector[2];
                const bool shapeValid = begin <= end && end <= capacity &&
                    (end - begin) % sizeof(void*) == 0 && capacity - begin <= (1u << 20);
                values["vector_bounds_valid"] = shapeValid;
                if (!shapeValid) break;
                const auto count = (end - begin) / sizeof(void*);
                values["actor_count"] = count;
                json actors = json::array();
                json actorContexts = json::array();
                for (std::size_t i = 0; i < std::min<std::size_t>(count, 8); ++i) {
                    std::uintptr_t actor{};
                    if (!safeRead(begin + i * sizeof(void*), &actor, sizeof(actor))) break;
                    actors.push_back(hex(actor));
                    if (state->actorContextCandidate) {
                        std::uintptr_t registry{};
                        std::uint32_t entityId{};
                        const bool registryRead = safeReadAt(actor,
                            state->actorRegistryOffset, &registry, sizeof(registry));
                        const bool entityIdRead = safeReadAt(actor,
                            state->actorEntityIdOffset, &entityId, sizeof(entityId));
                        json context = {
                            {"actor", hex(actor)},
                            {"registry_read", registryRead},
                            {"entity_id_read", entityIdRead}
                        };
                        if (registryRead) context["registry_pointer"] = hex(registry);
                        if (entityIdRead) context["entity_id"] = hex(entityId);
                        actorContexts.push_back(std::move(context));
                    }
                }
                values["sample_actor_pointers"] = std::move(actors);
                values["sample_limit"] = 8;
                if (state->actorContextCandidate) {
                    values["actor_context_layout_status"] = "candidate";
                    values["actor_context_evidence"] = state->actorContextEvidence;
                    values["actor_context_offsets_from_actor"] = {
                        {"registry_pointer", hex(state->actorRegistryOffset)},
                        {"entity_id", hex(state->actorEntityIdOffset)}
                    };
                    values["sample_actor_contexts"] = std::move(actorContexts);
                }
                break;
            }
            case BDS_HOOK_ACTOR_GET_NAME_TAG: {
                const auto* a = static_cast<const BDS_ActorNameTagArgs*>(raw);
                if (a) values = {{"actor", pointerString(a->actor)},
                                {"name_tag_pointer", pointerString(a->name_tag)}};
                break;
            }
            case BDS_HOOK_GAMECORE_KEYBOARD_EVENT: {
                const auto* a = static_cast<const BDS_GameCoreKeyboardEventArgs*>(raw);
                if (a) values = {{"self", pointerString(a->self)},
                    {"output", pointerString(a->output)}, {"context", pointerString(a->context)},
                    {"result", pointerString(a->result)}};
                break;
            }
            case BDS_HOOK_CLIENT_PLAYER_LOADED: {
                const auto* a = static_cast<const BDS_ClientPlayerLoadedArgs*>(raw);
                if (a) values = {{"self", pointerString(a->self)},
                                {"player", pointerString(a->player)}};
                break;
            }
            case BDS_HOOK_CLIENT_LEVEL_EXIT:
            case BDS_HOOK_CLIENT_DIMENSION_CHANGED:
            case BDS_HOOK_CLIENT_JOIN_CANCELLED: {
                const auto* a = static_cast<const BDS_ClientWorldArgs*>(raw);
                if (a) values = {{"self", pointerString(a->self)}};
                break;
            }
            default:
                break;
            }
            values["call_number"] = state->calls.load(std::memory_order_relaxed);
            setLatest(*state, std::move(values));
        } catch (...) {}
    }

    void flushHookState() {
        std::lock_guard mapLock(hookMapMutex_);
        for (const auto& [name, holder] : hooks_) {
            json value;
            bool dirty = false;
            {
                std::lock_guard stateLock(holder->mutex);
                dirty = holder->dirty;
                if (dirty) {
                    value = holder->latest;
                    holder->dirty = false;
                }
            }
            if (dirty) {
                value["symbol"] = name;
                value["total_calls"] = holder->calls.load(std::memory_order_relaxed);
                writeEvent("hook-observation", std::move(value));
            }
        }
    }

    bool loadProfileText(const std::string& text, std::string path,
                         std::string& error) {
        auto resolver = std::make_unique<bedrock::Resolver>();
        if (!resolver->loadJson(text, error)) return false;
        json document;
        try { document = json::parse(text); }
        catch (const std::exception& e) { error = e.what(); return false; }
        const auto& target = document.at("target");
        if (identity_.fileSha256.size() == 64 &&
            lower(target.at("sha256").get<std::string>()) != lower(identity_.fileSha256)) {
            error = "profile file SHA-256 does not match the loaded executable";
            return false;
        }
        if (!resolver->attachWindowsMainModule(identity_, error) || !resolver->exactBuild()) {
            if (error.empty()) error = "profile does not match the loaded executable";
            return false;
        }
        targetProfile_ = std::move(document);
        targetPath_ = std::move(path);
        resolver_ = std::move(resolver);
        return true;
    }

    void loadSource(const std::string& argument) {
        const auto path = fromUtf8(argument);
        std::string text, error;
        if (!readFile(path, text, kMaxProfileBytes, error))
            throw std::runtime_error("source profile: " + error);
        auto parsed = json::parse(text);
        if (!parsed.contains("symbols") || !parsed["symbols"].is_object() ||
            parsed["symbols"].size() > kMaxSymbols)
            throw std::runtime_error("source profile has no usable symbols object");
        const auto definitions = sourceDefinitions(parsed);
        std::size_t anchorCount = 0;
        for (const auto& [name, symbol] : definitions) {
            (void)name;
            if (!symbol.is_object()) continue;
            if (symbol.contains("signature_anchors") &&
                symbol["signature_anchors"].is_array()) {
                anchorCount += symbol["signature_anchors"].size();
            }
            if ((!symbol.contains("signature_anchors") ||
                 !symbol["signature_anchors"].is_array() ||
                 symbol["signature_anchors"].empty()) &&
                !symbol.value("signature", std::string{}).empty()) {
                ++anchorCount;
            } else if ((!symbol.contains("signature_anchors") ||
                        !symbol["signature_anchors"].is_array() ||
                        symbol["signature_anchors"].empty()) &&
                       symbol.value("signature", std::string{}).empty() &&
                       !symbol.value("entry_check", std::string{}).empty()) {
                ++anchorCount;
            }
            if (anchorCount > 1024)
                throw std::runtime_error("source profile has over 1024 anchors; split it into smaller profiles");
        }
        sourcePath_ = pathUtf8(path);
        sourceProfile_ = std::move(parsed);
        writeEvent("source-loaded", {{"path", sourcePath_},
            {"version", sourceProfile_.value("target", json::object())
                .value("version", std::string{})},
            {"symbols", sourceProfile_["symbols"].size()},
            {"portable_definitions", definitions.size()}});
    }

    std::vector<std::byte> copyText() const {
        if (!image_ || !identity_.textSize || identity_.textSize > kMaxTextBytes)
            throw std::runtime_error("invalid .text size");
        std::vector<std::byte> text(identity_.textSize);
        if (!copyRva(*image_, identity_.textRva, text.data(), text.size()))
            throw std::runtime_error("could not read the loaded .text safely");
        return text;
    }

    void portSource(std::string version) {
        if (sourceProfile_.is_null())
            throw std::runtime_error("load an old definition first with source <path>");
        if (identity_.fileSha256.size() != 64)
            throw std::runtime_error("the EXE file hash is unavailable; candidate profile was not emitted");
        const auto text = copyText();
        json symbols = json::object();
        json support = json::object();
        json warnings = json::array();
        const auto definitions = sourceDefinitions(sourceProfile_);
        if (definitions.empty())
            throw std::runtime_error("source profile has no portable function definitions");
        const auto totalSymbols = definitions.size();
        std::size_t processedSymbols = 0;

        for (const auto& [name, oldSymbol] : definitions) {
            ++processedSymbols;
            if (processedSymbols % 5 == 0 || processedSymbols == totalSymbols)
                writeEvent("port-progress", {
                    {"symbols_done", processedSymbols}, {"symbols_total", totalSymbols},
                    {"candidates_found", symbols.size()}
                });
            if (!oldSymbol.is_object()) {
                warnings.push_back(name + ": source symbol entry is not an object");
                continue;
            }
            std::vector<json> anchors;
            if (oldSymbol.contains("signature_anchors") &&
                oldSymbol["signature_anchors"].is_array()) {
                for (const auto& anchor : oldSymbol["signature_anchors"])
                    if (anchor.is_object() && !anchor.value("pattern", std::string{}).empty())
                        anchors.push_back(anchor);
            }
            if (anchors.empty() && !oldSymbol.value("signature", std::string{}).empty())
                anchors.push_back({{"pattern", oldSymbol["signature"]}, {"offset", 0}});
            if (anchors.empty() && !oldSymbol.value("entry_check", std::string{}).empty())
                anchors.push_back({{"pattern", oldSymbol["entry_check"]},
                                   {"offset", 0}, {"kind", "source-entry-check"}});
            if (anchors.empty()) {
                warnings.push_back(name + ": no source signature anchors");
                continue;
            }

            std::map<std::uint32_t, std::set<std::size_t>> votes;
            std::map<std::uint32_t, std::vector<std::pair<std::size_t, std::string>>> matched;
            std::size_t capped = 0;
            for (std::size_t ai = 0; ai < anchors.size(); ++ai) {
                const auto pattern = anchors[ai].value("pattern", std::string{});
                const auto parsed = parsePattern(pattern);
                if (!parsed || parsed->size() > 128) {
                    warnings.push_back(name + ": anchor " + std::to_string(ai) +
                                       " is invalid or longer than 128 bytes");
                    continue;
                }
                const auto hits = findMatches(text, pattern);
                if (hits.capped) {
                    ++capped;
                    warnings.push_back(name + ": anchor " + std::to_string(ai) +
                                       " exceeded the match cap and was ignored");
                    continue;
                }
                for (const auto offset : hits.offsets) {
                    const auto rva64 = static_cast<std::uint64_t>(identity_.textRva) + offset;
                    if (rva64 > UINT32_MAX) continue;
                    const auto rva = static_cast<std::uint32_t>(rva64);
                    const auto* function = containingFunction(*image_, rva);
                    if (!function || parsed->size() > function->end - rva) continue;
                    votes[function->begin].insert(ai);
                    matched[function->begin].emplace_back(rva - function->begin, pattern);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (votes.empty()) {
                warnings.push_back(name + ": no signature hit mapped to a .pdata function");
                continue;
            }
            std::vector<std::pair<std::uint32_t, std::set<std::size_t>>> ranked(
                votes.begin(), votes.end());
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
                if (a.second.size() != b.second.size()) return a.second.size() > b.second.size();
                return a.first < b.first;
            });
            const auto topScore = ranked.front().second.size();
            if (ranked.size() > 1 && ranked[1].second.size() == topScore) {
                warnings.push_back(name + ": tied .pdata function candidates; omitted");
                support[name] = {{"state", "ambiguous"}, {"top_score", topScore},
                                 {"candidate_count", ranked.size()}, {"capped_anchors", capped}};
                continue;
            }
            const auto functionRva = ranked.front().first;
            const auto* function = containingFunction(*image_, functionRva);
            if (!function || function->begin != functionRva) {
                warnings.push_back(name + ": selected address is not a function entry");
                continue;
            }
            const auto guardSize = std::min<std::size_t>(16, function->end - function->begin);
            const auto entry = hexBytesAt(*image_, function->begin, guardSize);
            if (entry.empty()) {
                warnings.push_back(name + ": could not read selected function entry");
                continue;
            }
            const auto matchedCount = ranked.front().second.size();
            json matchedIndices = json::array();
            for (const auto index : ranked.front().second) matchedIndices.push_back(index);
            const auto sourceVersion = sourceProfile_.value("target", json::object())
                .value("version", std::string("unknown"));
            const std::string origin = "CheckSig anchor port from " + sourceVersion +
                " (" + std::to_string(matchedCount) + "/" +
                std::to_string(anchors.size()) + " anchors; candidate only)";
            symbols[name] = {
                {"rva", hex(function->begin)}, {"signature", ""},
                {"entry_check", entry}, {"cross_version_signature_available", false},
                {"status", "candidate"}, {"source", origin},
                {"function_boundary", true}, {"signature_length", 0},
                {"abi_verified", false}, {"runtime_verified", false},
                {"source_abi", oldSymbol.value("abi", std::string{})},
                {"source_status", oldSymbol.value("status", std::string("candidate"))},
                {"source_description", oldSymbol.value("source", std::string{})},
                {"source_definition_path", name},
                {"source_evidence", oldSymbol.value("evidence", json(nullptr))},
                {"source_rva", oldSymbol.value("rva", json(nullptr))},
                {"checksig_evidence", contextFor(*image_, *function, matched[functionRva])}
            };
            support[name] = {
                {"state", "candidate"}, {"matched_anchors", matchedCount},
                {"available_anchors", anchors.size()}, {"capped_anchors", capped},
                {"candidate_functions", ranked.size()}, {"rva", hex(functionRva)},
                {"matched_anchor_indices", std::move(matchedIndices)}
            };
            if (matchedCount < 2 && anchors.size() > 1)
                warnings.push_back(name + ": weak candidate, one anchor supported the function");
        }

        if (version.empty()) version = "unknown";
        json diagnosticCandidates = json::object();
        if (lower(identity_.fileSha256) == k126605ExeSha256) {
            diagnosticCandidates["actor_context"] = {
                {"status", "candidate"},
                {"runtime_verified", false},
                {"registry_pointer_offset_from_actor", "0x10"},
                {"entity_id_offset_from_actor", "0x18"},
                {"actor_type_component_hash", "0x4F6BA419"},
                {"actor_type_value_offset", "0x0"},
                {"actor_type_value_size", 4},
                {"player_type_id", 319},
                {"evidence_id", k126605ActorContextEvidence},
                {"evidence", "Exact 1.26.60.5 EXE static Ghidra candidate at RVA 0xB94CA0 reads the registry pointer and 32-bit entity id at Actor+0x10/+0x18, then resolves ActorTypeComponent 0x4F6BA419 and compares its 32-bit value at +0 with 319. Treating the function argument as Actor* and its relation to Level::getRuntimeActorList entries are static inferences; runtime behavior is unverified."}
            };
            const std::string actorTypeName = "candidate::ActorTypeComponent";
            if (!symbols.contains(actorTypeName)) {
                symbols[actorTypeName] = actorTypeComponentCandidate();
                support[actorTypeName] = {
                    {"state", "static-candidate"}, {"rva", "0xB94CA0"},
                    {"abi_known", false}, {"probe_only", true},
                    {"evidence_id", k126605ActorContextEvidence}
                };
            }
        }
        if (targetProfile_.is_object() && targetProfile_.contains("target") &&
            targetProfile_.contains("symbols") && targetProfile_["symbols"].is_object() &&
            lower(targetProfile_["target"].value("sha256", std::string{})) ==
                lower(identity_.fileSha256)) {
            json mergedSymbols = targetProfile_["symbols"];
            for (auto it = symbols.begin(); it != symbols.end(); ++it) {
                const auto& name = it.key();
                if (!mergedSymbols.contains(name)) {
                    mergedSymbols[name] = it.value();
                    continue;
                }
                const auto& existing = mergedSymbols[name];
                const bool sameRva = existing.is_object() && existing.contains("rva") &&
                    jsonU32(existing.at("rva")) == jsonU32(it.value().at("rva"));
                if (sameRva) {
                    mergedSymbols[name]["checksig_cross_version_port"] =
                        support.value(name, json::object());
                    continue;
                }
                std::string alias = "candidate::cross_version_port::" + name;
                if (mergedSymbols.contains(alias))
                    alias += "::" + it.value().value("rva", std::string("unknown"));
                mergedSymbols[alias] = it.value();
                warnings.push_back(name + ": target definition kept; cross-version port saved as " + alias);
            }
            symbols = std::move(mergedSymbols);
        }
        json output = {
            {"schema", 1},
            {"target", {
                {"product", "minecraft-bedrock"}, {"kind", "client"},
                {"platform", "windows"}, {"arch", "x64"}, {"version", version},
                {"sha256", identity_.fileSha256},
                {"text_sha256", identity_.textSha256},
                {"image_base", hex(image_->base)}, {"size_of_image", image_->imageSize},
                {"text_rva", hex(identity_.textRva)}, {"text_size", identity_.textSize}
            }},
            {"symbols", std::move(symbols)},
            {"diagnostic_candidates", std::move(diagnosticCandidates)},
            {"generation", {
                {"generator", "CheckSig"}, {"generated_utc", timestampUtc()},
                {"source_profile", sourcePath_},
                {"source_version", sourceProfile_.value("target", json::object())
                    .value("version", std::string{})},
                {"warnings", std::move(warnings)}, {"port_support", std::move(support)}
            }},
            {"analysis", {
                {"scope", "loaded Minecraft.Windows.exe .text and .pdata"},
                {"candidate_only", true}, {"writes_game_memory", false},
                {"note", "A signature hit does not verify semantics or ABI."}
            }}
        };
        const auto path = output_ / L"candidate-profile.json";
        const auto serialized = output.dump(2) + "\n";
        atomicWrite(path, serialized);
        std::string loadError;
        if (!loadProfileText(serialized, pathUtf8(path), loadError))
            throw std::runtime_error("candidate profile saved, but identity attach failed: " + loadError);
        writeEvent("port-complete", {
            {"path", pathUtf8(path)}, {"candidate_count", targetProfile_["symbols"].size()},
            {"warnings", targetProfile_["generation"]["warnings"]},
            {"identity", moduleIdentityJson(*image_, identity_)}
        });
    }

    static std::string commandArgument(const std::string& line, std::string_view command) {
        auto value = std::string_view(line).substr(command.size());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.remove_prefix(1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.remove_suffix(1);
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        return std::string(value);
    }

    void processCommands() {
        if (commands_.empty() || !fs::exists(commands_)) return;
        std::vector<fs::path> pending;
        std::error_code ec;
        for (fs::directory_iterator it(commands_, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || lower(it->path().extension().string()) != ".txt")
                continue;
            const auto modified = it->last_write_time(ec);
            if (ec) { ec.clear(); continue; }
            const auto size = it->file_size(ec);
            if (ec) { ec.clear(); continue; }
            const auto state = std::make_pair(modified, size);
            const auto [saved, inserted] = commandFileState_.emplace(it->path(), state);
            if (inserted || saved->second != state) {
                saved->second = state;
                continue;
            }
            pending.push_back(it->path());
        }
        std::sort(pending.begin(), pending.end());
        for (const auto& path : pending) {
            if (stopping_.load()) break;
            std::string line, error;
            if (!readFile(path, line, 4096, error)) {
                if (error == "file read was incomplete" || error == "file could not be opened")
                    continue;
                writeEvent("command-error", {{"file", pathUtf8(path)}, {"message", error}});
                fs::remove(path, ec);
                commandFileState_.erase(path);
                continue;
            }
            fs::remove(path, ec);
            commandFileState_.erase(path);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (line.empty()) continue;
            try { execute(line); }
            catch (const std::exception& e) {
                writeEvent("command-error", {{"command", line}, {"message", e.what()}});
            } catch (...) {
                writeEvent("command-error", {{"command", line}, {"message", "unknown error"}});
            }
        }
    }

    void execute(const std::string& line) {
        std::istringstream input(line);
        std::string command;
        input >> command;
        if (command == "help") {
            writeEvent("help", {{"commands", json::array({
                "identity", "source <old-profile.json>", "profile <target-profile.json>",
                "port [version]", "list", "probe <symbol>", "hook <symbol>",
                "unhook <symbol>", "scan <masked-pattern>",
                "read <VA|rva:0xRVA> <type>", "quit"
            })}});
        } else if (command == "identity") {
            writeEvent("identity", {{"identity", moduleIdentityJson(*image_, identity_)}});
        } else if (command == "source") {
            const auto argument = commandArgument(line, command);
            if (argument.empty()) throw std::runtime_error("usage: source <old-profile.json>");
            loadSource(argument);
        } else if (command == "profile") {
            const auto argument = commandArgument(line, command);
            if (argument.empty()) throw std::runtime_error("usage: profile <target-profile.json>");
            const auto path = fromUtf8(argument);
            std::string text, error;
            if (!readFile(path, text, kMaxProfileBytes, error))
                throw std::runtime_error("target profile: " + error);
            if (!loadProfileText(text, argument, error))
                throw std::runtime_error("target profile identity mismatch: " + error);
            writeEvent("profile-loaded", {{"path", argument},
                {"symbols", targetProfile_["symbols"].size()}, {"exact_build", true}});
        } else if (command == "port") {
            std::string version;
            input >> version;
            portSource(version);
        } else if (command == "list") {
            if (!resolver_) throw std::runtime_error("load a target profile first with profile or port");
            json entries = json::array();
            for (auto it = targetProfile_["symbols"].begin();
                 it != targetProfile_["symbols"].end(); ++it) {
                const auto& symbolEntry = it.value();
                const auto rva = jsonU32(symbolEntry.at("rva"));
                const auto resolution = resolver_->resolve(it.key(), true);
                const auto* function = containingFunction(*image_, rva);
                const auto* hook = hookSpec(it.key());
                const auto target = reinterpret_cast<void*>(image_->base + rva);
                const bool exactGuard = resolution && resolution->method == "exact-rva" &&
                    resolution->address == image_->base + rva;
                const bool functionEntry = rva >= identity_.textRva &&
                    rva < identity_.textRva + identity_.textSize && function &&
                    function->begin == rva;
                const bool runtimeSupports = hook && BDS_HookRuntimeSupports(hook->point) != 0;
                const bool targetOwned = hook && BDS_HookOwnsTarget(hook->point, target) != 0;
                const bool jumpStub = suspectedJumpStub(*image_, rva);
                const bool hookReady = exactGuard && functionEntry && runtimeSupports &&
                    (!jumpStub || targetOwned);
                entries.push_back({
                    {"symbol", it.key()}, {"rva", hex(rva)},
                    {"status", symbolEntry.value("status", std::string("candidate"))},
                    {"abi_verified", symbolEntry.value("abi_verified", false)},
                    {"runtime_verified", symbolEntry.value("runtime_verified", false)},
                    {"source", symbolEntry.value("source", std::string{})},
                    {"candidate_evidence", symbolEntry.value("candidate_evidence", std::string{})},
                    {"entry_guard_hit", exactGuard},
                    {"pdata_function_entry", functionEntry},
                    {"hook_point", hook ? json(static_cast<unsigned>(hook->point)) : json(nullptr)},
                    {"hook_runtime_supports_point", runtimeSupports},
                    {"entry_looks_like_jump_stub", jumpStub},
                    {"target_already_owned_by_sdk", targetOwned},
                    {"hook_ready_for_explicit_test", hookReady}
                });
            }
            writeEvent("candidate-list", {
                {"exact_build", resolver_->exactBuild()},
                {"symbol_count", entries.size()}, {"symbols", std::move(entries)}
            });
        } else if (command == "probe") {
            std::string symbol; input >> symbol;
            if (symbol.empty()) throw std::runtime_error("usage: probe <symbol>");
            probe(symbol);
        } else if (command == "hook") {
            std::string symbol; input >> symbol;
            if (symbol.empty()) throw std::runtime_error("usage: hook <symbol>");
            installHook(symbol);
        } else if (command == "unhook") {
            std::string symbol; input >> symbol;
            if (symbol.empty()) throw std::runtime_error("usage: unhook <symbol>");
            removeHook(symbol);
        } else if (command == "scan") {
            const auto pattern = commandArgument(line, command);
            const auto parsed = parsePattern(pattern);
            if (!parsed || parsed->size() > 128)
                throw std::runtime_error("usage: scan <masked-pattern, 4..128 bytes>");
            scanPattern(pattern);
        } else if (command == "read") {
            std::string address, type; input >> address >> type;
            if (address.empty() || type.empty())
                throw std::runtime_error("usage: read <VA|rva:0xRVA> <supported-type>");
            readValue(address, type);
        } else if (command == "quit") {
            writeEvent("stopping", {{"reason", "quit command"}});
            stopping_.store(true);
        } else {
            throw std::runtime_error("unknown command; write help to commands/next.txt");
        }
    }

    void scanPattern(const std::string& pattern) {
        const auto text = copyText();
        const auto found = findMatches(text, pattern);
        if (found.capped) {
            writeEvent("pattern-scan", {
                {"pattern", pattern}, {"state", "over-cap"},
                {"match_cap", kMaxPatternHits}, {"candidate_functions", json::array()},
                {"note", "More than the bounded hit cap; no partial results were accepted."}
            });
            return;
        }
        std::map<std::uint32_t, std::vector<std::uint32_t>> byFunction;
        json outsideFunction = json::array();
        for (const auto offset : found.offsets) {
            const auto rva = static_cast<std::uint32_t>(identity_.textRva + offset);
            if (const auto* function = containingFunction(*image_, rva))
                byFunction[function->begin].push_back(rva);
            else if (outsideFunction.size() < 32)
                outsideFunction.push_back(hex(rva));
        }
        json candidates = json::array();
        std::size_t emitted = 0;
        for (const auto& [begin, hitRvas] : byFunction) {
            if (emitted++ >= 64) break;
            const auto* function = containingFunction(*image_, begin);
            if (!function) continue;
            std::vector<std::pair<std::size_t, std::string>> matched;
            json samples = json::array();
            for (std::size_t i = 0; i < hitRvas.size(); ++i) {
                if (i < 16) {
                    matched.emplace_back(hitRvas[i] - begin, pattern);
                    samples.push_back(hex(hitRvas[i]));
                }
            }
            json matchingSymbols = json::array();
            if (targetProfile_.contains("symbols") && targetProfile_["symbols"].is_object()) {
                for (const auto& [name, symbol] : targetProfile_["symbols"].items()) {
                    if (symbol.is_object() && symbol.contains("rva") &&
                        jsonU32(symbol["rva"]) == begin) {
                        const auto* spec = hookSpec(name);
                        matchingSymbols.push_back({
                            {"name", name},
                            {"hook_point", spec ? json(static_cast<unsigned>(spec->point)) : json(nullptr)}
                        });
                    }
                }
            }
            candidates.push_back({
                {"function_rva", hex(begin)}, {"function_end_rva", hex(function->end)},
                {"function_size", function->end - function->begin},
                {"match_count_in_function", hitRvas.size()},
                {"match_at_function_entry",
                    std::find(hitRvas.begin(), hitRvas.end(), begin) != hitRvas.end()},
                {"sample_match_rvas", std::move(samples)},
                {"matching_profile_symbols", std::move(matchingSymbols)},
                {"context", contextFor(*image_, *function, matched)}
            });
        }
        writeEvent("pattern-scan", {
            {"pattern", pattern}, {"state", found.offsets.empty() ? "no-hit" : "hits"},
            {"match_count", found.offsets.size()},
            {"function_candidate_count", byFunction.size()},
            {"candidate_functions_emitted", candidates.size()},
            {"candidate_functions_truncated", byFunction.size() > candidates.size()},
            {"non_pdata_match_count", found.offsets.size() -
                std::accumulate(byFunction.begin(), byFunction.end(), std::size_t{},
                    [](std::size_t total, const auto& entry) {
                        return total + entry.second.size();
                    })},
            {"non_pdata_sample_rvas", std::move(outsideFunction)},
            {"candidate_functions", std::move(candidates)},
            {"hook_note", "Use probe/hook by a matching known symbol; arbitrary signatures have no inferred ABI."}
        });
    }

    json contextAround(std::uint32_t rva, const Function* function) const {
        const auto before = function ? std::min<std::uint32_t>(16, rva - function->begin) : 0;
        const auto endLimit = function ? function->end :
            std::min<std::uint32_t>(identity_.textRva + identity_.textSize, rva + 64);
        const auto after = rva < endLimit ? std::min<std::uint32_t>(64, endLimit - rva) : 0;
        return {
            {"bytes_before", hexBytesAt(*image_, rva - before, before)},
            {"bytes_at", hexBytesAt(*image_, rva, after)},
            {"function", function ? json{
                {"begin_rva", hex(function->begin)}, {"end_rva", hex(function->end)},
                {"size", function->end - function->begin},
                {"code_references", referencesFor(*image_, *function)}
            } : json(nullptr)}
        };
    }

    void probe(const std::string& symbol) {
        if (!resolver_) throw std::runtime_error("load a target profile first with profile or port");
        const auto it = targetProfile_["symbols"].find(symbol);
        if (it == targetProfile_["symbols"].end())
            throw std::runtime_error("symbol is not present in the target profile");
        const auto resolution = resolver_->resolve(symbol, true);
        const auto& symbolEntry = it.value();
        const auto rva = jsonU32(symbolEntry.at("rva"));
        const auto* function = containingFunction(*image_, rva);
        const auto* hook = hookSpec(symbol);
        std::vector<std::pair<std::string, std::string>> patterns;
        std::set<std::string> seenPatterns;
        const auto appendPattern = [&](std::string label, const std::string& pattern) {
            if (!pattern.empty() && patterns.size() < 64 && seenPatterns.insert(pattern).second)
                patterns.emplace_back(std::move(label), pattern);
        };
        if (symbolEntry.contains("signature_anchors") &&
            symbolEntry["signature_anchors"].is_array()) {
            std::size_t index = 0;
            for (const auto& anchor : symbolEntry["signature_anchors"]) {
                if (anchor.is_object())
                    appendPattern("anchor-" + std::to_string(index),
                                  anchor.value("pattern", std::string{}));
                ++index;
                if (patterns.size() >= 64) break;
            }
        }
        if (patterns.empty() && symbolEntry.contains("checksig_evidence") &&
            symbolEntry["checksig_evidence"].contains("anchor_hits") &&
            symbolEntry["checksig_evidence"]["anchor_hits"].is_array()) {
            std::size_t index = 0;
            for (const auto& hit : symbolEntry["checksig_evidence"]["anchor_hits"]) {
                if (hit.is_object())
                    appendPattern("ported-anchor-" + std::to_string(index),
                                  hit.value("pattern", std::string{}));
                ++index;
                if (patterns.size() >= 64) break;
            }
        }
        if (patterns.empty())
            appendPattern("signature", symbolEntry.value("signature", std::string{}));
        json patternHits = json::array();
        if (!patterns.empty()) {
            const auto text = copyText();
            for (const auto& [label, pattern] : patterns) {
                const auto found = findMatches(text, pattern);
                json hitRvas = json::array();
                for (std::size_t i = 0; i < std::min<std::size_t>(16, found.offsets.size()); ++i)
                    hitRvas.push_back(hex(identity_.textRva + found.offsets[i]));
                patternHits.push_back({
                    {"label", label}, {"pattern", pattern},
                    {"match_count", found.capped ? json(nullptr) : json(found.offsets.size())},
                    {"state", found.capped ? "over-cap" :
                        found.offsets.empty() ? "no-hit" :
                        found.offsets.size() == 1 ? "unique-hit" : "ambiguous"},
                    {"sample_rvas", std::move(hitRvas)}
                });
            }
        }
        const bool exactGuard = resolution && resolution->method == "exact-rva" &&
                                resolution->address == image_->base + rva;
        const bool functionEntry = rva >= identity_.textRva &&
            rva < identity_.textRva + identity_.textSize && function && function->begin == rva;
        const bool runtimeSupports = hook && BDS_HookRuntimeSupports(hook->point) != 0;
        const bool jumpStub = suspectedJumpStub(*image_, rva);
        const bool targetOwned = hook && BDS_HookOwnsTarget(
            hook->point, reinterpret_cast<void*>(image_->base + rva)) != 0;
        const bool hookReady = exactGuard && functionEntry && runtimeSupports &&
            (!jumpStub || targetOwned);
        writeEvent("probe-result", {
            {"symbol", symbol}, {"rva", hex(rva)},
            {"status", symbolEntry.value("status", std::string("candidate"))},
            {"source", symbolEntry.value("source", std::string{})},
            {"candidate_evidence", symbolEntry.value("candidate_evidence", std::string{})},
            {"abi_verified", symbolEntry.value("abi_verified", false)},
            {"runtime_verified", symbolEntry.value("runtime_verified", false)},
            {"resolver_method", resolution ? resolution->method : std::string("no-hit")},
            {"exact_build", resolver_->exactBuild()}, {"entry_guard_hit", exactGuard},
            {"pdata_function_entry", functionEntry},
            {"hook_abi_known", hook != nullptr},
            {"hook_point", hook ? json(static_cast<unsigned>(hook->point)) : json(nullptr)},
            {"hook_runtime_supports_point", runtimeSupports},
            {"entry_looks_like_jump_stub", jumpStub},
            {"target_already_owned_by_sdk", targetOwned},
            {"hook_ready_for_explicit_test", hookReady},
            {"pattern_hits", std::move(patternHits)},
            {"anchor_port_support", targetProfile_.value("generation", json::object())
                .value("port_support", json::object()).value(symbol, json::object())},
            {"hook_test", hookReady
                ? "run hook <symbol> to install a transparent observer"
                : "not eligible for a live hook"},
            {"context", contextAround(rva, function)},
            {"runtime_diagnostic", BDS_HookRuntimeDiagnostic()}
        });
    }

    void installHook(const std::string& symbol) {
        if (!resolver_) throw std::runtime_error("load a target profile first with profile or port");
        const auto* spec = hookSpec(symbol);
        if (!spec)
            throw std::runtime_error("no SDK-known ABI/HookPoint for this symbol; use probe only");
        const auto entry = targetProfile_["symbols"].find(symbol);
        if (entry == targetProfile_["symbols"].end())
            throw std::runtime_error("symbol is not present in the target profile");
        const auto rva = jsonU32(entry.value().at("rva"));
        const auto resolution = resolver_->resolve(symbol, true);
        const auto* function = containingFunction(*image_, rva);
        const auto target = reinterpret_cast<void*>(image_->base + rva);
        const bool jumpStub = suspectedJumpStub(*image_, rva);
        const bool targetOwned = BDS_HookOwnsTarget(spec->point, target) != 0;
        if (jumpStub && !targetOwned)
            throw std::runtime_error("hook refused: entry looks like a jump stub not owned by the SDK hook broker");
        if (!resolution || resolution->method != "exact-rva" ||
            resolution->address != image_->base + rva ||
            !function || function->begin != rva ||
            rva < identity_.textRva || rva >= identity_.textRva + identity_.textSize)
            throw std::runtime_error("hook refused: exact entry guard or .pdata entry check failed");
        if (!BDS_HookRuntimeSupports(spec->point))
            throw std::runtime_error("hook runtime does not support this point: " +
                                     std::string(BDS_HookRuntimeDiagnostic()));

        std::lock_guard lock(hookMapMutex_);
        auto& holder = hooks_[symbol];
        if (!holder) {
            holder = std::make_unique<HookState>();
            holder->symbol = symbol;
            holder->point = spec->point;
            if (spec->point == BDS_HOOK_LEVEL_RUNTIME_ACTOR_LIST &&
                lower(identity_.fileSha256) == k126605ExeSha256 &&
                targetProfile_.contains("diagnostic_candidates") &&
                targetProfile_["diagnostic_candidates"].is_object() &&
                targetProfile_["diagnostic_candidates"].contains("actor_context")) {
                const auto& candidate = targetProfile_["diagnostic_candidates"]["actor_context"];
                if (candidate.is_object() &&
                    candidate.value("status", std::string{}) == "candidate" &&
                    !candidate.value("runtime_verified", false) &&
                    candidate.value("evidence_id", std::string{}) == k126605ActorContextEvidence &&
                    jsonU32(candidate.at("actor_type_component_hash")) == 0x4F6BA419 &&
                    jsonU32(candidate.at("actor_type_value_offset")) == 0 &&
                    candidate.at("actor_type_value_size").get<unsigned>() == 4 &&
                    candidate.at("player_type_id").get<unsigned>() == 319) {
                    const auto registryOffset = jsonU32(
                        candidate.at("registry_pointer_offset_from_actor"));
                    const auto entityIdOffset = jsonU32(
                        candidate.at("entity_id_offset_from_actor"));
                    if (registryOffset == 0x10 && entityIdOffset == 0x18) {
                        holder->actorContextCandidate = true;
                        holder->actorRegistryOffset = registryOffset;
                        holder->actorEntityIdOffset = entityIdOffset;
                        holder->actorContextEvidence = candidate.value("evidence_id", std::string{});
                    }
                }
            }
        }
        if (holder->token) throw std::runtime_error("hook is already installed");
        if (!BDS_HookSubscribe(spec->point, target,
                &CheckSigMod::observeHook, holder.get(), &holder->token)) {
            holder->token = nullptr;
            throw std::runtime_error("BDS_HookSubscribe rejected the target");
        }
        writeEvent("hook-installed", {
            {"symbol", symbol}, {"rva", hex(rva)},
            {"hook_runtime", BDS_HookRuntimeVersion()},
            {"transparent_pass_through", true},
            {"actor_context_candidate_observation", holder->actorContextCandidate},
            {"actor_context_evidence", holder->actorContextEvidence},
            {"note", "Callback always calls next and records values after the original."}
        });
    }

    void removeHook(const std::string& symbol) {
        std::lock_guard lock(hookMapMutex_);
        const auto it = hooks_.find(symbol);
        if (it == hooks_.end() || !it->second->token)
            throw std::runtime_error("no active CheckSig hook for this symbol");
        BDS_HookUnsubscribe(it->second->token);
        it->second->token = nullptr;
        writeEvent("hook-removed", {{"symbol", symbol},
            {"total_calls", it->second->calls.load(std::memory_order_relaxed)}});
    }

    void unsubscribeAll() {
        std::lock_guard lock(hookMapMutex_);
        for (auto& [name, state] : hooks_) {
            if (!state->token) continue;
            BDS_HookUnsubscribe(state->token);
            state->token = nullptr;
            try { writeEvent("hook-removed", {{"symbol", name},
                {"reason", "CheckSig stopped"}}); } catch (...) {}
        }
    }

    void readValue(const std::string& addressToken, const std::string& type) {
        std::uintptr_t address{};
        if (addressToken.rfind("rva:", 0) == 0) {
            const auto value = parseUnsigned(std::string_view(addressToken).substr(4));
            if (value >= image_->imageSize || image_->base > UINTPTR_MAX - value)
                throw std::runtime_error("RVA is outside the loaded image");
            address = image_->base + static_cast<std::uintptr_t>(value);
        } else {
            const auto value = parseUnsigned(addressToken);
            if (value > UINTPTR_MAX) throw std::runtime_error("address exceeds x64 range");
            address = static_cast<std::uintptr_t>(value);
        }
        std::size_t size = 0;
        if (type == "u8" || type == "i8") size = 1;
        else if (type == "u16" || type == "i16") size = 2;
        else if (type == "u32" || type == "i32" || type == "f32") size = 4;
        else if (type == "u64" || type == "i64" || type == "ptr" || type == "f64") size = 8;
        else if (type.rfind("bytes:", 0) == 0)
            size = static_cast<std::size_t>(
                parseUnsigned(std::string_view(type).substr(6), 10));
        else throw std::runtime_error("unsupported value type");
        if (!size || size > kMaxReadBytes)
            throw std::runtime_error("read size must be between 1 and 64 bytes");
        std::array<std::byte, kMaxReadBytes> data{};
        if (!safeRead(address, data.data(), size))
            throw std::runtime_error("ReadProcessMemory refused the range (unmapped, guarded, or unreadable)");
        json result = {
            {"address", hex(address)}, {"type", type}, {"size", size},
            {"bytes", bytesHex(data.data(), size)}, {"readable", true}
        };
        if (type == "u8") result["value"] = std::to_integer<std::uint8_t>(data[0]);
        else if (type == "i8") { std::int8_t v{}; std::memcpy(&v, data.data(), 1); result["value"] = v; }
        else if (type == "u16") { std::uint16_t v{}; std::memcpy(&v, data.data(), 2); result["value"] = v; }
        else if (type == "i16") { std::int16_t v{}; std::memcpy(&v, data.data(), 2); result["value"] = v; }
        else if (type == "u32") { std::uint32_t v{}; std::memcpy(&v, data.data(), 4); result["value"] = v; }
        else if (type == "i32") { std::int32_t v{}; std::memcpy(&v, data.data(), 4); result["value"] = v; }
        else if (type == "u64") { std::uint64_t v{}; std::memcpy(&v, data.data(), 8); result["value"] = hex(v); }
        else if (type == "i64") { std::int64_t v{}; std::memcpy(&v, data.data(), 8); result["value"] = v; }
        else if (type == "ptr") { std::uintptr_t v{}; std::memcpy(&v, data.data(), 8); result["value"] = hex(v); }
        else if (type == "f32") { float v{}; std::memcpy(&v, data.data(), 4); result["value"] = v; }
        else if (type == "f64") { double v{}; std::memcpy(&v, data.data(), 8); result["value"] = v; }
        writeEvent("memory-read", std::move(result));
    }
};

CheckSigMod g_mod;

DWORD WINAPI worker(void*) {
    g_mod.run();
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        (void)module;
        DisableThreadLibraryCalls(module);
        const auto thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_mod.requestStop();
    }
    return TRUE;
}

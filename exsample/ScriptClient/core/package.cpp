#include "package.hpp"
#include "manifest.hpp"

#include <miniz.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <regex>
#include <stdexcept>

namespace stageb {
namespace {
constexpr std::size_t maxArchive = 32 * 1024 * 1024;
constexpr std::size_t maxExpanded = 64 * 1024 * 1024;
constexpr std::size_t maxEntries = 4096;

struct TempDirectory {
    fs::path path;
    TempDirectory(const fs::path& parent, const char* prefix) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int i = 0; i != 8; ++i) {
            auto candidate = parent / (prefix + std::to_string(now) + "-" + std::to_string(i));
            if (fs::create_directory(candidate)) {
                path = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("cannot create a private staging directory");
    }
    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
};

bool writable(const fs::path& dir) {
    if (dir.empty()) return false;
    try {
        const TempDirectory probe(dir, ".scriptclient-write-");
        std::ofstream file(probe.path / "probe", std::ios::binary);
        file.put('1');
        file.close();
        return static_cast<bool>(file);
    } catch (const fs::filesystem_error&) {
        return false;
    }
}

// Portable ASCII names with no Windows transformations, aliases or streams.
bool safeComponent(const std::string& part) {
    if (part.empty() || part == "." || part == ".." || part.size() > 128 ||
        part.back() == '.' || part.back() == ' ' ||
        !std::all_of(part.begin(), part.end(), [](unsigned char c) {
            return c >= 32 && c < 127 && std::string_view("<>:\"\\|?*/~").find(c) == std::string_view::npos;
        })) return false;
    auto base = lowerCopy(part.substr(0, part.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    return base != "con" && base != "prn" && base != "aux" && base != "nul" &&
           !(base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) &&
             base.back() >= '0' && base.back() <= '9');
}

struct Zip {
    mz_zip_archive zip{};
    ~Zip() { mz_zip_reader_end(&zip); }
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::size_t writeMember(void* output, mz_uint64, const void* bytes, std::size_t size) {
    auto& file = *static_cast<std::ofstream*>(output);
    file.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    return file ? size : 0;
}
} // namespace

fs::path selectStorageRoot(const fs::path& exeDir, const fs::path& dllDir) {
    if (writable(exeDir)) return exeDir;
    if (exeDir != dllDir && writable(dllDir)) return dllDir;
    throw std::runtime_error("neither the game EXE nor ScriptClient DLL directory is writable");
}

bool isScriptModName(const std::string& name) {
    static const std::regex pattern(R"([A-Za-z0-9][A-Za-z0-9._-]{0,63})");
    return safeComponent(name) && std::regex_match(name, pattern);
}

bool isPackagePath(const fs::path& path) {
    const auto ext = path.extension();
    const auto utf8 = ext.u8string();
    const auto lower = lowerCopy(std::string(utf8.begin(), utf8.end()));
    return lower == ".mods" || lower == ".zip";
}

std::vector<fs::path> listPackages(const fs::path& inbox) {
    std::vector<fs::path> result;
    for (const auto& entry : fs::directory_iterator(inbox)) {
        if (fs::is_regular_file(entry.symlink_status()) && isPackagePath(entry.path()))
            result.push_back(entry.path().filename());
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string installPackage(const fs::path& archive, const fs::path& modsDir,
                           const std::string& gameVersion) {
    require(isPackagePath(archive) && fs::is_regular_file(fs::symlink_status(archive)),
            "select a regular .mods or .zip file");
    std::ifstream input(archive, std::ios::binary | std::ios::ate);
    const auto length = input.tellg();
    require(length > 0 && length <= static_cast<std::streamoff>(maxArchive), "invalid ZIP size");
    std::vector<char> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    require(static_cast<bool>(input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))),
            "cannot read package");
    Zip zip;
    require(mz_zip_reader_init_mem(&zip.zip, bytes.data(), bytes.size(), 0), "cannot read ZIP");
    const auto files = mz_zip_reader_get_num_files(&zip.zip);
    require(files > 0 && files <= maxEntries, "invalid ZIP entry count");

    const TempDirectory stage(modsDir, ".scriptclient-install-");
    const auto body = stage.path / "mod";
    fs::create_directory(body);
    struct Entry { std::string spelling; bool directory; bool explicitEntry; };
    std::map<std::string, Entry> seen;
    std::size_t expanded = 0;
    bool marker = false;
    for (mz_uint i = 0; i < files; ++i) {
        mz_zip_archive_file_stat info{};
        char raw[512]{};
        const auto size = mz_zip_reader_get_filename(&zip.zip, i, raw, sizeof(raw));
        require(size > 1 && size <= sizeof(raw) &&
                std::char_traits<char>::length(raw) == size - 1 &&
                mz_zip_reader_file_stat(&zip.zip, i, &info) && info.m_is_supported,
                "unsupported ZIP entry");
        const auto type = (info.m_external_attr >> 16) & 0170000;
        require(type == 0 || type == (info.m_is_directory ? 0040000u : 0100000u),
                "ZIP links and special files are not supported");
        require(info.m_uncomp_size <= maxExpanded - expanded, "expanded package is too large");
        expanded += static_cast<std::size_t>(info.m_uncomp_size);
        std::string name(raw, size - 1);
        if (name == "scriptclient-package.json" && !info.m_is_directory) {
            require(!marker && info.m_uncomp_size > 0 && info.m_uncomp_size <= 4096,
                    "invalid ScriptClient package marker");
            std::string json(static_cast<std::size_t>(info.m_uncomp_size), '\0');
            require(mz_zip_reader_extract_to_mem(&zip.zip, i, json.data(), json.size(), 0),
                    "cannot read ScriptClient package marker");
            const auto doc = nlohmann::json::parse(json);
            require(doc == nlohmann::json{{"format", "scriptclient-mod"}, {"version", 1}},
                    "unsupported ScriptClient package format");
            marker = true;
            continue;
        }
        require(name.starts_with("mod/"), "ZIP entries must be under mod/");
        if (name == "mod/" && info.m_is_directory) continue;
        if (info.m_is_directory && name.ends_with('/')) name.pop_back();
        fs::path target = body;
        std::string relative;
        for (std::size_t start = 4;;) {
            const auto slash = name.find('/', start);
            const auto component = name.substr(start, slash - start);
            require(safeComponent(component), "unsafe ZIP path");
            relative += (relative.empty() ? "" : "/") + component;
            const bool directory = slash != std::string::npos || info.m_is_directory;
            const bool explicitEntry = slash == std::string::npos;
            require(lowerCopy(relative) != ".pex-studio-owner", "reserved mod file");
            const auto [it, added] = seen.try_emplace(lowerCopy(relative),
                                                      Entry{relative, directory, explicitEntry});
            if (!added) {
                require(it->second.spelling == relative && it->second.directory == directory &&
                        !(it->second.explicitEntry && explicitEntry), "duplicate/conflicting ZIP path");
                it->second.explicitEntry |= explicitEntry;
            }
            target /= component;
            if (directory && added) fs::create_directory(target);
            if (!explicitEntry) { start = slash + 1; continue; }
            if (directory) break;
            const auto ext = lowerCopy(target.extension().string());
            require(ext != ".dll" && ext != ".exe" && ext != ".asi" && ext != ".node",
                    "native binaries are not ScriptClient mods");
            std::ofstream out(target, std::ios::binary);
            require(out && mz_zip_reader_extract_to_callback(&zip.zip, i, writeMember, &out, 0),
                    "cannot extract ZIP entry");
            out.close();
            require(static_cast<bool>(out), "cannot write ZIP entry");
            break;
        }
    }
    require(marker, "ScriptClient package marker is missing");
    const auto manifest = body / "manifest.json";
    require(fs::is_regular_file(manifest) && fs::file_size(manifest) <= 256 * 1024,
            "mod/manifest.json is missing or too large");
    const auto doc = [&] {
        std::ifstream stream(manifest, std::ios::binary);
        return nlohmann::json::parse(stream);
    }(); // Windows cannot rename the staged directory while this handle is open.
    const auto name = doc.value("name", std::string{});
    require(isScriptModName(name), "invalid mod name");
    const auto validation = validateManifest(doc, name, body, gameVersion);
    if (!validation.ok) throw std::runtime_error(validation.error);
    for (const auto& entry : fs::directory_iterator(modsDir))
        require(lowerCopy(entry.path().filename().string()) != lowerCopy(name),
                "a mod with this name already exists; no files were overwritten");
    fs::rename(body, modsDir / name);
    return name;
}
} // namespace stageb

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include "bedrock/resolver.hpp"
#include "bedrock/remote_catalog.hpp"
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
} // namespace bedrock
#endif

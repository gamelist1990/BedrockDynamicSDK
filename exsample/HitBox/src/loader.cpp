#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kTargetName[] = L"Minecraft.Windows.exe";
constexpr DWORD kWaitMs = 30000;

std::wstring utf8ToWide(const char* input) {
    if (!input || !*input) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1,
                                         nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1,
                             result.data(), count)) return {};
    result.resize(static_cast<std::size_t>(count - 1));
    return result;
}

std::wstring basenameOf(const wchar_t* path) {
    if (!path) return {};
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    return base;
}

std::wstring processImagePath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::vector<wchar_t> path(32768);
    DWORD length = static_cast<DWORD>(path.size());
    const bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
    CloseHandle(process);
    return ok ? std::wstring(path.data(), length) : std::wstring{};
}

std::wstring targetModulePath(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return {};
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring path;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, kTargetName) == 0 ||
                _wcsicmp(basenameOf(entry.szExePath).c_str(), kTargetName) == 0) {
                path = entry.szExePath;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return path;
}

bool isTargetEntry(const PROCESSENTRY32W& entry) {
    if (_wcsicmp(entry.szExeFile, kTargetName) == 0) return true;
    const auto imagePath = processImagePath(entry.th32ProcessID);
    if (!imagePath.empty() &&
        _wcsicmp(basenameOf(imagePath.c_str()).c_str(), kTargetName) == 0) return true;
    return !targetModulePath(entry.th32ProcessID).empty();
}

bool isTargetProcess(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == pid && isTargetEntry(entry)) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

int listTargets(bool all = false) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "process list unavailable (error=%lu)\n", GetLastError());
        return 2;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    unsigned count = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (all || isTargetEntry(entry)) {
                const auto imagePath = processImagePath(entry.th32ProcessID);
                const auto modulePath = targetModulePath(entry.th32ProcessID);
                std::wprintf(L"pid=%lu exe=\"%ls\" image=\"%ls\" target_module=\"%ls\"\n",
                             entry.th32ProcessID, entry.szExeFile,
                             imagePath.empty() ? L"<unavailable>" : imagePath.c_str(),
                             modulePath.empty() ? L"<unavailable>" : modulePath.c_str());
                ++count;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (!count) std::puts(all ? "No Windows processes found in this Wine/Windows session."
                              : "No Minecraft.Windows.exe process found in this Wine/Windows session.");
    return 0;
}

bool isX64Process(HANDLE process) {
    BOOL isWow64 = FALSE;
    if (!IsWow64Process(process, &isWow64)) return false;
    return sizeof(void*) == 8 && !isWow64;
}

bool isX64Dll(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    bool valid = ReadFile(file, &dos, sizeof(dos), &read, nullptr) &&
                 read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
                 dos.e_lfanew >= static_cast<LONG>(sizeof(dos));
    LARGE_INTEGER position{};
    position.QuadPart = dos.e_lfanew;
    if (valid) valid = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE;
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    if (valid) valid = ReadFile(file, &signature, sizeof(signature), &read, nullptr) &&
                       read == sizeof(signature) && signature == IMAGE_NT_SIGNATURE;
    if (valid) valid = ReadFile(file, &header, sizeof(header), &read, nullptr) &&
                       read == sizeof(header) && header.Machine == IMAGE_FILE_MACHINE_AMD64;
    CloseHandle(file);
    return valid;
}

bool findModule(DWORD pid, const std::wstring& moduleName, std::uintptr_t& base) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName.c_str()) == 0) {
                base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

int inject(DWORD pid, const std::wstring& dllPath) {
    if (!isTargetProcess(pid)) {
        std::fprintf(stderr, "refusing target: PID %lu is not %ls\n", pid, kTargetName);
        return 2;
    }

    wchar_t absolutePath[32768]{};
    const DWORD pathLength = GetFullPathNameW(dllPath.c_str(), 32768, absolutePath, nullptr);
    if (!pathLength || pathLength >= 32768 || !isX64Dll(absolutePath)) {
        std::fprintf(stderr, "DLL path is missing, invalid, or not a PE x64 DLL\n");
        return 2;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        std::fprintf(stderr, "OpenProcess failed (error=%lu)\n", GetLastError());
        return 3;
    }
    if (!isX64Process(process)) {
        std::fprintf(stderr, "target architecture could not be verified as x64\n");
        CloseHandle(process);
        return 3;
    }

    HMODULE localKernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC localLoadLibrary = localKernel ? GetProcAddress(localKernel, "LoadLibraryW") : nullptr;
    HMODULE ownerModule = nullptr;
    if (!localLoadLibrary ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(localLoadLibrary), &ownerModule)) {
        std::fprintf(stderr, "could not resolve the local LoadLibraryW address\n");
        CloseHandle(process);
        return 4;
    }
    wchar_t ownerPath[32768]{};
    if (!GetModuleFileNameW(ownerModule, ownerPath, 32768)) {
        std::fprintf(stderr, "could not identify the LoadLibraryW module\n");
        CloseHandle(process);
        return 4;
    }
    const std::wstring ownerName = basenameOf(ownerPath);
    std::uintptr_t remoteModule = 0;
    if (!findModule(pid, ownerName, remoteModule)) {
        std::fwprintf(stderr, L"target module unavailable: %ls\n", ownerName.c_str());
        CloseHandle(process);
        return 4;
    }
    const auto offset = reinterpret_cast<std::uintptr_t>(localLoadLibrary) -
                        reinterpret_cast<std::uintptr_t>(ownerModule);
    auto remoteLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteModule + offset);

    const std::size_t bytes = (std::wcslen(absolutePath) + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_READWRITE);
    if (!remotePath) {
        std::fprintf(stderr, "VirtualAllocEx failed (error=%lu)\n", GetLastError());
        CloseHandle(process);
        return 5;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, absolutePath, bytes, &written) || written != bytes) {
        std::fprintf(stderr, "WriteProcessMemory failed (error=%lu)\n", GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 5;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, remoteLoadLibrary, remotePath, 0, nullptr);
    if (!thread) {
        std::fprintf(stderr, "CreateRemoteThread failed (error=%lu)\n", GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 6;
    }
    const DWORD wait = WaitForSingleObject(thread, kWaitMs);
    if (wait != WAIT_OBJECT_0) {
        std::fprintf(stderr, "LoadLibraryW did not finish within %lu ms; leaving its remote path intact\n",
                     kWaitMs);
        CloseHandle(thread);
        CloseHandle(process);
        return 7;
    }

    std::uintptr_t loadedBase = 0;
    const bool loaded = findModule(pid, basenameOf(absolutePath), loadedBase);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(thread);
    CloseHandle(process);
    if (!loaded) {
        std::fprintf(stderr, "DLL load did not appear in the target module list\n");
        return 8;
    }
    std::wprintf(L"Loaded %ls into Minecraft.Windows.exe (pid=%lu, module=0x%llx)\n",
                 basenameOf(absolutePath).c_str(), pid,
                 static_cast<unsigned long long>(loadedBase));
    return 0;
}

void usage() {
    std::puts("BedrockHitBoxLoader - local HitBox test loader");
    std::puts("  BedrockHitBoxLoader.exe list");
    std::puts("  BedrockHitBoxLoader.exe list --all");
    std::puts("  BedrockHitBoxLoader.exe inject <Minecraft.Windows.exe PID> <x64 DLL path>");
    std::puts("Only Minecraft.Windows.exe is accepted as an injection target.");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "list") return listTargets();
    if (argc == 3 && std::string(argv[1]) == "list" && std::string(argv[2]) == "--all")
        return listTargets(true);
    if (argc == 4 && std::string(argv[1]) == "inject") {
        errno = 0;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(argv[2], &end, 10);
        if (errno || !end || *end || !parsed || parsed > MAXDWORD) {
            usage();
            return 2;
        }
        const auto path = utf8ToWide(argv[3]);
        if (path.empty()) {
            std::fprintf(stderr, "DLL path is not valid UTF-8\n");
            return 2;
        }
        return inject(static_cast<DWORD>(parsed), path);
    }
    usage();
    return argc == 1 ? 0 : 2;
}

#include <windows.h>
#include <chrono>
#include <iostream>
#include <thread>

int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 1;
    const auto module = LoadLibraryW(argv[1]);
    if (!module) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return 2;
    }
    const auto status = reinterpret_cast<unsigned (*)()>(
        GetProcAddress(module, "GlobalResourcePack_Status"));
    if (!status)
        return 3;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto value = status();
        if (value == 4)
            return 0; // This host is not Minecraft: reject its executable.
        if (value == 2) {
            std::cerr << "Adapter accepted an unrelated executable\n";
            return 4;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    std::cerr << "Adapter did not reject unsupported executable before deadline\n";
    return 5;
    // Do not unload: a worker can still be unwinding its final return.
}

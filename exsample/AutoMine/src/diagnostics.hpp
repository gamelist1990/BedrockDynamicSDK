#pragma once

#include <windows.h>
#include <filesystem>

namespace automine::client::crashrec {

extern char g_section[64];
extern volatile LONG g_ticks;
extern volatile LONG g_sehFaults;

void ringPush(const char *text);
void setSection(const char *name);
void install(const std::filesystem::path &dir);

// Restores the previous breadcrumb when a guarded operation ends.
struct Section {
    char prev[64];
    explicit Section(const char *name);
    ~Section();
};

} // namespace automine::client::crashrec

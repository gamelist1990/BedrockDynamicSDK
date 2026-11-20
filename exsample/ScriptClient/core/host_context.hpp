#pragma once

#include "command_model.hpp"

#include <map>
#include <string>

namespace stageb {

class ModuleLoader;
class SdkBindings;
class CommandRegistry;
class WorldBackend;
class LogSink;

// Stored as the quickjs context opaque. C callbacks reach the host services
// through this struct; no JS value ever leaves the owning tick (§1.2).
struct HostContext {
    ModuleLoader* loader = nullptr;
    SdkBindings* bindings = nullptr;
    CommandRegistry* commands = nullptr;
    WorldBackend* world = nullptr;
    LogSink* log = nullptr;

    // Set only while a command handler runs on the owning tick (§1.2).
    const std::map<std::string, ContextValue>* activeValues = nullptr;
    std::string* response = nullptr;
    const std::string* rawLine = nullptr;
};

} // namespace stageb

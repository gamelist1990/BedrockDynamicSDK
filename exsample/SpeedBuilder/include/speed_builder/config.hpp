#pragma once

#include "model.hpp"
#include <nlohmann/json.hpp>

namespace speed_builder {

// Same header-only parseConfig/configJson convention as Freecam.
struct Config : Options {};

inline nlohmann::json configJson(const Config& c) {
    validateOptions(c);
    return {{"config_revision",2}, {"enabled",c.enabled}, {"radius",c.radius},
        {"max_blocks",c.maxBlocks}, {"capture_cells_per_tick",c.captureCellsPerTick},
        {"build_interval_ticks",c.buildIntervalTicks}, {"max_reach",c.maxReach},
        {"max_attempts",c.maxAttempts}, {"confirmation_ticks",c.confirmationTicks},
        {"max_steps_per_tick",c.maxStepsPerTick}, {"max_wait_ticks",c.maxWaitTicks},
        {"max_build_ticks",c.maxBuildTicks}, {"prompt",c.prompt},
        {"keys",{{"cancel",c.cancelKey}}}};
}

inline Config parseConfig(std::string_view source) {
    const auto j = nlohmann::json::parse(source);
    if (!j.is_object()) throw std::runtime_error("config must be an object");
    Config c;
    const auto integer = [](const nlohmann::json& value) -> unsigned {
        if (!value.is_number_integer()) throw std::runtime_error("expected unsigned integer");
        if (value.is_number_unsigned()) {
            const auto n = value.get<std::uint64_t>();
            if (n > std::numeric_limits<unsigned>::max()) throw std::runtime_error("integer out of range");
            return static_cast<unsigned>(n);
        }
        const auto n = value.get<std::int64_t>();
        if (n < 0 || std::uint64_t(n) > std::numeric_limits<unsigned>::max())
            throw std::runtime_error("integer out of range");
        return static_cast<unsigned>(n);
    };
    if(j.contains("config_revision")&&integer(j.at("config_revision"))!=1&&integer(j.at("config_revision"))!=2)
        throw std::runtime_error("unsupported config_revision");
    if (j.contains("enabled")) {
        if (!j.at("enabled").is_boolean()) throw std::runtime_error("enabled must be boolean");
        c.enabled = j.at("enabled").get<bool>();
    }
    const auto read = [&](const char* key, unsigned& target) {
        if (j.contains(key)) target = integer(j.at(key));
    };
    read("radius",c.radius); read("max_blocks",c.maxBlocks);
    read("capture_cells_per_tick",c.captureCellsPerTick);
    read("build_interval_ticks",c.buildIntervalTicks);
    read("max_attempts",c.maxAttempts); read("confirmation_ticks",c.confirmationTicks);
    read("max_steps_per_tick",c.maxStepsPerTick);
    read("max_wait_ticks",c.maxWaitTicks); read("max_build_ticks",c.maxBuildTicks);
    if (j.contains("max_reach")) {
        if (!j.at("max_reach").is_number()) throw std::runtime_error("max_reach must be numeric");
        c.maxReach = j.at("max_reach").get<double>();
    }
    if (j.contains("prompt")) {
        if (!j.at("prompt").is_string()) throw std::runtime_error("prompt must be a string");
        c.prompt = j.at("prompt").get<std::string>();
    }
    if (j.contains("keys")) {
        const auto& keys = j.at("keys");
        if (!keys.is_object()) throw std::runtime_error("keys must be an object");
        if (keys.contains("cancel")) c.cancelKey = integer(keys.at("cancel"));
    }
    // Upgrade only the previous defaults. Explicit non-default budgets stay intact.
    if(j.value("config_revision",1)==1){
        if(c.captureCellsPerTick==256)c.captureCellsPerTick=1024;
        if(c.buildIntervalTicks==2)c.buildIntervalTicks=1;
    }
    validateOptions(c);
    return c;
}

} // namespace speed_builder

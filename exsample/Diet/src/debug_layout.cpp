#include "diet/debug_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace diet {
namespace {

std::string format(const char* pattern, auto... values) {
    char buffer[160];
    const int written = std::snprintf(buffer, sizeof(buffer), pattern, values...);
    if (written <= 0) return {};
    return std::string(buffer, static_cast<std::size_t>(
                                  std::min<int>(written, static_cast<int>(sizeof(buffer) - 1))));
}

constexpr const char* kDirections[4] = {"south", "west", "north", "east"};
constexpr const char* kTowards[4] = {"positive Z", "negative X", "negative Z", "positive X"};

}  // namespace

std::string javaFacingLine(double yawDeg, double pitchDeg) {
    // Direction.fromYRot: (floor(yaw / 90 + 0.5) & 3) -> south, west, north, east.
    double yaw = std::fmod(yawDeg, 360.0);
    if (yaw < 0.0) yaw += 360.0;
    int index = static_cast<int>(std::floor(yaw / 90.0 + 0.5)) & 3;
    // Re-normalise to Java's [-180, 180] yaw for display.
    double displayYaw = yawDeg;
    while (displayYaw > 180.0) displayYaw -= 360.0;
    while (displayYaw < -180.0) displayYaw += 360.0;
    return format("Facing: %s (Towards %s) (%.1f / %.1f)", kDirections[index],
                  kTowards[index], displayYaw, pitchDeg);
}

std::string javaCoordinateLine(double x, double y, double z) {
    return format("XYZ: %.3f / %.5f / %.3f", x, y, z);
}

std::string javaBlockLine(double x, double y, double z) {
    return format("Block: %d %d %d", static_cast<int>(std::floor(x)),
                  static_cast<int>(std::floor(y)), static_cast<int>(std::floor(z)));
}

std::string javaChunkLine(double x, double y, double z) {
    // Java appends the region-file coordinates; this client has no region-file
    // identity, so only the chunk coordinates (which are real) are printed.
    return format("Chunk: %d %d %d", static_cast<int>(std::floor(x)) >> 4,
                  static_cast<int>(std::floor(y)) >> 4,
                  static_cast<int>(std::floor(z)) >> 4);
}

std::size_t f3Width(const std::vector<std::string>& column) {
    std::size_t width = 0;
    for (const auto& line : column) width = std::max(width, line.size());
    return width;
}

F3Layout buildF3Layout(const F3Snapshot& snapshot) {
    F3Layout layout;

    if (snapshot.gameVersionKnown && !snapshot.gameVersion.empty())
        layout.left.push_back("Minecraft " + snapshot.gameVersion + " (Bedrock)");
    if (snapshot.fpsValid)
        layout.left.push_back(format("%.0f fps", snapshot.fps));
    if (snapshot.positionValid) {
        layout.left.push_back(javaCoordinateLine(snapshot.x, snapshot.y, snapshot.z));
        layout.left.push_back(javaBlockLine(snapshot.x, snapshot.y, snapshot.z));
        layout.left.push_back(javaChunkLine(snapshot.x, snapshot.y, snapshot.z));
    }
    if (snapshot.facingValid)
        layout.left.push_back(javaFacingLine(snapshot.yawDeg, snapshot.pitchDeg));

    // Diet's own statistics, same visual style, appended at the bottom of the
    // left column. Every field is included only when it was actually measured.
    if (snapshot.cullValid) {
        std::string line = "Diet: targets=" + std::to_string(snapshot.tracked) +
                           " hidden=" + std::to_string(snapshot.hidden);
        if (snapshot.skippedDrawsValid)
            line += " skipped=" + std::to_string(snapshot.skippedDraws);
        line += " checked=" + std::to_string(snapshot.checked) +
                " samples=" + std::to_string(snapshot.samples) +
                " block_queries=" + std::to_string(snapshot.blockQueries) +
                " budget_stops=" + std::to_string(snapshot.budgetStops);
        layout.left.push_back(std::move(line));
    }
    if (snapshot.trafficValid) {
        std::string line = "Diet: packets=" + std::to_string(snapshot.packets);
        if (snapshot.deferValid) {
            line += " deferred=" + std::to_string(snapshot.deferredPackets) +
                    " queue=" + std::to_string(snapshot.deferredQueue);
        }
        if (!snapshot.packetClasses.empty()) line += " classes " + snapshot.packetClasses;
        layout.left.push_back(std::move(line));
    }
    if (snapshot.pingValid)
        layout.left.push_back("Diet: ping=" + std::to_string(snapshot.pingMs) + "ms");
    if (snapshot.frameStatsValid)
        layout.left.push_back(format("Diet: frame p50=%.2fms p95=%.2fms max=%.2fms",
                                     snapshot.frameP50Ms, snapshot.frameP95Ms,
                                     snapshot.frameMaxMs));

    const auto& system = snapshot.system;
    if (!system.runtimeValue.empty())
        layout.right.push_back((system.runtimeLabel.empty() ? "Runtime" : system.runtimeLabel) +
                               ": " + system.runtimeValue);
    if (!system.cpuBrand.empty()) {
        if (system.cpuThreads > 0)
            layout.right.push_back(format("CPU: %ux %s", system.cpuThreads,
                                          system.cpuBrand.c_str()));
        else
            layout.right.push_back("CPU: " + system.cpuBrand);
    }
    if (!system.display.empty()) layout.right.push_back("Display: " + system.display);
    if (!system.gpu.empty()) layout.right.push_back("GPU: " + system.gpu);
    if (system.memoryValid)
        layout.right.push_back(format("Mem: %.0f%% %llu/%lluMB", system.memoryPercent,
                                      static_cast<unsigned long long>(system.memoryUsedMb),
                                      static_cast<unsigned long long>(system.memoryTotalMb)));
    if (system.allocatedValid)
        layout.right.push_back(format("Allocated: %.0f%% %lluMB", system.allocatedPercent,
                                      static_cast<unsigned long long>(system.allocatedMb)));

    return layout;
}

}  // namespace diet

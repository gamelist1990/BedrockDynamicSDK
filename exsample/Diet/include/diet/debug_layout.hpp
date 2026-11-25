#pragma once
// Diet — Java-Edition-style F3 debug screen layout.
//
// Host-testable: no Windows, no ImGui, no native pointers. The layout function
// takes one snapshot of already-copied values and returns the left and right
// columns exactly as they are drawn. Line order, labels and number formats
// follow Java Edition's debug screen (see docs/diet-f3.md); lines whose data
// this client cannot honestly supply are omitted, never fabricated.
#include <cstdint>
#include <string>
#include <vector>

namespace diet {

// System block for the right column. Java prints its runtime/OS/CPU/GPU/memory
// there; these are the equivalents this platform can actually read.
struct F3SystemInfo {
    // Java's "Java:" line position. Label and value are supplied by the
    // platform layer because the truthful runtime differs (Wine / Windows).
    std::string runtimeLabel;
    std::string runtimeValue;
    std::string cpuBrand;
    unsigned cpuThreads{};
    std::string display;
    std::string gpu;
    bool memoryValid{};
    double memoryPercent{};
    std::uint64_t memoryUsedMb{};
    std::uint64_t memoryTotalMb{};
    bool allocatedValid{};
    double allocatedPercent{};
    std::uint64_t allocatedMb{};
};

struct F3Snapshot {
    std::string gameVersion;
    bool gameVersionKnown{};

    // Frame rate. Max framerate / present mode are not exposed by this client.
    bool fpsValid{};
    double fps{};
    bool frameStatsValid{};
    double frameP50Ms{};
    double frameP95Ms{};
    double frameMaxMs{};

    // Local player.
    bool positionValid{};
    double x{}, y{}, z{};
    bool facingValid{};
    double yawDeg{}, pitchDeg{};

    // Diet — occlusion decisions.
    bool cullValid{};
    unsigned tracked{};
    unsigned hidden{};
    std::uint64_t checked{};
    std::uint64_t samples{};
    std::uint64_t blockQueries{};
    std::uint64_t budgetStops{};
    bool skippedDrawsValid{};
    std::uint64_t skippedDraws{};

    // Diet — network.
    bool trafficValid{};
    std::uint64_t packets{};
    bool deferValid{};
    std::uint64_t deferredPackets{};
    std::uint64_t deferredQueue{};
    std::string packetClasses;  // "chunk=12 block-update=4", empty when none
    bool pingValid{};
    unsigned pingMs{};

    F3SystemInfo system;
};

struct F3Layout {
    std::vector<std::string> left;
    std::vector<std::string> right;
};

// Java's Direction.fromYRot / debug-screen facing format:
//   "Facing: north (Towards negative Z) (180.0 / 0.0)"
[[nodiscard]] std::string javaFacingLine(double yawDeg, double pitchDeg);

[[nodiscard]] std::string javaCoordinateLine(double x, double y, double z);
[[nodiscard]] std::string javaBlockLine(double x, double y, double z);
[[nodiscard]] std::string javaChunkLine(double x, double y, double z);

// Build both columns. Left column top-to-bottom mirrors Java's order
// (version, fps, XYZ/Block/Chunk/Facing) followed by Diet's own statistics in
// the same style; the right column is the Java system block order.
[[nodiscard]] F3Layout buildF3Layout(const F3Snapshot& snapshot);

// Number of lines the background panel must reserve for a column.
[[nodiscard]] std::size_t f3Width(const std::vector<std::string>& column);

}  // namespace diet

// Host test for the Java-Edition F3 layout: line order, formats, and the rule
// that unmeasurable values never produce a line.
#include "diet/debug_layout.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void checkContains(const std::string& line, const char* needle, const char* what) {
    if (line.find(needle) != std::string::npos) return;
    std::fprintf(stderr, "FAIL: %s (line was \"%s\")\n", what, line.c_str());
    ++failures;
}

void testFacing() {
    // Java Direction.fromYRot: 0 -> south, 90 -> west, 180 -> north, -90 -> east.
    checkContains(diet::javaFacingLine(0.0, 12.5), "Facing: south (Towards positive Z)",
                  "yaw 0 is south");
    checkContains(diet::javaFacingLine(90.0, 0.0), "Facing: west (Towards negative X)",
                  "yaw 90 is west");
    checkContains(diet::javaFacingLine(180.0, -30.0), "Facing: north (Towards negative Z)",
                  "yaw 180 is north");
    checkContains(diet::javaFacingLine(-90.0, 0.0), "Facing: east (Towards positive X)",
                  "yaw -90 is east");
    checkContains(diet::javaFacingLine(180.0, 12.5), "(180.0 / 12.5)", "yaw/pitch format");
    checkContains(diet::javaFacingLine(-180.0, 0.0), "(-180.0 / 0.0)", "yaw normalisation");
}

void testCoordinates() {
    const auto xyz = diet::javaCoordinateLine(100.1234, 64.0, -200.5);
    checkContains(xyz, "XYZ: 100.123 / 64.00000 / -200.500", "XYZ precision");
    checkContains(diet::javaBlockLine(100.9, 64.0, -200.1), "Block: 100 64 -201",
                  "block floors coordinates");
    checkContains(diet::javaChunkLine(100.0, 64.0, -200.0), "Chunk: 6 4 -13",
                  "chunk coordinates");
}

void testFullLayout() {
    diet::F3Snapshot snapshot;
    snapshot.gameVersion = "1.26.65.1";
    snapshot.gameVersionKnown = true;
    snapshot.fpsValid = true;
    snapshot.fps = 61.7;
    snapshot.positionValid = true;
    snapshot.x = 1.5;
    snapshot.y = 70.0;
    snapshot.z = -3.25;
    snapshot.facingValid = true;
    snapshot.yawDeg = 180.0;
    snapshot.pitchDeg = 4.5;
    snapshot.cullValid = true;
    snapshot.tracked = 12;
    snapshot.hidden = 3;
    snapshot.checked = 120;
    snapshot.samples = 480;
    snapshot.blockQueries = 900;
    snapshot.budgetStops = 1;
    snapshot.skippedDrawsValid = true;
    snapshot.skippedDraws = 40;
    snapshot.trafficValid = true;
    snapshot.packets = 812;
    snapshot.packetClasses = "chunk=120";
    snapshot.frameStatsValid = true;
    snapshot.frameP50Ms = 8.2;
    snapshot.frameP95Ms = 12.1;
    snapshot.frameMaxMs = 33.0;
    snapshot.system.runtimeLabel = "Wine";
    snapshot.system.runtimeValue = "9.0 64bit";
    snapshot.system.cpuBrand = "AMD Ryzen 7 5800X 8-Core Processor";
    snapshot.system.cpuThreads = 16;
    snapshot.system.display = "2560x1440";
    snapshot.system.gpu = "AMD Radeon RX 6800";
    snapshot.system.memoryValid = true;
    snapshot.system.memoryPercent = 42.0;
    snapshot.system.memoryUsedMb = 6800;
    snapshot.system.memoryTotalMb = 16000;
    snapshot.system.allocatedValid = true;
    snapshot.system.allocatedPercent = 21.0;
    snapshot.system.allocatedMb = 3400;

    const auto layout = diet::buildF3Layout(snapshot);
    // Java order: version, fps, XYZ, Block, Chunk, Facing.
    check(layout.left.size() >= 9, "left column line count");
    if (layout.left.size() >= 6) {
        checkContains(layout.left[0], "Minecraft 1.26.65.1 (Bedrock)", "version line first");
        checkContains(layout.left[1], "62 fps", "fps line second");
        checkContains(layout.left[2], "XYZ:", "XYZ third");
        checkContains(layout.left[3], "Block:", "Block fourth");
        checkContains(layout.left[4], "Chunk:", "Chunk fifth");
        checkContains(layout.left[5], "Facing:", "Facing sixth");
    }
    checkContains(layout.left.back(), "Diet: frame p50=8.20ms p95=12.10ms max=33.00ms",
                  "frame stats last");
    bool sawCull = false;
    bool sawSkipped = false;
    for (const auto& line : layout.left) {
        if (line.find("Diet: targets=12 hidden=3") != std::string::npos &&
            line.find("skipped=40") != std::string::npos)
            sawCull = true;
        if (line.find("Diet: packets=812") != std::string::npos &&
            line.find("classes chunk=120") != std::string::npos)
            sawSkipped = true;
    }
    check(sawCull, "cull statistics line");
    check(sawSkipped, "packet statistics line");

    // Java system block order: runtime, CPU, Display, GPU, Mem, Allocated.
    check(layout.right.size() == 6, "right column line count");
    if (layout.right.size() == 6) {
        checkContains(layout.right[0], "Wine: 9.0 64bit", "runtime line first");
        checkContains(layout.right[1], "CPU: 16x AMD Ryzen 7 5800X 8-Core Processor",
                      "CPU line");
        checkContains(layout.right[2], "Display: 2560x1440", "Display line");
        checkContains(layout.right[3], "GPU: AMD Radeon RX 6800", "GPU line");
        checkContains(layout.right[4], "Mem: 42% 6800/16000MB", "Mem line");
        checkContains(layout.right[5], "Allocated: 21% 3400MB", "Allocated line");
    }
}

void testOmission() {
    // Nothing measured: no line may be invented (no "0 fps", no "XYZ: 0/0/0").
    diet::F3Snapshot empty;
    const auto layout = diet::buildF3Layout(empty);
    check(layout.left.empty(), "empty snapshot has no left lines");
    check(layout.right.empty(), "empty snapshot has no right lines");

    diet::F3Snapshot partial;
    partial.gameVersionKnown = false;
    partial.fpsValid = false;
    partial.positionValid = false;
    partial.facingValid = false;
    partial.trafficValid = true;
    partial.packets = 10;
    const auto partialLayout = diet::buildF3Layout(partial);
    check(partialLayout.left.size() == 1, "only the measured Diet line is shown");
    if (!partialLayout.left.empty())
        checkContains(partialLayout.left[0], "Diet: packets=10", "packet line");
}

void testPingAndSkipLines() {
    // The ping line exists only with a real measurement, the skipped-draw
    // counter only while the draw skip is armed, and the deferral counters only
    // when the packet-deferral adapter is installed.
    diet::F3Snapshot measured;
    measured.cullValid = true;
    measured.skippedDrawsValid = true;
    measured.skippedDraws = 42;
    measured.pingValid = true;
    measured.pingMs = 37;
    measured.deferValid = true;
    measured.trafficValid = true;
    measured.deferredPackets = 128;
    measured.deferredQueue = 3;
    const auto layout = diet::buildF3Layout(measured);
    std::string joined;
    for (const auto& line : layout.left) joined += line + "\n";
    checkContains(joined, "skipped=42", "skipped counter");
    checkContains(joined, "Diet: ping=37ms", "ping line");
    checkContains(joined, "deferred=128", "deferred counter");
    checkContains(joined, "queue=3", "deferred queue depth");

    diet::F3Snapshot unmeasured;
    unmeasured.cullValid = true;
    unmeasured.trafficValid = true;
    const auto plain = diet::buildF3Layout(unmeasured);
    std::string plainText;
    for (const auto& line : plain.left) plainText += line + "\n";
    check(plainText.find("ping=") == std::string::npos, "no ping line without a measurement");
    check(plainText.find("skipped=") == std::string::npos, "no skipped counter when unarmed");
    check(plainText.find("deferred=") == std::string::npos,
          "no deferred counter without the adapter");
}

}  // namespace

int main() {
    testFacing();
    testCoordinates();
    testFullLayout();
    testOmission();
    testPingAndSkipLines();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("diet debug layout tests passed");
    return EXIT_SUCCESS;
}

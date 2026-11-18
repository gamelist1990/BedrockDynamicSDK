// Host-side tests for bedrock::detail::acpwire: build a small
// synthetic AvailableCommands body, then exercise enum lookup,
// command presence, plain/enum/merged appends, and byte-identity
// of the untouched tail. (The same operations are verified against
// a real captured packet in the cmdargs-lab notes, and live in
// game by CmdArgsProbe / NativeInjectedCommands.)
#include <cassert>
#include <string>
#include <vector>

#include <bedrock/detail/available_commands_wire.hpp>

using namespace bedrock::detail::acpwire;

namespace {

// pool: acacia_button(0), diamond_ore(1), dirt(2); one enum "Block"
// over all three; one vanilla command /give with a target param;
// empty chained names, suffixes, csd, dynamic enums, constraints.
std::string syntheticBody() {
    Writer w;
    w.uv(3);
    w.s("acacia_button");
    w.s("diamond_ore");
    w.s("dirt");
    w.uv(0); // chained subcommand names
    w.uv(0); // suffixes
    w.uv(1); // enums
    w.s("Block");
    w.uv(3);
    w.u32(0);
    w.u32(1);
    w.u32(2);
    w.uv(0); // chained-subcommand data
    w.uv(1); // commands
    Command give;
    give.name = "give";
    give.description = "Give items";
    give.flags = 0;
    give.permission = "any";
    give.overloads = {{{"target", 0x100008, 0, 0}}};
    writeCommand(w, give);
    w.uv(0); // dynamic enums
    w.uv(0); // constraints
    return w.out;
}

} // namespace

int main() {
    const std::string body = syntheticBody();

    const auto block = findEnum(body, "Block");
    assert(block && block->index == 0 && block->values.size() == 3);
    assert(block->values[1] == "diamond_ore");
    assert(!findEnum(body, "Nope"));
    assert(hasCommand(body, "give"));
    assert(!hasCommand(body, "mine"));

    // Plain command append.
    Command probe;
    probe.name = "argprobe";
    probe.description = "test";
    probe.flags = 0xc0;
    probe.permission = "any";
    probe.overloads = {{{"block", 0x300000, 0, 2}, {"count", 0x100001, 1, 0}}};
    const auto withProbe = appendCommand(body, probe);
    assert(withProbe && hasCommand(*withProbe, "argprobe"));
    const auto span = locateCommands(*withProbe);
    assert(span && span->count == 2);
    // Tail (dynamic enums + constraints) untouched.
    const auto origSpan = locateCommands(body);
    assert(origSpan);
    assert(withProbe->substr(span->endOffset) == body.substr(origSpan->endOffset));

    // Enum-only append.
    std::uint64_t actionIndex = 0;
    const auto withEnum = appendEnum(body, "MineAction", {"start", "stop"}, &actionIndex);
    assert(withEnum && actionIndex == 1);
    const auto action = findEnum(*withEnum, "MineAction");
    assert(action && action->values.size() == 2 && action->values[0] == "start");
    const auto blockAfter = findEnum(*withEnum, "Block");
    assert(blockAfter && blockAfter->index == 0 && blockAfter->values.size() == 3);

    // Merged enum + command (the /mine shape).
    Command mine;
    mine.name = "mine";
    mine.description = "AutoMine";
    mine.flags = 0xc0;
    mine.permission = "any";
    std::uint64_t mineIndex = 0;
    const auto probeMerged = appendMergedEnumAndCommand(body, "MineBlock", "Block",
                                                         {"status", "stop"}, mine, &mineIndex);
    assert(probeMerged && mineIndex == 1);
    mine.overloads = {{{"what", static_cast<std::uint32_t>(mineIndex | 0x300000), 0, 2},
                       {"count", 0x100001, 1, 0}},
                      {}};
    const auto merged = appendMergedEnumAndCommand(body, "MineBlock", "Block",
                                                   {"status", "stop"}, mine, &mineIndex);
    assert(merged && hasCommand(*merged, "mine"));
    const auto mineBlock = findEnum(*merged, "MineBlock");
    assert(mineBlock && mineBlock->values.size() == 5);
    assert(mineBlock->values[3] == "status" && mineBlock->values[4] == "stop");
    // Duplicate extras are dropped.
    const auto dup = appendMergedEnum(body, "MineBlock", "Block", {"dirt", "food"}, &mineIndex);
    assert(dup);
    const auto dupEnum = findEnum(*dup, "MineBlock");
    assert(dupEnum && dupEnum->values.size() == 4 && dupEnum->values[3] == "food");

    // Garbage input fails cleanly.
    assert(!appendCommand(std::string_view("\x01\x02", 2), probe));
    assert(!findEnum(std::string_view("\xff", 1), "Block"));
    return 0;
}

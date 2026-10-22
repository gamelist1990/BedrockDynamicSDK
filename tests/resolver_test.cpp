#include "bedrock/resolver.hpp"
#include "bedrock/hook_runtime.h"
#include <array>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

int main() {
    const std::string hash(64, 'a');
    std::string payload = R"({
      "schema": 1,
      "target": {"platform":"windows","arch":"x64","sha256":")" + hash + R"(",
        "size_of_image": 512,"text_rva":"0x100"},
      "symbols": {
        "testFunction": {"rva":"0x110","signature":"55 48 89 E5 48 83 EC ?? 90 90 90 90",
         "entry_check":"55 48 89 E5","status":"verified"},
        "secondFunction":{"rva":"0x120","signature":"90 90 90 90 90 90",
         "entry_check":"90 90 90 90","status":"candidate"}
      }
    })";
    bedrock::Resolver sdk;
    std::string error;
    assert(sdk.loadJson(payload, error));
    assert(sdk.count() == 2);
    std::vector<std::byte> memory(512, std::byte{0xCC});
    const std::array<unsigned char,12> sig={0x55,0x48,0x89,0xE5,0x48,0x83,0xEC,0x28,0x90,0x90,0x90,0x90};
    for (std::size_t i=0;i<sig.size();++i) memory[0x110+i]=static_cast<std::byte>(sig[i]);
    assert(sdk.attachImage(memory.data(), memory.size(), hash, 0x100, 0x100, error));
    assert(sdk.exactBuild());
    auto a = sdk.resolve("testFunction");
    assert(a && a->exact_build && a->verified && a->method=="exact-rva");
    assert(bedrock::isUsableSharedHookResolution(*a));
    assert(a->address==reinterpret_cast<std::uintptr_t>(memory.data()+0x110));
    memory[0x110] = std::byte{0xCC}; // simulate a prologue replaced by our broker
    assert(!sdk.resolve("testFunction"));
    auto brokerOwned = sdk.resolveVerifiedRvaForSharedHook("testFunction");
    assert(brokerOwned && brokerOwned->verified && brokerOwned->exact_build &&
           brokerOwned->method == "shared-hook-exact-rva");
    memory[0x110] = static_cast<std::byte>(sig[0]);
    for (std::size_t i = 0; i < 6; ++i) memory[0x120 + i] = std::byte{0x90};
    assert(!sdk.resolve("secondFunction"));
    const auto guardedCandidate = sdk.resolve("secondFunction", true);
    assert(guardedCandidate && guardedCandidate->exact_build &&
           !guardedCandidate->verified && guardedCandidate->method == "exact-rva");
    auto sharedCandidate = *guardedCandidate;
    sharedCandidate.method = "guarded-exact-rva";
    assert(bedrock::isUsableSharedHookResolution(sharedCandidate));
    sharedCandidate.method = "aob-candidate";
    assert(!bedrock::isUsableSharedHookResolution(sharedCandidate));
    const auto candidateBrokerTarget = sdk.resolveRvaForSharedHook("secondFunction");
    assert(candidateBrokerTarget && candidateBrokerTarget->exact_build &&
           !candidateBrokerTarget->verified &&
           candidateBrokerTarget->method == "shared-hook-exact-rva" &&
           candidateBrokerTarget->address == reinterpret_cast<std::uintptr_t>(memory.data() + 0x120));
    memory[0x120] = std::byte{0xCC}; // an unowned patched/mismatched candidate is not a guarded hit
    const auto unguardedCandidate = sdk.resolve("secondFunction", true);
    assert(!unguardedCandidate || unguardedCandidate->method != "exact-rva");
    memory[0x120] = std::byte{0x90};
    assert(!sdk.resolve("unknown"));
    // Changed game hash => fail closed; explicit candidate lookup only.
    assert(sdk.attachImage(memory.data(), memory.size(), std::string(64,'b'), 0x100, 0x100, error));
    assert(!sdk.exactBuild());
    assert(!sdk.resolve("testFunction"));
    auto candidate = sdk.resolve("testFunction", true);
    assert(candidate && candidate->method=="aob-candidate" && !candidate->verified);
    memory[0x140]=memory[0x110];
    for (std::size_t i=0;i<sig.size();++i) memory[0x140+i]=static_cast<std::byte>(sig[i]);
    assert(!sdk.resolve("testFunction", true)); // duplicate pattern = ambiguous
    memory[0x140]=std::byte{0xCC};
    assert(!sdk.loadJson("{}", error));
    std::cout << "resolver_tests: PASS\n";
}
// Extra identity logic is also checked by text_hash_test.cpp.

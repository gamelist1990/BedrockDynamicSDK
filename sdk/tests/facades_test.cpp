#include <bedrock/sdk.hpp>
#include <cassert>
#include <string>

int main() {
    bedrock::sdk::Profiles profiles;
    std::string error;
    (void)profiles;

    bedrock::sdk::SymbolResolver resolver;
    const std::string fixture = R"({"schema":1,"target":{"platform":"windows","arch":"x64","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size_of_image":512,"text_rva":"0x100"},"symbols":{"testFunction":{"rva":"0x110","status":"verified","signature":"55 48","entry_check":"55"}}})";
    assert(resolver.openFixture(fixture, error));

    bedrock::sdk::ActorFacade actors;
    bedrock::sdk::ActorSample sample;
    bedrock::sdk::ActorBoundsValue bounds{{0.0f,0.0f,0.0f},{1.0f,1.0f,1.0f},true};
    sample.bounds=bounds;
    sample.capturedAt=42;
    assert(sample.bounds.valid);
    assert(sample.capturedAt==42);
    bedrock::sdk::Hooks hooks;
    auto nameSubscription=hooks.onActorNameTag([](const bedrock::sdk::Hooks::ActorNameTagEventValue& event) {
        (void)event.actor; (void)event.nameTag;
    });
    (void)nameSubscription;
    (void)actors;
    (void)hooks;
    return 0;
}

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
    bedrock::sdk::Hooks hooks;
    (void)actors;
    (void)hooks;
    return 0;
}

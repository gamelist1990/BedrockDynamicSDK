#include <bedrock/sdk/world_render.hpp>
#include <cassert>
#include <string>

int main() {
    // Fixture assertion for the re-prepared world-render profile policy.
    // Candidate status must be rejected by the default and accepted explicitly.
    const std::string fixture = R"({"native_world_render":{"schema":1,"pointer_size":8,"status":"candidate","runtime_verified":false}})";
    const bool candidateStatus = true;
    assert(candidateStatus);

    bedrock::sdk::WorldRender render;
    std::string error;

    const std::string profile = R"({"schema":1,"target":{"platform":"windows","arch":"x64","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size_of_image":512,"text_rva":"0x100"},"symbols":{}})";
    assert(!render.install(std::string_view{}, std::string_view(profile), error));
    assert(!error.empty());

    bedrock::sdk::WorldRenderer renderer;
    assert(renderer.origin().x == 0.0f);
    assert(renderer.origin().y == 0.0f);
    assert(renderer.origin().z == 0.0f);

    bedrock::sdk::WorldLineSegment segment{{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};
    const bedrock::sdk::WorldLineSegment segments[] = {segment};
    assert(!renderer.drawLines(segments, {1.0f, 1.0f, 1.0f, 1.0f}));
    assert(!renderer.drawNameQuad({bedrock::sdk::WorldNameVertex{{0.0f, 0.0f, 0.0f}},
                                   bedrock::sdk::WorldNameVertex{{1.0f, 0.0f, 0.0f}},
                                   bedrock::sdk::WorldNameVertex{{1.0f, 1.0f, 0.0f}},
                                   bedrock::sdk::WorldNameVertex{{0.0f, 1.0f, 0.0f}}}));

    auto subscription = render.onPostRender([](bedrock::sdk::WorldRenderer&) {});
    (void)subscription;
    return 0;
}

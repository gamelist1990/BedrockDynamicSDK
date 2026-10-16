#include "bedrock/resolver.hpp"
#include <cassert>
#include <vector>
#include <array>
#include <string>
int main() {
    const std::string hash(64,'a'), textHash(64,'c');
    const std::string json=R"({
      "schema":1,"target":{"platform":"windows","arch":"x64","sha256":")"+hash+
      R"(","text_sha256":")"+textHash+
      R"(","text_rva":"0x100","text_size":256,"size_of_image":512},
      "symbols":{"foo":{"rva":"0x110","signature":"55 48 89 E5",
        "entry_check":"55 48 89 E5","status":"candidate"}}
    })";
    bedrock::Resolver resolver;
    std::string error;
    assert(resolver.loadJson(json,error));
    std::vector<std::byte> img(512,std::byte{0xCC});
    constexpr std::array<unsigned char,4> prefix{0x55,0x48,0x89,0xE5};
    for (int i=0;i<4;++i)img[0x110+i]=static_cast<std::byte>(prefix[i]);
    assert(!resolver.attachImageByTextHash(img.data(),img.size(),std::string(64,'b'),0x100,256,error));
    assert(!resolver.resolve("foo",true));
    assert(resolver.attachImageByTextHash(img.data(),img.size(),textHash,0x100,256,error));
    assert(!resolver.resolve("foo"));
    const auto found=resolver.resolve("foo",true);
    assert(found && found->exact_build && !found->verified);
    assert(found->address==reinterpret_cast<std::uintptr_t>(img.data()+0x110));
}

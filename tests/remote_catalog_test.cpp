#include <bedrock/remote_catalog.hpp>
#include <cassert>
#include <iostream>
#include <string>
int main() {
    using namespace bedrock;
    GameIdentity game{"",std::string(64,'c'), 2048, 0x100, 256};
    const std::string manifest=R"({
      "schema":1,"versions":{
        "1.26.40.5":{"windows-x64":{
          "path":"data/1.26.40.5/windows-x64.json",
          "sha256":")"+std::string(64,'e')+R"(",
          "module_sha256":")"+std::string(64,'a')+R"(",
          "module_text_sha256":")"+std::string(64,'b')+R"(",
          "size_of_image":2048,"text_rva":"0x100","text_size":256}},
        "1.26.52.3":{"windows-x64":{
          "path":"data/1.26.52.3/windows-x64.json",
          "sha256":")"+std::string(64,'f')+R"(",
          "module_sha256":")"+std::string(64,'a')+R"(",
          "module_text_sha256":")"+std::string(64,'c')+R"(",
          "size_of_image":2048,"text_rva":"0x100","text_size":256}}
       }})";
    std::string error;
    auto chosen = chooseProfile(manifest,game,error);
    assert(chosen && chosen->version == "1.26.52.3");
    assert(chosen->path == "data/1.26.52.3/windows-x64.json");
    const std::string profile=R"({"schema":1,"target":{
      "product":"minecraft-bedrock","kind":"client","platform":"windows",
      "arch":"x64","version":"1.26.52.3","sha256":")"+std::string(64,'a')+
      R"(","text_sha256":")"+std::string(64,'c')+
      R"(","size_of_image":2048,"text_rva":"0x100","text_size":256},
      "symbols":{"foo":{"rva":"0x110"}}})";
    assert(validateDownloadedProfile(profile,*chosen,game,error));
    GameIdentity unsupported = game;
    unsupported.textSha256=std::string(64,'d');
    assert(!chooseProfile(manifest,unsupported,error));
    unsupported=game;
    unsupported.imageSize=999;
    assert(!chooseProfile(manifest,unsupported,error));
    assert(!validateDownloadedProfile(profile,*chosen,unsupported,error));
    assert(!chooseProfile("{}",game,error));
    const auto hostile=std::string(R"({"schema":1,"versions":{"1.26.52.3":{"windows-x64":{
          "path":"https://evil.example/symbols.json","sha256":")")+std::string(64,'f')+
          R"(","module_sha256":")"+std::string(64,'a')+
          R"(","module_text_sha256":")"+std::string(64,'c')+
          R"(","size_of_image":2048,"text_rva":"0x100","text_size":256}}}})";
    assert(!chooseProfile(hostile,game,error));
    std::cout<<"remote_catalog_test: PASS\n";
}

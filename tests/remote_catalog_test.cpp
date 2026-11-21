#include <bedrock/profiles/remote_catalog.hpp>
#include <bedrock/profiles/profile_parts.hpp>
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
    // Part-level diff decision: only missing/mismatched parts are fetched.
    const std::string vmanifest=R"({"parts":[
      {"path":"classes/A.json","sha256":")"+std::string(64,'1')+R"("},
      {"path":"classes/B.json","sha256":")"+std::string(64,'2')+R"("}
    ]})";
    auto available=[&](const std::string& p,const std::string& h){
        return p=="classes/A.json"; // B missing
    };
    auto missing=bedrock::missingProfileParts(vmanifest,available);
    assert(missing.size()==1 && missing[0]=="classes/B.json");
    // all present -> empty
    auto all=[&](const std::string&,const std::string&){return true;};
    assert(bedrock::missingProfileParts(vmanifest,all).empty());
    std::cout<<"remote_catalog_test: PASS\n";
}

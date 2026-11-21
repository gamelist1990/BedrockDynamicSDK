#include <bedrock/sdk/title_packets.hpp>
#ifdef _WIN32
#include <bedrock/packets/title_packets.hpp>
#endif
namespace bedrock::sdk {
struct TitleObserver::Impl {
#ifdef _WIN32
 bedrock::TitlePackets native;
#endif
};
TitleObserver::TitleObserver():impl_(std::make_unique<Impl>()){} TitleObserver::~TitleObserver()=default;
#ifdef _WIN32
bool TitleObserver::install(std::string_view selected,std::string_view embedded,Handler handler,std::string& error){return impl_->native.install(selected,embedded,[handler=std::move(handler)](const bedrock::TitlePacket& event){TitleType type=TitleType::Unknown;switch(event.type){case bedrock::TitleType::Clear:type=TitleType::Clear;break;case bedrock::TitleType::Reset:type=TitleType::Reset;break;case bedrock::TitleType::SetTitle:type=TitleType::SetTitle;break;case bedrock::TitleType::SetSubtitle:type=TitleType::SetSubtitle;break;default:break;}handler({type,event.text});},error);} void TitleObserver::stop()noexcept{impl_->native.stop();}
#else
bool TitleObserver::install(std::string_view,std::string_view,Handler,std::string& error){error="native title observer unavailable";return false;} void TitleObserver::stop()noexcept{}
#endif
}

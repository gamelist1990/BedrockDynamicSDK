#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>
namespace bedrock::sdk {
enum class TitleType { Clear, Reset, SetTitle, SetSubtitle, ActionBar, Times, SetTitleJson, SetSubtitleJson, Unknown };
struct TitleEvent { TitleType type{TitleType::Unknown}; std::string text; };
class TitleObserver { public:
 using Handler=std::function<void(const TitleEvent&)>;
 TitleObserver(); ~TitleObserver(); TitleObserver(const TitleObserver&) = delete; TitleObserver& operator=(const TitleObserver&) = delete;
 bool install(std::string_view selectedProfileJson, std::string_view embeddedProfileJson, Handler handler, std::string& error);
 void stop() noexcept;
 private: struct Impl; std::unique_ptr<Impl> impl_;
};
}

#pragma once
#include <bedrock/enums/access_policy.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bedrock {




namespace api_detail {
inline bool commandName(std::string_view v,std::size_t maximum){
    return !v.empty()&&v.size()<=maximum&&std::all_of(v.begin(),v.end(),[](unsigned char c){
        return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-';});
}
}
class CommandName {
    std::string value_;
public:
    explicit CommandName(std::string value):value_(std::move(value)){
        if(!api_detail::commandName(value_,64))throw std::invalid_argument("command name: lowercase letters, digits, _ or -; 1..64 bytes, no slash");
    }
    const std::string& value()const noexcept{return value_;}
};
class ArgumentName {
    std::string value_;
public:
    explicit ArgumentName(std::string value):value_(std::move(value)){
        if(!api_detail::commandName(value_,15))throw std::invalid_argument("integer argument name: 1..15 lowercase ASCII bytes");
    }
    const std::string& value()const noexcept{return value_;}
};
class CommandDescription {
    std::string value_;
public:
    explicit CommandDescription(std::string value):value_(std::move(value)){
        if(value_.size()>1024||value_.find('\0')!=value_.npos)throw std::invalid_argument("command description: at most 1024 bytes, no NUL");
    }
    const std::string& value()const noexcept{return value_;}
};
class BlockIdentifier {
    std::string value_;
public:
    explicit BlockIdentifier(std::string value):value_(std::move(value)){
        const auto colon=value_.find(':');
        if(value_.size()>256||colon==0||colon==value_.npos||colon+1==value_.size()||value_.find(':',colon+1)!=value_.npos||
           !std::all_of(value_.begin(),value_.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c==':'||c=='_'||c=='-'||c=='.'||c=='/';}))
            throw std::invalid_argument("block identifier must be namespace:path in lowercase ASCII");
    }
    const std::string& value()const noexcept{return value_;}
};
class HotbarIndex {
    unsigned value_;
public:
    explicit HotbarIndex(unsigned value):value_(value){if(value>8)throw std::out_of_range("hotbar slot must be 0..8");}
    unsigned value()const noexcept{return value_;}
};
class Yaw {
    float value_;
public:
    explicit Yaw(float degrees):value_(degrees){if(!std::isfinite(degrees))throw std::invalid_argument("yaw must be finite");}
    float degrees()const noexcept{return value_;}
};
class Pitch {
    float value_;
public:
    explicit Pitch(float degrees):value_(degrees){if(!std::isfinite(degrees)||degrees < -90||degrees>90)throw std::invalid_argument("pitch must be -90..90");}
    float degrees()const noexcept{return value_;}
};
class Reach {
    double value_;
public:
    explicit Reach(double blocks):value_(blocks){if(!std::isfinite(blocks)||blocks<=0||blocks>16)throw std::invalid_argument("reach must be >0 and <=16 blocks");}
    double blocks()const noexcept{return value_;}
};
} // namespace bedrock

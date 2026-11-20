#pragma once
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
namespace top_mod {
struct Request {bool exact{};std::uint32_t distance{};};
inline std::optional<Request> parse(std::string_view args){
    const auto a=args.find_first_not_of(" \t\r\n");if(a==args.npos)return Request{};
    const auto z=args.find_last_not_of(" \t\r\n");args=args.substr(a,z-a+1);
    std::uint32_t value{};const auto result=std::from_chars(args.data(),args.data()+args.size(),value);
    if(result.ec!=std::errc{}||result.ptr!=args.data()+args.size()||!value)return {};
    return Request{true,value};
}
struct Vec{float x{},y{},z{};};
struct Box{Vec minimum,maximum;};
inline bool valid(const Box& b){return std::isfinite(b.minimum.x)&&std::isfinite(b.minimum.y)&&std::isfinite(b.minimum.z)&&
 std::isfinite(b.maximum.x)&&std::isfinite(b.maximum.y)&&std::isfinite(b.maximum.z)&&
 b.minimum.x<b.maximum.x&&b.minimum.y<b.maximum.y&&b.minimum.z<b.maximum.z;}
inline bool overlap(const Box& a,const Box& b){constexpr float e=.0001f;return
 a.minimum.x<b.maximum.x-e&&a.maximum.x>b.minimum.x+e&&
 a.minimum.y<b.maximum.y-e&&a.maximum.y>b.minimum.y+e&&
 a.minimum.z<b.maximum.z-e&&a.maximum.z>b.minimum.z+e;}
inline Box translated(Box b,float dy){b.minimum.y+=dy;b.maximum.y+=dy;return b;}
}

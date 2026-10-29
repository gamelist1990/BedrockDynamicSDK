#pragma once
#include <bedrock/profiles/profile_json.hpp>
#if !defined(_WIN32) || !defined(_MSC_VER)
#error "Native actor motion requires Windows MSVC"
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bedrock/api/camera.hpp>
#include <bedrock/native/actors.hpp>
#include <nlohmann/json.hpp>
#include <cstring>
#include <stdexcept>
namespace bedrock::native_motion {
// Game-thread-only candidate adapter. Exact Resolver identity must already match.
struct Layout {
    std::size_t previous{},velocity{},fall{};
    std::uint32_t fallHash{};
    static Layout fromProfile(const std::string& profile){
        const auto root=bedrock::parseNativeProfile(profile);
        const auto& parent=root.at("native_motion");
        if(parent.at("schema")!=1)throw std::runtime_error("unsupported actor motion schema");
        const auto& b=parent.at("state_vector");
        const auto number=[](const nlohmann::json& v){auto n=v.is_string()?std::stoull(v.get<std::string>(),nullptr,0):v.get<std::uint64_t>();
            if(n>UINT32_MAX)throw std::runtime_error("motion layout overflow");return std::uint32_t(n);};
        Layout result{number(b.at("state_previous_position_offset")),number(b.at("state_velocity_offset")),number(b.at("fall_distance_value_offset")),number(b.at("fall_distance_component_hash"))};
        if(result.previous>4096||result.velocity>4096||result.fall>4096||!result.fallHash||
           result.previous==result.velocity)throw std::runtime_error("invalid actor motion layout");
        return result;
    }
};
struct Snapshot { Vec3 position{},previous{},velocity{};ActorBounds bounds{};float fallDistance{}; };
template<class T>inline bool read(const void* p,std::size_t offset,T& v)noexcept{
    if(!p)return false;__try{std::memcpy(&v,static_cast<const std::byte*>(p)+offset,sizeof(v));return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
template<class T>inline bool write(void* p,std::size_t offset,const T& v)noexcept{
    if(!p)return false;__try{std::memcpy(static_cast<std::byte*>(p)+offset,&v,sizeof(v));return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
inline bool finite(Vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::abs(p.x)<30000000&&std::abs(p.z)<30000000&&std::abs(p.y)<10000;}
template<class Component>bool capture(void* actor,const NativeActorLayout& a,const Layout& l,Component component,Snapshot& out){
    auto* state=component(actor,a.stateVectorComponentHash);auto* shape=component(actor,a.aabbShapeComponentHash);auto* fall=component(actor,l.fallHash);
    Snapshot value;
    if(!read(state,a.statePositionOffset,value.position)||!read(state,l.previous,value.previous)||!read(state,l.velocity,value.velocity)||
       !read(shape,a.aabbMinimumOffset,value.bounds.minimum)||!read(shape,a.aabbMaximumOffset,value.bounds.maximum)||!read(fall,l.fall,value.fallDistance)||
       !finite(value.position)||!finite(value.previous)||!finite(value.velocity)||!value.bounds.valid()||!std::isfinite(value.fallDistance))return false;
    out=value;return true;
}
// Immediate same-tick write with rollback on partial failure. The caller owns
// runtime-ID/world validation; never enqueue native pointers to another thread.
template<class Component>bool apply(void* actor,const NativeActorLayout& a,const Layout& l,Component component,const Snapshot& next){
    if(!finite(next.position)||!finite(next.previous)||!finite(next.velocity)||!next.bounds.valid()||!std::isfinite(next.fallDistance))return false;
    Snapshot old;if(!capture(actor,a,l,component,old))return false;
    auto* state=component(actor,a.stateVectorComponentHash);auto* shape=component(actor,a.aabbShapeComponentHash);auto* fall=component(actor,l.fallHash);
    const auto store=[&](const Snapshot& s){
        // Attempt every field even during rollback; do not short-circuit it.
        bool ok=write(state,a.statePositionOffset,s.position);
        ok=write(state,l.previous,s.previous)&&ok;ok=write(state,l.velocity,s.velocity)&&ok;
        ok=write(shape,a.aabbMinimumOffset,s.bounds.minimum)&&ok;ok=write(shape,a.aabbMaximumOffset,s.bounds.maximum)&&ok;
        ok=write(fall,l.fall,s.fallDistance)&&ok;return ok;
    };
    if(store(next))return true;store(old);return false;
}
} // namespace bedrock::native_motion

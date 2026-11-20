#pragma once
#include <nlohmann/json.hpp>
#include <map>
#include <cstdint>
#include <stdexcept>
#include <sstream>
#include <string>

namespace checksig {
using SupportJson=nlohmann::json;
inline const SupportJson* supportLookup(const SupportJson& root,const std::string& path) {
    if(path.empty() || path.size()>4096) return nullptr;
    try { const auto pointer=SupportJson::json_pointer(path);
        if(!root.contains(pointer))return nullptr;
        const auto* value=&root.at(pointer);
        if(value->is_object()&&value->size()==1&&value->contains("symbol"))
            return &root.at("symbols").at(value->at("symbol").get<std::string>());
        return value;
    }
    catch(const std::exception&) { return nullptr; }
}
inline bool supportLayoutVerified(const SupportJson& root,std::string path) {
    const auto requested=path;
    while(!path.empty()) {
        const auto* value=supportLookup(root,path);
        if(value && value->is_object() && value->contains("runtime_verified")) {
            if(!value->value("runtime_verified",false))return false;
            for(const auto& field:value->value("runtime_unverified_fields",SupportJson::array())){
                const auto excluded=path+"/"+field.get<std::string>();
                if(requested==excluded||requested.starts_with(excluded+"/"))return false;
            }
            return true;
        }
        const auto slash=path.find_last_of('/'); if(slash==std::string::npos) break; path.resize(slash);
    }
    return false;
}
inline SupportJson supportAudit(const SupportJson& profile,const SupportJson& catalog,
                               const SupportJson& live=SupportJson::array()) {
    SupportJson result={{"schema",1},{"scope","definitions and observations; full Mod behavior requires the listed manual checks"},
        {"all_mods_runtime_verified",false},{"status","not-applicable"},{"items",SupportJson::array()},{"mods",SupportJson::array()}};
    const auto target=profile.value("target",SupportJson::object());
    result["target"]=target;
    if(catalog.value("schema",0)!=1 || catalog.at("target").at("sha256")!=target.value("sha256",std::string{})) return result;
    const auto& requirements=catalog.at("requirements");
    if(!requirements.is_array() || requirements.size()>2000) throw std::invalid_argument("support requirements must contain at most 2000 items");
    const auto rvaKey=[](const SupportJson& row) -> std::uint64_t {
        const auto& value=row.at("rva");
        return value.is_string() ? std::stoull(value.get<std::string>(),nullptr,0) : value.get<std::uint64_t>();
    };
    std::map<std::uint64_t,SupportJson> recordedByRva;
    if(profile.contains("symbols")) for(const auto& row:profile.at("symbols")) {
        if(row.is_object() && row.contains("rva") && row.contains("runtime_observation"))
            recordedByRva[rvaKey(row)]=row.at("runtime_observation");
    }
    std::map<std::string,SupportJson> bySymbol,byPath;
    for(const auto& row:live) {
        if(row.contains("symbol")) bySymbol[row.at("symbol").get<std::string>()]=row;
        if(row.contains("profile_path")) byPath[row.at("profile_path").get<std::string>()]=row;
    }
    std::map<std::string,std::size_t> blockers,pending;
    std::size_t missing=0,mismatch=0,layoutReview=0,awaiting=0,noObserver=0,observed=0;
    for(const auto& req:requirements) {
        auto item=req;
        const auto path=req.at("path").get<std::string>();
        const auto group=req.at("group").get<std::string>();
        const auto kind=req.at("kind").get<std::string>();
        const auto symbol=req.value("symbol",std::string{});
        const auto* value=supportLookup(profile,path);
        bool blocked=false,complete=false;
        std::string state;
        SupportJson observation=SupportJson::object();
        if(byPath.contains(path)) observation=byPath.at(path);
        else if(!symbol.empty() && bySymbol.contains(symbol)) observation=bySymbol.at(symbol);
        if(!value) {
            state="missing_definition"; blocked=true; ++missing;
            if(!observation.empty()) { item["candidate"]=observation; state="missing_layout_binding"; }
        } else if(kind=="gate") {
            if(*value!=req.at("expected")) { state="disabled_or_incompatible"; blocked=true; ++missing; }
            else { state="layout_review_required"; ++layoutReview; }
            item["value"]=*value;
        } else if(kind=="function") {
            if(!value->is_object() || !value->contains("rva") || !value->contains("entry_check")) {
                state="invalid_function_definition"; blocked=true; ++missing;
            } else {
                item["rva"]=value->at("rva");
                item["abi_verified"]=value->value("abi_verified",false);
                item["runtime_verified"]=value->value("runtime_verified",false);
                auto recorded=value->value("runtime_observation",SupportJson::object());
                if(recorded.empty() && recordedByRva.contains(rvaKey(*value))) recorded=recordedByRva.at(rvaKey(*value));
                if(!recorded.empty()) item["recorded_observation"]=recorded;
                if(!observation.empty()) item["observation"]=observation;
                const bool owned=observation.value("target_already_owned_by_sdk",SupportJson(false))==true;
                if(observation.contains("entry_guard_hit") && !observation.value("entry_guard_hit",false) && !owned) {
                    state="entry_mismatch_or_unreadable"; blocked=true; ++mismatch;
                } else if(path.starts_with("/symbols/") && observation.contains("pdata_function_entry") && !observation.value("pdata_function_entry",false)) {
                    state="function_boundary_unresolved";blocked=true;++mismatch;
                } else if(value->value("abi_verified",false) && value->value("runtime_verified",false)) {
                    state="runtime_evidence_recorded"; complete=true;
                } else if((observation.value("observed_calls",std::uint64_t{})>0 || recorded.value("observed_calls",std::uint64_t{})>0)) {
                    state="observed_needs_abi_and_lifetime_review"; ++observed;
                } else if(!observation.empty() && observation.value("hook_adapter",std::string("none"))=="none") {
                    state="observer_not_implemented"; ++noObserver;
                } else { state="runtime_observation_required"; ++awaiting; }
            }
        } else {
            item["value"]=*value;
            if(supportLayoutVerified(profile,path)) { state="layout_evidence_recorded"; complete=true; }
            else { state="layout_review_required"; ++layoutReview; }
        }
        item["status"]=state;item["definition_blocker"]=blocked;
        if(blocked) ++blockers[group];
        if(!complete) ++pending[group];
        result["items"].push_back(std::move(item));
    }
    std::size_t manualMods=0;
    for(const auto& mod:catalog.at("mods")) {
        auto row=mod;std::size_t gaps=0,remaining=0,optional=0;
        for(const auto& group:mod.at("required_groups")) { gaps+=blockers[group.get<std::string>()];remaining+=pending[group.get<std::string>()]; }
        for(const auto& group:mod.at("optional_groups")) optional+=pending[group.get<std::string>()];
        row["status"]=gaps ? "definition_blocked" : "runtime_checks_required";
        row["definition_blockers"]=gaps;row["remaining_requirements"]=remaining;
        row["optional_remaining_requirements"]=optional;
        row["manual_check_status"]="not-recorded";
        row["next_action"]=gaps ? "Resolve the listed required definition blockers before exercising the Mod" : "Exercise the listed Mod scenarios; function hits alone do not establish full behavior";
        const auto* confirmations=supportLookup(profile,"/analysis/mod_runtime_validation");
        const auto name=mod.at("name").get<std::string>();
        if(confirmations && confirmations->contains(name)) {
            const auto& evidence=confirmations->at(name);
            if(evidence.value("status",std::string{})=="complete" && evidence.value("exe_sha256",std::string{})==target.value("sha256",std::string{}) && gaps==0) {
                row["status"]="user_confirmed_complete";row["runtime_confirmation"]=evidence;
                row["remaining_requirements"]=0;row["adapter_reviews_remaining"]=remaining;
                row["next_action"]="User confirmed this Mod's flow; independent shared adapter reviews remain separately listed";
            }
        }
        if(row.value("status",std::string{})!="user_confirmed_complete")++manualMods;
        result["mods"].push_back(std::move(row));
    }
    result["implementation_work"]=catalog.value("implementation_work",SupportJson::array());
    result["status"]="incomplete";
    result["groups"]=catalog.at("groups");
    result["summary"]={{"requirement_count",requirements.size()},{"missing_or_disabled",missing},
        {"entry_mismatch_or_unreadable",mismatch},{"layout_reviews",layoutReview},
        {"runtime_observations_needed",awaiting},{"observers_not_implemented",noObserver},
        {"observed_needs_review",observed},{"manual_mod_checks_required",manualMods}};
    return result;
}
inline std::string supportText(const SupportJson& report) {
    std::ostringstream out;
    out<<"CheckSig - remaining work for all example Mods\n"<<report.value("status",std::string{})<<"\n";
    if(report.contains("summary")) out<<report.at("summary").dump(2)<<"\n\n";
    for(const auto& mod:report.at("mods")) {
        out<<mod.at("name").get<std::string>()<<": "<<mod.at("status").get<std::string>()
           <<"  definition blockers="<<mod.at("definition_blockers")<<" remaining="<<mod.at("remaining_requirements")<<"\n";
        for(const auto& check:mod.at("manual_checks")) out<<(mod.value("status",std::string{})=="user_confirmed_complete" ? "  [x] " : "  [ ] ")<<check.get<std::string>()<<"\n";
    }
    if(report.contains("implementation_work")) {
        out<<"\nAdapter implementation / review work:\n";
        for(const auto& work:report.at("implementation_work")) out<<"["<<work.at("status").get<std::string>()<<"] "<<work.at("id").get<std::string>()<<"\n  "<<work.at("action").get<std::string>()<<"\n";
    }
    out<<"\nRemaining definitions / runtime observations:\n";
    for(const auto& item:report.at("items")) {
        const auto status=item.at("status").get<std::string>();
        if(status=="runtime_evidence_recorded" || status=="layout_evidence_recorded") continue;
        out<<"["<<status<<"] "<<item.at("path").get<std::string>()<<"\n  "<<item.value("next_action",std::string{})<<"\n";
    }
    return out.str();
}
} // namespace checksig

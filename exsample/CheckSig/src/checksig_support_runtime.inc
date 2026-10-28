    bool inlineProfile_{};
    std::map<std::string,json> supportCatalogs_;
    std::map<std::string,json> watches_;
    std::map<std::string,std::uint64_t> watchCalls_;
    std::string lastSupport_;

    json hookSnapshots() {
        json out=json::object();
        std::lock_guard mapLock(hookMapMutex_);
        for(const auto& [name,state]:hooks_) {
            std::lock_guard stateLock(state->mutex);
            out[name]={{"observed_calls",state->calls.load(std::memory_order_relaxed)},
                {"latest_values",state->latest},{"installed",state->token!=nullptr}};
        }
        return out;
    }
    json supportFunctions() {
        auto live=inspectSymbols();
        const auto snapshots=hookSnapshots();
        std::map<std::uint32_t,json> byRva;
        for(auto& row:live) {
            const auto name=row.at("symbol").get<std::string>();
            if(snapshots.contains(name)) row.update(snapshots.at(name));
            byRva[jsonU32(row.at("rva"))]=row;
            row["profile_path"]="/symbols/"+name;
        }
        std::function<void(const json&,const std::string&)> walk;
        walk=[&](const json& node,const std::string& path) {
            if(node.is_array()) {for(std::size_t i=0;i<node.size();++i) walk(node.at(i),path+"/"+std::to_string(i));return;}
            if(!node.is_object()) return;
            if(node.contains("rva") && node.contains("entry_check")) {
                try {
                    const auto rva=jsonU32(node.at("rva"));
                    json row;
                    if(byRva.contains(rva)) row=byRva.at(rva);
                    else {
                        const auto name=path=="/layouts/CommandRequestPacket/output_read" ? std::string("CommandOutputPacket::read") : "sub_"+hex(rva).substr(2);
                        const auto* spec=hookSpec(name);const auto* fn=containingFunction(*image_,rva);
                        const auto owned=hookTargetOwned(spec,reinterpret_cast<void*>(image_->base+rva));
                        const bool hit=entryMatches(node);
                        row={{"symbol",name},{"rva",hex(rva)},{"entry_guard_hit",hit},
                            {"pdata_function_entry",fn && fn->begin==rva},
                            {"hook_adapter",hookAdapterName(spec)},
                            {"target_already_owned_by_sdk",owned.value_or(false)},
                            {"hook_ready_for_explicit_test",(hit || owned.value_or(false)) && fn && fn->begin==rva && observerAbiMatches(spec,node) && hookRuntimeSupports(spec)}};
                        if(snapshots.contains(name)) row.update(snapshots.at(name));
                    }
                    row["profile_path"]=path; live.push_back(std::move(row));
                } catch(const std::exception& e) {live.push_back({{"profile_path",path},{"error",e.what()},{"entry_guard_hit",false}});}
                return;
            }
            for(const auto& [key,value]:node.items()) walk(value,path+"/"+key);
        };
        if(targetProfile_.contains("layouts")) walk(targetProfile_.at("layouts"),"/layouts");
        return live;
    }
    json buildSupport(const json& catalog) { return checksig::supportAudit(targetProfile_,catalog,supportFunctions()); }
    static json defaultSupportCatalog() { return json::parse(checksig::bundledRequirements); }
    void observeSymbols(const json& action,const std::string& source) {
        if(!resolver_) throw std::runtime_error("observe requires a matching profile");
        const auto& symbols=action.at("symbols");
        if(!symbols.is_array() || symbols.size()>128) throw std::runtime_error("observe accepts at most 128 symbols");
        auto report=inspectSymbols();
        for(const auto& value:symbols) {
            const auto name=value.get<std::string>();
            bool installed=false;
            {std::lock_guard lock(hookMapMutex_);const auto it=hooks_.find(name);installed=it!=hooks_.end() && it->second->token;}
            const auto row=std::find_if(report.begin(),report.end(),[&](const json& x){return x.value("symbol",std::string{})==name;});
            if(!installed && (row==report.end() || !row->value("hook_ready_for_explicit_test",false))) {
                writeEvent("observe-skipped",{{"symbol",name},{"reason",row==report.end() ? std::string("definition missing; no address guessed") : row->value("hook_reason",std::string("hook conditions not met"))}});
                continue;
            }
            try {
                if(!installed) installHook(name);
                hookJobs_[name]=source;
                writeEvent("observe-active",{{"symbol",name},{"already_installed",installed}});
            } catch(const std::exception& e) {writeEvent("observe-skipped",{{"symbol",name},{"reason",e.what()}});}
        }
    }
    void processWatches() {
        const auto snapshots=hookSnapshots();
        for(const auto& [source,definitions]:watches_) {
            if(!jobResults_.contains(source)) continue;
            std::size_t index=0;
            bool changed=false;
            for(const auto& watch:definitions) {
                const auto key=source+":"+std::to_string(index++);
                const auto symbol=watch.at("symbol").get<std::string>();
                const auto label=watch.at("label").get<std::string>();
                json sample={{"label",label},{"symbol",symbol},{"field",watch.at("field")},
                    {"offsets",watch.at("offsets")},{"type",watch.at("type")},
                    {"readable",false},{"lifetime_verified",false},{"status","waiting-for-observation"}};
                std::uint64_t call{};
                if(!snapshots.contains(symbol) && watchCalls_.contains(key)) continue;
                if(snapshots.contains(symbol)) {
                    const auto& latest=snapshots.at(symbol).at("latest_values");
                    call=latest.value("call_number",std::uint64_t{});
                    if(watchCalls_.contains(key) && watchCalls_.at(key)==call) continue;
                    const auto field=watch.at("field").get<std::string>();
                    if(latest.contains(field) && latest.at(field).is_string()) {
                        sample["source_call_number"]=call;
                        try {
                            auto address=static_cast<std::uintptr_t>(parseUnsigned(latest.at(field).get<std::string>()));
                            const auto& offsets=watch.at("offsets");
                            for(std::size_t i=0;i<offsets.size();++i) {
                                const auto offset=checksig::readU32(offsets.at(i));
                                if(address<0x10000 || address>UINTPTR_MAX-offset) throw std::runtime_error("null, low or overflowing source pointer");
                                address+=offset;
                                if(i+1<offsets.size() && !safeRead(address,&address,8)) throw std::runtime_error("pointer chain unreadable");
                            }
                            const auto type=watch.at("type").get<std::string>();
                            std::size_t size=type=="u8" || type=="i8" ? 1 : type=="u16" || type=="i16" ? 2 : type=="u32" || type=="i32" || type=="f32" ? 4 : type.starts_with("bytes:") ? checksig::readU32(type.substr(6)) : 8;
                            std::array<std::byte,64> bytes{};
                            if(!safeRead(address,bytes.data(),size)) throw std::runtime_error("memory copy refused the range");
                            sample.update({{"address",hex(address)},{"bytes",bytesHex(bytes.data(),size)},{"readable",true},{"status","sampled-needs-review"}});
#define CS_WATCH_VALUE(T) {T value{};std::memcpy(&value,bytes.data(),sizeof(value));sample["value"]=value;}
                            if(type=="u8") CS_WATCH_VALUE(std::uint8_t)
                            else if(type=="i8") CS_WATCH_VALUE(std::int8_t)
                            else if(type=="u16") CS_WATCH_VALUE(std::uint16_t)
                            else if(type=="i16") CS_WATCH_VALUE(std::int16_t)
                            else if(type=="u32") CS_WATCH_VALUE(std::uint32_t)
                            else if(type=="i32") CS_WATCH_VALUE(std::int32_t)
                            else if(type=="i64") CS_WATCH_VALUE(std::int64_t)
                            else if(type=="f32") CS_WATCH_VALUE(float)
                            else if(type=="f64") CS_WATCH_VALUE(double)
                            else if(type=="ptr" || type=="u64") {std::uint64_t value{};std::memcpy(&value,bytes.data(),8);sample["value"]=hex(value);
                                if(value>=image_->base && value-image_->base<image_->imageSize) {
                                    const auto rva=static_cast<std::uint32_t>(value-image_->base);
                                    sample["value_rva"]=hex(rva);
                                    const auto* function=containingFunction(*image_,rva);
                                    sample["value_function_entry"]=function && function->begin==rva;
                                    if(function) sample["value_function_begin_rva"]=hex(function->begin);
                                }
                            }
#undef CS_WATCH_VALUE
                        } catch(const std::exception& e) { sample["status"]="unreadable";sample["error"]=e.what(); }
                    }
                }
                changed=true;
                watchCalls_[key]=call;
                sample["time_utc"]=timestampUtc();
                jobResults_[source]["watch_samples"][label]=std::move(sample);
            }
            if(changed) {jobResults_[source]["watch_updated_utc"]=timestampUtc();saveJob(source);}
        }
    }
    void refreshSupportReports() {
        if(!resolver_) return;
        processWatches();
        auto report=buildSupport(defaultSupportCatalog());
        const auto serialized=report.dump(2)+"\n";
        if(serialized!=lastSupport_) {
            fs::create_directories(output_/L"result");
            atomicWrite(output_/L"result"/L"remaining-work.json",serialized);
            atomicWrite(output_/L"result"/L"remaining-work.txt",checksig::supportText(report));
            lastSupport_=serialized;
        }
        for(const auto& [source,catalog]:supportCatalogs_) {
            if(!jobResults_.contains(source)) continue;
            jobResults_[source]["support_report"]=buildSupport(catalog);
            jobResults_[source]["support_updated_utc"]=timestampUtc();
            saveJob(source);
        }
    }

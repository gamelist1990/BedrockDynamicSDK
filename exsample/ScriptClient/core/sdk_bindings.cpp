#include "sdk_bindings.hpp"
#include "generated_sdk_bootstrap.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace stageb {
namespace {

// §5.3 / §6.4 / §11: the frozen apiVersion 1.0.0 binding bootstrap is generated
// by Stage C (`tools/generate_ts_sdk.py`) from `data/binding-registry.json`.
// The C++ host functions are attached as __bedrockHost before this runs; plugin
// code only sees the Mod/Arg/world/system/Setting/ItemStack/brightness surface.
constexpr const char* kBootstrap = kGeneratedBootstrap;

std::string filesSafeName(const std::string& name) {
    std::string safe = name.empty() ? std::string("unnamed-mod") : name;
    for (char& ch : safe)
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch == '.')
            ch = '_';
    return safe;
}

ArgSpec specFromValue(JSContext* ctx, JSValueConst value) {
    ArgSpec spec;
    const JsValue kind(ctx, JS_GetPropertyStr(ctx, value, "__kind"));
    const auto kindText = jsToString(ctx, kind.get());
    if (kindText == "effect") spec.kind = ArgKind::Effect;
    else if (kindText == "duration") spec.kind = ArgKind::Duration;
    else if (kindText == "int") spec.kind = ArgKind::Int;
    else if (kindText == "bool") spec.kind = ArgKind::Bool;
    else if (kindText == "message") spec.kind = ArgKind::Message;
    else if (kindText == "float") spec.kind = ArgKind::Float;
    else spec.kind = ArgKind::String;

    const JsValue optional(ctx, JS_GetPropertyStr(ctx, value, "optional"));
    spec.optional = jsToBool(ctx, optional.get(), false);
    const JsValue infinity(ctx, JS_GetPropertyStr(ctx, value, "infinity"));
    spec.infinity = jsToBool(ctx, infinity.get(), false);
    // JS Arg.int({ default: 7 }) omits min/max. The old code converted the
    // absent (undefined) properties to 0, collapsing the accepted range to
    // [INT_MIN, INT_MIN] via the clamp in parseInt. Default to full int32.
    const JsValue minimum(ctx, JS_GetPropertyStr(ctx, value, "min"));
    spec.min = JS_IsUndefined(minimum.get())
                   ? std::numeric_limits<int>::min()
                   : static_cast<int>(jsToNumber(ctx, minimum.get(),
                                                 std::numeric_limits<int>::min()));
    const JsValue maximum(ctx, JS_GetPropertyStr(ctx, value, "max"));
    spec.max = JS_IsUndefined(maximum.get())
                   ? std::numeric_limits<int>::max()
                   : static_cast<int>(jsToNumber(ctx, maximum.get(),
                                                 std::numeric_limits<int>::max()));
    const JsValue suggested(ctx, JS_GetPropertyStr(ctx, value, "suggestions"));
    if (JS_IsArray(suggested.get()))
        for (const auto& entry : jsToStringArray(ctx, suggested.get()))
            spec.suggestions.push_back(entry);
    const JsValue fallback(ctx, JS_GetPropertyStr(ctx, value, "default"));
    // An absent `default` property is JS undefined; stringifying it would
    // produce the literal text "undefined" as the default (the historical
    // no-arg /effectfake clear bug). Track presence explicitly so the
    // parser can leave undeclared-default optionals out of the values map
    // and getOptional can return real undefined for them.
    spec.hasDefault = !JS_IsUndefined(fallback.get());
    if (spec.hasDefault) {
        if (JS_IsBool(fallback.get()))
            spec.stringDefault = jsToBool(ctx, fallback.get()) ? "true" : "false";
        else
            spec.stringDefault = jsToString(ctx, fallback.get());
        if (spec.kind == ArgKind::Int || spec.kind == ArgKind::Duration ||
            spec.kind == ArgKind::Float)
            spec.defaultValue = jsToNumber(ctx, fallback.get(), 0);
    }
    return spec;
}

JSValue throwJsError(JSContext* ctx, const char* code, const std::string& message) {
    JsValue err(ctx, JS_NewError(ctx));
    JS_SetPropertyStr(ctx, err.get(), "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, err.get(), "message", JS_NewString(ctx, message.c_str()));
    return JS_Throw(ctx, err.release());
}

JSValue makeVector3(JSContext* ctx, const Vector3Data& v) {
    JsValue obj(ctx, JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, obj.get(), "x", JS_NewFloat64(ctx, v.x));
    JS_SetPropertyStr(ctx, obj.get(), "y", JS_NewFloat64(ctx, v.y));
    JS_SetPropertyStr(ctx, obj.get(), "z", JS_NewFloat64(ctx, v.z));
    return obj.release();
}

JSValue makeRotation(JSContext* ctx, const RotationData& r) {
    JsValue obj(ctx, JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, obj.get(), "x", JS_NewFloat64(ctx, r.x));
    JS_SetPropertyStr(ctx, obj.get(), "y", JS_NewFloat64(ctx, r.y));
    return obj.release();
}

JSValue dynamicPropToJs(JSContext* ctx, const std::optional<DynamicPropertyData>& prop) {
    if (!prop) return JS_UNDEFINED;
    if (const auto* b = std::get_if<bool>(&*prop)) return JS_NewBool(ctx, *b);
    if (const auto* d = std::get_if<double>(&*prop)) return JS_NewFloat64(ctx, *d);
    if (const auto* s = std::get_if<std::string>(&*prop)) return JS_NewString(ctx, s->c_str());
    if (const auto* v = std::get_if<Vector3Data>(&*prop)) return makeVector3(ctx, *v);
    return JS_UNDEFINED;
}

bool jsToDynamicProp(JSContext* ctx, JSValueConst val,
                     std::optional<DynamicPropertyData>& out, std::string& error) {
    if (JS_IsUndefined(val) || JS_IsNull(val)) {
        out = std::nullopt;
        return true;
    }
    if (JS_IsBool(val)) {
        out = DynamicPropertyData{jsToBool(ctx, val)};
        return true;
    }
    if (JS_IsNumber(val)) {
        const double num = jsToNumber(ctx, val);
        if (!std::isfinite(num)) {
            error = "Dynamic property number must be finite";
            return false;
        }
        out = DynamicPropertyData{num};
        return true;
    }
    if (JS_IsString(val)) {
        out = DynamicPropertyData{jsToString(ctx, val)};
        return true;
    }
    if (JS_IsObject(val)) {
        const JsValue vx(ctx, JS_GetPropertyStr(ctx, val, "x"));
        const JsValue vy(ctx, JS_GetPropertyStr(ctx, val, "y"));
        const JsValue vz(ctx, JS_GetPropertyStr(ctx, val, "z"));
        if (JS_IsNumber(vx.get()) && JS_IsNumber(vy.get()) && JS_IsNumber(vz.get())) {
            Vector3Data v{
                static_cast<float>(jsToNumber(ctx, vx.get())),
                static_cast<float>(jsToNumber(ctx, vy.get())),
                static_cast<float>(jsToNumber(ctx, vz.get()))
            };
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
                error = "Dynamic property Vector3 coordinates must be finite";
                return false;
            }
            out = DynamicPropertyData{v};
            return true;
        }
    }
    error = "Unsupported dynamic property type (expected boolean, number, string, Vector3, or undefined)";
    return false;
}

std::vector<bedrock::sdk::Translation> parseTranslations(JSContext* ctx, JSValueConst entriesVal) {
    std::vector<bedrock::sdk::Translation> entries;
    if (JS_IsArray(entriesVal)) {
        const auto len = jsArrayLength(ctx, entriesVal);
        for (std::uint32_t i = 0; i < len; ++i) {
            const JsValue item(ctx, JS_GetPropertyUint32(ctx, entriesVal, i));
            if (JS_IsArray(item.get())) {
                const JsValue k(ctx, JS_GetPropertyUint32(ctx, item.get(), 0));
                const JsValue v(ctx, JS_GetPropertyUint32(ctx, item.get(), 1));
                entries.push_back({jsToString(ctx, k.get()), jsToString(ctx, v.get())});
            } else if (JS_IsObject(item.get())) {
                const JsValue k(ctx, JS_GetPropertyStr(ctx, item.get(), "key"));
                const JsValue v(ctx, JS_GetPropertyStr(ctx, item.get(), "value"));
                entries.push_back({jsToString(ctx, k.get()), jsToString(ctx, v.get())});
            }
        }
    } else if (JS_IsObject(entriesVal)) {
        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &count, entriesVal,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < count; ++i) {
                const char* keyStr = JS_AtomToCString(ctx, props[i].atom);
                const JsValue val(ctx, JS_GetProperty(ctx, entriesVal, props[i].atom));
                if (keyStr) {
                    entries.push_back({keyStr, jsToString(ctx, val.get())});
                    JS_FreeCString(ctx, keyStr);
                }
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        }
    }
    return entries;
}

bool parseSettingControl(JSContext* ctx, JSValueConst val,
                         bedrock::sdk::Setting& out, std::string& error) {
    if (JS_IsString(val)) {
        const auto id = jsToString(ctx, val);
        if (id.empty()) { error = "setting id is empty"; return false; }
        out = bedrock::sdk::Setting::text(id, id);
        return true;
    }
    if (!JS_IsObject(val)) {
        error = "settings.add expects a Setting descriptor object";
        return false;
    }
    const JsValue idVal(ctx, JS_GetPropertyStr(ctx, val, "id"));
    const auto id = jsToString(ctx, idVal.get());
    if (id.empty()) {
        error = "setting descriptor requires a non-empty 'id'";
        return false;
    }
    const JsValue labelVal(ctx, JS_GetPropertyStr(ctx, val, "labelKey"));
    const JsValue labelAlt(ctx, JS_GetPropertyStr(ctx, val, "label"));
    std::string labelKey = !JS_IsUndefined(labelVal.get()) ? jsToString(ctx, labelVal.get())
                         : !JS_IsUndefined(labelAlt.get()) ? jsToString(ctx, labelAlt.get())
                         : id;
    if (labelKey.empty()) labelKey = id;

    const JsValue descVal(ctx, JS_GetPropertyStr(ctx, val, "descriptionKey"));
    const JsValue descAlt(ctx, JS_GetPropertyStr(ctx, val, "description"));
    std::string descKey = !JS_IsUndefined(descVal.get()) ? jsToString(ctx, descVal.get())
                        : !JS_IsUndefined(descAlt.get()) ? jsToString(ctx, descAlt.get())
                        : std::string{};

    const JsValue kindVal(ctx, JS_GetPropertyStr(ctx, val, "kind"));
    const JsValue typeVal(ctx, JS_GetPropertyStr(ctx, val, "type"));
    const auto kindStr = lowerCopy(!JS_IsUndefined(kindVal.get()) ? jsToString(ctx, kindVal.get())
                                 : !JS_IsUndefined(typeVal.get()) ? jsToString(ctx, typeVal.get())
                                 : std::string{});
    const JsValue initVal(ctx, JS_GetPropertyStr(ctx, val, "initial"));
    const JsValue defVal(ctx, JS_GetPropertyStr(ctx, val, "default"));
    const JSValueConst initial = !JS_IsUndefined(initVal.get()) ? initVal.get() : defVal.get();

    if (kindStr == "toggle" || kindStr == "boolean" || kindStr == "bool" ||
        (kindStr.empty() && JS_IsBool(initial))) {
        out = bedrock::sdk::Setting::toggle(id, labelKey, jsToBool(ctx, initial, false));
        out.descriptionKey = descKey;
        return true;
    }
    if (kindStr == "slider" || kindStr == "number" ||
        (kindStr.empty() && JS_IsNumber(initial))) {
        const JsValue minVal(ctx, JS_GetPropertyStr(ctx, val, "minimum"));
        const JsValue minAlt(ctx, JS_GetPropertyStr(ctx, val, "min"));
        const JsValue maxVal(ctx, JS_GetPropertyStr(ctx, val, "maximum"));
        const JsValue maxAlt(ctx, JS_GetPropertyStr(ctx, val, "max"));
        const JsValue stepVal(ctx, JS_GetPropertyStr(ctx, val, "step"));
        const double minimum = !JS_IsUndefined(minVal.get()) ? jsToNumber(ctx, minVal.get(), 0.0)
                                                             : jsToNumber(ctx, minAlt.get(), 0.0);
        const double maximum = !JS_IsUndefined(maxVal.get()) ? jsToNumber(ctx, maxVal.get(), 100.0)
                                                             : jsToNumber(ctx, maxAlt.get(), 100.0);
        const double step = !JS_IsUndefined(stepVal.get()) ? jsToNumber(ctx, stepVal.get(), 1.0) : 1.0;
        const double initNum = jsToNumber(ctx, initial, minimum);
        out = bedrock::sdk::Setting::slider(id, labelKey, initNum, minimum, maximum, step);
        out.descriptionKey = descKey;
        return true;
    }
    if (kindStr == "choice" || kindStr == "option") {
        const JsValue choicesVal(ctx, JS_GetPropertyStr(ctx, val, "choices"));
        std::vector<std::pair<std::string, std::string>> choices;
        if (JS_IsArray(choicesVal.get())) {
            const auto len = jsArrayLength(ctx, choicesVal.get());
            for (std::uint32_t i = 0; i < len; ++i) {
                const JsValue item(ctx, JS_GetPropertyUint32(ctx, choicesVal.get(), i));
                if (JS_IsArray(item.get())) {
                    const JsValue cid(ctx, JS_GetPropertyUint32(ctx, item.get(), 0));
                    const JsValue clabel(ctx, JS_GetPropertyUint32(ctx, item.get(), 1));
                    choices.emplace_back(jsToString(ctx, cid.get()), jsToString(ctx, clabel.get()));
                } else if (JS_IsObject(item.get())) {
                    const JsValue cid(ctx, JS_GetPropertyStr(ctx, item.get(), "id"));
                    const JsValue clabel(ctx, JS_GetPropertyStr(ctx, item.get(), "labelKey"));
                    const JsValue clabelAlt(ctx, JS_GetPropertyStr(ctx, item.get(), "label"));
                    const auto cIdStr = jsToString(ctx, cid.get());
                    const auto cLabelStr = !JS_IsUndefined(clabel.get()) ? jsToString(ctx, clabel.get())
                                         : !JS_IsUndefined(clabelAlt.get()) ? jsToString(ctx, clabelAlt.get())
                                         : cIdStr;
                    choices.emplace_back(cIdStr, cLabelStr);
                } else if (JS_IsString(item.get())) {
                    const auto s = jsToString(ctx, item.get());
                    choices.emplace_back(s, s);
                }
            }
        }
        const auto initChoice = !JS_IsUndefined(initial) ? jsToString(ctx, initial)
                              : (!choices.empty() ? choices.front().first : std::string{});
        out = bedrock::sdk::Setting::choice(id, labelKey, initChoice, std::move(choices));
        out.descriptionKey = descKey;
        return true;
    }
    if (kindStr == "text" || kindStr.empty()) {
        out = bedrock::sdk::Setting::text(id, labelKey, descKey);
        return true;
    }
    error = "unknown setting kind: " + kindStr;
    return false;
}

} // namespace

std::optional<bedrock::sdk::Key> parseKeyName(std::string_view raw) {
    using bedrock::sdk::Key;
    std::string s(raw);
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (s.rfind("KEY_", 0) == 0) s.erase(0, 4);
    else if (s.rfind("KEY", 0) == 0 && s.size() > 3) s.erase(0, 3);
    else if (s.rfind("DIGIT_", 0) == 0) s.erase(0, 6);
    else if (s.rfind("DIGIT", 0) == 0 && s.size() > 5) s.erase(0, 5);

    if (s == "TAB") return Key::Tab;
    if (s == "SPACE" || s == " ") return Key::Space;
    if (s.size() == 1) {
        const char ch = s[0];
        if (ch >= 'A' && ch <= 'Z') return static_cast<Key>(static_cast<unsigned>(ch));
        if (ch >= '0' && ch <= '9') return static_cast<Key>(static_cast<unsigned>(ch));
    }
    if (s.size() >= 2 && s[0] == 'F') {
        try {
            const int n = std::stoi(s.substr(1));
            if (n >= 1 && n <= 12) return static_cast<Key>(111 + n);
        } catch (...) {}
    }
    return std::nullopt;
}

std::string keyToName(bedrock::sdk::Key key) {
    using bedrock::sdk::Key;
    const auto code = static_cast<unsigned>(key);
    if (key == Key::Tab) return "Tab";
    if (key == Key::Space) return "Space";
    if (code >= 'A' && code <= 'Z') return std::string(1, static_cast<char>(code));
    if (code >= '0' && code <= '9') return std::string(1, static_cast<char>(code));
    if (code >= 112 && code <= 123) return "F" + std::to_string(code - 111);
    return std::to_string(code);
}

CommandRegistry::~CommandRegistry() {
    for (JSValue handler : handlers_) JS_FreeValue(runtime_.context(), handler);
    handlers_.clear();
}

int CommandRegistry::beginCommand(const std::string& name, const std::string& description) {
    unregisterCommand(name);
    declarations_.push_back(CommandDecl{name, description.empty() ? description_ : description, {}});
    builders_.push_back(Builder{declarations_.size() - 1});
    return static_cast<int>(builders_.size() - 1);
}

bool CommandRegistry::unregisterCommand(const std::string& name) {
    const auto before = declarations_.size();
    declarations_.erase(
        std::remove_if(declarations_.begin(), declarations_.end(),
                       [&](const CommandDecl& d) { return d.name == name; }),
        declarations_.end());
    return declarations_.size() != before;
}

void CommandRegistry::addBranch(int builderId,
                                const std::vector<std::string>& literals,
                                const std::vector<std::pair<std::string, ArgSpec>>& args,
                                JSValue handler) {
    if (builderId < 0 || static_cast<std::size_t>(builderId) >= builders_.size()) return;
    const auto cmdIdx = builders_[static_cast<std::size_t>(builderId)].command;
    if (cmdIdx >= declarations_.size()) return;
    auto& declaration = declarations_[cmdIdx];
    CommandBranch branch;
    branch.literals = literals;
    branch.args = args;
    if (!JS_IsUndefined(handler) && !JS_IsNull(handler)) {
        handlers_.push_back(JS_DupValue(runtime_.context(), handler));
        branch.handlerIndex = handlers_.size() - 1;
    }
    declaration.branches.push_back(std::move(branch));
}

JSValue CommandRegistry::handler(std::size_t index) const {
    if (index >= handlers_.size()) return JS_UNDEFINED;
    return JS_DupValue(runtime_.context(), handlers_[index]);
}

SdkBindings::SdkBindings(JsRuntime& runtime, HostContext& host,
                         CommandRegistry& commands, WorldBackend& world,
                         std::string modName, fs::path dataRoot)
    : runtime_(runtime), host_(host), commands_(commands), world_(world),
      modName_(std::move(modName)),
      filesRoot_(dataRoot.empty() ? fs::path{} : dataRoot / filesSafeName(modName_)) {
    host_.bindings = this;
}

SdkBindings::~SdkBindings() {
    forceCleanup();
}

void SdkBindings::forceCleanup() {
    if (aliveToken_) *aliveToken_ = false;
    const auto ctx = runtime_.context();
    if (settings_.registered && activePage_) {
        (void)world_.unregisterSettingsPage(modName_, *activePage_);
        settings_.registered = false;
    }
    activePage_.reset();
    if (hasGammaOverride_) {
        (void)world_.setBrightnessGamma(modName_, std::nullopt);
        hasGammaOverride_ = false;
        lastGamma_.reset();
    }
    if (ctx) {
        if (!JS_IsUndefined(settingsOnChange_)) {
            JS_FreeValue(ctx, settingsOnChange_);
            settingsOnChange_ = JS_UNDEFINED;
        }
        for (auto& [id, fn] : playerSpawnSubscribers_) {
            (void)id;
            if (!JS_IsUndefined(fn)) JS_FreeValue(ctx, fn);
        }
        playerSpawnSubscribers_.clear();
        for (auto& [id, fn] : keybindSubscribers_) {
            (void)id;
            if (!JS_IsUndefined(fn)) JS_FreeValue(ctx, fn);
        }
        keybindSubscribers_.clear();
        for (auto& [id, task] : tasks_) {
            (void)id;
            if (!JS_IsUndefined(task.function)) JS_FreeValue(ctx, task.function);
        }
        tasks_.clear();
    }
}

JSValue SdkBindings::throwBackendError(JSContext* ctx, const BackendOutcome& outcome) {
    return throwJsError(ctx, outcome.code.empty() ? "Unavailable" : outcome.code.c_str(),
                        outcome.message.empty() ? "SDK operation failed" : outcome.message);
}

bool SdkBindings::ensureFilesRoot(std::string& error) {
    if (filesRoot_.empty()) {
        error = "mod data root is not configured";
        return false;
    }
    std::error_code ec;
    fs::create_directories(filesRoot_, ec);
    if (ec) {
        error = "cannot create mod data root: " + ec.message();
        return false;
    }
    return true;
}

std::optional<fs::path> SdkBindings::resolveFilesPath(const std::string& relative,
                                                      std::string& error) const {
    fs::path input(relative);
    if (input.empty()) {
        error = "path must not be empty";
        return std::nullopt;
    }
    if (input.is_absolute() || input.has_root_name() || input.has_root_directory()) {
        error = "absolute paths are not allowed; paths are relative to the mod data root";
        return std::nullopt;
    }
    const auto resolved = confinedResolve(filesRoot_, input);
    if (!resolved) {
        error = "path escapes the mod data root";
        return std::nullopt;
    }
    return resolved;
}

bool SdkBindings::readFileBytes(const fs::path& path, std::vector<std::uint8_t>& out,
                                std::string& error) const {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        error = "not a regular file: " + path.generic_string();
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open file: " + path.generic_string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "cannot read file: " + path.generic_string();
        return false;
    }
    return true;
}

bool SdkBindings::writeFileBytes(const fs::path& path, const std::vector<std::uint8_t>& data,
                                 bool append, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "cannot create parent directory: " + ec.message();
        return false;
    }
    std::ofstream out(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out) {
        error = "cannot open file for writing: " + path.generic_string();
        return false;
    }
    if (!data.empty())
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    if (!out) {
        error = "cannot write file: " + path.generic_string();
        return false;
    }
    return true;
}

bool SdkBindings::install(std::string& error) {
    const auto ctx = runtime_.context();
    JsValue hostObject(ctx, JS_NewObject(ctx));
    struct Entry { const char* name; Op op; };
    static const Entry entries[] = {
        {"beginCommand", Op::BeginCommand}, {"unregisterCommand", Op::UnregisterCommand},
        {"executes", Op::Executes}, {"setDescription", Op::SetDescription},
        {"createScope", Op::CreateScope},
        {"worldHasPlayer", Op::WorldHasPlayer}, {"worldLocalPlayer", Op::WorldLocalPlayer},
        {"worldGetAllPlayers", Op::WorldGetAllPlayers},
        {"worldCurrentDimension", Op::WorldCurrentDimension},
        {"worldGetDimension", Op::WorldGetDimension},
        {"worldSendMessage", Op::WorldSendMessage},
        {"worldGetDynamicProperty", Op::WorldGetDynamicProperty},
        {"worldSetDynamicProperty", Op::WorldSetDynamicProperty},
        {"worldSubscribePlayerSpawn", Op::WorldSubscribePlayerSpawn},
        {"worldUnsubscribePlayerSpawn", Op::WorldUnsubscribePlayerSpawn},
        {"entityIsValid", Op::EntityIsValid}, {"entityTypeId", Op::EntityTypeId},
        {"entityLocation", Op::EntityLocation}, {"entityRotation", Op::EntityRotation},
        {"entityDimension", Op::EntityDimension}, {"entityGetItemStack", Op::EntityGetItemStack},
        {"entityTeleport", Op::EntityTeleport}, {"entityAddTag", Op::EntityAddTag},
        {"entityGetDynamicProperty", Op::EntityGetDynamicProperty},
        {"entitySetDynamicProperty", Op::EntitySetDynamicProperty},
        {"entityGetComponent", Op::EntityGetComponent},
        {"playerName", Op::PlayerName}, {"playerSendMessage", Op::PlayerSendMessage},
        {"playerAddEffect", Op::PlayerAddEffect}, {"playerRemoveEffect", Op::PlayerRemoveEffect},
        {"playerClearEffects", Op::PlayerClearEffects}, {"playerGetEffects", Op::PlayerGetEffects},
        {"dimensionTypeId", Op::DimensionTypeId}, {"dimensionGetBlock", Op::DimensionGetBlock},
        {"dimensionGetEntities", Op::DimensionGetEntities},
        {"dimensionSpawnEntity", Op::DimensionSpawnEntity},
        {"blockTypeId", Op::BlockTypeId}, {"blockLocation", Op::BlockLocation},
        {"blockSetType", Op::BlockSetType},
        {"systemRun", Op::SystemRun}, {"systemRunTimeout", Op::SystemRunTimeout},
        {"systemRunInterval", Op::SystemRunInterval}, {"systemClear", Op::SystemClear},
        {"settingsAdd", Op::SettingsAdd}, {"settingsLocale", Op::SettingsLocale},
        {"settingsOnChange", Op::SettingsOnChange}, {"settingsModName", Op::SettingsModName},
        {"settingsRegisterPage", Op::SettingsRegisterPage},
        {"settingsUnregisterPage", Op::SettingsUnregisterPage},
        {"settingsValue", Op::SettingsValue},
        {"keybindAdd", Op::KeybindAdd}, {"keybindLocale", Op::KeybindLocale},
        {"keybindKey", Op::KeybindKey},
        {"keybindSubscribe", Op::KeybindSubscribe}, {"keybindUnsubscribe", Op::KeybindUnsubscribe},
        {"brightnessSetGamma", Op::BrightnessSetGamma},
        {"filesDataDir", Op::FilesDataDir}, {"filesReadText", Op::FilesReadText},
        {"filesWriteText", Op::FilesWriteText}, {"filesAppendText", Op::FilesAppendText},
        {"filesReadBytes", Op::FilesReadBytes}, {"filesWriteBytes", Op::FilesWriteBytes},
        {"filesExists", Op::FilesExists}, {"filesStat", Op::FilesStat},
        {"filesList", Op::FilesList}, {"filesMkdir", Op::FilesMkdir},
        {"filesRemove", Op::FilesRemove}, {"filesRename", Op::FilesRename},
        {"worldObject", Op::WorldObject}, {"systemObject", Op::SystemObject},
        {"brightnessObject", Op::BrightnessObject}
    };
    for (const auto& entry : entries) {
        JsValue fn(ctx, JS_NewCFunctionMagic(ctx, &hostCallback, entry.name, 0,
                                             JS_CFUNC_generic_magic, static_cast<int>(entry.op)));
        JS_DefinePropertyValueStr(ctx, hostObject.get(), entry.name, fn.release(), JS_PROP_C_W_E);
    }

    systemObject_ = JsValue(ctx, JS_NewObject(ctx));
    auto bind = [&](JSValueConst object, const char* name, Op op) {
        JsValue fn(ctx, JS_NewCFunctionMagic(ctx, &hostCallback, name, 0,
                                             JS_CFUNC_generic_magic, static_cast<int>(op)));
        JS_DefinePropertyValueStr(ctx, object, name, fn.release(), JS_PROP_C_W_E);
    };
    bind(systemObject_.get(), "run", Op::SystemRun);
    bind(systemObject_.get(), "runTimeout", Op::SystemRunTimeout);
    bind(systemObject_.get(), "runInterval", Op::SystemRunInterval);
    bind(systemObject_.get(), "clearRun", Op::SystemClear);

    const JsValue global(ctx, JS_GetGlobalObject(ctx));
    JS_DefinePropertyValueStr(ctx, global.get(), "__bedrockHost", hostObject.release(), JS_PROP_C_W_E);
    if (!runtime_.eval(kBootstrap, "stage-b-sdk-bootstrap.js", error)) return false;
    JsValue module(ctx, JS_GetPropertyStr(ctx, global.get(), "__sdkModule"));
    if (!JS_IsObject(module.get())) { error = "SDK bootstrap did not create __sdkModule"; return false; }
    worldObject_ = JsValue(ctx, JS_GetPropertyStr(ctx, module.get(), "world"));
    brightnessObject_ = JsValue(ctx, JS_GetPropertyStr(ctx, module.get(), "brightness"));
    module_ = std::move(module);
    error.clear();
    return true;
}

JSValue SdkBindings::hostCallback(JSContext* ctx, JSValueConst, int argc,
                                  JSValueConst* argv, int magic) {
    auto* host = static_cast<HostContext*>(JS_GetContextOpaque(ctx));
    if (!host || !host->bindings) {
        return JS_ThrowTypeError(ctx, "SDK binding unavailable");
    }
    auto& self = *host->bindings;
    const auto op = static_cast<Op>(magic);
    auto arg = [&](int i) -> JSValueConst { return i < argc ? argv[i] : JS_UNDEFINED; };

    switch (op) {
        case Op::BeginCommand: {
            const auto name = jsToString(ctx, arg(0));
            return JS_NewInt32(ctx, self.commands_.beginCommand(name, self.commands_.description()));
        }
        case Op::UnregisterCommand: {
            const auto name = jsToString(ctx, arg(0));
            return JS_NewBool(ctx, self.commands_.unregisterCommand(name));
        }
        case Op::SetDescription:
            self.commands_.setDescription(jsToString(ctx, arg(0)));
            return JS_UNDEFINED;
        case Op::CreateScope:
            return JS_NewInt32(ctx, 0);
        case Op::Executes: {
            std::int32_t builderId = 0;
            JS_ToInt32(ctx, &builderId, arg(0));
            std::vector<std::string> literals;
            if (JS_IsArray(arg(1)))
                literals = jsToStringArray(ctx, arg(1));
            std::vector<std::pair<std::string, ArgSpec>> args;
            if (JS_IsArray(arg(2))) {
                const auto length = jsArrayLength(ctx, arg(2));
                for (uint32_t i = 0; i < length; ++i) {
                    const JsValue entry(ctx, JS_GetPropertyUint32(ctx, arg(2), i));
                    const JsValue name(ctx, JS_GetPropertyStr(ctx, entry.get(), "name"));
                    const JsValue specValue(ctx, JS_GetPropertyStr(ctx, entry.get(), "spec"));
                    args.emplace_back(jsToString(ctx, name.get()), specFromValue(ctx, specValue.get()));
                }
            }
            self.commands_.addBranch(builderId, literals, args, arg(3));
            return JS_UNDEFINED;
        }
        case Op::WorldHasPlayer:
            return JS_NewBool(ctx, self.world_.hasLocalPlayer(self.modName_));
        case Op::WorldLocalPlayer: {
            const auto id = self.world_.localPlayerId(self.modName_);
            return id ? JS_NewFloat64(ctx, static_cast<double>(*id)) : JS_UNDEFINED;
        }
        case Op::WorldGetAllPlayers: {
            JsValue array(ctx, JS_NewArray(ctx));
            const auto ids = self.world_.allPlayerIds(self.modName_);
            for (std::size_t i = 0; i < ids.size(); ++i) {
                JS_SetPropertyInt64(ctx, array.get(), static_cast<int64_t>(i),
                                    JS_NewFloat64(ctx, static_cast<double>(ids[i])));
            }
            return array.release();
        }
        case Op::WorldCurrentDimension: {
            const auto dim = self.world_.currentDimensionId(self.modName_);
            return dim ? JS_NewString(ctx, dim->c_str()) : JS_UNDEFINED;
        }
        case Op::WorldGetDimension: {
            const auto dim = self.world_.getDimensionId(self.modName_, jsToString(ctx, arg(0)));
            return dim ? JS_NewString(ctx, dim->c_str()) : JS_UNDEFINED;
        }
        case Op::WorldSendMessage: {
            const auto res = self.world_.worldSendMessage(self.modName_, jsToString(ctx, arg(0)));
            if (!res.ok) return self.throwBackendError(ctx, res);
            return JS_UNDEFINED;
        }
        case Op::WorldGetDynamicProperty: {
            const auto prop = self.world_.worldGetDynamicProperty(self.modName_, jsToString(ctx, arg(0)));
            return dynamicPropToJs(ctx, prop);
        }
        case Op::WorldSetDynamicProperty: {
            std::optional<DynamicPropertyData> prop;
            std::string parseErr;
            if (!jsToDynamicProp(ctx, arg(1), prop, parseErr))
                return throwJsError(ctx, "Invalid", parseErr);
            const auto res = self.world_.worldSetDynamicProperty(
                self.modName_, jsToString(ctx, arg(0)), prop);
            if (!res.ok) return self.throwBackendError(ctx, res);
            return JS_UNDEFINED;
        }
        case Op::WorldSubscribePlayerSpawn: {
            if (!JS_IsFunction(ctx, arg(0)))
                return throwJsError(ctx, "Invalid", "playerSpawn.subscribe expects a function");
            const auto subId = self.nextSubId_++;
            self.playerSpawnSubscribers_[subId] = JS_DupValue(ctx, arg(0));
            return JS_NewFloat64(ctx, static_cast<double>(subId));
        }
        case Op::WorldUnsubscribePlayerSpawn: {
            const auto subId = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            if (const auto it = self.playerSpawnSubscribers_.find(subId);
                it != self.playerSpawnSubscribers_.end()) {
                JS_FreeValue(ctx, it->second);
                self.playerSpawnSubscribers_.erase(it);
            }
            return JS_UNDEFINED;
        }

        // --- Entity ---
        case Op::EntityIsValid: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            return JS_NewBool(ctx, self.world_.entityIsValid(self.modName_, id));
        }
        case Op::EntityTypeId: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            const auto t = self.world_.entityTypeId(self.modName_, id);
            return t ? JS_NewString(ctx, t->c_str()) : JS_UNDEFINED;
        }
        case Op::EntityLocation: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            const auto loc = self.world_.entityLocation(self.modName_, id);
            return loc ? makeVector3(ctx, *loc) : JS_UNDEFINED;
        }
        case Op::EntityRotation: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            const auto rot = self.world_.entityRotation(self.modName_, id);
            return rot ? makeRotation(ctx, *rot) : JS_UNDEFINED;
        }
        case Op::EntityDimension: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            const auto dim = self.world_.entityDimensionId(self.modName_, id);
            return dim ? JS_NewString(ctx, dim->c_str()) : JS_UNDEFINED;
        }
        case Op::EntityGetItemStack: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            const auto item = self.world_.entityItemStack(self.modName_, id);
            if (!item) return JS_UNDEFINED;
            JsValue obj(ctx, JS_NewObject(ctx));
            JS_SetPropertyStr(ctx, obj.get(), "typeId", JS_NewString(ctx, item->typeId.c_str()));
            JS_SetPropertyStr(ctx, obj.get(), "amount", JS_NewInt32(ctx, static_cast<int32_t>(item->amount)));
            return obj.release();
        }
        case Op::EntityTeleport: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            Vector3Data pos{
                static_cast<float>(jsToNumber(ctx, arg(1), NAN)),
                static_cast<float>(jsToNumber(ctx, arg(2), NAN)),
                static_cast<float>(jsToNumber(ctx, arg(3), NAN))
            };
            const auto res = self.world_.entityTeleport(self.modName_, id, pos);
            if (!res.ok) return self.throwBackendError(ctx, res);
            return JS_UNDEFINED;
        }
        case Op::EntityAddTag: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            bool added = false;
            const auto res = self.world_.entityAddTag(self.modName_, id, jsToString(ctx, arg(1)), added);
            if (!res.ok) return self.throwBackendError(ctx, res);
            return JS_NewBool(ctx, added);
        }
        case Op::EntityGetDynamicProperty: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            const auto prop = self.world_.entityGetDynamicProperty(self.modName_, id, jsToString(ctx, arg(1)));
            return dynamicPropToJs(ctx, prop);
        }
        case Op::EntitySetDynamicProperty: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            std::optional<DynamicPropertyData> prop;
            std::string parseErr;
            if (!jsToDynamicProp(ctx, arg(2), prop, parseErr))
                return throwJsError(ctx, "Invalid", parseErr);
            const auto res = self.world_.entitySetDynamicProperty(
                self.modName_, id, jsToString(ctx, arg(1)), prop);
            if (!res.ok) return self.throwBackendError(ctx, res);
            return JS_UNDEFINED;
        }
        case Op::EntityGetComponent: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            std::string compId = jsToString(ctx, arg(1));
            if (compId.find(':') == std::string::npos) compId = "minecraft:" + compId;
            if (compId == "minecraft:health") {
                const auto h = self.world_.entityHealth(self.modName_, id);
                if (!h) return JS_UNDEFINED;
                JsValue obj(ctx, JS_NewObject(ctx));
                JS_SetPropertyStr(ctx, obj.get(), "typeId", JS_NewString(ctx, "minecraft:health"));
                JS_SetPropertyStr(ctx, obj.get(), "currentValue", JS_NewFloat64(ctx, *h));
                return obj.release();
            }
            if (compId == "minecraft:food" || compId == "minecraft:player.hunger" ||
                compId == "minecraft:hunger") {
                const auto f = self.world_.entityFood(self.modName_, id);
                if (!f) return JS_UNDEFINED;
                JsValue obj(ctx, JS_NewObject(ctx));
                JS_SetPropertyStr(ctx, obj.get(), "typeId", JS_NewString(ctx, "minecraft:food"));
                JS_SetPropertyStr(ctx, obj.get(), "currentValue", JS_NewFloat64(ctx, *f));
                return obj.release();
            }
            if (compId == "minecraft:movement") {
                const auto v = self.world_.entityMovementVelocity(self.modName_, id);
                if (!v) return JS_UNDEFINED;
                JsValue obj(ctx, JS_NewObject(ctx));
                JS_SetPropertyStr(ctx, obj.get(), "typeId", JS_NewString(ctx, "minecraft:movement"));
                JS_SetPropertyStr(ctx, obj.get(), "velocity", makeVector3(ctx, *v));
                return obj.release();
            }
            if (compId == "minecraft:inventory") {
                const auto inv = self.world_.entityInventory(self.modName_, id);
                if (!inv) return JS_UNDEFINED;
                JsValue obj(ctx, JS_NewObject(ctx));
                JS_SetPropertyStr(ctx, obj.get(), "__kind", JS_NewString(ctx, "inventory"));
                JsValue slots(ctx, JS_NewArray(ctx));
                for (std::size_t i = 0; i < inv->size(); ++i) {
                    if ((*inv)[i]) {
                        JsValue s(ctx, JS_NewObject(ctx));
                        JS_SetPropertyStr(ctx, s.get(), "typeId",
                                          JS_NewString(ctx, (*inv)[i]->typeId.c_str()));
                        JS_SetPropertyStr(ctx, s.get(), "amount",
                                          JS_NewInt32(ctx, static_cast<int32_t>((*inv)[i]->amount)));
                        JS_SetPropertyInt64(ctx, slots.get(), static_cast<int64_t>(i), s.release());
                    } else {
                        JS_SetPropertyInt64(ctx, slots.get(), static_cast<int64_t>(i), JS_UNDEFINED);
                    }
                }
                JS_SetPropertyStr(ctx, obj.get(), "slots", slots.release());
                return obj.release();
            }
            if (compId == "minecraft:equippable") {
                const auto eq = self.world_.entityEquippable(self.modName_, id);
                if (!eq) return JS_UNDEFINED;
                JsValue obj(ctx, JS_NewObject(ctx));
                JS_SetPropertyStr(ctx, obj.get(), "__kind", JS_NewString(ctx, "equippable"));
                JsValue slots(ctx, JS_NewObject(ctx));
                static const char* const slotNames[6] = {
                    "MainHand", "Offhand", "Head", "Chest", "Legs", "Feet"
                };
                static const char* const slotLower[6] = {
                    "mainhand", "offhand", "head", "chest", "legs", "feet"
                };
                for (std::size_t i = 0; i < 6; ++i) {
                    if ((*eq)[i]) {
                        auto makeSlotObj = [&]() {
                            JsValue s(ctx, JS_NewObject(ctx));
                            JS_SetPropertyStr(ctx, s.get(), "typeId",
                                              JS_NewString(ctx, (*eq)[i]->typeId.c_str()));
                            JS_SetPropertyStr(ctx, s.get(), "amount",
                                              JS_NewInt32(ctx, static_cast<int32_t>((*eq)[i]->amount)));
                            return s.release();
                        };
                        JS_SetPropertyStr(ctx, slots.get(), slotNames[i], makeSlotObj());
                        JS_SetPropertyStr(ctx, slots.get(), slotLower[i], makeSlotObj());
                        JS_SetPropertyStr(ctx, slots.get(), std::to_string(i).c_str(), makeSlotObj());
                    }
                }
                JS_SetPropertyStr(ctx, obj.get(), "slots", slots.release());
                return obj.release();
            }
            if (compId == "minecraft:item") {
                const auto item = self.world_.entityItemStack(self.modName_, id);
                if (!item) return JS_UNDEFINED;
                JsValue obj(ctx, JS_NewObject(ctx));
                JS_SetPropertyStr(ctx, obj.get(), "typeId", JS_NewString(ctx, "minecraft:item"));
                JsValue stackObj(ctx, JS_NewObject(ctx));
                JS_SetPropertyStr(ctx, stackObj.get(), "typeId", JS_NewString(ctx, item->typeId.c_str()));
                JS_SetPropertyStr(ctx, stackObj.get(), "amount",
                                  JS_NewInt32(ctx, static_cast<int32_t>(item->amount)));
                JS_SetPropertyStr(ctx, obj.get(), "itemStack", stackObj.release());
                return obj.release();
            }
            return JS_UNDEFINED;
        }

        // --- Player ---
        case Op::PlayerName: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            const auto name = self.world_.playerName(self.modName_, id);
            return name ? JS_NewString(ctx, name->c_str()) : JS_UNDEFINED;
        }
        case Op::PlayerSendMessage: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            const auto res = self.world_.playerSendMessage(self.modName_, id, jsToString(ctx, arg(1)));
            if (!res.ok) return self.throwBackendError(ctx, res);
            return JS_UNDEFINED;
        }
        case Op::PlayerAddEffect: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            const auto typeId = jsToString(ctx, arg(1));
            const double seconds = jsToNumber(ctx, arg(2), 0.0);
            const int amplifier = static_cast<int>(jsToNumber(ctx, arg(3), 0.0));
            const bool showParticles = jsToBool(ctx, arg(4), true);
            const auto res = self.world_.playerAddEffect(
                self.modName_, id, typeId, seconds, amplifier, showParticles);
            if (!res.ok) return self.throwBackendError(ctx, res);
            return JS_UNDEFINED;
        }
        case Op::PlayerRemoveEffect: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            bool removed = false;
            const auto res = self.world_.playerRemoveEffect(
                self.modName_, id, jsToString(ctx, arg(1)), removed);
            if (!res.ok) return self.throwBackendError(ctx, res);
            return JS_NewBool(ctx, removed);
        }
        case Op::PlayerClearEffects: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            const auto res = self.world_.playerClearEffects(self.modName_, id);
            if (!res.ok) return self.throwBackendError(ctx, res);
            return JS_UNDEFINED;
        }
        case Op::PlayerGetEffects: {
            const auto id = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            std::vector<EffectData> effects;
            const auto res = self.world_.playerGetEffects(self.modName_, id, effects);
            if (!res.ok) return self.throwBackendError(ctx, res);
            JsValue array(ctx, JS_NewArray(ctx));
            for (std::size_t i = 0; i < effects.size(); ++i) {
                JsValue effect(ctx, JS_NewObject(ctx));
                JS_SetPropertyStr(ctx, effect.get(), "typeId",
                                  JS_NewString(ctx, effects[i].typeId.c_str()));
                JS_SetPropertyStr(ctx, effect.get(), "duration",
                                  JS_NewFloat64(ctx, effects[i].durationSeconds));
                JS_SetPropertyStr(ctx, effect.get(), "amplifier",
                                  JS_NewInt32(ctx, effects[i].amplifier));
                JS_SetPropertyStr(ctx, effect.get(), "showParticles",
                                  JS_NewBool(ctx, effects[i].showParticles));
                JS_SetPropertyInt64(ctx, array.get(), static_cast<int64_t>(i), effect.release());
            }
            return array.release();
        }

        // --- Dimension & Block ---
        case Op::DimensionTypeId:
            return JS_NewString(ctx, jsToString(ctx, arg(0)).c_str());
        case Op::DimensionGetBlock: {
            const auto dimId = jsToString(ctx, arg(0));
            BlockPosData pos{
                static_cast<int>(jsToNumber(ctx, arg(1), 0)),
                static_cast<int>(jsToNumber(ctx, arg(2), 0)),
                static_cast<int>(jsToNumber(ctx, arg(3), 0))
            };
            const auto typeId = self.world_.dimensionGetBlockType(self.modName_, dimId, pos);
            return typeId ? JS_NewString(ctx, typeId->c_str()) : JS_UNDEFINED;
        }
        case Op::DimensionGetEntities: {
            const auto dimId = jsToString(ctx, arg(0));
            const auto ids = self.world_.dimensionGetEntityIds(self.modName_, dimId);
            const auto playerIds = self.world_.allPlayerIds(self.modName_);
            JsValue array(ctx, JS_NewArray(ctx));
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const bool isPlayer = std::find(playerIds.begin(), playerIds.end(), ids[i]) != playerIds.end();
                JsValue entry(ctx, JS_NewObject(ctx));
                JS_SetPropertyStr(ctx, entry.get(), "id", JS_NewFloat64(ctx, static_cast<double>(ids[i])));
                JS_SetPropertyStr(ctx, entry.get(), "isPlayer", JS_NewBool(ctx, isPlayer));
                JS_SetPropertyInt64(ctx, array.get(), static_cast<int64_t>(i), entry.release());
            }
            return array.release();
        }
        case Op::DimensionSpawnEntity: {
            const auto dimId = jsToString(ctx, arg(0));
            const auto typeId = jsToString(ctx, arg(1));
            Vector3Data pos{
                static_cast<float>(jsToNumber(ctx, arg(2), 0.0)),
                static_cast<float>(jsToNumber(ctx, arg(3), 0.0)),
                static_cast<float>(jsToNumber(ctx, arg(4), 0.0))
            };
            std::uint64_t entityId = 0;
            const auto res = self.world_.dimensionSpawnEntity(self.modName_, dimId, typeId, pos, entityId);
            if (!res.ok) return self.throwBackendError(ctx, res);
            return JS_NewFloat64(ctx, static_cast<double>(entityId));
        }
        case Op::BlockTypeId:
        case Op::BlockLocation:
            return JS_UNDEFINED;
        case Op::BlockSetType: {
            const auto dimId = jsToString(ctx, arg(0));
            BlockPosData pos{
                static_cast<int>(jsToNumber(ctx, arg(1), 0)),
                static_cast<int>(jsToNumber(ctx, arg(2), 0)),
                static_cast<int>(jsToNumber(ctx, arg(3), 0))
            };
            const auto typeId = jsToString(ctx, arg(4));
            const auto res = self.world_.blockSetType(self.modName_, dimId, pos, typeId);
            if (!res.ok) return self.throwBackendError(ctx, res);
            return JS_UNDEFINED;
        }

        // --- System ---
        case Op::SystemRun:
        case Op::SystemRunTimeout:
        case Op::SystemRunInterval: {
            if (!JS_IsFunction(ctx, arg(0))) return JS_ThrowTypeError(ctx, "expected a function");
            const auto id = self.nextTaskId_++;
            Task task;
            task.function = JS_DupValue(ctx, arg(0));
            const unsigned delay = op == Op::SystemRun ? 0
                                   : static_cast<unsigned>(jsToNumber(ctx, arg(1), 0));
            task.period = op == Op::SystemRunInterval ? std::max(1u, delay) : 0;
            task.due = self.tick_ + delay;
            self.tasks_[id] = std::move(task);
            return JS_NewFloat64(ctx, static_cast<double>(id));
        }
        case Op::SystemClear:
            if (const auto it = self.tasks_.find(static_cast<std::uint64_t>(
                    jsToNumber(ctx, arg(0), 0))); it != self.tasks_.end()) {
                JS_FreeValue(ctx, it->second.function);
                self.tasks_.erase(it);
                return JS_NewBool(ctx, true);
            }
            return JS_NewBool(ctx, false);

        // --- SettingsPage (§5.2 / §7.3) ---
        case Op::SettingsAdd: {
            self.settings_.titleKey = jsToString(ctx, arg(0));
            bedrock::sdk::Setting control;
            std::string parseErr;
            if (!parseSettingControl(ctx, arg(1), control, parseErr))
                return throwJsError(ctx, "Invalid", parseErr);
            self.settings_.added.push_back(control.id);
            self.settings_.controls.push_back(std::move(control));
            return JS_UNDEFINED;
        }
        case Op::SettingsLocale: {
            self.settings_.titleKey = jsToString(ctx, arg(0));
            const auto localeId = jsToString(ctx, arg(1));
            auto entries = parseTranslations(ctx, arg(2));
            self.settings_.locales.push_back({localeId, std::move(entries)});
            return JS_UNDEFINED;
        }
        case Op::SettingsOnChange: {
            self.settings_.titleKey = jsToString(ctx, arg(0));
            if (!JS_IsFunction(ctx, arg(1)))
                return throwJsError(ctx, "Invalid", "settings.onChange expects a function");
            if (!JS_IsUndefined(self.settingsOnChange_))
                JS_FreeValue(ctx, self.settingsOnChange_);
            self.settingsOnChange_ = JS_DupValue(ctx, arg(1));
            return JS_UNDEFINED;
        }
        case Op::SettingsModName: {
            self.settings_.titleKey = jsToString(ctx, arg(0));
            self.settings_.modNameKey = jsToString(ctx, arg(1));
            return JS_UNDEFINED;
        }
        case Op::SettingsRegisterPage: {
            const auto title = jsToString(ctx, arg(0));
            if (!title.empty()) self.settings_.titleKey = title;
            const std::string pageModId = self.modName_.empty() ? "script-mod"
                                                                : lowerCopy(self.modName_);
            auto page = std::make_unique<bedrock::sdk::SettingsPage>(
                pageModId, self.settings_.titleKey.empty() ? pageModId : self.settings_.titleKey);
            if (!self.settings_.modNameKey.empty())
                page->modName(self.settings_.modNameKey);
            for (const auto& ctrl : self.settings_.controls)
                page->add(ctrl);
            for (const auto& loc : self.settings_.locales)
                page->locale(loc.id, loc.entries);

            std::weak_ptr<bool> weakAlive = self.aliveToken_;
            SdkBindings* selfPtr = &self;
            page->onChange([weakAlive, selfPtr](std::string_view id,
                                                const bedrock::sdk::SettingValue& val) {
                if (const auto alive = weakAlive.lock(); alive && *alive) {
                    selfPtr->enqueueSettingChange(std::string(id), val);
                }
            });

            const auto res = self.world_.registerSettingsPage(self.modName_, *page);
            if (!res.ok) return self.throwBackendError(ctx, res);
            self.activePage_ = std::move(page);
            self.settings_.registered = true;
            return JS_UNDEFINED;
        }
        case Op::SettingsUnregisterPage: {
            const auto title = jsToString(ctx, arg(0));
            if (self.activePage_) {
                const auto res = self.world_.unregisterSettingsPage(self.modName_, *self.activePage_);
                self.activePage_.reset();
                self.settings_.registered = false;
                if (title.empty() || self.settings_.titleKey == title)
                    self.settings_.titleKey.clear();
                if (!res.ok) return self.throwBackendError(ctx, res);
            } else {
                if (title.empty() || self.settings_.titleKey == title)
                    self.settings_.titleKey.clear();
                self.settings_.registered = false;
            }
            return JS_UNDEFINED;
        }
        case Op::SettingsValue: {
            const auto settingId = jsToString(ctx, arg(1));
            if (!self.activePage_)
                return throwJsError(ctx, "NotFound", "Settings page is not registered");
            bedrock::sdk::SettingValue val;
            const auto res = self.world_.readSettingValue(self.modName_, *self.activePage_, settingId, val);
            if (!res.ok) return self.throwBackendError(ctx, res);
            if (const auto* b = std::get_if<bool>(&val)) return JS_NewBool(ctx, *b);
            if (const auto* d = std::get_if<double>(&val)) return JS_NewFloat64(ctx, *d);
            if (const auto* s = std::get_if<std::string>(&val)) return JS_NewString(ctx, s->c_str());
            return JS_UNDEFINED;
        }

        // --- KeyBindings (§5.2) ---
        case Op::KeybindAdd: {
            const auto id = jsToString(ctx, arg(0));
            const auto labelKey = jsToString(ctx, arg(1));
            std::optional<bedrock::sdk::Key> parsedKey;
            if (JS_IsNumber(arg(2))) {
                const auto code = static_cast<unsigned>(jsToNumber(ctx, arg(2), 0));
                parsedKey = static_cast<bedrock::sdk::Key>(code);
            } else {
                parsedKey = parseKeyName(jsToString(ctx, arg(2)));
            }
            if (!parsedKey || id.empty() || labelKey.empty())
                return throwJsError(ctx, "Invalid", "invalid keybind definition or unknown key name");
            const auto res = self.world_.keybindAdd(self.modName_, id, labelKey, *parsedKey);
            if (!res.ok) return self.throwBackendError(ctx, res);
            self.keybinds_.bindings.push_back({id, labelKey, *parsedKey});
            return JS_UNDEFINED;
        }
        case Op::KeybindLocale: {
            const auto localeId = jsToString(ctx, arg(0));
            auto entries = parseTranslations(ctx, arg(1));
            self.world_.keybindLocale(self.modName_, localeId, entries);
            self.keybinds_.locales.push_back({localeId, std::move(entries)});
            return JS_UNDEFINED;
        }
        case Op::KeybindKey: {
            const auto id = jsToString(ctx, arg(0));
            if (const auto k = self.world_.keybindKey(self.modName_, id))
                return JS_NewString(ctx, keyToName(*k).c_str());
            for (const auto& b : self.keybinds_.bindings)
                if (b.id == id) return JS_NewString(ctx, keyToName(b.defaultKey).c_str());
            return JS_UNDEFINED;
        }
        case Op::KeybindSubscribe: {
            if (!JS_IsFunction(ctx, arg(0)))
                return throwJsError(ctx, "Invalid", "keybinds.changed.subscribe expects a function");
            const auto subId = self.nextSubId_++;
            self.keybindSubscribers_[subId] = JS_DupValue(ctx, arg(0));
            return JS_NewFloat64(ctx, static_cast<double>(subId));
        }
        case Op::KeybindUnsubscribe: {
            const auto subId = static_cast<std::uint64_t>(jsToNumber(ctx, arg(0), 0));
            if (const auto it = self.keybindSubscribers_.find(subId);
                it != self.keybindSubscribers_.end()) {
                JS_FreeValue(ctx, it->second);
                self.keybindSubscribers_.erase(it);
            }
            return JS_UNDEFINED;
        }

        // --- BrightnessOverride (§5.2) ---
        case Op::BrightnessSetGamma: {
            std::optional<float> gamma;
            if (!JS_IsUndefined(arg(0)) && !JS_IsNull(arg(0))) {
                if (!JS_IsNumber(arg(0)))
                    return throwJsError(ctx, "Invalid", "Gamma must be a number or undefined");
                const double raw = jsToNumber(ctx, arg(0), NAN);
                if (!std::isfinite(raw) || raw < 0.0 || raw > 25.0)
                    return throwJsError(ctx, "Invalid", "Gamma must be finite and in 0..25");
                gamma = static_cast<float>(raw);
            }
            const auto res = self.world_.setBrightnessGamma(self.modName_, gamma);
            if (!res.ok) return self.throwBackendError(ctx, res);
            self.lastGamma_ = gamma;
            self.hasGammaOverride_ = gamma.has_value();
            return JS_UNDEFINED;
        }

        case Op::FilesDataDir: {
            std::string err;
            if (!self.ensureFilesRoot(err)) return throwJsError(ctx, "Storage", err);
            return JS_NewString(ctx, fs::absolute(self.filesRoot_).generic_string().c_str());
        }
        case Op::FilesReadText: {
            std::string err;
            if (!self.ensureFilesRoot(err)) return throwJsError(ctx, "Storage", err);
            const auto path = self.resolveFilesPath(jsToString(ctx, arg(0)), err);
            if (!path) return throwJsError(ctx, "Invalid", err);
            std::vector<std::uint8_t> bytes;
            if (!self.readFileBytes(*path, bytes, err)) return throwJsError(ctx, "NotFound", err);
            return bytes.empty()
                       ? JS_NewString(ctx, "")
                       : JS_NewStringLen(ctx, reinterpret_cast<const char*>(bytes.data()),
                                         bytes.size());
        }
        case Op::FilesWriteText:
        case Op::FilesAppendText: {
            std::string err;
            if (!self.ensureFilesRoot(err)) return throwJsError(ctx, "Storage", err);
            const auto path = self.resolveFilesPath(jsToString(ctx, arg(0)), err);
            if (!path) return throwJsError(ctx, "Invalid", err);
            const std::string text = jsToString(ctx, arg(1));
            const std::vector<std::uint8_t> bytes(text.begin(), text.end());
            if (!self.writeFileBytes(*path, bytes, op == Op::FilesAppendText, err))
                return throwJsError(ctx, "Storage", err);
            return JS_UNDEFINED;
        }
        case Op::FilesReadBytes: {
            std::string err;
            if (!self.ensureFilesRoot(err)) return throwJsError(ctx, "Storage", err);
            const auto path = self.resolveFilesPath(jsToString(ctx, arg(0)), err);
            if (!path) return throwJsError(ctx, "Invalid", err);
            std::vector<std::uint8_t> bytes;
            if (!self.readFileBytes(*path, bytes, err)) return throwJsError(ctx, "NotFound", err);
            return JS_NewUint8ArrayCopy(ctx, bytes.empty() ? reinterpret_cast<const std::uint8_t*>("")
                                                          : bytes.data(),
                                        bytes.size());
        }
        case Op::FilesWriteBytes: {
            std::string err;
            if (!self.ensureFilesRoot(err)) return throwJsError(ctx, "Storage", err);
            const auto path = self.resolveFilesPath(jsToString(ctx, arg(0)), err);
            if (!path) return throwJsError(ctx, "Invalid", err);
            const int arrayType = JS_GetTypedArrayType(arg(1));
            std::uint8_t* data = nullptr;
            std::size_t size = 0;
            bool valid = false;
            if (arrayType == JS_TYPED_ARRAY_UINT8 || arrayType == JS_TYPED_ARRAY_UINT8C) {
                data = JS_GetUint8Array(ctx, &size, arg(1));
                valid = true;
            } else if (JS_IsArrayBuffer(arg(1))) {
                data = JS_GetArrayBuffer(ctx, &size, arg(1));
                valid = true;
            }
            if (!valid)
                return throwJsError(ctx, "WrongType",
                                    "writeBytes expects a Uint8Array or ArrayBuffer");
            std::vector<std::uint8_t> bytes;
            if (size != 0) {
                if (!data)
                    return throwJsError(ctx, "Storage", "cannot access the byte array data");
                bytes.assign(data, data + size);
            }
            if (!self.writeFileBytes(*path, bytes, false, err))
                return throwJsError(ctx, "Storage", err);
            return JS_UNDEFINED;
        }
        case Op::FilesExists: {
            std::string err;
            if (!self.ensureFilesRoot(err)) return throwJsError(ctx, "Storage", err);
            const auto path = self.resolveFilesPath(jsToString(ctx, arg(0)), err);
            if (!path) return throwJsError(ctx, "Invalid", err);
            std::error_code ec;
            return JS_NewBool(ctx, fs::exists(*path, ec));
        }
        case Op::FilesStat: {
            std::string err;
            if (!self.ensureFilesRoot(err)) return throwJsError(ctx, "Storage", err);
            const auto path = self.resolveFilesPath(jsToString(ctx, arg(0)), err);
            if (!path) return throwJsError(ctx, "Invalid", err);
            std::error_code ec;
            const auto status = fs::status(*path, ec);
            if (ec || !fs::exists(status))
                return throwJsError(ctx, "NotFound", "path does not exist: " + path->generic_string());
            const bool directory = fs::is_directory(status);
            const auto size = directory ? 0 : fs::file_size(*path, ec);
            if (ec) return throwJsError(ctx, "Storage", "cannot stat file: " + ec.message());
            const auto ftime = fs::last_write_time(*path, ec);
            if (ec) return throwJsError(ctx, "Storage", "cannot stat timestamp: " + ec.message());
            const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            const auto modifiedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        sctp.time_since_epoch())
                                        .count();
            JsValue object(ctx, JS_NewObject(ctx));
            JS_DefinePropertyValueStr(ctx, object.get(), "size",
                                      JS_NewFloat64(ctx, static_cast<double>(size)), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, object.get(), "directory", JS_NewBool(ctx, directory),
                                      JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, object.get(), "modifiedMs",
                                      JS_NewFloat64(ctx, static_cast<double>(modifiedMs)),
                                      JS_PROP_C_W_E);
            return object.release();
        }
        case Op::FilesList: {
            std::string err;
            if (!self.ensureFilesRoot(err)) return throwJsError(ctx, "Storage", err);
            const auto path = self.resolveFilesPath(jsToString(ctx, arg(0)), err);
            if (!path) return throwJsError(ctx, "Invalid", err);
            std::error_code ec;
            if (!fs::is_directory(*path, ec))
                return throwJsError(ctx, "NotFound", "not a directory: " + path->generic_string());
            JsValue array(ctx, JS_NewArray(ctx));
            int index = 0;
            for (fs::directory_iterator it(*path, ec), end; it != end && !ec; it.increment(ec)) {
                const auto name = it->path().filename().generic_string();
                const bool isDirectory = it->is_directory(ec);
                if (ec) break;
                JsValue entry(ctx, JS_NewObject(ctx));
                JS_DefinePropertyValueStr(ctx, entry.get(), "name", JS_NewString(ctx, name.c_str()),
                                          JS_PROP_C_W_E);
                JS_DefinePropertyValueStr(ctx, entry.get(), "directory", JS_NewBool(ctx, isDirectory),
                                          JS_PROP_C_W_E);
                JS_SetPropertyInt64(ctx, array.get(), index++, entry.release());
            }
            if (ec) return throwJsError(ctx, "Storage", "cannot list directory: " + ec.message());
            return array.release();
        }
        case Op::FilesMkdir: {
            std::string err;
            if (!self.ensureFilesRoot(err)) return throwJsError(ctx, "Storage", err);
            const auto path = self.resolveFilesPath(jsToString(ctx, arg(0)), err);
            if (!path) return throwJsError(ctx, "Invalid", err);
            std::error_code ec;
            fs::create_directories(*path, ec);
            if (ec) return throwJsError(ctx, "Storage", "cannot create directory: " + ec.message());
            return JS_UNDEFINED;
        }
        case Op::FilesRemove: {
            std::string err;
            if (!self.ensureFilesRoot(err)) return throwJsError(ctx, "Storage", err);
            const auto path = self.resolveFilesPath(jsToString(ctx, arg(0)), err);
            if (!path) return throwJsError(ctx, "Invalid", err);
            const bool recursive = jsToBool(ctx, arg(1), false);
            std::error_code ec;
            const auto status = fs::status(*path, ec);
            if (ec || !fs::exists(status))
                return throwJsError(ctx, "NotFound", "path does not exist: " + path->generic_string());
            if (fs::is_directory(status) && !recursive)
                return throwJsError(ctx, "Invalid", "removing a directory requires { recursive: true }");
            if (recursive) fs::remove_all(*path, ec);
            else fs::remove(*path, ec);
            if (ec) return throwJsError(ctx, "Storage", "cannot remove path: " + ec.message());
            return JS_UNDEFINED;
        }
        case Op::FilesRename: {
            std::string err;
            if (!self.ensureFilesRoot(err)) return throwJsError(ctx, "Storage", err);
            const auto from = self.resolveFilesPath(jsToString(ctx, arg(0)), err);
            if (!from) return throwJsError(ctx, "Invalid", err);
            const auto to = self.resolveFilesPath(jsToString(ctx, arg(1)), err);
            if (!to) return throwJsError(ctx, "Invalid", err);
            std::error_code ec;
            fs::create_directories(to->parent_path(), ec);
            if (ec) return throwJsError(ctx, "Storage", "cannot create parent directory: " + ec.message());
            fs::rename(*from, *to, ec);
            if (ec) return throwJsError(ctx, "Storage", "cannot rename: " + ec.message());
            return JS_UNDEFINED;
        }
        case Op::WorldObject:
            return JS_DupValue(ctx, self.worldObject_.get());
        case Op::SystemObject:
            return JS_DupValue(ctx, self.systemObject_.get());
        case Op::BrightnessObject:
            return JS_DupValue(ctx, self.brightnessObject_.get());
    }
    return JS_UNDEFINED;
}

void SdkBindings::enqueueSettingChange(std::string id, bedrock::sdk::SettingValue value) {
    std::lock_guard lock(pendingSettingsMutex_);
    pendingSettings_.push_back({std::move(id), std::move(value)});
}

void SdkBindings::drainPendingSettings() {
    std::deque<PendingSettingChange> local;
    {
        std::lock_guard lock(pendingSettingsMutex_);
        if (pendingSettings_.empty()) return;
        local.swap(pendingSettings_);
    }
    const auto ctx = runtime_.context();
    if (!ctx || JS_IsUndefined(settingsOnChange_)) return;
    for (const auto& change : local) {
        JsValue idVal(ctx, JS_NewString(ctx, change.id.c_str()));
        JsValue valJs(ctx, JS_UNDEFINED);
        if (const auto* b = std::get_if<bool>(&change.value))
            valJs = JsValue(ctx, JS_NewBool(ctx, *b));
        else if (const auto* d = std::get_if<double>(&change.value))
            valJs = JsValue(ctx, JS_NewFloat64(ctx, *d));
        else if (const auto* s = std::get_if<std::string>(&change.value))
            valJs = JsValue(ctx, JS_NewString(ctx, s->c_str()));

        JSValue result = JS_UNDEFINED;
        std::string err;
        runtime_.callFunction(settingsOnChange_, JS_UNDEFINED,
                              {idVal.get(), valJs.get()}, result, err);
        if (!JS_IsUndefined(result)) JS_FreeValue(ctx, result);
        if (runtime_.faulted()) break;
    }
}

void SdkBindings::notifyPlayerSpawn(std::uint64_t playerId, bool initialSpawn) {
    const auto ctx = runtime_.context();
    if (!ctx || playerSpawnSubscribers_.empty()) return;

    const JsValue global(ctx, JS_GetGlobalObject(ctx));
    const JsValue factory(ctx, JS_GetPropertyStr(ctx, global.get(), "__stagebMakePlayer"));
    JsValue playerVal(ctx, JS_UNDEFINED);
    if (JS_IsFunction(ctx, factory.get())) {
        JSValue argId = JS_NewFloat64(ctx, static_cast<double>(playerId));
        playerVal = JsValue(ctx, JS_Call(ctx, factory.get(), JS_UNDEFINED, 1, &argId));
        JS_FreeValue(ctx, argId);
    }
    JsValue eventObj(ctx, JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, eventObj.get(), "player", JS_DupValue(ctx, playerVal.get()));
    JS_SetPropertyStr(ctx, eventObj.get(), "initialSpawn", JS_NewBool(ctx, initialSpawn));

    // Copy subscriber callbacks so unsubscription during dispatch only affects future events.
    std::vector<JSValue> callbacks;
    callbacks.reserve(playerSpawnSubscribers_.size());
    for (const auto& [id, fn] : playerSpawnSubscribers_) {
        (void)id;
        callbacks.push_back(JS_DupValue(ctx, fn));
    }
    for (JSValue fn : callbacks) {
        JSValue result = JS_UNDEFINED;
        std::string err;
        runtime_.callFunction(fn, JS_UNDEFINED, {eventObj.get()}, result, err);
        if (!JS_IsUndefined(result)) JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, fn);
        if (runtime_.faulted()) break;
    }
}

void SdkBindings::notifyKeybind(std::string_view id, bool pressed) {
    const auto ctx = runtime_.context();
    if (!ctx || keybindSubscribers_.empty()) return;

    JsValue eventObj(ctx, JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, eventObj.get(), "id",
                      JS_NewStringLen(ctx, id.data(), id.size()));
    JS_SetPropertyStr(ctx, eventObj.get(), "pressed", JS_NewBool(ctx, pressed));

    std::vector<JSValue> callbacks;
    callbacks.reserve(keybindSubscribers_.size());
    for (const auto& [subId, fn] : keybindSubscribers_) {
        (void)subId;
        callbacks.push_back(JS_DupValue(ctx, fn));
    }
    for (JSValue fn : callbacks) {
        JSValue result = JS_UNDEFINED;
        std::string err;
        runtime_.callFunction(fn, JS_UNDEFINED, {eventObj.get()}, result, err);
        if (!JS_IsUndefined(result)) JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, fn);
        if (runtime_.faulted()) break;
    }
}

void SdkBindings::advanceTick() {
    drainPendingSettings();
    ++tick_;
    const auto ctx = runtime_.context();
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->second.due > tick_) { ++it; continue; }
        const auto function = it->second.function;
        const bool repeat = it->second.period != 0;
        if (repeat) it->second.due = tick_ + it->second.period;
        else {
            JS_FreeValue(ctx, function);
            it = tasks_.erase(it);
        }
        JsRuntime::CallScope scope(runtime_);
        const JsValue result(ctx, JS_Call(ctx, function, JS_UNDEFINED, 0, nullptr));
        if (JS_IsException(result.get())) runtime_.takeException();
        if (runtime_.faulted()) { runtime_.clearFault(); break; }
        if (!repeat) continue;
    }
}

} // namespace stageb

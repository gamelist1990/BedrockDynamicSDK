#pragma once
// AvailableCommands (packet 76) body rewriter: appends command
// definitions and hard enums to the packet the server sent, copying
// every untouched section byte-for-byte, so the client builds its
// native parse/completion context for commands the server never
// defined. Verified live on 1.26.65.1 (2026-11-18): injected
// commands complete natively (block ids, custom enums, targets)
// and execute as CommandRequests the mod can intercept.
//
// Wire format (sections, in order): enum value pool (string array;
// enum records index into THIS array), chained subcommand names,
// suffixes, enums, chained-subcommand data, COMMANDS, dynamic
// enums, constraints.
// Command record: name, description, flags u16, permission string,
// alias i32, chained indexes, overloads{ chaining u8, params{
// name, type u32, optional u8, options u8 } }.
// A hard-enum parameter type is (packet enum index | 0x300000);
// built-in types live in the 0x100000 space (int = 0x100001,
// target = 0x100008, position = 0x100040, message = 0x100044).
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bedrock::detail::acpwire {

struct Reader {
    const std::uint8_t* d{};
    std::size_t n{}, p{};
    bool fail{};
    std::uint8_t u8() {
        if (p + 1 > n) { fail = true; return 0; }
        return d[p++];
    }
    std::uint16_t u16() {
        if (p + 2 > n) { fail = true; return 0; }
        std::uint16_t v;
        std::memcpy(&v, d + p, 2);
        p += 2;
        return v;
    }
    std::uint32_t u32() {
        if (p + 4 > n) { fail = true; return 0; }
        std::uint32_t v;
        std::memcpy(&v, d + p, 4);
        p += 4;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::uint64_t uv() {
        std::uint64_t r = 0;
        int s = 0;
        while (true) {
            const std::uint8_t b = u8();
            if (fail) return 0;
            r |= static_cast<std::uint64_t>(b & 0x7f) << s;
            if (!(b & 0x80)) break;
            s += 7;
            if (s > 63) { fail = true; return 0; }
        }
        return r;
    }
    std::string s() {
        const auto len = uv();
        if (fail || p + len > n) { fail = true; return {}; }
        std::string v(reinterpret_cast<const char*>(d + p), static_cast<std::size_t>(len));
        p += static_cast<std::size_t>(len);
        return v;
    }
    void skip(std::size_t k) {
        if (p + k > n) { fail = true; return; }
        p += k;
    }
};

struct Writer {
    std::string out;
    void raw(std::string_view v) { out += v; }
    void u8(std::uint8_t v) { out += static_cast<char>(v); }
    void u16(std::uint16_t v) {
        char b[2];
        std::memcpy(b, &v, 2);
        out.append(b, 2);
    }
    void u32(std::uint32_t v) {
        char b[4];
        std::memcpy(b, &v, 4);
        out.append(b, 4);
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void uv(std::uint64_t v) {
        do {
            std::uint8_t b = static_cast<std::uint8_t>(v & 0x7f);
            v >>= 7;
            if (v) b |= 0x80;
            u8(b);
        } while (v);
    }
    void s(std::string_view v) {
        uv(v.size());
        out += v;
    }
};

struct Param {
    std::string name;
    std::uint32_t type{};
    std::uint8_t optional{};
    std::uint8_t options{};
};
struct Command {
    std::string name;
    std::string description;
    std::uint16_t flags{};
    std::string permission{"any"};
    std::vector<std::vector<Param>> overloads;
};

// Locate the commands array inside the body. Returns the offset of
// the count varint, the offset just past it, the offset just past
// the last command record, and the command count.
struct CommandsSpan {
    std::size_t countOffset{};
    std::size_t firstRecord{};
    std::size_t endOffset{};
    std::uint64_t count{};
};
inline std::optional<CommandsSpan> locateCommands(std::string_view body) {
    Reader r{reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), 0};
    auto skipStrings = [&] {
        const auto c = r.uv();
        for (std::uint64_t i = 0; i < c && !r.fail; ++i) (void)r.s();
    };
    skipStrings(); // string pool
    skipStrings(); // chained subcommand names
    skipStrings(); // enum value pool
    const auto nEnums = r.uv(); // enums: name + u32 indexes into the pool
    for (std::uint64_t i = 0; i < nEnums && !r.fail; ++i) {
        (void)r.s();
        const auto cnt = r.uv();
        r.skip(static_cast<std::size_t>(cnt) * 4);
    }
    const auto nCsd = r.uv(); // chained-subcommand data: name + (u8,u8) pairs
    for (std::uint64_t i = 0; i < nCsd && !r.fail; ++i) {
        (void)r.s();
        const auto cnt = r.uv();
        r.skip(static_cast<std::size_t>(cnt) * 2);
    }
    if (r.fail) return std::nullopt;
    CommandsSpan span;
    span.countOffset = r.p;
    span.count = r.uv();
    span.firstRecord = r.p;
    for (std::uint64_t ci = 0; ci < span.count && !r.fail; ++ci) {
        (void)r.s(); // name
        (void)r.s(); // description
        (void)r.u16(); // flags
        (void)r.s(); // permission
        (void)r.i32(); // alias enum index
        const auto nCh = r.uv();
        r.skip(static_cast<std::size_t>(nCh) * 4);
        const auto nOv = r.uv();
        for (std::uint64_t oi = 0; oi < nOv && !r.fail; ++oi) {
            (void)r.u8(); // chaining
            const auto nPar = r.uv();
            for (std::uint64_t pi = 0; pi < nPar && !r.fail; ++pi) {
                (void)r.s();
                (void)r.u32();
                (void)r.u8();
                (void)r.u8();
            }
        }
    }
    if (r.fail) return std::nullopt;
    span.endOffset = r.p;
    return span;
}

inline void writeCommand(Writer& w, const Command& c) {
    w.s(c.name);
    w.s(c.description);
    w.u16(c.flags);
    w.s(c.permission);
    w.i32(-1); // no alias
    w.uv(0); // no chained subcommand indexes
    w.uv(c.overloads.size());
    for (const auto& overload : c.overloads) {
        w.u8(0); // not chaining
        w.uv(overload.size());
        for (const auto& p : overload) {
            w.s(p.name);
            w.u32(p.type);
            w.u8(p.optional);
            w.u8(p.options);
        }
    }
}

// The body with `extra` appended to its commands array; nullopt
// when the body does not parse.
inline std::optional<std::string> appendCommand(std::string_view body, const Command& extra) {
    const auto span = locateCommands(body);
    if (!span) return std::nullopt;
    Writer w;
    w.raw(body.substr(0, span->countOffset));
    w.uv(span->count + 1);
    w.raw(body.substr(span->firstRecord, span->endOffset - span->firstRecord));
    writeCommand(w, extra);
    w.raw(body.substr(span->endOffset));
    return w.out;
}

// Section map for the enum-appending rewrite. NOTE: enum records
// index into the FIRST string array (the enum value pool); the
// third string array is the suffix list and is NOT the pool.
struct Sections {
    std::size_t poolCountOffset{}, poolFirst{}, poolEnd{};
    std::uint64_t poolCount{};
    std::size_t enumCountOffset{}, enumFirst{}, enumEnd{};
    std::uint64_t enumCount{};
    CommandsSpan commands;
};
inline std::optional<Sections> locateSections(std::string_view body) {
    Reader r{reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), 0};
    auto skipStrings = [&] {
        const auto c = r.uv();
        for (std::uint64_t i = 0; i < c && !r.fail; ++i) (void)r.s();
    };
    Sections s;
    s.poolCountOffset = r.p;
    s.poolCount = r.uv();
    s.poolFirst = r.p;
    for (std::uint64_t i = 0; i < s.poolCount && !r.fail; ++i) (void)r.s();
    s.poolEnd = r.p;
    skipStrings(); // chained subcommand names
    skipStrings(); // suffixes
    s.enumCountOffset = r.p;
    s.enumCount = r.uv();
    s.enumFirst = r.p;
    for (std::uint64_t i = 0; i < s.enumCount && !r.fail; ++i) {
        (void)r.s();
        const auto cnt = r.uv();
        r.skip(static_cast<std::size_t>(cnt) * 4);
    }
    s.enumEnd = r.p;
    if (r.fail) return std::nullopt;
    auto commands = locateCommands(body);
    if (!commands) return std::nullopt;
    s.commands = *commands;
    return s;
}

// A named enum found in the packet: its index (the value OR'd with
// 0x300000 in a parameter type word) and its value strings.
struct EnumInfo {
    std::uint64_t index{};
    std::vector<std::string> values;
};
inline std::optional<EnumInfo> findEnum(std::string_view body, std::string_view name) {
    Reader r{reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), 0};
    std::vector<std::string> pool;
    {
        const auto c = r.uv();
        for (std::uint64_t i = 0; i < c && !r.fail; ++i) pool.push_back(r.s());
    }
    auto skipStrings = [&] {
        const auto c = r.uv();
        for (std::uint64_t i = 0; i < c && !r.fail; ++i) (void)r.s();
    };
    skipStrings();
    skipStrings();
    const auto n = r.uv();
    for (std::uint64_t i = 0; i < n && !r.fail; ++i) {
        auto nm = r.s();
        const auto cnt = r.uv();
        std::vector<std::uint32_t> idx;
        for (std::uint64_t j = 0; j < cnt && !r.fail; ++j) idx.push_back(r.u32());
        if (nm == name) {
            EnumInfo info;
            info.index = i;
            for (auto j : idx)
                if (j < pool.size()) info.values.push_back(pool[j]);
            return info;
        }
    }
    return std::nullopt;
}

// True when a command with this name is already defined (a rewrite
// must never append the same command twice).
inline bool hasCommand(std::string_view body, std::string_view name) {
    const auto span = locateCommands(body);
    if (!span) return false;
    Reader r{reinterpret_cast<const std::uint8_t*>(body.data()), body.size(), span->firstRecord};
    for (std::uint64_t i = 0; i < span->count && !r.fail; ++i) {
        if (r.s() == name) return true;
        (void)r.s(); // description
        r.skip(2);   // flags
        (void)r.s(); // permission string
        r.skip(4);   // alias index
        const auto nCh = r.uv(); // chained subcommand indexes
        r.skip(static_cast<std::size_t>(nCh) * 4);
        const auto nov = r.uv();
        for (std::uint64_t o = 0; o < nov && !r.fail; ++o) {
            r.skip(1); // chaining flag
            const auto np = r.uv();
            for (std::uint64_t p = 0; p < np && !r.fail; ++p) {
                (void)r.s(); // param name
                r.skip(6);   // type u32 + optional u8 + options u8
            }
        }
    }
    return false;
}

// Append a new hard enum (values appended to the shared pool) AND a
// command in one pass. The caller learns the new enum's packet
// index from `enumIndexOut` and uses (index | 0x300000) as the
// parameter type. Untouched regions are copied byte-for-byte.
inline std::optional<std::string> appendEnumAndCommand(std::string_view body,
                                                       std::string_view enumName,
                                                       const std::vector<std::string>& values,
                                                       const Command& extra,
                                                       std::uint64_t* enumIndexOut) {
    const auto s = locateSections(body);
    if (!s) return std::nullopt;
    if (enumIndexOut) *enumIndexOut = s->enumCount;
    Writer w;
    w.raw(body.substr(0, s->poolCountOffset));
    w.uv(s->poolCount + values.size());
    w.raw(body.substr(s->poolFirst, s->poolEnd - s->poolFirst));
    for (const auto& v : values) w.s(v);
    w.raw(body.substr(s->poolEnd, s->enumCountOffset - s->poolEnd));
    w.uv(s->enumCount + 1);
    w.raw(body.substr(s->enumFirst, s->enumEnd - s->enumFirst));
    w.s(enumName);
    w.uv(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
        w.u32(static_cast<std::uint32_t>(s->poolCount + i));
    w.raw(body.substr(s->enumEnd, s->commands.countOffset - s->enumEnd));
    w.uv(s->commands.count + 1);
    w.raw(body.substr(s->commands.firstRecord, s->commands.endOffset - s->commands.firstRecord));
    writeCommand(w, extra);
    w.raw(body.substr(s->commands.endOffset));
    return w.out;
}

// Like appendEnumAndCommand, but the new enum's values are the
// values of an existing packet enum (found by name, e.g. "Block")
// merged with extra values (duplicates dropped). This is the
// /mine shape: one enum offering every block id plus the mod's
// action words, so a single overload completes everything.
inline std::optional<std::string> appendMergedEnumAndCommand(
    std::string_view body, std::string_view enumName, std::string_view sourceEnumName,
    const std::vector<std::string>& extraValues, const Command& extra,
    std::uint64_t* enumIndexOut) {
    const auto source = findEnum(body, sourceEnumName);
    if (!source) return std::nullopt;
    std::vector<std::string> values = source->values;
    for (const auto& v : extraValues) {
        bool present = false;
        for (const auto& have : values)
            if (have == v) { present = true; break; }
        if (!present) values.push_back(v);
    }
    return appendEnumAndCommand(body, enumName, values, extra, enumIndexOut);
}

// Append ONLY a hard enum (no command). See appendEnumAndCommand.
inline std::optional<std::string> appendEnum(std::string_view body, std::string_view enumName,
                                             const std::vector<std::string>& values,
                                             std::uint64_t* enumIndexOut) {
    const auto s = locateSections(body);
    if (!s) return std::nullopt;
    if (enumIndexOut) *enumIndexOut = s->enumCount;
    Writer w;
    w.raw(body.substr(0, s->poolCountOffset));
    w.uv(s->poolCount + values.size());
    w.raw(body.substr(s->poolFirst, s->poolEnd - s->poolFirst));
    for (const auto& v : values) w.s(v);
    w.raw(body.substr(s->poolEnd, s->enumCountOffset - s->poolEnd));
    w.uv(s->enumCount + 1);
    w.raw(body.substr(s->enumFirst, s->enumEnd - s->enumFirst));
    w.s(enumName);
    w.uv(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
        w.u32(static_cast<std::uint32_t>(s->poolCount + i));
    w.raw(body.substr(s->enumEnd));
    return w.out;
}

// Append ONLY a merged enum (values of an existing packet enum plus
// extras). See appendMergedEnumAndCommand.
inline std::optional<std::string> appendMergedEnum(std::string_view body,
                                                   std::string_view enumName,
                                                   std::string_view sourceEnumName,
                                                   const std::vector<std::string>& extraValues,
                                                   std::uint64_t* enumIndexOut) {
    const auto source = findEnum(body, sourceEnumName);
    if (!source) return std::nullopt;
    std::vector<std::string> values = source->values;
    for (const auto& v : extraValues) {
        bool present = false;
        for (const auto& have : values)
            if (have == v) { present = true; break; }
        if (!present) values.push_back(v);
    }
    return appendEnum(body, enumName, values, enumIndexOut);
}

} // namespace bedrock::detail::acpwire

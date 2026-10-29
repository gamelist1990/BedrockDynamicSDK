#pragma once
#include <bedrock/native/packet_view.hpp>
#include <bedrock/packets/wire_packet.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <optional>

namespace bedrock {
// Borrowed for the callback only; copy payload() to retain it. Incoming data
// is observed after the game's successful read and is never modified here.
class PacketReadEvent {
  public:
    PacketReadEvent(unsigned id, std::string_view payload) : id_(id), payload_(payload) {}
    unsigned rawId() const noexcept {
        return id_;
    }
    PacketId id() const noexcept {
        return packetIdFromValue(id_).value_or(PacketId::Unknown);
    }
    std::string_view payload() const noexcept {
        return payload_;
    }

  private:
    unsigned id_{};
    std::string_view payload_;
};
class PacketReadEvents {
  public:
    void load(const nlohmann::json &root) {
        const auto &info = root.at("layouts").at("PacketRead");
        if (info.value("schema", 0) != 1 || info.value("pointer_size", 0) != 8 ||
            info.value("abi", std::string{}) !=
                "Result*(Packet*, Result*, ReadOnlyBinaryStream*, context*)" ||
            info.value("stream_view_offset", 0) != 0x28 ||
            info.value("stream_size_offset", 0) != 0x30 ||
            info.value("stream_cursor_offset", 0) != 0x38 ||
            info.value("result_success_offset", 0) != 0x40)
            throw std::runtime_error("unsupported common Packet::read ABI");
        access_.load(root);
        const auto &function = info.at("read");
        target_ =
            reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) +
                                     native_profile::number(function.at("rva")));
        if (!BDS_HookOwnsTarget(BDS_HOOK_PACKET_READ, target_))
            target_ = native_profile::checked(root, function);
        DWORD64 image{};
        const auto *entry =
            RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(target_), &image, nullptr);
        if (!entry || image + entry->BeginAddress != reinterpret_cast<DWORD64>(target_))
            throw std::runtime_error("Packet::read is not a function entry");
        if (!BDS_HookRuntimeSupports(BDS_HOOK_PACKET_READ))
            throw std::runtime_error("Packet read Hook requires runtime 4.7 or newer; replace DLLs "
                                     "and restart Minecraft");
    }
    bool subscribe(BDS_HookCallback callback, void *user, BDS_HookToken *token) const {
        return target_ && BDS_HookSubscribe(BDS_HOOK_PACKET_READ, target_, callback, user, token);
    }
    template <class Handler>
    void dispatch(BDS_PacketReadArgs &args, Handler &&handler, BDS_HookNext next, void *frame,
                  int filterId = -1) const {
        dispatchFiltered(args, std::forward<Handler>(handler), next, frame,
                         [filterId](unsigned id) {
                             return filterId < 0 || id == static_cast<unsigned>(filterId);
                         });
    }
    template <class Handler, class Filter>
    void dispatchFiltered(BDS_PacketReadArgs &args, Handler &&handler, BDS_HookNext next,
                          void *frame, Filter &&filter) const {
        std::optional<std::string> payload;
        int id = -1;
        try {
            id = access_.view(args.packet).rawId();
            if (id >= 0 && filter(static_cast<unsigned>(id))) {
                std::string copy;
                if (copyPayload(args.stream, copy))
                    payload.emplace(std::move(copy));
            }
        } catch (...) {
        } // Observation failure must not interrupt the game's read.
        next(frame, &args);
        unsigned char success{};
        if (!payload || !native_profile::read(args.result, 0x40, success) || !(success & 1))
            return;
        try {
            handler(PacketReadEvent(static_cast<unsigned>(id), *payload));
        } catch (...) {
        }
    }

  private:
    static bool copyBytes(const void *source, void *destination, std::size_t size) noexcept {
        __try {
            std::memcpy(destination, source, size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    static bool copyPayload(const void *stream, std::string &output) {
        std::uintptr_t bytes{};
        std::size_t size{}, cursor{};
        if (!native_profile::read(stream, 0x28, bytes) ||
            !native_profile::read(stream, 0x30, size) ||
            !native_profile::read(stream, 0x38, cursor) || cursor > size ||
            size > WirePacket::maximumBytes || !bytes || bytes > UINTPTR_MAX - size)
            return false;
        output.resize(size - cursor);
        return copyBytes(reinterpret_cast<const void *>(bytes + cursor), output.data(),
                         output.size());
    }
    NativePacketAccess access_;
    void *target_{};
};
} // namespace bedrock

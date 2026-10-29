#pragma once
#include <bedrock/native/packet_view.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/packets/wire_packet.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <optional>
#include <memory>
#include <atomic>
#include <bedrock/native/packet_read_routes.hpp>

namespace bedrock {
// Borrowed for the callback only; copy payload() to retain it. Incoming data
// is observed after the game's successful read and is never modified here.
class PacketReadEvent {
  public:
    PacketReadEvent(unsigned id, std::string_view payload) : id_(id), payload_(payload) {}
    PacketReadEvent(unsigned id, std::shared_ptr<const std::string> bytes)
        : id_(id), payload_(*bytes), owned_(std::move(bytes)) {}
    std::shared_ptr<const std::string> ownedPayload() const {
        return owned_ ? owned_ : std::make_shared<const std::string>(payload_);
    }
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
    std::shared_ptr<const std::string> owned_;
};
class PacketReadEvents {
  public:
    struct Statistics {
        std::uint64_t calls{}, filtered{}, copied{}, streamFailures{}, decodeFailures{},
            published{}, nested{};
    };
    Statistics statistics() const noexcept {
        return {calls_.load(),          filtered_.load(),  copied_.load(), streamFailures_.load(),
                decodeFailures_.load(), published_.load(), nested_.load()};
    }
    bool routed() const noexcept {
        return routed_;
    }
    const auto &routes() const noexcept {
        return routes_.routes();
    }
    void load(std::string_view selected, std::string_view embedded) {
        auto root = parseNativeProfile(selected);
        if (!root.at("layouts").contains("PacketRead") ||
            root.at("layouts").at("PacketRead").value("revision", 0) < 2) {
            const auto fallback = parseNativeProfile(embedded);
            for (const auto *field :
                 {"sha256", "text_sha256", "size_of_image", "text_rva", "text_size"})
                if (root.at("target").at(field) != fallback.at("target").at(field))
                    throw std::runtime_error(
                        "embedded Packet receiver belongs to another executable");
            root = fallback;
        }
        load(root);
    }
    bool subscribeTarget(void *target, BDS_HookCallback callback, void *user,
                         BDS_HookToken *token) const {
        return BDS_HookSubscribe(BDS_HOOK_PACKET_READ, target, callback, user, token) != 0;
    }
    void load(const nlohmann::json &root) {
        const auto &info = root.at("layouts").at("PacketRead");
        if (info.value("schema", 0) != 1 || info.value("pointer_size", 0) != 8 ||
            info.value("abi", std::string{}) !=
                "Result*(Packet*, Result*, ReadOnlyBinaryStream*, context*)")
            throw std::runtime_error("unsupported common Packet::read ABI");
        viewOffset_ = native_profile::number(info.at("stream_view_offset"));
        sizeOffset_ = native_profile::number(info.at("stream_size_offset"));
        cursorOffset_ = native_profile::number(info.at("stream_cursor_offset"));
        successOffset_ = native_profile::number(info.at("result_success_offset"));
        if (viewOffset_ > 4096 || sizeOffset_ > 4096 || cursorOffset_ > 4096 ||
            successOffset_ > 4096 || viewOffset_ == sizeOffset_ || sizeOffset_ == cursorOffset_ ||
            viewOffset_ == cursorOffset_)
            throw std::runtime_error("invalid Packet stream/result layout");
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
        routed_ = info.value("revision", 0) >= 2;
        if (routed_) {
            if (!BDS_HookRuntimeSupportsPacketRoutes())
                throw std::runtime_error("Packet decoder routes require runtime 4.8; replace Mod "
                                         "DLLs and restart Minecraft");
            routes_.load(root, target_);
        }
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
        ++calls_;
        // A wrapper may invoke the derived decoder. Publish one event at the
        // outermost call, after all native parsing/validation has succeeded.
        for (auto *call = activeRead_; call; call = call->previous)
            if (call->owner == this && call->packet == args.packet) {
                ++nested_;
                next(frame, &args);
                return;
            }
        ActiveRead call{this, args.packet, activeRead_};
        struct Scope {
            ActiveRead *previous;
            ~Scope() {
                activeRead_ = previous;
            }
        } scope{activeRead_};
        activeRead_ = &call;
        std::optional<std::string> payload;
        int id = -1;
        try {
            id = access_.view(args.packet).rawId();
            if (id >= 0 && filter(static_cast<unsigned>(id))) {
                std::string copy;
                if (copyPayload(args.stream, copy)) {
                    payload.emplace(std::move(copy));
                    ++copied_;
                } else
                    ++streamFailures_;
            } else
                ++filtered_;
        } catch (...) {
        } // Observation failure must not interrupt the game's read.
        next(frame, &args);
        unsigned char success{};
        if (!payload)
            return;
        if (!native_profile::read(args.result, successOffset_, success) || !(success & 1)) {
            ++decodeFailures_;
            return;
        }
        try {
            handler(PacketReadEvent(static_cast<unsigned>(id),
                                    std::make_shared<const std::string>(std::move(*payload))));
            ++published_;
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
    bool copyPayload(const void *stream, std::string &output) const {
        std::uintptr_t bytes{};
        std::size_t size{}, cursor{};
        if (!native_profile::read(stream, viewOffset_, bytes) ||
            !native_profile::read(stream, sizeOffset_, size) ||
            !native_profile::read(stream, cursorOffset_, cursor) || cursor > size ||
            size > WirePacket::maximumBytes || !bytes || bytes > UINTPTR_MAX - size)
            return false;
        output.resize(size - cursor);
        return copyBytes(reinterpret_cast<const void *>(bytes + cursor), output.data(),
                         output.size());
    }
    struct ActiveRead {
        const PacketReadEvents *owner;
        void *packet;
        ActiveRead *previous;
    };
    inline static thread_local ActiveRead *activeRead_{};
    mutable std::atomic<std::uint64_t> calls_{}, filtered_{}, copied_{}, streamFailures_{},
        decodeFailures_{}, published_{}, nested_{};
    std::size_t viewOffset_{}, sizeOffset_{}, cursorOffset_{}, successOffset_{};
    NativePacketReadRoutes routes_;
    bool routed_{};
    NativePacketAccess access_;
    void *target_{};
};
} // namespace bedrock

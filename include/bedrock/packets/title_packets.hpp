#pragma once
#include <bedrock/packets/packet_api.hpp>
#include <bedrock/packets/protocol_codec.hpp>
#include <bedrock/enums/title_type.hpp>
#include <bedrock/analysis/memory_inspector.hpp>

namespace bedrock {
struct TitlePacket {
    TitleType type{TitleType::Unknown};
    std::string text;
};
// Typed title observer. Selects the per-ID wire receiver when available; the
// older exact-build native title adapter stays entirely inside the SDK.
class TitlePackets {
  public:
    using Handler = std::function<void(const TitlePacket &)>;
    bool install(std::string_view selected, std::string_view embedded, Handler handler,
                 std::string &error) {
        try {
            if (installed_)
                throw std::runtime_error("title observer already installed");
            handler_ = std::move(handler);
            stopped_ = false;
            auto root = parseNativeProfile(selected);
            wire_ = root.contains("layouts") && root.at("layouts").contains("PacketRead");
            if (!wire_ &&
                !root.at("layouts").at("PacketSender").at("title").contains("type_offset")) {
                const auto fallback = parseNativeProfile(embedded);
                for (const auto *field :
                     {"sha256", "text_sha256", "size_of_image", "text_rva", "text_size"})
                    if (root.at("target").at(field) != fallback.at("target").at(field))
                        throw std::runtime_error(
                            "embedded title layout belongs to another executable");
                root = fallback;
            }
            if (wire_) {
                (void)ProtocolCodec::types();
                packets_.onReceive(PacketId::SetTitle, [this](const PacketReadEvent &event) {
                    if (stopped_)
                        return;
                    ++received_;
                    try {
                        const auto value = ProtocolCodec{}.decode(event.id(), event.payload());
                        TitlePacket title{protocol::titleType(value.at("type").get<std::string>()),
                                          value.at("text").get<std::string>()};
                        if (title.type == TitleType::Unknown || title.text.size() > 2048)
                            throw std::runtime_error("invalid title type/text length");
                        handler_(title);
                    } catch (...) {
                        ++rejected_;
                    }
                });
                if (!packets_.installReceive(selected, embedded, error))
                    throw std::runtime_error(error);
            } else {
                const auto &info = root.at("layouts").at("PacketSender").at("title");
                const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                table_ = base + native_profile::number(info.at("vtable_rva"));
                typeOffset_ = native_profile::number(info.at("type_offset"));
                textOffset_ = native_profile::number(info.at("text_offset"));
                successOffset_ = native_profile::number(info.at("result_success_offset"));
                maximum_ = native_profile::number(info.at("maximum_text_bytes"));
                if (typeOffset_ > 4096 || textOffset_ > 4096 || successOffset_ > 4096 ||
                    maximum_ > 2048)
                    throw std::runtime_error("invalid native title layout");
                if (active_)
                    throw std::runtime_error("native title observer already owned");
                active_ = this;
                if (!BDS_HookDirectInstall(native_profile::checked(root, info.at("read")),
                                           reinterpret_cast<void *>(&read),
                                           reinterpret_cast<void **>(&original_), &token_)) {
                    active_ = nullptr;
                    throw std::runtime_error("native title hook conflict");
                }
            }
            installed_ = true;
            error.clear();
            return true;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }
    void stop() {
        stopped_ = true;
        packets_.stop();
    } // Native trampoline remains pass-through, process-pinned.
    std::uint64_t received() const noexcept {
        return received_.load();
    }
    std::uint64_t rejected() const noexcept {
        return rejected_.load();
    }
    PacketReadEvents::Statistics receiveStatistics() const noexcept {
        return packets_.receiveStatistics();
    }

  private:
    using Read = void *(__fastcall *)(void *, void *, void *);
    static void *__fastcall read(void *packet, void *result, void *stream) {
        auto &self = *active_;
        auto *value = self.original_(packet, result, stream);
        if (self.stopped_)
            return value;
        try {
            std::uintptr_t table{};
            unsigned char success{};
            int type{};
            if (!native_profile::read(packet, 0, table) || table != self.table_ ||
                !native_profile::read(value, self.successOffset_, success) || !(success & 1) ||
                !native_profile::read(packet, self.typeOffset_, type))
                return value;
            TitlePacket title{static_cast<TitleType>(type), {}};
            if (type < static_cast<int>(TitleType::Clear) ||
                type > static_cast<int>(TitleType::ActionBarMessageJson))
                return value;
            if (title.type != TitleType::Clear && title.type != TitleType::Reset &&
                title.type != TitleType::SetDurations) {
                const auto *text = static_cast<const std::byte *>(packet) + self.textOffset_;
                std::size_t size{}, capacity{};
                std::uintptr_t bytes{};
                if (!native_profile::read(text, 16, size) ||
                    !native_profile::read(text, 24, capacity) || size > self.maximum_ ||
                    size > capacity)
                    throw std::runtime_error("native title string extent");
                if (capacity < 16)
                    bytes = reinterpret_cast<std::uintptr_t>(text);
                else if (!native_profile::read(text, 0, bytes))
                    throw std::runtime_error("native title string pointer");
                title.text.resize(size);
                if (!MemoryInspector::copy(bytes, std::as_writable_bytes(std::span(title.text))))
                    throw std::runtime_error("native title string unreadable");
            }
            ++self.received_;
            self.handler_(title);
        } catch (...) {
            ++self.rejected_;
        }
        return value;
    }
    inline static TitlePackets *active_{};
    PacketApi packets_;
    Handler handler_;
    Read original_{};
    BDS_HookToken token_{};
    std::uintptr_t table_{};
    std::size_t typeOffset_{}, textOffset_{}, successOffset_{}, maximum_{};
    std::atomic<bool> stopped_{};
    bool installed_{}, wire_{};
    std::atomic<std::uint64_t> received_{}, rejected_{};
};
} // namespace bedrock

#pragma once
#include <bedrock/enums/commands.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bedrock {

struct CommandOriginData {
    // Raw byte deliberately accepts values beyond the named origin types.
    std::uint8_t type = static_cast<std::uint8_t>(CommandOriginType::Player);
    std::array<std::uint64_t, 2> uuid{};
    std::string requestId;
    std::int64_t playerId{};
};
struct CommandRequestOptions {
    std::uint8_t origin = static_cast<std::uint8_t>(CommandOriginType::Player);
    std::int64_t playerId{};
    std::int32_t version{};
    bool internal{};
    std::chrono::milliseconds timeout{10000};
    std::optional<std::array<std::uint64_t,2>> uuid; // native invocation identity, when available
};
struct CommandRequest {
    std::string command;
    CommandOriginData origin;
    std::int32_t version{};
    bool internal{};
};
struct CommandOutputMessage {
    bool successful{};
    std::string id;
    std::vector<std::string> parameters;
};

inline const char* commandResponseStatusName(CommandResponseStatus status) noexcept {
    switch (status) {
    case CommandResponseStatus::Received: return "received";
    case CommandResponseStatus::Timeout: return "timeout";
    case CommandResponseStatus::Disconnected: return "disconnected";
    case CommandResponseStatus::Cancelled: return "cancelled";
    case CommandResponseStatus::InvalidRequest: return "invalid-request";
    case CommandResponseStatus::Unavailable: return "unavailable";
    default: return "queue-full";
    }
}
struct CommandResponse {
    CommandResponseStatus status{CommandResponseStatus::Received};
    CommandOriginData origin;
    std::uint32_t outputType{}, successCount{};
    std::vector<CommandOutputMessage> messages;
    // This adapter does not interpret the game's optional PropertyBag object.
    bool hasDataSet{};
    std::string detail;
    bool succeeded() const noexcept { return status == CommandResponseStatus::Received && successCount != 0; }
};
namespace command_request_detail {
struct Dispatcher {
    std::mutex mutex;
    std::deque<std::function<void()>> work;
    void post(std::function<void()> callback) { std::lock_guard lock(mutex); work.push_back(std::move(callback)); }
    void drain() {
        std::deque<std::function<void()>> batch;
        { std::lock_guard lock(mutex); batch.swap(work); }
        for (auto& callback : batch) try { callback(); } catch (...) { /* isolate subscribers */ }
    }
};
struct State {
    std::mutex mutex;
    std::optional<CommandResponse> result;
    std::vector<std::function<void(const CommandResponse&)>> handlers;
    std::weak_ptr<Dispatcher> dispatcher;
    std::atomic<bool> cancel{};
    void finish(CommandResponse response) {
        std::vector<std::function<void(const CommandResponse&)>> callbacks;
        {
            std::lock_guard lock(mutex);
            if (result) return;
            result = response; callbacks.swap(handlers);
        }
        if (auto owner = dispatcher.lock()) for (auto& callback : callbacks)
            owner->post([callback=std::move(callback), response] { callback(response); });
    }
};
}
class CommandFuture {
    friend class CommandRequests;
    std::shared_ptr<command_request_detail::State> state_;
    explicit CommandFuture(std::shared_ptr<command_request_detail::State> state) : state_(std::move(state)) {}
public:
    CommandFuture() = default;
    // Completion and late registrations both run from CommandRequests::poll().
    CommandFuture& then(std::function<void(const CommandResponse&)> handler) {
        if (!state_ || !handler) return *this;
        std::lock_guard lock(state_->mutex);
        if (state_->result) {
            if (auto owner = state_->dispatcher.lock())
                owner->post([handler=std::move(handler), response=*state_->result] { handler(response); });
        } else state_->handlers.push_back(std::move(handler));
        return *this;
    }
    void cancel() const noexcept { if (state_) state_->cancel = true; }
    std::optional<CommandResponse> result() const {
        if (!state_) return std::nullopt;
        std::lock_guard lock(state_->mutex); return state_->result;
    }
};

// Transport-independent SDK API. request()/receive() are thread-safe; the
// adapter calls poll() and reset() exclusively on its owning game thread.
class CommandRequests {
public:
    using Clock = std::chrono::steady_clock;
    using Transport = std::function<bool(const CommandRequest&)>;
    using Identity = std::function<CommandOriginData()>;
    explicit CommandRequests(Identity identity) : identity_(std::move(identity)) {}
    CommandRequests(const CommandRequests&) = delete;
    CommandRequests& operator=(const CommandRequests&) = delete;
    ~CommandRequests() { reset(); }
    CommandFuture request(std::string command, CommandRequestOptions options = {}) {
        auto state = std::make_shared<command_request_detail::State>(); state->dispatcher = dispatcher_;
        CommandFuture future{state};
        CommandResponse invalid; invalid.status = CommandResponseStatus::InvalidRequest;
        if (command.empty() || command.size() > 2048 || command.front() != '/' ||
            command.find_first_of(std::string{"\r\n\0", 3}) != std::string::npos ||
            options.timeout.count() < 1 || options.timeout > std::chrono::minutes{5}) {
            invalid.detail = "single slash command required; maximum 2048 bytes, timeout 1..300000ms";
            state->finish(std::move(invalid)); return future;
        }
        CommandOriginData origin;
        try { origin = identity_(); } catch (...) {
            invalid.detail = "request identity generation failed"; state->finish(std::move(invalid)); return future;
        }
        origin.type = options.origin; origin.playerId = options.playerId;
        if (options.uuid) origin.uuid=*options.uuid;
        std::lock_guard lock(mutex_);
        if (pending_.size() >= 128) { invalid.status = CommandResponseStatus::QueueFull; state->finish(std::move(invalid)); return future; }
        if (origin.requestId.empty() || pending_.contains(origin.requestId)) {
            invalid.detail = "invalid or duplicate request identity"; state->finish(std::move(invalid)); return future;
        }
        const auto id = origin.requestId;
        pending_.emplace(id, Pending{{std::move(command), std::move(origin), options.version, options.internal},
                                    state, Clock::now() + options.timeout, false});
        return future;
    }
    void receive(CommandResponse response) {
        std::lock_guard lock(mutex_);
        const auto it = pending_.find(response.origin.requestId);
        if (it == pending_.end() || !it->second.sent || response.origin.uuid != it->second.request.origin.uuid) return;
        response.status = CommandResponseStatus::Received;
        completed_.emplace_back(it->second.state, std::move(response)); pending_.erase(it);
    }
    void poll(const Transport& transport, Clock::time_point now = Clock::now()) {
        std::vector<std::pair<std::string, CommandRequest>> sends;
        std::vector<std::pair<std::shared_ptr<command_request_detail::State>, CommandResponse>> done;
        {
            std::lock_guard lock(mutex_); done.swap(completed_);
            for (auto it=pending_.begin(); it!=pending_.end();) {
                auto& p=it->second;
                if (p.state->cancel || now >= p.deadline || (!p.sent && !transport)) {
                    CommandResponse response;
                    response.status = p.state->cancel ? CommandResponseStatus::Cancelled :
                        (now >= p.deadline ? CommandResponseStatus::Timeout : CommandResponseStatus::Unavailable);
                    response.origin = p.request.origin;
                    done.emplace_back(p.state, std::move(response)); it = pending_.erase(it);
                } else {
                    if (!p.sent) { p.sent=true; sends.emplace_back(it->first, p.request); }
                    ++it;
                }
            }
        }
        for (const auto& [id, request] : sends) {
            bool sent=false; try { sent=transport(request); } catch (...) {}
            if (!sent) {
                std::lock_guard lock(mutex_);
                if (auto it=pending_.find(id); it!=pending_.end()) {
                    CommandResponse response; response.status=CommandResponseStatus::Unavailable;
                    response.origin=it->second.request.origin;
                    done.emplace_back(it->second.state, std::move(response)); pending_.erase(it);
                }
            }
        }
        for (auto& [state, response] : done) state->finish(std::move(response));
        dispatcher_->drain();
    }
    void reset(std::string detail = "request session reset") {
        std::vector<std::pair<std::shared_ptr<command_request_detail::State>, CommandResponse>> done;
        {
            std::lock_guard lock(mutex_); done.swap(completed_);
            for (auto& [id,p] : pending_) {
                CommandResponse response; response.status=CommandResponseStatus::Disconnected; response.origin=p.request.origin; response.detail=detail;
                done.emplace_back(p.state, std::move(response));
            }
            pending_.clear();
        }
        for (auto& [state,response] : done) state->finish(std::move(response));
    }
private:
    struct Pending {
        CommandRequest request;
        std::shared_ptr<command_request_detail::State> state;
        Clock::time_point deadline;
        bool sent{};
    };
    std::shared_ptr<command_request_detail::Dispatcher> dispatcher_ = std::make_shared<command_request_detail::Dispatcher>();
    Identity identity_;
    std::mutex mutex_;
    std::unordered_map<std::string, Pending> pending_;
    std::vector<std::pair<std::shared_ptr<command_request_detail::State>, CommandResponse>> completed_;
};
} // namespace bedrock

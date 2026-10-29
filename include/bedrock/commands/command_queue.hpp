#pragma once
#include <bedrock/enums/commands.hpp>
#include <bedrock/commands/command_builder.hpp>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>

namespace bedrock {



// Define commands before exposing callbacks. Native threads enqueue owned scalar
// arguments; only drain() invokes user handlers, on the owning client tick.
class CommandQueue {
public:
    bool add(CommandDefinition definition) {
        const auto name = definition.name().value();
        return definitions_.emplace(name, std::make_shared<CommandDefinition>(std::move(definition))).second;
    }
    bool contains(std::string_view name) const { return definitions_.contains(name); }
    CommandSubmission submit(std::string_view name, std::optional<int> value, CommandSource source) {
        const auto found = definitions_.find(name);
        if (found == definitions_.end()) return CommandSubmission::UnknownCommand;
        if (!value && !found->second->optional()) return CommandSubmission::InvalidArguments;
        return enqueue(found->second, value, source);
    }
    CommandSubmission submit(std::string_view line, CommandSource source) {
        if (line.empty() || line.front() != '/') return CommandSubmission::UnknownCommand;
        const auto end = line.find_first_of(" \t\r\n", 1);
        const auto name = line.substr(1, end == line.npos ? line.size() - 1 : end - 1);
        const auto found = definitions_.find(name);
        if (found == definitions_.end()) return CommandSubmission::UnknownCommand;
        if (line.size() > 2048) return CommandSubmission::InvalidArguments;
        auto args = end == line.npos ? std::string_view{} : line.substr(end);
        const auto first = args.find_first_not_of(" \t\r\n");
        std::optional<int> value;
        if (first != args.npos) {
            args = args.substr(first, args.find_last_not_of(" \t\r\n") - first + 1);
            int number{};
            const auto parsed = std::from_chars(args.data(), args.data() + args.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != args.data() + args.size())
                return CommandSubmission::InvalidArguments;
            value = number;
        }
        if (!value && !found->second->optional()) return CommandSubmission::InvalidArguments;
        return enqueue(found->second, value, source);
    }
    // A world change discards pending requests and duplicate-pair history.
    void reset(std::uint64_t epoch) {
        std::lock_guard lock(mutex_);
        pending_.clear(); recent_.reset(); epoch_ = epoch;
    }
    void stop() {
        std::lock_guard lock(mutex_);
        stopped_ = true; pending_.clear(); recent_.reset();
    }
    bool pending() const { std::lock_guard lock(mutex_); return !pending_.empty(); }
    std::size_t drain(std::size_t maximum = 1) {
        std::size_t dispatched{};
        while (dispatched < maximum) {
            std::optional<Request> request;
            {
                std::lock_guard lock(mutex_);
                if (stopped_ || pending_.empty()) break;
                request = std::move(pending_.front()); pending_.pop_front();
                if (request->epoch != epoch_) continue;
            }
            request->definition->invoke(request->value);
            ++dispatched;
        }
        return dispatched;
    }
private:
    using Clock = std::chrono::steady_clock;
    struct Request {
        std::shared_ptr<const CommandDefinition> definition;
        std::optional<int> value;
        std::uint64_t epoch{};
    };
    struct Recent {
        Request request;
        CommandSource source;
        Clock::time_point time;
        bool paired{};
    };
    CommandSubmission enqueue(std::shared_ptr<const CommandDefinition> definition,
                             std::optional<int> value, CommandSource source) {
        const auto now = Clock::now();
        std::lock_guard lock(mutex_);
        if (stopped_) return CommandSubmission::Stopped;
        if (recent_ && !recent_->paired && recent_->source != source &&
            recent_->request.definition == definition && recent_->request.value == value &&
            now - recent_->time < std::chrono::milliseconds{200}) {
            recent_->paired = true;
            return CommandSubmission::PairedDuplicate;
        }
        if (pending_.size() >= 16) return CommandSubmission::Full;
        Request request{std::move(definition), value, epoch_};
        pending_.push_back(request);
        recent_ = Recent{std::move(request), source, now, false};
        return CommandSubmission::Queued;
    }
    std::map<std::string, std::shared_ptr<const CommandDefinition>, std::less<>> definitions_;
    mutable std::mutex mutex_;
    std::deque<Request> pending_;
    std::optional<Recent> recent_;
    std::uint64_t epoch_{};
    bool stopped_{};
};
} // namespace bedrock

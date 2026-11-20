#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>

namespace bedrock::sdk {

class Mod;

// Scheduling is owned by one Mod and used on its game callback thread.
// Delays count delivered game ticks, not frames or wall-clock time.
class System {
public:
    using Task = std::function<void()>;
    using TaskId = std::uint64_t;

    TaskId run(Task task);
    TaskId runTimeout(Task task, unsigned ticks);
    TaskId runInterval(Task task, unsigned ticks);
    bool clearRun(TaskId id);

private:
    friend class Mod;
    struct Scheduled {
        std::uint64_t due;
        unsigned interval;
        std::shared_ptr<Task> task;
    };

    TaskId schedule(Task task, unsigned delay, unsigned interval);
    void advance();
    void clear();

    std::map<TaskId, Scheduled> tasks_;
    std::uint64_t tick_{};
    TaskId nextId_{1};
};

} // namespace bedrock::sdk

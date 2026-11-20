#include <bedrock/sdk/system.hpp>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bedrock::sdk {

System::TaskId System::run(Task task) {
    return runTimeout(std::move(task), 1);
}

System::TaskId System::runTimeout(Task task, unsigned ticks) {
    return schedule(std::move(task), std::max(1u, ticks), 0);
}

System::TaskId System::runInterval(Task task, unsigned ticks) {
    const auto period = std::max(1u, ticks);
    return schedule(std::move(task), period, period);
}

bool System::clearRun(TaskId id) {
    return tasks_.erase(id) != 0;
}

System::TaskId System::schedule(Task task, unsigned delay, unsigned interval) {
    if (!task) throw std::invalid_argument("cannot schedule an empty task");
    const auto id = nextId_++;
    tasks_.emplace(id, Scheduled{tick_ + delay, interval, std::make_shared<Task>(std::move(task))});
    return id;
}

void System::advance() {
    ++tick_;
    std::vector<TaskId> ready;
    for (const auto& [id, task] : tasks_)
        if (task.due <= tick_) ready.push_back(id);

    std::exception_ptr firstError;
    for (auto id : ready) {
        const auto found = tasks_.find(id);
        if (found == tasks_.end()) continue;
        const auto task = found->second.task;
        if (const auto interval = found->second.interval)
            found->second.due = tick_ + interval;
        else
            tasks_.erase(found);
        try {
            (*task)();
        } catch (...) {
            if (!firstError) firstError = std::current_exception();
        }
    }
    if (firstError) std::rethrow_exception(firstError);
}

void System::clear() {
    tasks_.clear();
    tick_ = 0;
}

} // namespace bedrock::sdk

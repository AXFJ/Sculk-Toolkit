#include "task_manager.h"

#include "console.h"

#include <algorithm>
#include <exception>

struct TaskState {
    mutable std::mutex mutex;
    TaskStatus status = TaskStatus::Running;
    double progress = 0.0;
    bool progress_available = true;
    std::string output;
};

struct TaskManager::TaskRecord {
    int id = 0;
    std::string command;
    std::shared_ptr<TaskState> state;
    std::jthread worker;
};

TaskManager::TaskManager() = default;
TaskManager::~TaskManager() = default;

TaskContext::TaskContext(std::shared_ptr<TaskState> state, std::stop_token stop_token)
    : state_(std::move(state)), stop_token_(stop_token) {}

bool TaskContext::cancelled() const {
    return stop_token_.stop_requested();
}

void TaskContext::set_progress(double progress) {
    std::lock_guard lock(state_->mutex);
    state_->progress = std::clamp(progress, 0.0, 1.0);
    state_->progress_available = true;
}

void TaskContext::set_progress_indeterminate() {
    std::lock_guard lock(state_->mutex);
    state_->progress_available = false;
}

void TaskContext::append_output(const std::string& text) {
    std::lock_guard lock(state_->mutex);
    state_->output += text;
}

int TaskManager::start(const std::string& command,
                       TaskFunction function,
                       bool progress_available) {
    const int id = next_id_.fetch_add(1);
    auto record = std::make_unique<TaskRecord>();
    record->id = id;
    record->command = command;
    record->state = std::make_shared<TaskState>();
    record->state->progress_available = progress_available;
    const auto state = record->state;

    record->worker = std::jthread(
        [state, function = std::move(function)](std::stop_token stop_token) mutable {
            TaskContext context(state, stop_token);
            try {
                function(context);
                std::lock_guard lock(state->mutex);
                state->status = stop_token.stop_requested()
                    ? TaskStatus::Cancelled
                    : TaskStatus::Completed;
                if (state->status == TaskStatus::Completed) {
                    if (state->progress_available) state->progress = 1.0;
                }
            } catch (const std::exception& exception) {
                std::lock_guard lock(state->mutex);
                state->output += console::colorize(
                    std::string("{COLOR_RED}任务异常: ") + exception.what() + "\n{COLOR_RESET}");
                state->status = TaskStatus::Failed;
            } catch (...) {
                std::lock_guard lock(state->mutex);
                state->output += console::colorize(
                    "{COLOR_RED}任务发生未知异常。\n{COLOR_RESET}");
                state->status = TaskStatus::Failed;
            }
        });

    std::lock_guard lock(mutex_);
    tasks_.emplace(id, std::move(record));
    return id;
}

std::vector<TaskSnapshot> TaskManager::list() const {
    std::vector<TaskSnapshot> snapshots;
    std::lock_guard manager_lock(mutex_);
    snapshots.reserve(tasks_.size());
    for (const auto& [id, record] : tasks_) {
        std::lock_guard state_lock(record->state->mutex);
        snapshots.push_back({id, record->command, record->state->status,
                             record->state->progress, record->state->progress_available, {}});
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    return snapshots;
}

std::optional<TaskSnapshot> TaskManager::snapshot(int id) const {
    std::lock_guard manager_lock(mutex_);
    const auto iterator = tasks_.find(id);
    if (iterator == tasks_.end()) {
        return std::nullopt;
    }

    const auto& record = iterator->second;
    std::lock_guard state_lock(record->state->mutex);
    TaskSnapshot snapshot{id, record->command, record->state->status,
                          record->state->progress, record->state->progress_available,
                          record->state->output};
    return snapshot;
}

bool TaskManager::remove(int id) {
    std::unique_ptr<TaskRecord> record;
    {
        std::lock_guard lock(mutex_);
        const auto iterator = tasks_.find(id);
        if (iterator == tasks_.end()) {
            return false;
        }
        record = std::move(iterator->second);
        tasks_.erase(iterator);
    }

    record->worker.request_stop();
    return true;
}

std::string task_status_text(TaskStatus status) {
    switch (status) {
        case TaskStatus::Running: return "运行中";
        case TaskStatus::Completed: return "已完成";
        case TaskStatus::Cancelled: return "已取消";
        case TaskStatus::Failed: return "失败";
    }
    return "未知";
}

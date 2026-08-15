#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

enum class TaskStatus {
    Running,
    Completed,
    Cancelled,
    Failed
};

struct TaskSnapshot {
    int id = 0;
    std::string command;
    TaskStatus status = TaskStatus::Running;
    double progress = 0.0;
    bool progress_available = true;
    std::string output;
};

struct TaskState;

class TaskContext {
public:
    bool cancelled() const;
    void set_progress(double progress);
    void set_progress_indeterminate();
    void append_output(const std::string& text);

private:
    friend class TaskManager;
    TaskContext(std::shared_ptr<TaskState> state, std::stop_token stop_token);

    std::shared_ptr<TaskState> state_;
    std::stop_token stop_token_;
};

class TaskManager {
public:
    using TaskFunction = std::function<void(TaskContext&)>;

    TaskManager();
    ~TaskManager();

    int start(const std::string& command,
              TaskFunction function,
              bool progress_available = true);
    std::vector<TaskSnapshot> list() const;
    std::optional<TaskSnapshot> snapshot(int id) const;
    bool remove(int id);

private:
    struct TaskRecord;

    mutable std::mutex mutex_;
    std::unordered_map<int, std::unique_ptr<TaskRecord>> tasks_;
    std::atomic<int> next_id_{1};
};

std::string task_status_text(TaskStatus status);

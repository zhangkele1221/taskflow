#include <iostream>
#include <vector>
#include <functional>
#include <memory>
#include <stdexcept>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

// 任务状态枚举
enum class TaskStatus {
    PENDING,    // 等待执行
    RUNNING,    // 执行中
    COMPLETED,  // 已完成
    FAILED      // 执行失败
};

// 任务异常类
class TaskException : public std::runtime_error {
public:
    TaskException(const std::string& msg) : std::runtime_error(msg) {}
};

// 基础任务接口
class ITask {
public:
    virtual ~ITask() = default;
    virtual void execute() = 0;
    virtual std::string name() const = 0;
    virtual TaskStatus status() const = 0;
    virtual void addDependency(std::shared_ptr<ITask> task) = 0;
    virtual const std::vector<std::shared_ptr<ITask>>& dependencies() const = 0;
};

// 具体任务实现
class Task : public ITask {
public:
    using TaskFunc = std::function<void()>;
    
    Task(const std::string& name, TaskFunc func) 
        : name_(name), func_(func), status_(TaskStatus::PENDING) {}
    
    void execute() override {
        try {
            status_ = TaskStatus::RUNNING;
            func_();
            status_ = TaskStatus::COMPLETED;
        } catch (const std::exception& e) {
            status_ = TaskStatus::FAILED;
            throw TaskException("Task '" + name_ + "' failed: " + e.what());
        }
    }
    
    std::string name() const override { return name_; }
    TaskStatus status() const override { return status_; }
    
    void addDependency(std::shared_ptr<ITask> task) override {
        dependencies_.push_back(task);
    }
    
    const std::vector<std::shared_ptr<ITask>>& dependencies() const override {
        return dependencies_;
    }

private:
    std::string name_;
    TaskFunc func_;
    TaskStatus status_;
    std::vector<std::shared_ptr<ITask>> dependencies_;
};

// 执行器接口
template <typename TaskType>
class IExecutor {
public:
    virtual ~IExecutor() = default;
    virtual void execute(TaskType task) = 0;
    virtual void shutdown() = 0;
};

// 简单顺序执行器
template <typename TaskType>
class SequentialExecutor : public IExecutor<TaskType> {
public:
    void execute(TaskType task) override {
        task->execute();
    }
    
    void shutdown() override {}
};

// 线程池执行器
template <typename TaskType>
class ThreadPoolExecutor : public IExecutor<TaskType> {
public:
    explicit ThreadPoolExecutor(size_t num_threads = std::thread::hardware_concurrency())
        : stop_(false) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    std::shared_ptr<TaskType> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex_);
                        condition_.wait(lock, [this] {
                            return stop_ || !tasks_.empty();
                        });
                        
                        if (stop_ && tasks_.empty()) return;
                        
                        task = tasks_.front();
                        tasks_.pop();
                    }
                    
                    try {
                        (*task)->execute();
                    } catch (...) {
                        // 异常处理逻辑
                    }
                }
            });
        }
    }
    
    void execute(TaskType task) override {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            tasks_.push(std::make_shared<TaskType>(task));
        }
        condition_.notify_one();
    }
    
    void shutdown() override {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (std::thread& worker : workers_) {
            worker.join();
        }
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::shared_ptr<TaskType>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_;
};

// 任务流框架
template <template <typename...> typename E>
class AdvancedTaskflow {
public:
    using TaskPtr = std::shared_ptr<ITask>;
    
    AdvancedTaskflow() : executor_(std::make_unique<E<TaskPtr>>()) {}
    
    explicit AdvancedTaskflow(size_t thread_count) 
        : executor_(std::make_unique<E<TaskPtr>>(thread_count)) {}
    
    TaskPtr create_task(const std::string& name, std::function<void()> func) {
        auto task = std::make_shared<Task>(name, func);
        tasks_[name] = task;
        return task;
    }
    
    void add_dependency(const std::string& from, const std::string& to) {
        if (!tasks_.count(from) || !tasks_.count(to)) {
            throw std::runtime_error("Task not found");
        }
        tasks_[to]->addDependency(tasks_[from]);
    }
    
    void run() {
        // 拓扑排序确定执行顺序
        auto ordered_tasks = topological_sort();
        
        // 执行任务
        for (auto& task : ordered_tasks) {
            // 检查依赖是否完成
            bool dependencies_completed = true;
            for (const auto& dep : task->dependencies()) {
                if (dep->status() != TaskStatus::COMPLETED) {
                    dependencies_completed = false;
                    break;
                }
            }
            
            if (dependencies_completed) {
                executor_->execute(task);
            } else {
                throw std::runtime_error("Dependencies not completed for task: " + task->name());
            }
        }
    }
    
    void shutdown() {
        executor_->shutdown();
    }
    
    TaskStatus task_status(const std::string& name) const {
        if (tasks_.count(name)) {
            return tasks_.at(name)->status();
        }
        throw std::runtime_error("Task not found");
    }

private:
    std::vector<TaskPtr> topological_sort() {
        std::vector<TaskPtr> result;
        std::queue<TaskPtr> queue;
        std::unordered_map<TaskPtr, size_t> in_degree;
        
        // 计算入度
        for (const auto& [name, task] : tasks_) {
            in_degree[task] = task->dependencies().size();
            if (in_degree[task] == 0) {
                queue.push(task);
            }
        }
        
        // 拓扑排序
        while (!queue.empty()) {
            auto current = queue.front();
            queue.pop();
            result.push_back(current);
            
            // 减少依赖项的入度
            for (const auto& dep : current->dependencies()) {
                if (--in_degree[dep] == 0) {
                    queue.push(dep);
                }
            }
        }
        
        // 检查是否有环
        if (result.size() != tasks_.size()) {
            throw std::runtime_error("Task dependency cycle detected");
        }
        
        return result;
    }

    std::unique_ptr<IExecutor<TaskPtr>> executor_;
    std::unordered_map<std::string, TaskPtr> tasks_;
};

// 测试用例
int main() {
    try {
        AdvancedTaskflow<ThreadPoolExecutor> taskflow(4);
        
        // 创建任务
        auto task1 = taskflow.create_task("Task1", [] {
            std::cout << "Task 1: Loading data...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        });
        
        auto task2 = taskflow.create_task("Task2", [] {
            std::cout << "Task 2: Processing data...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        });
        
        auto task3 = taskflow.create_task("Task3", [] {
            std::cout << "Task 3: Saving results...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        });
        
        auto task4 = taskflow.create_task("Task4", [] {
            std::cout << "Task 4: Sending notifications...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        });
        
        // 设置依赖关系
        taskflow.add_dependency("Task1", "Task2");
        taskflow.add_dependency("Task2", "Task3");
        taskflow.add_dependency("Task3", "Task4");
        
        // 执行任务流
        taskflow.run();
        
        // 检查任务状态
        std::cout << "Task4 status: " 
                  << static_cast<int>(taskflow.task_status("Task4")) 
                  << " (0=PENDING, 1=RUNNING, 2=COMPLETED, 3=FAILED)\n";
        
        // 关闭执行器
        taskflow.shutdown();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}

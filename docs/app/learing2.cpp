/*
SimpleExecutor: 顺序执行（已有）
WorkStealingExecutor: 工作窃取执行器（多线程优化）
ParallelForExecutor: 并行循环执行器
EventLoopExecutor: 事件循环执行器

*/

#include <iostream>
#include <vector>
#include <functional>
#include <memory>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <random>
#include <algorithm>

// ================== 执行器策略实现 ==================

// 1. 简单顺序执行器
template <typename TaskType>
class SimpleExecutor {
public:
    void execute(TaskType task) {
        std::cout << "[SimpleExecutor] Executing task\n";
        task();
    }
};

// 2. 工作窃取执行器（多线程优化）
template <typename TaskType>
class WorkStealingExecutor {
public:
    explicit WorkStealingExecutor(size_t num_threads = std::thread::hardware_concurrency())
        : stop_(false) {
        for (size_t i = 0; i < num_threads; ++i) {
            queues_.emplace_back();
            workers_.emplace_back([this, i] { worker_loop(i); });
        }
    }
    
    ~WorkStealingExecutor() {
        stop_ = true;
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }
    
    void execute(TaskType task) {
        // 随机选择一个队列添加任务
        static thread_local std::random_device rd;
        static thread_local std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> dist(0, queues_.size() - 1);
        
        size_t idx = dist(gen);
        {
            std::lock_guard<std::mutex> lock(queue_mutexes_[idx]);
            queues_[idx].push_back(task);
        }
        condition_.notify_one();
    }

private:
    void worker_loop(size_t my_index) {
        while (!stop_) {
            TaskType task;
            bool found = false;
            
            // 首先尝试从自己的队列获取任务
            {
                std::lock_guard<std::mutex> lock(queue_mutexes_[my_index]);
                if (!queues_[my_index].empty()) {
                    task = queues_[my_index].front();
                    queues_[my_index].pop_front();
                    found = true;
                }
            }
            
            // 如果自己的队列为空，尝试从其他队列窃取任务
            if (!found) {
                for (size_t i = 0; i < queues_.size(); ++i) {
                    if (i == my_index) continue;
                    
                    std::lock_guard<std::mutex> lock(queue_mutexes_[i]);
                    if (!queues_[i].empty()) {
                        task = queues_[i].back(); // 窃取队列尾部的任务
                        queues_[i].pop_back();
                        found = true;
                        break;
                    }
                }
            }
            
            // 如果找到任务则执行
            if (found) {
                std::cout << "[WorkStealingExecutor] Thread " << my_index 
                          << " executing task\n";
                task();
            } else {
                // 等待新任务
                std::unique_lock<std::mutex> lock(global_mutex_);
                condition_.wait(lock);
            }
        }
    }

    std::vector<std::thread> workers_;
    std::vector<std::deque<TaskType>> queues_;
    std::vector<std::mutex> queue_mutexes_;
    std::mutex global_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_;
};

// 3. 并行循环执行器
template <typename TaskType>
class ParallelForExecutor {
public:
    void execute(TaskType task) {
        // 在实际实现中，这里会处理并行循环逻辑
        // 为简化，我们只执行任务
        std::cout << "[ParallelForExecutor] Executing task\n";
        task();
    }
    
    // 模拟并行循环的接口
    template <typename IndexType, typename Func>
    void parallel_for(IndexType start, IndexType end, Func func) {
        std::cout << "[ParallelForExecutor] Running parallel loop from " 
                  << start << " to " << end << "\n";
        
        const size_t num_tasks = std::thread::hardware_concurrency();
        std::vector<std::thread> threads;
        
        IndexType chunk_size = (end - start) / num_tasks;
        IndexType current = start;
        
        for (size_t i = 0; i < num_tasks; ++i) {
            IndexType chunk_end = (i == num_tasks - 1) ? end : current + chunk_size;
            
            threads.emplace_back([=, &func] {
                for (IndexType j = current; j < chunk_end; ++j) {
                    func(j);
                }
            });
            
            current = chunk_end;
        }
        
        for (auto& t : threads) {
            t.join();
        }
    }
};

// 4. 事件循环执行器
template <typename TaskType>
class EventLoopExecutor {
public:
    EventLoopExecutor() : stop_(false) {
        worker_ = std::thread([this] { event_loop(); });
    }
    
    ~EventLoopExecutor() {
        stop_ = true;
        condition_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    
    void execute(TaskType task) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            task_queue_.push(task);
        }
        condition_.notify_one();
    }
    
    // 添加定时任务
    void schedule(TaskType task, std::chrono::milliseconds delay) {
        auto exec_time = std::chrono::steady_clock::now() + delay;
        {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            timer_queue_.emplace(exec_time, task);
        }
        condition_.notify_one();
    }

private:
    void event_loop() {
        while (!stop_) {
            // 处理定时任务
            {
                std::lock_guard<std::mutex> lock(timer_mutex_);
                auto now = std::chrono::steady_clock::now();
                
                while (!timer_queue_.empty() && timer_queue_.top().first <= now) {
                    auto task = timer_queue_.top().second;
                    timer_queue_.pop();
                    
                    std::cout << "[EventLoopExecutor] Executing scheduled task\n";
                    task();
                }
            }
            
            // 处理普通任务
            TaskType task;
            bool has_task = false;
            
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (!task_queue_.empty()) {
                    task = task_queue_.front();
                    task_queue_.pop();
                    has_task = true;
                }
            }
            
            if (has_task) {
                std::cout << "[EventLoopExecutor] Executing task\n";
                task();
            } else {
                // 计算下一个定时任务的等待时间
                auto next_time = std::chrono::steady_clock::time_point::max();
                
                {
                    std::lock_guard<std::mutex> lock(timer_mutex_);
                    if (!timer_queue_.empty()) {
                        next_time = timer_queue_.top().first;
                    }
                }
                
                // 等待新任务或下一个定时任务
                std::unique_lock<std::mutex> lock(global_mutex_);
                if (next_time == std::chrono::steady_clock::time_point::max()) {
                    condition_.wait(lock);
                } else {
                    condition_.wait_until(lock, next_time);
                }
            }
        }
    }

    std::thread worker_;
    std::queue<TaskType> task_queue_;
    std::mutex queue_mutex_;
    
    using TimerTask = std::pair<std::chrono::steady_clock::time_point, TaskType>;
    std::priority_queue<TimerTask, std::vector<TimerTask>, 
                        std::greater<TimerTask>> timer_queue_;
    std::mutex timer_mutex_;
    
    std::mutex global_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_;
};

// ================== 任务流框架 ==================

template <template <typename...> typename Executor>
class Taskflow {
public:
    using Task = std::function<void()>;
    
    Taskflow() : executor_(std::make_unique<Executor<Task>>()) {}
    
    template <typename... Args>
    explicit Taskflow(Args&&... args) 
        : executor_(std::make_unique<Executor<Task>>(std::forward<Args>(args)...)) {}
    
    void add_task(Task task) {
        tasks_.push_back(task);
    }
    
    void run() {
        std::cout << "Starting taskflow with " << tasks_.size() << " tasks\n";
        for (auto& task : tasks_) {
            executor_->execute(task);
        }
    }
    
    // 特殊接口：仅适用于ParallelForExecutor
    template <typename IndexType, typename Func>
    void parallel_for(IndexType start, IndexType end, Func func) {
        if constexpr (std::is_same_v<Executor<Task>, ParallelForExecutor<Task>>) {
            static_cast<ParallelForExecutor<Task>*>(executor_.get())
                ->parallel_for(start, end, func);
        } else {
            std::cerr << "Error: parallel_for only supported by ParallelForExecutor\n";
        }
    }
    
    // 特殊接口：仅适用于EventLoopExecutor
    void schedule(Task task, std::chrono::milliseconds delay) {
        if constexpr (std::is_same_v<Executor<Task>, EventLoopExecutor<Task>>) {
            static_cast<EventLoopExecutor<Task>*>(executor_.get())
                ->schedule(task, delay);
        } else {
            std::cerr << "Error: schedule only supported by EventLoopExecutor\n";
        }
    }

private:
    std::unique_ptr<Executor<Task>> executor_;
    std::vector<Task> tasks_;
};

// ================== 示例使用 ==================

int main() {
    std::cout << "===== 示例1: 简单顺序执行器 =====" << std::endl;
    {
        Taskflow<SimpleExecutor> taskflow;
        
        taskflow.add_task([] {
            std::cout << "Task 1: Loading configuration\n";
        });
        
        taskflow.add_task([] {
            std::cout << "Task 2: Initializing resources\n";
        });
        
        taskflow.add_task([] {
            std::cout << "Task 3: Starting application\n";
        });
        
        taskflow.run();
    }
    
    std::cout << "\n===== 示例2: 工作窃取执行器 =====" << std::endl;
    {
        Taskflow<WorkStealingExecutor> taskflow(4); // 4个工作线程
        
        for (int i = 0; i < 10; i++) {
            taskflow.add_task([i] {
                std::cout << "Task " << i << " executed on thread " 
                          << std::this_thread::get_id() << "\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            });
        }
        
        taskflow.run();
    }
    
    std::cout << "\n===== 示例3: 并行循环执行器 =====" << std::endl;
    {
        Taskflow<ParallelForExecutor> taskflow;
        
        // 添加并行循环任务
        taskflow.parallel_for(0, 20, [](int i) {
            std::cout << "Processing element " << i << " on thread "
                      << std::this_thread::get_id() << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });
        
        // 添加后续任务
        taskflow.add_task([] {
            std::cout << "Post-processing completed\n";
        });
        
        taskflow.run();
    }
    
    std::cout << "\n===== 示例4: 事件循环执行器 =====" << std::endl;
    {
        Taskflow<EventLoopExecutor> taskflow;
        
        // 添加立即执行的任务
        taskflow.add_task([] {
            std::cout << "Immediate task executed\n";
        });
        
        // 添加定时任务
        taskflow.schedule([] {
            std::cout << "Delayed task executed after 500ms\n";
        }, std::chrono::milliseconds(500));
        
        taskflow.schedule([] {
            std::cout << "Delayed task executed after 1000ms\n";
        }, std::chrono::milliseconds(1000));
        
        std::cout << "Starting event loop (will run for 1500ms)\n";
        taskflow.run();
        
        // 让事件循环运行一段时间
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
    
    return 0;
}
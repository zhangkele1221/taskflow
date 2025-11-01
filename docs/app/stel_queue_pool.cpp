// 2019/01/03 - modified by Tsung-Wei Huang
//   - updated the load balancing strategy
//
// 2018/12/24 - modified by Tsung-Wei Huang
//   - refined the work balancing strategy 
//
// 2018/12/06 - modified by Tsung-Wei Huang
//   - refactored the code
//   - added load balancing strategy
//   - removed the storage alignment in WorkStealingQueue
//
// 2018/12/03 - created by Tsung-Wei Huang
//   - added WorkStealingQueue class

#include <iostream>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <memory>
#include <cassert>
#include <deque>
#include <optional>
#include <thread>
#include <algorithm>
#include <set>
#include <numeric>
#include <unordered_map>
#include <chrono>
#include <future>
#include <random>

// 包含上面的WorkStealingThreadpool实现
// 假设上面的代码已经包含在同一个文件中

//using namespace tf;
using namespace std;
//using namespace std::chrono_literals;


namespace tf {

/**
@class: WorkStealingQueue

@tparam T data type

@brief Lock-free unbounded single-producer multiple-consumer queue.

This class implements the work stealing queue described in the paper, 
"Dynamic Circular Work-stealing Deque," SPAA, 2015.
Only the queue owner can perform pop and push operations,
while others can steal data from the queue.
*/
template <typename T>
class WorkStealingQueue {

  //constexpr static int64_t cacheline_size = 64;

  //using storage_type = std::aligned_storage_t<sizeof(T), cacheline_size>;

  struct Array {

    int64_t C;
    int64_t M;
    //storage_type* S;
    T* S;

    Array(int64_t c) : 
      C {c},
      M {c-1},
      //S {new storage_type[C]} {
      S {new T[static_cast<size_t>(C)]} {
      //for(int64_t i=0; i<C; ++i) {
      //  ::new (std::addressof(S[i])) T();
      //}
    }

    ~Array() {
      //for(int64_t i=0; i<C; ++i) {
      //  reinterpret_cast<T*>(std::addressof(S[i]))->~T();
      //}
      delete [] S;
    }

    int64_t capacity() const noexcept {
      return C;
    }
    
    template <typename O>
    void push(int64_t i, O&& o) noexcept {
      //T* ptr = reinterpret_cast<T*>(std::addressof(S[i & M]));
      //*ptr = std::forward<O>(o); 
      S[i & M] = std::forward<O>(o);
    }

    T pop(int64_t i) noexcept {
      //return *reinterpret_cast<T*>(std::addressof(S[i & M]));
      return S[i & M];
    }

    Array* resize(int64_t b, int64_t t) {
      Array* ptr = new Array {2*C};
      for(int64_t i=t; i!=b; ++i) {
        ptr->push(i, pop(i));
      }
      return ptr;
    }

  };

  std::atomic<int64_t> _top;
  std::atomic<int64_t> _bottom;
  std::atomic<Array*> _array;
  std::vector<Array*> _garbage;
  //char _padding[cacheline_size];

  public:
    
    /**
    @brief constructs the queue with a given capacity

    @param capacity the capacity of the queue (must be power of 2)
    */
    WorkStealingQueue(int64_t capacity = 4096);

    /**
    @brief destructs the queue
    */
    ~WorkStealingQueue();
    
    /**
    @brief queries if the queue is empty at the time of this call
    */
    bool empty() const noexcept;
    
    /**
    @brief queries the number of items at the time of this call
    */
    int64_t size() const noexcept;

    /**
    @brief queries the capacity of the queue
    */
    int64_t capacity() const noexcept;
    
    /**
    @brief inserts an item to the queue

    Only the owner thread can insert an item to the queue. 
    The operation can trigger the queue to resize its capacity 
    if more space is required.

    @tparam O data type 

    @param item the item to perfect-forward to the queue
    */
    template <typename O>
    void push(O&& item);
    
    /**
    @brief pops out an item from the queue

    Only the owner thread can pop out an item from the queue. 
    The return can be a @std_nullopt if this operation failed (not necessary empty).
    */
    std::optional<T> pop();

    /**
    @brief steals an item from the queue

    Any threads can try to steal an item from the queue.
    The return can be a @std_nullopt if this operation failed (not necessary empty).
    */
    std::optional<T> steal();
};

// Constructor
template <typename T>
WorkStealingQueue<T>::WorkStealingQueue(int64_t c) {
  assert(c && (!(c & (c-1))));
  _top.store(0, std::memory_order_relaxed);
  _bottom.store(0, std::memory_order_relaxed);
  _array.store(new Array{c}, std::memory_order_relaxed);
  _garbage.reserve(32);
}

// Destructor
template <typename T>
WorkStealingQueue<T>::~WorkStealingQueue() {
  for(auto a : _garbage) {
    delete a;
  }
  delete _array.load();
}
  
// Function: empty
template <typename T>
bool WorkStealingQueue<T>::empty() const noexcept {
  int64_t b = _bottom.load(std::memory_order_relaxed);
  int64_t t = _top.load(std::memory_order_relaxed);
  return b <= t;
}

// Function: size
template <typename T>
int64_t WorkStealingQueue<T>::size() const noexcept {
  int64_t b = _bottom.load(std::memory_order_relaxed);
  int64_t t = _top.load(std::memory_order_relaxed);
  return b - t;
}

// Function: push
template <typename T>
template <typename O>
void WorkStealingQueue<T>::push(O&& o) {
  int64_t b = _bottom.load(std::memory_order_relaxed);
  int64_t t = _top.load(std::memory_order_acquire);
  Array* a = _array.load(std::memory_order_relaxed);

  // queue is full
  if(a->capacity() - 1 < (b - t)) {
    Array* tmp = a->resize(b, t);
    _garbage.push_back(a);
    std::swap(a, tmp);
    _array.store(a, std::memory_order_relaxed);
  }

  a->push(b, std::forward<O>(o));
  std::atomic_thread_fence(std::memory_order_release);
  _bottom.store(b + 1, std::memory_order_relaxed);
}

// Function: pop
template <typename T>
std::optional<T> WorkStealingQueue<T>::pop() {
  int64_t b = _bottom.load(std::memory_order_relaxed) - 1;
  Array* a = _array.load(std::memory_order_relaxed);
  _bottom.store(b, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  int64_t t = _top.load(std::memory_order_relaxed);

  std::optional<T> item;

  if(t <= b) {
    item = a->pop(b);
    if(t == b) {
      // the last item just got stolen
      if(!_top.compare_exchange_strong(t, t+1, 
                                       std::memory_order_seq_cst, 
                                       std::memory_order_relaxed)) {
        item = std::nullopt;
      }
      _bottom.store(b + 1, std::memory_order_relaxed);
    }
  }
  else {
    _bottom.store(b + 1, std::memory_order_relaxed);
  }

  return item;
}

// Function: steal
template <typename T>
std::optional<T> WorkStealingQueue<T>::steal() {
  int64_t t = _top.load(std::memory_order_acquire);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  int64_t b = _bottom.load(std::memory_order_acquire);
  
  std::optional<T> item;

  if(t < b) {
    Array* a = _array.load(std::memory_order_consume);
    item = a->pop(t);
    if(!_top.compare_exchange_strong(t, t+1,
                                     std::memory_order_seq_cst,
                                     std::memory_order_relaxed)) {
      return std::nullopt;
    }
  }

  return item;
}

// Function: capacity
template <typename T>
int64_t WorkStealingQueue<T>::capacity() const noexcept {
  return _array.load(std::memory_order_relaxed)->capacity();
}

// ----------------------------------------------------------------------------


/** 
@class: WorkStealingThreadpool

@brief Executor that implements an efficient work stealing algorithm.

@tparam Closure closure type

*/
template <typename Closure>
class WorkStealingThreadpool {
    
  struct Worker {
    std::condition_variable cv;
    WorkStealingQueue<Closure> queue;// 每个 worker 准备一个 WorkStealingQueue<Closure>（以及线程池级别的 _queue），这些队列在无参构造时会隐式使用默认实参 4096 作为容量上限
    std::optional<Closure> cache;
    bool exit  {false};
    bool ready {false};
    uint64_t seed;
    unsigned last_victim;
  };

  public:
    
    /**
    @brief constructs the executor with a given number of worker threads

    @param N the number of worker threads
    */
    explicit WorkStealingThreadpool(unsigned N);

    /**
    @brief destructs the executor

    Destructing the executor will immediately force all worker threads to stop.
    The executor does not guarantee all tasks to finish upon destruction.
    */
    ~WorkStealingThreadpool();
    
    /**
    @brief queries the number of worker threads
    */
    size_t num_workers() const;
    
    /**
    @brief queries if the caller is the owner of the executor
    */
    bool is_owner() const;
    
    /**
    @brief constructs the closure in place in the executor

    @tparam ArgsT... argument parameter pack

    @param args... arguments to forward to the constructor of the closure
    */
    template <typename... ArgsT>
    void emplace(ArgsT&&... args);
    
    /**
    @brief moves a batch of closures to the executor

    @param closures a vector of closures to move
    */
    void batch(std::vector<Closure>&& closures);

  private:
    
    const std::thread::id _owner {std::this_thread::get_id()};

    mutable std::mutex _mutex;

    std::vector<Worker> _workers;
    std::vector<Worker*> _idlers;
    std::vector<std::thread> _threads;

    std::unordered_map<std::thread::id, size_t> _worker_maps;

    WorkStealingQueue<Closure> _queue;

    void _spawn(unsigned);
    void _shutdown();
    void _balance_load(unsigned);

    unsigned _randomize(uint64_t&) const;
    unsigned _fast_modulo(unsigned, unsigned) const;

    std::optional<Closure> _steal(unsigned);
};

// Constructor
template <typename Closure>
WorkStealingThreadpool<Closure>::WorkStealingThreadpool(unsigned N) : _workers {N} {
  _worker_maps.reserve(N);
  _spawn(N);
}

// Destructor
template <typename Closure>
WorkStealingThreadpool<Closure>::~WorkStealingThreadpool() {
  _shutdown();
}

// Procedure: _shutdown
template <typename Closure>
void WorkStealingThreadpool<Closure>::_shutdown(){

  assert(is_owner());

  {
    std::scoped_lock lock(_mutex);
    for(auto& w : _workers){
      w.exit = true;
      w.cv.notify_one();
    }
  } 

  for(auto& t : _threads){
    t.join();
  } 

  //_threads.clear();  
  //_workers.clear();
  //_worker_maps.clear();
}

// Function: _randomize
template <typename Closure>
unsigned WorkStealingThreadpool<Closure>::_randomize(uint64_t& state) const {
  uint64_t current = state;
  state = current * 6364136223846793005ULL + 0xda3e39cb94b95bdbULL;
  // Generate the random output (using the PCG-XSH-RS scheme)
  return static_cast<unsigned>((current ^ (current >> 22)) >> (22 + (current >> 61)));
}

// http://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
template <typename Closure>
unsigned WorkStealingThreadpool<Closure>::_fast_modulo(unsigned x, unsigned N) const {
  return ((uint64_t) x * (uint64_t) N) >> 32;
}

// Procedure: _spawn
template <typename Closure>//负责创建工作线程并实现工作窃取算法
void WorkStealingThreadpool<Closure>::_spawn(unsigned N) {
  
  // Lock to synchronize all workers before creating _worker_mapss
  std::scoped_lock lock(_mutex);
  
  for(unsigned i=0; i<N; ++i) {
    _threads.emplace_back([this, i, N] () -> void {

      std::optional<Closure> t;// 当前任务
      Worker& w = (_workers[i]);// 当前工作线程
      w.last_victim = (i + 1) % N;// 初始化窃取目标（避免自窃取）
      w.seed = i + 1;// 线程特定的随机种子
      
      // 延迟加锁的unique_lock（提高性能）
      std::unique_lock lock(_mutex, std::defer_lock);
      
      // 主工作循环
      while(!w.exit) {
        
        // === 阶段1：获取任务 ===
        // pop from my own queue // 策略1：优先从本地队列获取（LIFO，缓存友好）
        if(t = w.queue.pop(); !t) {
          // steal from others  // 策略2：本地队列空，尝试窃取其他线程任务（FIFO）
          t = _steal(i);
        }
        
        // no tasks
        // === 阶段2：处理无任务情况 ===
        // 如果上述策略都失败，进入等待状态    其实就是没有任务 自己没有任务和steal也没有获取到任务
        if(!t) {// 无任务
          if(lock.try_lock()) {  // avoid contention  // 非阻塞尝试获取锁，避免锁竞争
            if(_queue.empty()) {  // 检查全局队列（需要锁保护）
              w.ready = false;    // 标记为空闲，加入空闲队列
              _idlers.push_back(&w);

              while(!w.ready && !w.exit) { // 等待新任务或终止信号
                w.cv.wait(lock);
              }

            }
            lock.unlock(); // 释放锁，让其他线程可以修改空闲队列
          }

          if(w.cache) {// 检查是否被分配了缓存任务
            std::swap(t, w.cache);// 获取缓存任务
          }
        }
        
        // === 阶段3：执行任务 ===
        // 执行当前任务和可能的缓存任务（DFS优化）
        while(t) {
          
          (*t)();     // 执行任务

          // 检查是否有新的缓存任务（DFS策略）
          if(w.cache) {
            t = std::move(w.cache);
            w.cache = std::nullopt;
          }
          else {
            t = std::nullopt;// 准备获取下一个任务
          }

        }

        // balance load
        // === 阶段4：负载均衡 ===
        // 执行完任务后检查是否需要负载均衡
        _balance_load(i);

      } // End of while ------------------------------------------------------ 

    });     
    
    // 记录线程ID到工作线程索引的映射
    _worker_maps.insert({_threads.back().get_id(), i});
  }
}

// Function: is_owner
template <typename Closure>
bool WorkStealingThreadpool<Closure>::is_owner() const {
  return std::this_thread::get_id() == _owner;
}

// Function: num_workers
template <typename Closure>
size_t WorkStealingThreadpool<Closure>::num_workers() const { 
  return _threads.size();  
}

// Procedure: _balance_load
template <typename Closure>
void WorkStealingThreadpool<Closure>::_balance_load(unsigned me) {

  auto n = _workers[me].queue.size();

  // return if no idler - this might not be the right value
  // but it doesn't affect the correctness
  if(_idlers.empty() || n <= 1) {
    return;
  }
  
  // try with probability 1/n
  //if(_fast_modulo(_randomize(_workers[me].seed), n) == 0u) {
    // wake up my partner to help balance
    if(_mutex.try_lock()) {
      if(!_idlers.empty()) {
        Worker* w = _idlers.back();
        _idlers.pop_back();
        w->ready = true;
        w->cache = _workers[me].queue.pop();
        w->cv.notify_one();
        w->last_victim = me;
      }
      _mutex.unlock();
    }
  //}
}

// Function: _steal
template <typename Closure>
std::optional<Closure> WorkStealingThreadpool<Closure>::_steal(unsigned thief) {

  std::optional<Closure> task;
  
  for(int round=0; round<1024; ++round) {

    // try getting a task from the centralized queue
    if(task = _queue.steal(); task) {
      return task;
    }

    // try stealing a task from other workers
    unsigned victim = _workers[thief].last_victim;

    for(unsigned i=0; i<_workers.size(); i++){

      if(victim != thief) {
        if(task = _workers[victim].queue.steal(); task){
          _workers[thief].last_victim = victim;
          return task;
        }
      }

      if(++victim; victim == _workers.size()){
        victim = 0;
      }
    }

    // nothing happens this round
    std::this_thread::yield();
  }
  
  return std::nullopt; 
}

// Procedure: emplace
template <typename Closure>
template <typename... ArgsT>
void WorkStealingThreadpool<Closure>::emplace(ArgsT&&... args){

  //no worker thread available
  if(num_workers() == 0){
    Closure{std::forward<ArgsT>(args)...}();
    return;
  }

  // caller is not the owner
  if(auto tid = std::this_thread::get_id(); tid != _owner){

    // the caller is the worker of the threadpool
    if(auto itr = _worker_maps.find(tid); itr != _worker_maps.end()){

      unsigned me = itr->second;

      // dfs speculation
      if(!_workers[me].cache){
        _workers[me].cache.emplace(std::forward<ArgsT>(args)...);
      }
      // bfs load balancing
      else {
        _workers[me].queue.push(Closure{std::forward<ArgsT>(args)...});
      }
      return;
    }
  }

  std::scoped_lock lock(_mutex);
  
  if(_idlers.empty()){
    _queue.push(Closure{std::forward<ArgsT>(args)...});
  } 
  else{
    Worker* w = _idlers.back();
    _idlers.pop_back();
    w->ready = true;
    w->cache.emplace(std::forward<ArgsT>(args)...);
    w->cv.notify_one();   
  }
}

// Procedure: batch
template <typename Closure>
void WorkStealingThreadpool<Closure>::batch(std::vector<Closure>&& tasks) {

  if(tasks.empty()) {
    return;
  }

  //no worker thread available
  if(num_workers() == 0){
    for(auto &t: tasks){
      t();
    }
    return;
  }
  
  // caller is not the owner
  if(auto tid = std::this_thread::get_id(); tid != _owner){

    // the caller is the worker of the threadpool
    if(auto itr = _worker_maps.find(tid); itr != _worker_maps.end()){

      unsigned me = itr->second;

      size_t i = 0;

      if(!_workers[me].cache) {
        _workers[me].cache = std::move(tasks[i++]);
      }

      for(; i<tasks.size(); ++i) {
        _workers[me].queue.push(std::move(tasks[i]));
      }

      return;
    }
  }

  std::scoped_lock lock(_mutex);

  size_t N = std::min(tasks.size(), _idlers.size());

  for(size_t k=N; k<tasks.size(); ++k) {
    _queue.push(std::move(tasks[k]));
  }

  for(size_t i=0; i<N; ++i) {
    Worker* w = _idlers.back();
    _idlers.pop_back();
    w->ready = true;
    w->cache = std::move(tasks[i]);
    w->cv.notify_one();   
  }
} 

};  // end of namespace tf. ---------------------------------------------------



// 测试用例1：基本功能测试
void test_basic_functionality() {
    std::cout << "=== 测试1：基本功能测试 ===" << std::endl;
    
    // 创建4个工作线程的线程池
    tf::WorkStealingThreadpool<std::function<void()>> pool(4);
    
    std::atomic<int> counter{0};
    std::atomic<int> completed{0};
    
    // 提交10个任务
    for (int i = 0; i < 10; ++i) {
        pool.emplace([i, &counter, &completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            counter.fetch_add(1, std::memory_order_relaxed);
            completed.fetch_add(1, std::memory_order_relaxed);
            std::cout << "任务 " << i << " 在线程 " << std::this_thread::get_id() 
                      << " 执行完成, 当前计数: " << counter.load() << std::endl;
        });
    }
    
    // 等待所有任务完成
    while (completed.load() < 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 正确：指定毫秒
    }
    
    std::cout << "最终计数: " << counter.load() << std::endl;
    std::cout << "测试1完成\n\n";
}

// 测试用例2：工作窃取机制测试
void test_work_stealing() {
    std::cout << "=== 测试2：工作窃取机制测试 ===" << std::endl;
    
    tf::WorkStealingThreadpool<std::function<void()>> pool(4);
    std::atomic<int> task_count{0};
    
    // 模拟不均匀的任务分布
    // 前3个线程获得大量任务，最后一个线程任务很少
    for (int i = 0; i < 50; ++i) {
        pool.emplace([i, &task_count]() {
            // 模拟计算密集型任务
            volatile int result = 0;
            for (int j = 0; j < 100000; ++j) {
                result += j * j;
            }
            task_count.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    // 等待所有任务完成
    while (task_count.load() < 50) {
        std::this_thread::sleep_for(50ms);
        std::cout << "已完成任务: " << task_count.load() << "/50\r" << std::flush;
    }
    
    std::cout << "\n工作窃取测试完成\n\n";
}

// 测试用例3：批量任务提交
void test_batch_submission() {
    std::cout << "=== 测试3：批量任务提交测试 ===" << std::endl;
    
    tf::WorkStealingThreadpool<std::function<void()>> pool(4);
    std::atomic<int> completed{0};
    
    // 创建批量任务
    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < 20; ++i) {
        tasks.emplace_back([i, &completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            completed.fetch_add(1, std::memory_order_relaxed);
            std::cout << "批量任务 " << i << " 完成" << std::endl;
        });
    }
    
    // 批量提交
    pool.batch(std::move(tasks));
    
    // 等待完成
    while (completed.load() < 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 正确：指定毫秒
    }
    
    std::cout << "批量提交测试完成\n\n";
}

// 测试用例4：性能对比测试
void test_performance_comparison() {
    std::cout << "=== 测试4：性能对比测试 ===" << std::endl;
    
    const int NUM_TASKS = 1000;
    const int TASK_COMPLEXITY = 10000;
    
    // 测试1：使用工作窃取线程池
    auto start1 = std::chrono::high_resolution_clock::now();
    {
        tf::WorkStealingThreadpool<std::function<void()>> pool(4);
        std::atomic<int> counter{0};
        
        for (int i = 0; i < NUM_TASKS; ++i) {
            pool.emplace([&counter, TASK_COMPLEXITY]() {
                volatile int result = 0;
                for (int j = 0; j < TASK_COMPLEXITY; ++j) {
                    result += j * j;
                }
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
        
        while (counter.load() < NUM_TASKS) {
            std::this_thread::yield();
        }
    }
    auto end1 = std::chrono::high_resolution_clock::now();
    
    // 测试2：使用标准线程（无工作窃取）
    auto start2 = std::chrono::high_resolution_clock::now();
    {
        std::vector<std::thread> threads;
        std::atomic<int> counter{0};
        const int THREADS = 4;
        const int TASKS_PER_THREAD = NUM_TASKS / THREADS;
        
        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([t, TASKS_PER_THREAD, &counter, TASK_COMPLEXITY]() {
                for (int i = 0; i < TASKS_PER_THREAD; ++i) {
                    volatile int result = 0;
                    for (int j = 0; j < TASK_COMPLEXITY; ++j) {
                        result += j * j;
                    }
                    counter.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        
        for (auto& t : threads) {
            t.join();
        }
    }
    auto end2 = std::chrono::high_resolution_clock::now();
    
    auto duration1 = std::chrono::duration_cast<std::chrono::milliseconds>(end1 - start1);
    auto duration2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2);
    
    std::cout << "工作窃取线程池耗时: " << duration1.count() << "ms" << std::endl;
    std::cout << "标准线程池耗时: " << duration2.count() << "ms" << std::endl;
    std::cout << "性能提升: " << (100.0 * (duration2.count() - duration1.count()) / duration2.count()) << "%\n\n";
}

// 测试用例5：负载均衡测试
void test_load_balancing() {
    std::cout << "=== 测试5：负载均衡测试 ===" << std::endl;
    
    tf::WorkStealingThreadpool<std::function<void()>> pool(4);
    std::vector<std::atomic<int>> thread_task_count(4);
    
    // 初始化原子计数器
    for (auto& counter : thread_task_count) {
        counter.store(0, std::memory_order_relaxed);
    }
    
    // 提交混合复杂度的任务
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> complexity_dist(1000, 100000);
    
    const int NUM_TASKS = 100;
    std::atomic<int> completed{0};
    
    for (int i = 0; i < NUM_TASKS; ++i) {
        int complexity = complexity_dist(gen);
        pool.emplace([complexity, &completed]() {
            volatile int result = 0;
            for (int j = 0; j < complexity; ++j) {
                result += j * j;
            }
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    // 等待完成
    while (completed.load() < NUM_TASKS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 正确：指定毫秒
    }
    
    std::cout << "负载均衡测试完成（任务复杂度随机分布）\n\n";
}

// 测试用例6：异常处理测试
void test_exception_handling() {
    std::cout << "=== 测试6：异常处理测试 ===" << std::endl;
    
    tf::WorkStealingThreadpool<std::function<void()>> pool(2);
    std::atomic<int> normal_tasks{0};
    std::atomic<int> exception_tasks{0};
    
    // 正常任务
    for (int i = 0; i < 5; ++i) {
        pool.emplace([i, &normal_tasks]() {
            std::cout << "正常任务 " << i << " 执行" << std::endl;
            normal_tasks.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    // 抛出异常的任务
    for (int i = 0; i < 3; ++i) {
        pool.emplace([i, &exception_tasks]() {
            std::cout << "异常任务 " << i << " 即将抛出异常" << std::endl;
            exception_tasks.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("测试异常");
        });
    }
    
    // 更多正常任务（测试异常是否影响后续任务）
    for (int i = 5; i < 10; ++i) {
        pool.emplace([i, &normal_tasks]() {
            std::cout << "后续正常任务 " << i << " 执行" << std::endl;
            normal_tasks.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    // 等待一段时间让任务执行
    std::this_thread::sleep_for(2s);
    
    std::cout << "正常任务完成数: " << normal_tasks.load() << std::endl;
    std::cout << "异常任务完成数: " << exception_tasks.load() << std::endl;
    std::cout << "异常处理测试完成\n\n";
}

// 测试用例7：零工作线程测试
void test_zero_workers() {
    std::cout << "=== 测试7：零工作线程测试 ===" << std::endl;
    
    // 创建0个工作线程的线程池（任务在当前线程执行）
    tf::WorkStealingThreadpool<std::function<void()> > pool(0);
    
    std::atomic<int> counter{0};
    
    for (int i = 0; i < 5; ++i) {
        pool.emplace([i, &counter]() {
            std::cout << "任务 " << i << " 在当前线程执行" << std::endl;
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    std::cout << "零工作线程测试完成，计数器: " << counter.load() << "\n\n";
}

// 测试用例8：内存使用测试
void test_memory_usage() {
    std::cout << "=== 测试8：内存使用测试 ===" << std::endl;
    
    const int NUM_TASKS = 10000;
    
    auto start_mem = std::chrono::high_resolution_clock::now();
    
    {
        tf::WorkStealingThreadpool<std::function<void()>> pool(4);
        std::atomic<int> counter{0};
        
        // 提交大量小任务测试内存管理
        for (int i = 0; i < NUM_TASKS; ++i) {
            pool.emplace([i, &counter]() {
                // 极小任务，测试队列内存管理
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
        
        while (counter.load() < NUM_TASKS) {
            std::this_thread::yield();
        }
    }
    
    auto end_mem = std::chrono::high_resolution_clock::now();
    auto duration_mem = std::chrono::duration_cast<std::chrono::milliseconds>(end_mem - start_mem);
    
    std::cout << "处理 " << NUM_TASKS << " 个任务耗时: " << duration_mem.count() << "ms" << std::endl;
    std::cout << "平均每个任务: " << (duration_mem.count() * 1000.0 / NUM_TASKS) << "微秒\n\n";
}

// 主测试函数
int main() {
    std::cout << "开始测试无锁工作窃取线程池\n";
    std::cout << "============================\n\n";
    
    try {
        test_basic_functionality();
        test_work_stealing();
        test_batch_submission();
        test_performance_comparison();
        test_load_balancing();
        test_exception_handling();
        test_zero_workers();
        test_memory_usage();
        
        std::cout << "所有测试完成！\n";
    }
    catch (const std::exception& e) {
        std::cerr << "测试过程中发生异常: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}



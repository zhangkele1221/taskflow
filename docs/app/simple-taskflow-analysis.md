# `example/simple.cpp` 运行过程深入解析

本文档分步骤拆解 Taskflow 示例 `example/simple.cpp` 的执行路径，帮助你从源代码到运行时调度全面理解其行为。

## 1. 项目背景与依赖

- 项目根：`/workspaces/taskflow`
- 示例文件：`example/simple.cpp`
- 头文件依赖：`#include <taskflow/taskflow.hpp>`
  - 该头会拉入 Taskflow 框架核心实现，包括图构建、调度器和执行器。

示例代码在 `example/simple.cpp:6-26` 定义 `main` 函数，展示四个任务节点的依赖关系：

```
TaskA ----> TaskB ----> TaskD
   \                      ^
    \----> TaskC --------/
```

## 2. 构建 Taskflow 图

### 2.1 `tf::Taskflow` 对象创建

- 先看类定义：`template <template <typename...> typename E> class BasicTaskflow : public FlowBuilder`（`taskflow/graph/basic_taskflow.hpp:18-40`）。
  - `template <template <typename...> typename E>` 是“模板模板参数”的写法，表示 `E` 本身是一个模板，且满足 `E<Closure>` 的形式。Taskflow 用它来接收各种线程池实现（如 `WorkStealingThreadpool`、`SimpleThreadpool` 等），只要这些模板以若干类型参数（这里是 `Closure`）实例化即可。
  - 类继承 `FlowBuilder`，让 `BasicTaskflow` 直接拥有建图能力（例如 `emplace`、`silent_emplace`、`precede` 等）。
- `tf::Taskflow tf;` (`example/simple.cpp:10`)
  - `tf::Taskflow` 是对 `tf::BasicTaskflow<tf::WorkStealingThreadpool>` 的别名（`taskflow/taskflow.hpp:7-11`）。
  - 默认构造函数位于 `taskflow/graph/basic_taskflow.hpp:270-275`，核心动作按顺序完成：
    1. `_graph` 默认构造为 `tf::Graph`（即 `std::list<Node>`，`taskflow/graph/graph.hpp:14-36`），作为任务节点的存储容器。
    2. 基类 `FlowBuilder` 以 `_graph` 为参数构造（`taskflow/graph/flow_builder.hpp:286-289`），使得所有建图 API 都操作这份链表。
    3. `_executor` 通过 `std::make_shared<Executor>(std::thread::hardware_concurrency())` 创建默认的工作窃取线程池。其中 `Executor` 为 `WorkStealingThreadpool<Closure>`，`Closure` 是封装节点执行逻辑的可调用对象。
       - `WorkStealingThreadpool` 构造函数会立刻调用 `_spawn(N)` 创建 `N` 个工作线程（`taskflow/threadpool/workstealing_threadpool.hpp:308-369`），每个线程维护一个本地队列并具备窃取能力，支持后续的 `_executor->emplace` 和 `_executor->batch` 调度请求。
       - 若硬件并发数返回 0，C++ 标准允许返回 0 表示未知，此时线程池仍会按 0 初始化；实际运行时通常提供非零值，如需固定线程数可改用带 `N` 参数的构造。
    4. `_topologies` 默认初始化为空的 `std::forward_list<Topology>`，用于保存 dispatch 后的拓扑及其 future。
  - 默认 `WorkStealingThreadpool` 会为每个 worker 准备一个 `WorkStealingQueue<Closure>`（以及线程池级别的 `_queue`），这些队列在无参构造时会隐式使用默认实参 `4096` 作为容量上限（`taskflow/threadpool/workstealing_threadpool.hpp:168-220`）。构造函数将 `_top`、`_bottom` 设为 0，分配长度 4096 的循环数组，并预留 `_garbage` 容器空间，确保后续 push/resize 操作线程安全且高效。
  - 额外的构造重载：
    - `BasicTaskflow(unsigned N)`：显式指定线程池大小（`taskflow/graph/basic_taskflow.hpp:278-282`），仍会经过步骤 1、2、4，只是将 `N` 传给执行器。
    - `BasicTaskflow(std::shared_ptr<Executor> e)`：重用外部提供的执行器（`taskflow/graph/basic_taskflow.hpp:284-294`），用于多个 Taskflow 共享线程池；若传入空指针会抛出 `tf::Error`。
  - 核心成员：
    - `_graph`：存储当前构建中的任务图。
    - `_executor`：负责任务调度与线程管理。
    - `_topologies`：记录已提交执行的拓扑，便于阻塞等待和清理。

### 2.2 `silent_emplace` 创建任务节点

- 结构化绑定：`auto [A, B, C, D] = tf.silent_emplace(...);` (`example/simple.cpp:12-17`)
- `tf::FlowBuilder::silent_emplace` 定义见 `taskflow/graph/flow_builder.hpp:854-870`：
  - 对于静态任务（普通 lambda），直接在 `_graph` 末尾 `emplace_back` 一个 `Node`，其 `_work` 存储该 lambda。
  - 返回 `tf::Task` 句柄（`taskflow/graph/task.hpp:180-199`），封装对节点的访问。
- 调用链具体顺序如下：
  1. `tf::Taskflow` 继承自 `FlowBuilder`（`taskflow/graph/basic_taskflow.hpp:18-41`），因此 `tf.silent_emplace` 直接分派到 `FlowBuilder::silent_emplace`。
  2. 模板函数先用 `std::is_invocable_v<C, SubflowBuilder&>` 判定是否为动态任务；本示例为静态 lambda，命中 `std::is_invocable_v<C>` 分支。
  3. `_graph.emplace_back(std::forward<C>(c))` 构造新的 `Node`，调用 `Node::Node(C&&)` 将 lambda 放入 `_work`（`taskflow/graph/graph.hpp:40-112`），同时初始化依赖计数与拓扑指针。
  4. 返回 `Task` 对象包裹该节点指针，为后续 `precede` 等操作提供句柄（`taskflow/graph/task.hpp:180-243`）。
  5. 若调用了可变参数版本（`silent_emplace(C&&... callables)`），则通过递归展开将每个 functor 依次执行上述 1-4 步骤，并最终打包成 `std::tuple<Task...>` 返回（`taskflow/graph/flow_builder.hpp:322-326`）。

四个 lambda 的行为：

- `TaskA`：打印 `"TaskA\n"`
- `TaskB`：打印 `"TaskB\n"`
- `TaskC`：打印 `"TaskC\n"`
- `TaskD`：打印 `"TaskD\n"`

### 2.3 设置依赖关系

- `A.precede(B);` / `A.precede(C);` / `B.precede(D);` / `C.precede(D);` (`example/simple.cpp:19-22`)
- `tf::Task::precede` 定义在 `taskflow/graph/task.hpp:203-243`，效果：
  - 将当前节点 `_node` 与目标节点 `*(tgts._node)` 建立有向边。
  - 更新目标节点的 `_dependents` 计数（入度）以反映依赖数量。

最终形成：`A` 为唯一源节点，`D` 的 `_dependents` 为 2，需要等待 `B` 和 `C`。

## 3. 提交与执行任务图

### 3.1 `wait_for_all` 的流程

- 调用：`tf.wait_for_all();` (`example/simple.cpp:24`)
- 实现：`taskflow/graph/basic_taskflow.hpp:387-394`
  1. 若 `_graph` 非空，先调用 `silent_dispatch()`。
  2. 等待所有拓扑执行完毕 `wait_for_topologies()`。
- `wait_for_topologies()` (`taskflow/graph/basic_taskflow.hpp:397-403`) 逐个 `get()` 已提交拓扑的 `std::shared_future<void>`，阻塞直到完成。

### 3.2 `silent_dispatch` 如何启动任务

- 定义：`taskflow/graph/basic_taskflow.hpp:329-335`
  1. 将当前 `_graph` 移动到 `_topologies` 前端，构造 `Topology` 对象，记录起始节点。
  2. 调用 `_schedule(topology._sources)`，批量调度所有源节点。
- 在本示例中：
  - `_sources` 只包含 `A` 节点（唯一入度为零的节点）。

### 3.3 `_schedule` 与线程池协同

- `_schedule(Node&)`（`taskflow/graph/basic_taskflow.hpp:405-411`）：
  - 将 `Node` 包装成 `Closure` 丢给执行器 `_executor->emplace(*this, node);`。
- `Closure`（`taskflow/graph/basic_taskflow.hpp:175-262`）：
  - 保存指向 `taskflow` 与 `node` 的指针。
  - 重载 `operator()` 是执行节点的核心逻辑。

## 4. 节点执行细节

### 4.1 `Closure::operator()` 执行流程

- 关键位置：`taskflow/graph/basic_taskflow.hpp:181-262`
- 执行步骤：
  1. 记录 `num_successors = node->num_successors()`，避免拓扑清理导致失效。
  2. 根据 `_work` 类型区分：
     - 静态任务（本示例）：
       - `std::get<StaticWork>(node->_work)` 返回 lambda 并通过 `std::invoke` 调用。
       - 即打印对应字符串。
     - 动态任务（带子图）：
       - 需要构建并调度子图，本示例未涉及。
  3. 遍历所有后继 `node->_successors[i]`：
     - `--(successor->_dependents)` 减少依赖计数。
     - 若计数归零，则 `_schedule(*successor)`，表示可以执行后继任务。
  
```
对  A.precede(B);         
   A.precede(C);    
   B.precede(D);         
   C.precede(D); 
分析 
• 在 example/simple.cpp:12-24 中，silent_emplace 返回的 A/B/C/D 是 tf::Task 句柄，它们内部都握有一个
  Node*，后续的 precede 调用只是在构建任务图而不会立即执行任务。

  - A.precede(B) (example/simple.cpp:19) 落到可变参模板 Task::precede(Ts&&…) (taskflow/graph/
    task.hpp:205)，其中的折叠表达式会调用一次 A._node->precede(*B._node)，把 B 作为 A 的后继。
  - Node::precede (taskflow/graph/graph.hpp:97) 负责真正修改图结构：把目标节点指针推入调用者的
    _successors 向量，并将目标节点的 _dependents 计数原子加一。这意味着 B 需要等待一个前驱完成。
  - A.precede(C) (example/simple.cpp:20) 重复同样的流程，使得 C 也成为 A 的后继，因此 C 的
    _dependents 变为 1。
  - B.precede(D) 与 C.precede(D) (example/simple.cpp:21-22) 先后让 D 被登记为 B、C 的后继，所以 D 的
    _dependents 累加到 2，只有在两个前驱都执行完后才会被调度。
  - 这些 precede 调用都返回对原任务的引用，因此可以链式书写；但在这个示例里每个调用都单独写出，最终
    形成注释所示的菱形依赖图。

  等依赖关系全部写好后，tf.wait_for_all() 会启动调度器：只有 _dependents 计数为 0 的节点会先入队执行
  （此例中先执行 A），前驱完成时调度器会把对应后继的 _dependents 减一，直到某节点的计数降到 0 才会真
  正运行，从而保证输出顺序满足所有 precede 约束。若还想继续深入，可以看调度器如何在任务完成时对这些
  计数做 fetch_sub。
```

### 4.2 示例的执行顺序

1. 线程池首先执行 `TaskA` 的 `Closure`：
   - 输出 `TaskA`
   - 将 `B`、`C` 的 `_dependents` 减至 0，分别调度它们。
2. `TaskB` 与 `TaskC` 并行：
   - 可能在不同线程同时运行，输出顺序不确定。
   - 各自完成后都会对 `TaskD` 的 `_dependents` 递减。
3. `TaskD` 只有在 `B`、`C` 都完成时才被调度：
   - 输出 `TaskD`，保证最后打印。
4. 拓扑的 `std::shared_future` 达成，`wait_for_all` 返回，程序结束。

### 4.3 输出特点

- 每个 lambda 都输出带换行符的字符串，因此常见输出：

```
TaskA
TaskB   (或 TaskC，顺序不定)
TaskC   (或 TaskB)
TaskD
```

- `TaskD` 总是最后出现；`TaskB` 与 `TaskC` 顺序反映并行调度。

## 5. 运行与验证建议

1. 生成并编译（若未配置 CMake 构建）：
   ```bash
   cmake -S . -B build
   cmake --build build
   ```
2. 运行示例：
   ```bash
   ./build/example/simple
   ```
   多运行几次观察 `TaskB` 与 `TaskC` 输出顺序，验证并行性。
3. 若想查看更多调度细节，可在 lambda 中加入 `std::this_thread::get_id()` 打印线程 ID。

## 6. 关键源码索引

| 功能 | 文件与行号 |
| --- | --- |
| 主程序 `main` | `example/simple.cpp:6-26` |
| `tf::Taskflow` 构造 | `taskflow/graph/basic_taskflow.hpp:270-288` |
| `silent_emplace` | `taskflow/graph/flow_builder.hpp:854-870` |
| `Task::precede` | `taskflow/graph/task.hpp:203-243` |
| `wait_for_all` | `taskflow/graph/basic_taskflow.hpp:387-394` |
| `silent_dispatch` | `taskflow/graph/basic_taskflow.hpp:329-335` |
| `_schedule` | `taskflow/graph/basic_taskflow.hpp:405-420` |
| `Closure::operator()` | `taskflow/graph/basic_taskflow.hpp:181-262` |

---

通过上述分析可见，Taskflow 将简单的任务依赖图映射到线程池调度中；用户只需描述依赖关系，框架便负责任务解锁、并行执行与最终同步。建议配合源码与运行日志进一步理解调度实现细节。

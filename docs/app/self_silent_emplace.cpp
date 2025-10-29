#include <iostream>
#include <vector>
#include <functional>
#include <type_traits>

// 模拟子流构建器
class SubflowBuilder {
public:
    std::vector<std::function<void()>> _graph;
    
    template<typename F>
    void emplace(F&& f) {
        _graph.emplace_back(std::forward<F>(f));
        std::cout << "Added sub-task to subflow\n";
    }
};

// 模拟任务节点
class Node {
public:
    std::function<void(SubflowBuilder&)> func;
    
    template<typename F>
    Node(F&& f) : func(std::forward<F>(f)) {std::cout<<" new Node "<<std::endl;}
    
    void execute(SubflowBuilder& subflow) {
        func(subflow);
    }
};

// 任务包装器
class Task {
public:
    Node* node;
    
    Task(Node& n) : node(&n) {std::cout<<" new task "<<std::endl;}
};

// 流构建器
class FlowBuilder {
private:
    std::vector<Node> _graph;

public:
    template<typename C>
    auto silent_emplace(C&& c) {
        // 动态任务
        if constexpr(std::is_invocable_v<C, SubflowBuilder&>) {
            auto& n = _graph.emplace_back(
                [c=std::forward<C>(c)] (SubflowBuilder& fb) {
                    // 第一次执行时才调用
                    if(fb._graph.empty()) {
                        std::cout << "Executing dynamic task\n";
                        c(fb);
                    } else {
                        std::cout << "Skipping dynamic task (already executed)\n";
                    }
                });
            return Task(n);
        }
        // 静态任务
        else if constexpr(std::is_invocable_v<C>) {
            auto& n = _graph.emplace_back(
                [c=std::forward<C>(c)] (SubflowBuilder&) {
                    std::cout << "Executing static task: ";
                    c();
                });
            return Task(n);
        }
        else {
            static_assert(std::is_invocable_v<C>, "invalid task work type");
        }
    }

    /*
    sizeof...(C)获取参数包的大小
    std::enable_if_t<condition, void>当条件为true时有效
    只有当参数个数>1时，这个模板才会被启用

    return std::make_tuple(silent_emplace(std::forward<C>(callables))...);

    (silent_emplace(std::forward<C>(callables))...对每个参数展开调用
    std::make_tuple(...)将所有结果打包成tuple
    使用结构化绑定 auto [taskA, taskB, taskC]来接收

    std::forward<C>(callables)...

    保持每个callable的值类别（左值/右值）
    避免不必要的拷贝
    */

    template <typename... C, std::enable_if_t<(sizeof...(C)>1), void>*>
    auto silent_emplace(C&&... cs) {
        return std::make_tuple(silent_emplace(std::forward<C>(cs))...);
    }
    
    void execute() {
        SubflowBuilder subflow;
        for (auto& node : _graph) {
            node.execute(subflow);
            std::cout << "执行 一个主任务结束 \n";
        }
        
        // 执行子任务
        for (auto& subtask : subflow._graph) {
            subtask();
        }
    }
};

// 测试示例
int main() {
    FlowBuilder builder;
    
    // 静态任务示例
    builder.silent_emplace([]() {
        std::cout << "Hello from static task!\n";
    });
    
    // 动态任务示例（可接收SubflowBuilder参数）
    builder.silent_emplace([](SubflowBuilder& sub) {
        std::cout << "Dynamic task adding sub-tasks...\n";
        sub.emplace([]() {
            std::cout << "Sub-task 1\n";
        });
        sub.emplace([]() {
            std::cout << "Sub-task 2\n";
        });
    });
    
    // 另一个静态任务
    
    builder.silent_emplace([]() {
        std::cout << "Another static task!\n";
    });
    
    
    std::cout << "beging  execute!\n";
    // 执行所有任务
   builder.execute();


    std::cout << "\n======================\n";

    std::cout << "\n=== 测试多参数版本（静态任务） ===\n";
    FlowBuilder builder1;
    // 多参数静态任务
    auto [taskA, taskB, taskC] = builder1.silent_emplace(
        []() { std::cout << "Multi-static task A\n"; },
        []() { std::cout << "Multi-static task B\n"; },
        []() { std::cout << "Multi-static task C\n"; }
    );

    std::cout << "\n=== 测试多参数版本（动态任务） ===\n";
    // 多参数动态任务
    auto [dynamic1, dynamic2] = builder1.silent_emplace(
        [](SubflowBuilder& sub) {
            std::cout << "Dynamic task 1\n";
            sub.emplace([]() { std::cout << "Dynamic1 sub-task\n"; });
        },
        [](SubflowBuilder& sub) {
            std::cout << "Dynamic task 2\n";
            sub.emplace([]() { std::cout << "Dynamic2 sub-task\n"; });
        }
    );


    std::cout << "\n=== 测试多参数版本（混合任务） ===\n";
    // 混合任务
    auto [static1, dynamic3, static2] = builder1.silent_emplace(
        []() { std::cout << "Static between dynamics\n"; },
        [](SubflowBuilder& sub) {
            std::cout << "Mixed dynamic task\n";
            sub.emplace([]() { std::cout << "Mixed sub-task\n"; });
        },
        []() { std::cout << "Final static task\n"; }
    );
    
    std::cout << "\n=== 测试边界情况：两个任务 ===\n";
    // 刚好两个任务的情况
    auto [taskX, taskY] = builder1.silent_emplace(
        []() { std::cout << "Task X\n"; },
        []() { std::cout << "Task Y\n"; }
    );
    
    std::cout << "\n=== 开始执行所有任务 ===\n";
    // 执行所有任务
    builder1.execute();

    
    return 0;
}
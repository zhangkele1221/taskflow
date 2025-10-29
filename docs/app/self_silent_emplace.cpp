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
    Node(F&& f) : func(std::forward<F>(f)) {}
    
    void execute(SubflowBuilder& subflow) {
        func(subflow);
    }
};

// 任务包装器
class Task {
public:
    Node* node;
    
    Task(Node& n) : node(&n) {}
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
    
    void execute() {
        SubflowBuilder subflow;
        for (auto& node : _graph) {
            node.execute(subflow);
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
    /*
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
    */
    
    // 执行所有任务
    builder.execute();
    
    return 0;
}
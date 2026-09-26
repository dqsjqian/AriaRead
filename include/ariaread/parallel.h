#pragma once
/// @file parallel.h
/// @brief 轻量并发工具：把"裸线程池 + 原子任务索引 work-stealing + join"
///        这套在 engine 层重复了 5 次的骨架收敛成一个自由函数。
///
/// 设计取舍（重要）：
///   - **不引入 Aria 依赖**。核心库 `ariaread` 刻意只链 quickjs/json/sqlite/gumbo，
///     不依赖 MVVM 框架（Aria 仅在 ViewModel/web 层使用）。engine 层是纯 C++ 核心，
///     用标准库 `std::thread` 实现 work-stealing 即可，无需把 Aria 的
///     `ThreadPoolExecutor` 下沉到核心库、破坏现有分层。
///   - **保持同步阻塞语义**。原先各处 `searchAllConcurrent`/`downloadBook`/校验/RSS
///     都是"函数返回时全部任务已完成"。本工具同样阻塞到所有任务跑完才返回，
///     调用方零改动。
///   - **消除 detach**。原 `engine_validate` 超时分支用 `detach` 兜底（潜在 UAF）。
///     统一改用取消谓词 + join，调用方通过 `cancel` 让 worker 在下个检查点优雅退出。
///
/// 取消语义：每次领取任务前、以及每个任务执行后都会检查 `cancel()`。返回 true 即停止
///           领新任务（已在执行的任务不被打断），所有 worker 随后 join 返回。

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace ariaread::detail {

/// 并发遍历 [0, count) 的索引区间。
///
/// 内部起 min(concurrency, count) 个 worker 线程，以原子计数器 work-stealing 方式
/// 领取索引，对每个 i 调用 `body(i)`。函数阻塞直到全部任务完成或被取消。
///
/// @param count        任务总数
/// @param concurrency  期望并发度（<=0 时取 1；自动收敛到 [1, count]）
/// @param body         单任务体，在 worker 线程被调用，须自身保证线程安全。
///                     `body` 抛出的异常会被吞掉（与原各处 worker 的 catch(...) 行为
///                     对齐——各处 body 内部已自行处理异常并回调，外层不应让异常
///                     逃逸出线程）。
/// @param cancel       取消谓词（可空）。返回 true 时停止领取新任务。每个 worker 在
///                     领新任务前、每个任务后各检查一次。
template <typename Body>
inline void parallelForEach(
    std::size_t count,
    int concurrency,
    Body&& body,
    std::function<bool()> cancel = nullptr) {

    if (count == 0) return;

    if (concurrency <= 0) concurrency = 1;
    std::size_t threadCount =
        std::min<std::size_t>(static_cast<std::size_t>(concurrency), count);

    std::atomic<std::size_t> next{0};

    auto worker = [&]() {
        while (true) {
            if (cancel && cancel()) break;
            std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= count) break;
            try {
                body(i);
            } catch (...) {
                // body 内部已负责异常处理/回调；外层吞掉以免线程 terminate。
            }
            if (cancel && cancel()) break;
        }
    };

    // 单线程退化：直接在当前线程跑，省一次线程创建/join。
    if (threadCount <= 1) {
        worker();
        return;
    }

    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (std::size_t t = 0; t < threadCount; ++t) {
        workers.emplace_back(worker);
    }
    for (auto& th : workers) {
        if (th.joinable()) th.join();
    }
}

/// 并发遍历容器元素的便捷重载：对 `items[i]` 调用 `body(item)`。
/// @param items 随机访问容器（vector 等）
template <typename Container, typename Body>
inline void parallelForEachItem(
    const Container& items,
    int concurrency,
    Body&& body,
    std::function<bool()> cancel = nullptr) {

    parallelForEach(
        items.size(), concurrency,
        [&](std::size_t i) { body(items[i]); },
        std::move(cancel));
}

}  // namespace ariaread::detail

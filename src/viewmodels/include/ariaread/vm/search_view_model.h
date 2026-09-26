#pragma once
/// @file search_view_model.h
/// @brief AriaRead 搜索 ViewModel —— 基于 Aria 响应式 MVVM 框架。
///
/// 流式版本：还原 AriaRead "多源并发搜索、结果慢慢显示出来" 的体验。
///   - keyword / results / found_count / is_searching / last_error_message
///     全部是 Aria 响应式状态，各端 UI 直接绑定。
///   - 用 AsyncCommand 封装"流式多源搜索"这一异步操作。
///   - 通过依赖注入（StreamSearchFn）解耦真实引擎：
///       * 生产环境注入驱动 BookSourceEngine::searchAllConcurrent 的适配器
///         （见 engine_search_adapter.h）；
///       * 单元测试注入 mock，无需真实网络/书源/线程。
///
/// 线程契约（关键）：
///   引擎逐源回调在工作线程触发，Aria 响应式图严格单线程。适配器在 worker
///   executor 上驱动搜索；每当一个源返回，ViewModel 通过 ui.post 把该批结果
///   marshal 回 ui（图）线程再 append 到 ObservableList，保证 list-change
///   信号始终在图线程触发。
///
/// 实现说明：Property / ObservableList / AsyncCommand 是模板，作为成员必须
/// 在头中声明；但所有方法体与构造逻辑都移到 search_view_model.cpp。

#include "aria/property.hpp"
#include "aria/observable_list.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/async_command.hpp"
#include "aria/binding/view_model.hpp"

#include "ariaread/types.h"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ariaread::vm {

/// 逐源结果推送回调：每当一个书源返回，就用其结果调用一次（worker 线程）。
using SearchEmit = std::function<void(const std::vector<ariaread::Book>& books)>;

/// 流式多源搜索抽象。
/// @param keyword  搜索关键词
/// @param emit     逐源结果推送（每个源命中后调用一次，可调用多次）
/// @param cancel   取消标志：实现应周期性检查，为 true 时尽快停止
/// 在 worker 线程被调用，应阻塞直到全部源完成或被取消。
using StreamSearchFn = std::function<void(const std::string& keyword,
                                          const SearchEmit& emit,
                                          const std::atomic<bool>& cancel)>;

/// 搜索 ViewModel（流式）。
class SearchViewModel : public aria::binding::ViewModel {
public:
    /// @param ui      响应式图线程 executor（Property/List 必须在其上读写）
    /// @param worker  工作线程 executor（流式搜索在其上驱动）
    /// @param search  流式搜索实现（依赖注入，便于 mock）
    SearchViewModel(aria::async::IExecutor& ui,
                    aria::async::IExecutor& worker,
                    StreamSearchFn search);

    // ── 响应式状态（各端 UI 绑定这些）──────────────────────────────
    /// 搜索关键词（双向绑定到输入框）
    aria::Property<std::string> keyword{""};

    /// 搜索结果列表（绑定到列表视图；元素为 shared_ptr<Book>，流式 append）
    aria::ObservableList<ariaread::Book> results;

    /// 已找到的结果数（随每个源返回实时递增）
    aria::Property<int> found_count{0};

    /// 是否正在搜索（绑定到 loading 指示）
    [[nodiscard]] aria::Property<bool>& is_searching();

    /// 最近一次错误信息（绑定到错误提示）
    [[nodiscard]] aria::Property<std::string>& last_error_message();

    /// 最近一次搜索完成时的总命中数（无值表示尚未搜索过）
    [[nodiscard]] aria::Property<std::optional<int>>& last_total();

    // ── 命令 ───────────────────────────────────────────────────────
    /// 用当前 keyword 触发一次搜索。
    void search();

    /// 用指定关键词触发搜索（不改 keyword）。
    void search_with(const std::string& kw);

    /// 请求取消当前搜索（流式实现会在下个检查点停止）。
    void cancel();

private:
    void start_(const std::string& kw);
    void append_results_(const std::vector<ariaread::Book>& books);

    aria::async::IExecutor& ui_;
    StreamSearchFn search_;
    std::atomic<bool> cancel_flag_{false};
    aria::async::AsyncCommand<int, std::string> search_command_;
};

}  // namespace ariaread::vm

#pragma once
/// @file bookshelf_view_model.h
/// @brief AriaRead 书架 ViewModel —— 基于 Aria 响应式 MVVM 框架。
///
/// 管理书架列表（加载 / 收藏 / 移除），各端 UI 直接绑定响应式状态。
///   - books            : ObservableList<BookshelfDetail>（书架列表）
///   - is_loading       : 是否正在加载/操作
///   - last_error_message
///
/// 后端通过 BookshelfBackend 接口注入，解耦真实引擎：
///   * 生产环境用 EngineBookshelfBackend（驱动 BookSourceEngine）；
///   * 测试用 fake，无需真实 DB。
///
/// 线程契约：加载/写操作在 worker executor 上执行，结果 marshal 回 ui（图）
/// 线程再写 ObservableList，保证 list-change 信号在图线程触发。

#include "aria/property.hpp"
#include "aria/observable_list.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/async_command.hpp"
#include "aria/binding/view_model.hpp"

#include "ariaread/types.h"

#include <optional>
#include <string>
#include <vector>

namespace ariaread::vm {

/// 书架后端抽象：ViewModel 只依赖此接口，不直接碰引擎。
/// 所有方法在 worker 线程被调用，可阻塞（DB / 网络）。
class BookshelfBackend {
public:
    virtual ~BookshelfBackend() = default;

    /// 加载书架全部条目（含缓存状态与阅读进度）。
    virtual std::vector<ariaread::BookshelfDetail> load() = 0;

    /// 移除一本书（按 bookUrl）。返回是否成功。
    virtual bool remove(const std::string& bookUrl) = 0;

    /// 加入一本书。返回新插入 id（已存在返回 -1）。
    virtual int64_t add(const ariaread::BookshelfItem& item) = 0;
};

/// 书架 ViewModel。
class BookshelfViewModel : public aria::binding::ViewModel {
public:
    /// @param ui      响应式图线程 executor
    /// @param worker  工作线程 executor
    /// @param backend 书架后端（依赖注入；调用方保证其生命周期长于本 VM）
    BookshelfViewModel(aria::async::IExecutor& ui,
                       aria::async::IExecutor& worker,
                       BookshelfBackend& backend);

    // ── 响应式状态（各端 UI 绑定这些）──────────────────────────────
    /// 书架列表（元素为 shared_ptr<BookshelfDetail>）
    aria::ObservableList<ariaread::BookshelfDetail> books;

    /// 书架本数
    aria::Property<int> count{0};

    /// 是否正在加载/操作
    [[nodiscard]] aria::Property<bool>& is_loading();

    /// 最近一次错误信息
    [[nodiscard]] aria::Property<std::string>& last_error_message();

    // ── 命令 ───────────────────────────────────────────────────────
    /// 重新加载书架。
    void refresh();

    /// 移除一本书并自动刷新。
    void remove(const std::string& bookUrl);

    /// 加入一本书并自动刷新。
    void add(const ariaread::BookshelfItem& item);

private:
    void reload_into_list_(const std::vector<ariaread::BookshelfDetail>& items);

    aria::async::IExecutor& ui_;
    BookshelfBackend& backend_;
    // 加载命令：返回加载到的本数。
    aria::async::AsyncCommand<int> load_command_;
    // 写命令（移除/加入）：完成后链式触发一次 refresh。
    aria::async::AsyncCommand<bool, std::string> remove_command_;
    aria::async::AsyncCommand<int64_t, ariaread::BookshelfItem> add_command_;
};

}  // namespace ariaread::vm

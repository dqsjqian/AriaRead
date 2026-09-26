#pragma once
/// @file source_view_model.h
/// @brief AriaRead 书源管理 ViewModel —— 列表 / 加载 / 验证 / 清理。
///
/// 对应 web_server 的 /api/sources 组（11 个路由）。
/// 响应式状态：
///   - sources        : ObservableList<SourceSummary>（书源列表）
///   - stats          : Property<SourceStats>（各状态计数）
///   - valid_count / total_count
///   - is_validating / validation_progress
///   - last_error_message
///
/// 命令：
///   - refresh()               从后端拉取最新列表
///   - load_from_json(json)     导入书源 JSON
///   - load_from_file(path)     从文件导入
///   - validate(testQuery, timeout, concurrency)  并发验证
///   - cancel_validate()        取消验证
///   - clear_all()              清空全部书源
///   - export_good()            导出优+良书源

#include "aria/property.hpp"
#include "aria/observable_list.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/async_command.hpp"
#include "aria/binding/view_model.hpp"

#include "ariaread/types.h"

#include <string>

namespace ariaread::vm {

/// 书源管理后端抽象。所有方法在 worker 线程被调用，可阻塞。
class SourceBackend {
public:
    virtual ~SourceBackend() = default;

    /// 获取书源列表摘要（过滤无效源、映射 validity、兜底 latency）。
    virtual ariaread::SourceListResult get_source_list() = 0;

    /// 从 JSON 数组字符串加载书源，返回加载数量。
    virtual int load_sources_from_json(const std::string& jsonArray) = 0;

    /// 从文件加载书源，返回加载数量。
    virtual int load_sources_from_file(const std::string& filePath) = 0;

    /// 并发验证书源。perSource 在每个源验证完时回调（worker 线程），
    /// done 在全部完成时回调。
    virtual void validate_sources(
        const std::string& testQuery, int timeoutMs, int concurrency,
        std::function<void(size_t, const std::string&, SourceValidity, int)> perSource,
        std::function<void(int, int)> done,
        std::shared_ptr<ariaread::CancelToken> cancelToken) = 0;

    /// 清空全部书源，返回清空数量。
    virtual int clear_all_sources() = 0;

    /// 导出优+良书源 JSON。
    virtual std::string export_good_sources() = 0;
};

/// 书源管理 ViewModel。
class SourceViewModel : public aria::binding::ViewModel {
public:
    SourceViewModel(aria::async::IExecutor& ui,
                   aria::async::IExecutor& worker,
                   SourceBackend& backend);

    // ── 响应式状态（UI 绑定）──────────────────────────────────────
    /// 书源列表
    aria::ObservableList<ariaread::SourceSummary> sources;
    /// 各状态计数（拆为独立 Property，便于 UI 分别绑定）
    aria::Property<int> stat_excellent{0};
    aria::Property<int> stat_good{0};
    aria::Property<int> stat_poor{0};
    aria::Property<int> stat_invalid{0};
    aria::Property<int> stat_unknown{0};
    /// 有效书源数量
    aria::Property<int> valid_count{0};
    /// 总书源数量
    aria::Property<int> total_count{0};

    /// 是否正在验证
    aria::Property<bool> is_validating{false};
    /// 验证进度（已完成数）
    aria::Property<int> validation_done_count{0};
    /// 验证总数
    aria::Property<int> validation_total_count{0};

    /// 最近一次错误
    [[nodiscard]] aria::Property<std::string>& last_error_message();

    // ── 命令 ───────────────────────────────────────────────────────
    /// 刷新书源列表（从后端拉取）。
    void refresh();

    /// 从 JSON 数组字符串导入书源。
    void load_from_json(const std::string& jsonArray);

    /// 从文件导入书源。
    void load_from_file(const std::string& filePath);

    /// 并发验证书源。
    void validate(const std::string& testQuery = "\xe6\x88\x91",
                  int timeoutMs = 10000, int concurrency = 8);

    /// 取消正在进行的验证。
    void cancel_validate();

    /// 清空全部书源。
    void clear_all();

    /// 导出优+良书源 JSON。
    void export_good();

    /// 导出结果（export_good 完成后有值）。
    aria::Property<std::string> exported_json{""};

private:
    void apply_list_(const ariaread::SourceListResult& result);

    aria::async::IExecutor& ui_;
    SourceBackend& backend_;

    // refresh: 返回源数量
    aria::async::AsyncCommand<int> refresh_cmd_;
    // load_from_json: 参数 jsonArray，返回加载数
    aria::async::AsyncCommand<int, std::string> load_json_cmd_;
    // load_from_file: 参数 filePath，返回加载数
    aria::async::AsyncCommand<int, std::string> load_file_cmd_;
    // clear_all: 返回清空数
    aria::async::AsyncCommand<int> clear_cmd_;
    // export_good: 返回 JSON
    aria::async::AsyncCommand<std::string> export_cmd_;

    // 验证取消令牌
    std::shared_ptr<ariaread::CancelToken> cancel_token_;
};

}  // namespace ariaread::vm

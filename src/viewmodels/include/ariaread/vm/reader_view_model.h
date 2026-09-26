#pragma once
/// @file reader_view_model.h
/// @brief AriaRead 阅读器 ViewModel —— 目录 / 正文 / 阅读进度。
///
/// 承载 web_server 的 /api/catalog、/api/content、/api/bookshelf/progress
/// 这组阅读核心逻辑。各端 UI 直接绑定响应式状态：
///   - chapters       : ObservableList<Chapter>（目录）
///   - current_index  : 当前章节序号
///   - content        : 当前章节正文
///   - read_percent   : 阅读百分比
///   - is_busy / last_error_message
///
/// 后端通过 ReaderBackend 接口注入，解耦真实引擎（生产用
/// EngineReaderBackend，测试用 fake）。
///
/// 线程契约：catalog/content/progress 的网络与 DB 操作在 worker executor
/// 执行，结果 marshal 回 ui（图）线程再写 Property/List。

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

/// 阅读器后端抽象。所有方法在 worker 线程被调用，可阻塞。
class ReaderBackend {
public:
    virtual ~ReaderBackend() = default;

    /// 获取目录（优先缓存，未命中则网络请求并缓存）。
    virtual std::vector<ariaread::Chapter> load_catalog(
        const std::string& bookUrl, const std::string& sourceUrl) = 0;

    /// 获取章节正文（优先缓存）。
    virtual std::string load_content(
        const std::string& bookUrl, const std::string& chapterUrl,
        int chapterIndex, const std::string& sourceUrl) = 0;

    /// 保存阅读进度。
    virtual void save_progress(const ariaread::ReadProgress& progress) = 0;

    /// 读取阅读进度。
    virtual ariaread::ReadProgress load_progress(const std::string& bookUrl) = 0;
};

/// 阅读器 ViewModel。
class ReaderViewModel : public aria::binding::ViewModel {
public:
    ReaderViewModel(aria::async::IExecutor& ui,
                    aria::async::IExecutor& worker,
                    ReaderBackend& backend);

    // ── 当前书标识（打开一本书前设置）──────────────────────────────
    aria::Property<std::string> book_url{""};
    aria::Property<std::string> source_url{""};

    // ── 响应式状态（UI 绑定）──────────────────────────────────────
    /// 目录（元素为 shared_ptr<Chapter>）
    aria::ObservableList<ariaread::Chapter> chapters;
    /// 章节总数
    aria::Property<int> chapter_count{0};
    /// 当前阅读章节序号
    aria::Property<int> current_index{0};
    /// 当前章节标题
    aria::Property<std::string> current_title{""};
    /// 当前章节正文
    aria::Property<std::string> content{""};
    /// 阅读百分比 0~1
    aria::Property<double> read_percent{0.0};

    /// 是否正在加载（目录或正文）
    [[nodiscard]] aria::Property<bool>& is_busy();
    /// 最近一次错误信息
    [[nodiscard]] aria::Property<std::string>& last_error_message();

    // ── 命令 ───────────────────────────────────────────────────────
    /// 加载当前书的目录（用 book_url / source_url）。
    void load_catalog();

    /// 打开指定序号的章节（加载正文，更新 current_index/title/content）。
    void open_chapter(int index);

    /// 保存当前阅读进度（current_index/title）。
    void save_progress();

private:
    void apply_catalog_(const std::vector<ariaread::Chapter>& chs);
    void apply_content_(int index, const std::string& title,
                        const std::string& text, double percent);

    aria::async::IExecutor& ui_;
    ReaderBackend& backend_;
    // 加载目录：返回章节数。
    aria::async::AsyncCommand<int> catalog_command_;
    // 打开章节：参数为章节序号，返回正文字节数。
    aria::async::AsyncCommand<int, int> content_command_;
    // 保存进度：无返回。
    aria::async::AsyncCommand<void> save_command_;
};

}  // namespace ariaread::vm

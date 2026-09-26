#pragma once
/// @file engine.h
/// @brief AriaRead 书源引擎 —— 核心入口

#include "ariaread/types.h"
#include "ariaread/source_parser.h"
#include "ariaread/database.h"
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>

namespace ariaread {

// ──────────────────────────────────────────────
// 书源引擎
// ──────────────────────────────────────────────
class BookSourceEngine {
public:
    BookSourceEngine();
    ~BookSourceEngine();

    // 禁止拷贝，允许移动
    BookSourceEngine(const BookSourceEngine&) = delete;
    BookSourceEngine& operator=(const BookSourceEngine&) = delete;
    BookSourceEngine(BookSourceEngine&&) noexcept;
    BookSourceEngine& operator=(BookSourceEngine&&) noexcept;

    // ──────────────────────────────────────────
    // 配置
    // ──────────────────────────────────────────

    /// 设置 HTTP 请求实现（简易版，必须在使用前设置）
    void setHttpRequest(HttpRequestFunc func);

    /// 设置 HTTP 客户端实现（完整版，含状态码和响应头）
    void setHttpClient(HttpClientFunc func);

    /// 为单源同步搜索/目录/正文设置协作式取消或期限检查。
    /// HTTP、规则执行前后调用；抛异常会终止操作，JS 执行期间也会检查。
    /// 回调应快速返回；原生 HTTP/选择器仍需自行限制执行时间。
    /// 必须在操作开始前配置，空回调清除检查；不影响并发搜索/校验 worker。
    /// 首次取消异常会保留，直到再次调用本方法重置检查。
    void setOperationCheck(std::function<void()> check);

    /// 设置 JS 日志回调（console.log 输出定向）
    void setJsLogCallback(JsLogFunc func);

    /// 设置通用日志回调（引擎内部的警告/错误信息输出）
    void setLogCallback(JsLogFunc func);

    /// 设置数据库路径（启用持久化，url 唯一键去重）
    /// @param dbPath 数据库文件路径（如 "ariaread.db"），空则禁用持久化
    void setDatabasePath(const std::string& dbPath);

    /// 从数据库加载所有书源（替代内存中的书源列表）
    /// @return 加载的书源数量
    int loadSourcesFromDatabase();

    /// 将当前内存中的书源同步到数据库（插入新源，跳过已有）
    /// @return 新插入的数量
    int syncSourcesToDatabase();

    // ──────────────────────────────────────────
    // 书源管理
    // ──────────────────────────────────────────

    /// 加载单个书源（JSON 格式）
    bool loadSource(const std::string& json);

    /// 加载书源数组
    int loadSources(const std::string& jsonArray);

    /// 从文件加载书源
    int loadSourcesFromFile(const std::string& filePath);

    /// 获取当前书源
    const BookSource& currentSource() const;

    /// 获取所有已加载书源
    const std::vector<BookSource>& sources() const;

    /// 选择当前使用的书源（按索引）
    bool selectSource(size_t index);

    /// 选择当前使用的书源（按名称）
    bool selectSourceByName(const std::string& name);

    /// 删除指定索引的书源
    bool removeSource(size_t index);

    /// 删除指定 URL 的书源
    bool removeSourceByUrl(const std::string& url);

    /// 删除所有无效书源，返回删除数量
    int removeInvalidSources();

    /// 获取有效书源数量
    int validSourceCount() const;

    /// 设置书源有效性标记（供上层在验证时调用）
    void setSourceValidity(size_t index, SourceValidity validity, int latencyMs = -1);

    /// 设置默认并发数
    void setConcurrency(int n);

    /// 设置验证单任务最大超时秒数（E2：原先硬编码 35，配置化）
    /// @param sec 单任务最长耗时，合理范围 10~120 秒
    void setValidateMaxTaskSec(int sec);

    /// 预热 JsRuntime 对象池（在后台线程中调用，加速首次验证）
    void warmUpJsPool(int count = 8);

    // ──────────────────────────────────────────
    // 书源验证
    // ──────────────────────────────────────────

    /// 验证所有书源有效性（逐源回调通知，串行版本）
    /// @param testQuery  测试关键词（默认"我"）
    /// @param timeoutMs  单源超时毫秒（默认 10000）
    /// @param callback   逐源验证结果回调
    /// @return (validCount, invalidCount)
    std::pair<int, int> validateSources(
        const std::string& testQuery = "\xe6\x88\x91",  // "我" UTF-8
        int timeoutMs = 10000,
        SourceValidateCallback callback = nullptr
    );

    /// 并发验证所有书源有效性（线程池并发）
    /// 无效的自动删除，有效的持久化到数据库
    /// @param testQuery    测试关键词
    /// @param timeoutMs    单源超时毫秒
    /// @param concurrency  并发线程数（默认 8）
    /// @param callback     逐源验证结果回调（线程安全）
    /// @param doneCallback 全部完成回调
    /// @param cancelToken  取消令牌（可选）
    void validateSourcesConcurrent(
        const std::string& testQuery = "\xe6\x88\x91",
        int timeoutMs = 10000,
        int concurrency = 8,
        ConcurrentValidateCallback callback = nullptr,
        ConcurrentDoneCallback doneCallback = nullptr,
        std::shared_ptr<CancelToken> cancelToken = nullptr
    );

    // ──────────────────────────────────────────
    // 核心操作
    // ──────────────────────────────────────────

    /// 搜索书籍（单源）
    /// @param keyword     搜索关键词
    /// @param matchName   是否按书名匹配过滤（默认 false 表示不过滤，返回所有结果）
    /// @param matchAuthor 是否按作者匹配过滤
    /// @param matchIntro  是否按简介匹配过滤
    /// @note 当 matchName/matchAuthor/matchIntro 全为 false 时，不做任何过滤（兼容旧行为）
    std::vector<Book> search(const std::string& keyword,
                             bool matchName = false,
                             bool matchAuthor = false,
                             bool matchIntro = false);

    /// 搜索所有有效书源（逐源回调推送结果，串行版本）
    /// @param keyword   搜索关键词
    /// @param callback  逐源结果回调
    /// @return (totalBooks, totalSources, totalErrors)
    struct SearchAllResult {
        int totalBooks = 0;
        int totalSources = 0;
        int totalErrors = 0;
    };
    SearchAllResult searchAll(const std::string& keyword, SearchAllCallback callback = nullptr);

    /// 并发搜索所有有效书源（线程池并发）
    /// 结果通过回调逐源推送，实现"慢慢显示出来"的效果
    /// @param keyword     搜索关键词
    /// @param concurrency 并发线程数（默认 8）
    /// @param callback    逐源结果回调（线程安全）
    /// @param doneCallback 全部完成回调
    /// @param cancelToken  取消令牌（可选）
    void searchAllConcurrent(
        const std::string& keyword,
        int concurrency = 8,
        ConcurrentSearchCallback callback = nullptr,
        ConcurrentDoneCallback doneCallback = nullptr,
        std::shared_ptr<CancelToken> cancelToken = nullptr,
        bool matchName = true,
        bool matchAuthor = false,
        bool matchIntro = false,
        const std::vector<std::string>& sourceNames = {}
    );

    /// 获取书籍目录
    std::vector<Chapter> getCatalog(const std::string& bookUrl);

    /// 按指定书源获取目录（无状态，不污染当前选中书源）
    std::vector<Chapter> getCatalogForSource(
        const std::string& bookUrl,
        int sourceIndex,
        const std::string& sourceName = ""
    );

    /// 获取目录（自动缓存：优先读缓存，未命中则网络请求并自动缓存）
    /// @param bookUrl    书籍URL
    /// @param sourceUrl  书源URL（用于缓存键，空则不缓存）
    /// @param sourceIndex 书源索引
    /// @param sourceName  书源名称
    /// @return 章节列表
    std::vector<Chapter> getCatalogWithCache(
        const std::string& bookUrl,
        const std::string& sourceUrl,
        int sourceIndex = -1,
        const std::string& sourceName = ""
    );

    /// 刷新目录（纯网络请求 + 防退化保护写入缓存 + 更新书架表）
    /// 如果网络返回的目录退化严重，则拒绝覆盖缓存，返回旧缓存目录
    /// @param bookUrl     书籍URL
    /// @param sourceUrl   书源URL
    /// @param sourceIndex 书源索引
    /// @param sourceName  书源名称
    /// @return 最终应该展示的章节列表（可能是新目录，也可能是旧缓存）
    std::vector<Chapter> refreshCatalog(
        const std::string& bookUrl,
        const std::string& sourceUrl,
        int sourceIndex = -1,
        const std::string& sourceName = ""
    );

    /// 获取章节正文
    std::string getContent(const std::string& chapterUrl);

    /// 按指定书源获取正文（无状态，不污染当前选中书源）
    std::string getContentForSource(
        const std::string& chapterUrl,
        int sourceIndex,
        const std::string& sourceName = ""
    );

    /// RSS 源正文提取（使用 rss_sources 表的 ruleContent）
    std::string getContentForRssSource(
        const std::string& chapterUrl,
        const RssSource& rssSrc
    );

    /// 获取正文（自动缓存：优先读缓存，未命中则网络请求并自动缓存）
    /// @param chapterUrl  章节URL
    /// @param bookUrl     书籍 URL（用于缓存关联）
    /// @param chapterIndex 章节序号（用于缓存关联，-1 表示不缓存）
    /// @param sourceUrl   书源URL（用于缓存关联）
    /// @param sourceIndex  书源索引
    /// @param sourceName   书源名称
    /// @return 正文内容
    std::string getContentWithCache(
        const std::string& chapterUrl,
        const std::string& bookUrl = "",
        int chapterIndex = -1,
        const std::string& sourceUrl = "",
        int sourceIndex = -1,
        const std::string& sourceName = ""
    );
    /// 清空全部书源（内存+数据库）
    int clearAllSources();

    /// 发现/推荐（如果书源支持）
    std::vector<Book> explore(const std::string& exploreUrl);

    // ──────────────────────────────────────────
    // 书架管理
    // ──────────────────────────────────────────

    /// 添加书籍到书架
    /// @param item 书架条目（bookUrl + sourceUrl 唯一）
    /// @return 新插入的 id，已存在返回 -1
    int64_t addToBookshelf(const BookshelfItem& item);

    /// 从书架移除书籍（CASCADE 自动删除关联目录/正文/进度）
    bool removeFromBookshelf(const std::string& bookUrl);

    /// 获取书架列表（按最后阅读时间倒序）
    std::vector<BookshelfItem> getBookshelf();

    /// 获取书架列表（含缓存状态和阅读进度，一次性查询，避免 N+1 问题）
    std::vector<BookshelfDetail> getBookshelfWithDetails();

    /// 检查书籍是否在书架中
    bool isInBookshelf(const std::string& bookUrl);

    /// 保存阅读进度
    void saveReadProgress(const ReadProgress& progress);

    /// 获取阅读进度
    ReadProgress getReadProgress(const std::string& bookUrl);

    /// 换源：更新书架条目的书源信息
    void changeBookSource(const std::string& bookUrl, const std::string& oldSourceUrl,
                           const std::string& newSourceName, const std::string& newSourceUrl,
                           const std::string& newBookUrl);

    /// 检查单本书的更新（对比最新章节）
    /// @return true 表示有更新
    bool checkBookUpdate(const std::string& bookUrl, const std::string& sourceUrl,
                          int sourceIndex = -1, const std::string& sourceName = "");

    /// 更新书架条目的最新章节信息（用于下载完成后重置 hasUpdate 等）
    void updateBookshelfLastChapter(const std::string& bookUrl, const std::string& sourceUrl,
                                     const std::string& lastChapter, int totalChapters, bool hasUpdate);

    // ──────────────────────────────────────────
    // 单本书状态管理（per-book state）
    // ──────────────────────────────────────────

    /// 打开一本书并返回当前状态快照。
    /// 会同步装载缓存统计、阅读进度，并在需要时触发后台目录刷新。
    BookReadingState openBookSession(const std::string& bookUrl,
                                     const std::string& sourceUrl,
                                     int sourceIndex = -1,
                                     const std::string& sourceName = "",
                                     const std::string& kind = "");

    /// 获取某本书当前的状态快照；若不存在则返回默认空状态。
    BookReadingState getBookReadingState(const std::string& bookUrl,
                                         const std::string& sourceUrl) const;

    /// 获取当前引擎中所有活跃书本状态快照。
    std::vector<BookReadingState> listBookReadingStates() const;

    /// 手动触发某本书的后台目录刷新。
    /// @return true 表示已成功入队或已在刷新中。
    bool requestBookRefresh(const std::string& bookUrl,
                            const std::string& sourceUrl,
                            int sourceIndex = -1,
                            const std::string& sourceName = "",
                            const std::string& kind = "");

    // ──────────────────────────────────────────
    // 缓存管理（目录 + 正文）
    // ──────────────────────────────────────────

    /// 缓存书籍目录到数据库（全量替换）
    void cacheBookCatalog(const std::string& bookUrl, const std::string& sourceUrl,
                           const std::vector<Chapter>& chapters);

    /// 获取缓存的目录（空则无缓存）
    std::vector<Chapter> getCachedCatalog(const std::string& bookUrl, const std::string& sourceUrl);

    /// 获取缓存的目录章节数
    int getCachedCatalogCount(const std::string& bookUrl, const std::string& sourceUrl = "");

    /// 缓存章节正文
    void cacheChapterContent(const std::string& chapterUrl, const std::string& bookUrl,
                              const std::string& sourceUrl, const std::string& content);

    /// 获取缓存的正文（以 bookUrl + chapterIndex 查询）
    std::string getCachedContent(const std::string& bookUrl, int chapterIndex);

    /// 获取某本书已缓存的正文章节数
    int getCachedContentCount(const std::string& bookUrl, const std::string& sourceUrl = "");

    /// 清除某本书的所有缓存（目录 + 正文）
    void clearBookCache(const std::string& bookUrl, const std::string& sourceUrl = "");

    // ──────────────────────────────────────────
    // 高级操作（编排逻辑下沉）
    // ──────────────────────────────────────────

    /// 下载进度回调
    /// @param done 已处理章节数, total 总章节数, cached 已缓存数, failed 失败数,
    ///        chapterTitle 当前章节标题, status 状态("cached"/"ok"/"empty"/"error")
    using DownloadProgressCallback = std::function<void(
        int done, int total, int cached, int failed,
        const std::string& chapterTitle, const std::string& status)>;

    /// 全量下载缓存（增量：跳过已缓存章节，并发下载）
    /// @param bookUrl    书籍URL
    /// @param sourceUrl  书源URL
    /// @param sourceIndex 书源索引（-1 表示按名称匹配）
    /// @param sourceName  书源名称
    /// @param callback    逐章进度回调
    /// @param concurrency 并发线程数（默认 8）
    /// @return {total, cached, failed}
    struct DownloadResult {
        int total = 0;
        int cached = 0;
        int failed = 0;
    };
    DownloadResult downloadBook(
        const std::string& bookUrl, const std::string& sourceUrl,
        int sourceIndex, const std::string& sourceName,
        DownloadProgressCallback callback = nullptr,
        int concurrency = 8);

    /// 批量更新检测回调
    /// @param index 当前序号, total 总数, bookName 书名, hasUpdate 是否有更新
    using CheckUpdateCallback = std::function<void(
        int index, int total, const std::string& bookName, bool hasUpdate)>;

    /// 检查书架所有书籍的更新（逐本检测，有更新的自动重新缓存目录）
    /// @param callback 逐本回调
    /// @return 有更新的书籍数量
    int checkAllUpdates(CheckUpdateCallback callback = nullptr);

    // ──────────────────────────────────────────
    // D6: TXT 导出
    // ──────────────────────────────────────────

    /// 将已全量缓存的书导出为纯文本（TXT）
    /// @param bookUrl   书籍URL
    /// @param sourceUrl 书源URL
    /// @return TXT 字符串；若未全量缓存则返回空串并设置 lastError
    std::string exportBookToTxt(const std::string& bookUrl,
                                 const std::string& sourceUrl = "");

    // ──────────────────────────────────────────
    // D7: 智能自动换源
    // ──────────────────────────────────────────

    /// 换源候选结果
    struct SourceCandidate {
        Book book;                  ///< 从新源搜索到的书籍信息（含新 bookUrl）
        std::string sourceName;     ///< 新书源名称
        std::string sourceUrl;      ///< 新书源URL
        int sourceIndex = -1;       ///< 新书源索引
        int latencyMs = -1;         ///< 搜索延迟
        int matchScore = 0;         ///< 匹配分（书名+作者综合）
    };

    /// 自动寻找可用替代书源（按书名+作者多源并发搜索，结果按匹配度+延迟排序）
    /// 回调方式：逐源推送候选，全部完成后调用 doneCallback
    /// @param bookName           原书名（必填）
    /// @param bookAuthor         原作者（可空，用于辅助过滤）
    /// @param excludeSourceUrl   要排除的源URL（通常是当前绑定源，避免搜到自己）
    /// @param concurrency        并发数
    /// @param onCandidate        每找到一个候选就回调（线程安全）
    /// @param doneCallback       全部完成时回调（传入候选总数）
    /// @param cancelToken        取消令牌
    using AutoSourceCandidateCallback = std::function<void(const SourceCandidate&)>;
    using AutoSourceDoneCallback = std::function<void(int totalCandidates)>;
    void findAlternativeSources(
        const std::string& bookName,
        const std::string& bookAuthor,
        const std::string& excludeSourceUrl,
        int concurrency,
        AutoSourceCandidateCallback onCandidate,
        AutoSourceDoneCallback doneCallback = nullptr,
        std::shared_ptr<CancelToken> cancelToken = nullptr);

    // ──────────────────────────────────────────
    // 状态查询
    // ──────────────────────────────────────────

    /// 书源是否可用
    bool isAvailable() const;

    /// 输出日志（通过 logCallback 回调，若未设置则静默丢弃）
    void log(const std::string& msg) const;

    /// 获取最后一次错误信息
    std::string getLastError() const;
    /// 获取书源信息（JSON 字符串）
    std::string getSourceInfo() const;

    /// 获取书源列表摘要（过滤无效源、映射 validity 字符串、兜底 latency）
    /// 所有平台统一调用此方法，无需各自实现过滤/格式化逻辑
    SourceListResult getSourceList() const;

    /// 导出优+良书源为 JSON 字符串（供分享给他人）
    /// @return 格式化的 JSON 数组字符串，仅包含 Excellent 和 Good 状态的书源
    std::string exportGoodSources() const;

    /// 获取版本号
    static std::string version();

    /// 获取数据库实例（供上层直接操作）
    SourceDatabase* database();

    // ──────────────────────────────────────────
    // D8: RSS 订阅源
    // ──────────────────────────────────────────

    /// 添加/更新 RSS 源
    void upsertRssSource(const RssSource& source);

    /// 获取所有 RSS 源
    std::vector<RssSource> getRssSources();

    /// 删除 RSS 源（级联删除文章）
    bool removeRssSource(const std::string& sourceUrl);

    /// 抓取并解析 RSS 源
    RssFetchResult fetchRssSource(const std::string& sourceUrl);

    /// 检测 RSS 源连通性，移除不可达的源（20 线程并发）
    /// @param progressCallback 进度回调 (done, total, currentUrl, ok)
    /// @return 被移除的 sourceUrl 列表
    std::vector<std::string> checkRssSources(
        std::function<void(int done, int total, const std::string& current, bool ok)> progressCallback = nullptr);

    /// 检测 RSS 源并按延迟评级（不删除，仅更新 validity/latencyMs 并持久化）。
    /// 评级标准（对齐 legado/书源检测）：
    ///   无有效文章为 invalid；列表或抽样正文出错为 poor。
    ///   列表与抽样正文均可读时按总耗时评级：< 1000ms excellent，< 3000ms good，否则 poor。
    /// @param progressCallback (done, total, currentUrl, validity, latencyMs)
    void checkRssSourcesRated(
        std::function<void(int done, int total, const std::string& current,
                           const std::string& validity, int latencyMs)> progressCallback = nullptr);

    /// 清空被评级为"无效(invalid)"的 RSS 源（及其文章）。
    /// @return 被移除的数量
    int clearInvalidRssSources();

    /// 获取 RSS 文章列表（分页）
    RssArticleListResult getRssArticles(const std::string& sourceUrl, int page, int pageSize);

    /// 打开 RSS 列表：有缓存直接读取，无缓存时抓取一次并返回诊断（不依赖导入时间戳）。
    RssArticleListResult loadRssArticles(const std::string& sourceUrl, int page = 1, int pageSize = 50);

    /// 获取单篇 RSS 文章
    RssArticle getRssArticle(int64_t id);

    /// 获取单篇 RSS 文章的正文内容（懒加载：点击时才抓取）
    std::string getRssArticleContent(int64_t articleId);
    RssContentResult getRssArticleContentResult(int64_t articleId);

    /// 清空所有 RSS 数据
    void clearAllRss();

    /// 从本地 JSON 文件导入 RSS 源
    /// @param filePath 本地文件路径
    /// @return (成功导入数量, 错误信息)
    std::pair<int, std::string> importRssSourcesFromFile(const std::string& filePath);

    /// 从 URL 下载并导入 RSS 源
    /// @param url 远程 JSON 文件 URL
    /// @return (成功导入数量, 错误信息)
    std::pair<int, std::string> importRssSourcesFromUrl(const std::string& url);

    /// 从 JSON 字符串导入 RSS 源
    /// @param jsonText JSON 对象、JSON 数组或含 sourceUrls 的包装对象
    /// @return (成功导入数量, 错误信息)
    std::pair<int, std::string> importRssSourcesFromJson(const std::string& jsonText);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;

    /// 内部：抓取「Legado 规则订阅源」（ruleArticles 非空）的文章。
    /// 遍历 sortUrl 各通道，按 ruleArticles/ruleTitle/ruleLink/... 解析。
    /// @param src    完整订阅源（含规则字段）
    /// @param result 出参，失败时写入 error
    /// @return 解析到的文章列表

};

} // namespace ariaread

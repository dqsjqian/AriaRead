#pragma once
/// @file database.h
/// @brief AriaRead 书源数据库 —— SQLite 持久化层

#include "ariaread/types.h"
#include <sqlite_modern_cpp.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <tuple>

namespace ariaread {

// ──────────────────────────────────────────────
// 书源数据库（SQLite 持久化）
// ──────────────────────────────────────────────
class SourceDatabase {
public:
    /// 打开/创建数据库文件
    /// @param dbPath 数据库文件路径（如 "ariaread.db"），不存在则自动创建
    explicit SourceDatabase(const std::string& dbPath);

    ~SourceDatabase();

    // 禁止拷贝，允许移动
    SourceDatabase(const SourceDatabase&) = delete;
    SourceDatabase& operator=(const SourceDatabase&) = delete;
    SourceDatabase(SourceDatabase&&) noexcept;
    SourceDatabase& operator=(SourceDatabase&&) noexcept;

    // ──────────────────────────────────────────
    // 书源 CRUD
    // ──────────────────────────────────────────

    /// 批量插入书源（url 唯一键去重，INSERT OR IGNORE）
    /// @return 实际新插入的数量（跳过已存在的）
    int insertSources(const std::vector<BookSource>& sources);

    /// 获取所有书源（按 latency_ms ASC, weight DESC 排序）
    std::vector<BookSource> getAllSources();

    /// 更新书源有效性标记和延迟
    void updateValidity(const std::string& url, SourceValidity validity, int latencyMs = -1);

    /// 删除所有无效书源，返回删除数量
    int removeInvalidSources();

    /// 删除指定 URL 的书源
    bool removeSource(const std::string& url);

    /// 检查 URL 是否已存在
    bool sourceExists(const std::string& url);

    // ──────────────────────────────────────────
    // 统计
    // ──────────────────────────────────────────

    /// 书源总数
    int getSourceCount();

    /// 有效书源数量
    int getValidCount();

    /// 无效书源数量
    int getInvalidCount();

    // ──────────────────────────────────────────
    // 排序
    // ──────────────────────────────────────────

    /// 静默排序：查询时已按延迟排序，无需额外操作
    void sortByLatency();

    // ──────────────────────────────────────────
    // 书架 CRUD
    // ──────────────────────────────────────────

    /// 添加书籍到书架（book_url 唯一主键去重，INSERT OR IGNORE）
    /// @return 新插入的 rowid，已存在则返回 -1
    int64_t addToBookshelf(const BookshelfItem& item);

    /// 从书架移除书籍（同时 CASCADE 删除关联目录/正文/进度）
    bool removeFromBookshelf(const std::string& bookUrl);

    /// 获取书架列表（按最后阅读时间倒序）
    std::vector<BookshelfItem> getBookshelf();

    /// 检查书籍是否在书架中
    bool isInBookshelf(const std::string& bookUrl);

    /// 更新书架条目的最新章节信息
    void updateBookshelfLastChapter(const std::string& bookUrl,
                                     const std::string& lastChapter, int totalChapters, bool hasUpdate);

    /// 换源：更新 bookshelf 的 source_name/source_url/book_url，
    /// 同时 CASCADE 删除旧 book_url 关联的目录/正文/进度（新 book_url 重新拉取）
    void updateBookshelfSource(const std::string& oldBookUrl,
                                const std::string& newSourceName, const std::string& newSourceUrl,
                                const std::string& newBookUrl);

    /// 获取书架书籍数量
    int getBookshelfCount();

    // ──────────────────────────────────────────
    // URL 历史记录
    // ──────────────────────────────────────────

    /// 添加 URL 到历史记录（自动去重，最多保留 maxCount 条）
    void addUrlHistory(const std::string& url, int maxCount = 10);

    /// 获取 URL 历史记录（按最近使用时间倒序）
    std::vector<std::string> getUrlHistory(int limit = 10);

    /// 删除指定 URL 历史记录
    bool removeUrlHistory(const std::string& url);

    // ──────────────────────────────────────────
    // 阅读进度
    // ──────────────────────────────────────────

    /// 保存阅读进度（INSERT OR REPLACE，以 book_url 为主键）
    void saveReadProgress(const ReadProgress& progress);

    /// 获取阅读进度
    ReadProgress getReadProgress(const std::string& bookUrl);

    // ──────────────────────────────────────────
    // 目录缓存
    // ──────────────────────────────────────────

    /// 缓存书籍目录（全量替换：先删旧的再插入新的）
    void cacheBookCatalog(const std::string& bookUrl,
                          const std::vector<Chapter>& chapters);

    /// 获取缓存的目录（空则无缓存）
    std::vector<Chapter> getCachedCatalog(const std::string& bookUrl);

    /// 删除某本书的目录缓存
    void removeCachedCatalog(const std::string& bookUrl);

    /// 获取缓存的目录章节数
    int getCachedCatalogCount(const std::string& bookUrl);

    /// 批量获取所有书籍的目录缓存数量（避免 N+1 查询）
    /// @return vector of (bookUrl, count)
    std::vector<std::tuple<std::string, int>> batchGetCachedCatalogCounts();

    // ──────────────────────────────────────────
    // 正文缓存
    // ──────────────────────────────────────────

    /// 缓存章节正文（以 chapter_index 为主键，支持重复 chapter_url 的书源）
    void cacheChapterContent(const std::string& bookUrl, int chapterIndex,
                             const std::string& chapterUrl, const std::string& content);

    /// 获取缓存的正文（以 chapter_index 查询，空则无缓存）
    std::string getCachedContent(const std::string& bookUrl, int chapterIndex);

    /// 删除某本书的所有正文缓存
    void removeCachedContent(const std::string& bookUrl);

    /// 获取某本书已缓存的正文章节数
    int getCachedContentCount(const std::string& bookUrl);

    /// 批量获取所有书籍的正文缓存数量（避免 N+1 查询）
    /// @return vector of (bookUrl, count)
    std::vector<std::tuple<std::string, int>> batchGetCachedContentCounts();

    /// 批量获取所有阅读进度（避免 N+1 查询）
    std::vector<ReadProgress> batchGetReadProgress();

    // ──────────────────────────────────────────
    // D8: RSS 订阅源 CRUD
    // ──────────────────────────────────────────

    /// 初始化 RSS 表（在 initSchema 中自动调用）
    void initRssSchema();

    /// 添加/更新 RSS 源
    void upsertRssSource(const RssSource& source);

    /// 获取所有 RSS 源
    std::vector<RssSource> getAllRssSources();

    /// 按 sourceUrl 查找单个 RSS 源（未找到返回 std::nullopt）
    std::optional<RssSource> getRssSourceByUrl(const std::string& sourceUrl);

    /// 删除 RSS 源（级联删除文章）
    bool removeRssSource(const std::string& sourceUrl);

    /// 更新 RSS 源最后更新时间
    void updateRssSourceLastUpdateTime(const std::string& sourceUrl, int64_t timestamp);

    /// 清空所有 RSS 源和文章
    void clearAllRss();

    /// 插入/更新 RSS 文章
    void upsertRssArticle(const RssArticle& article);

    /// 获取 RSS 文章列表（分页）
    std::vector<RssArticle> getRssArticles(const std::string& sourceUrl, int page, int pageSize, int& total);

    /// 获取单篇 RSS 文章
    RssArticle getRssArticle(int64_t id);

    /// 删除某 RSS 源的所有文章
    void removeRssArticlesBySource(const std::string& sourceUrl);

    /// 从 JSON 字符串批量导入 RSS 源
    /// @param jsonText JSON 对象、JSON 数组或含 sourceUrls 的包装对象
    /// @return (成功导入数量, 错误信息)
    std::pair<int, std::string> importRssSourcesFromJson(const std::string& jsonText);

private:
    /// 初始化表结构
    void initSchema();

    /// 一次性数据迁移（清理旧枚举值 / 补列等）
    void runMigrations();

    /// 枚举转换
    static int validityToInt(SourceValidity v);
    static SourceValidity intToValidity(int v);

    /// 序列化完整书源为 JSON（含规则，用于 source_json 字段）
    static std::string serializeSource(const BookSource& s);

    /// 从 JSON 反序列化规则到 BookSource
    static void deserializeRules(const std::string& jsonStr, BookSource& s);

    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace ariaread

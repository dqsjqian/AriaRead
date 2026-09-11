#pragma once
/// @file types.h
/// @brief OpenRead 核心数据类型定义

#include <cstddef>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>

namespace openread {

// ──────────────────────────────────────────────
// 书源验证状态（前向声明，BookSource 需要）
// ──────────────────────────────────────────────
enum class SourceValidity {
    Unknown,    ///< 未知（待检测）
    Excellent,  ///< 优：搜索 + 命中书的目录 + 正文，端到端总耗时 < 2000ms
    Good,       ///< 良：端到端总耗时 2000 ~ 5000ms
    Poor,       ///< 差：端到端总耗时 5000 ~ 9000ms
    Invalid     ///< 无效：> 9000ms，或报错，或无结果、无目录、无正文
                ///<        评级以「2 个随机单字关键词分别独立搜索，前 5 本任意一本能解出
                ///<         目录(≥3章) 且第 1 章正文(≥50 字节)」为成功判据。
};

// ──────────────────────────────────────────────
// 书籍信息
// ──────────────────────────────────────────────
struct Book {
    std::string name;           ///< 书名
    std::string author;         ///< 作者
    std::string coverUrl;       ///< 封面URL
    std::string bookUrl;        ///< 书籍详情页URL
    std::string lastChapter;    ///< 最新章节
    std::string intro;          ///< 简介
    std::string kind;           ///< 分类
    std::string wordCount;      ///< 字数
    int matchScore = 0;         ///< 匹配分（多源搜索过滤/排序用）
};

// ──────────────────────────────────────────────
// 章节信息
// ──────────────────────────────────────────────
struct Chapter {
    std::string title;          ///< 章节标题
    std::string url;            ///< 章节URL
    int index = 0;              ///< 章节序号
    bool isVip = false;         ///< 是否VIP章节
    bool isVolume = false;      ///< 是否为卷标题（分卷，非真实章节）
};

// ──────────────────────────────────────────────
// 搜索规则
// ──────────────────────────────────────────────
struct SearchRule {
    std::string bookList;       ///< 书籍列表选择器
    std::string name;           ///< 书名
    std::string author;         ///< 作者
    std::string coverUrl;       ///< 封面
    std::string bookUrl;        ///< 书籍链接
    std::string lastChapter;    ///< 最新章节
    std::string intro;          ///< 简介
    std::string kind;           ///< 分类
    std::string wordCount;      ///< 字数
};

// ──────────────────────────────────────────────
// 书籍详情规则
// ──────────────────────────────────────────────
struct BookInfoRule {
    std::string init;           ///< 详情页初始化规则（可选）
    std::string name;           ///< 书名
    std::string author;         ///< 作者
    std::string coverUrl;       ///< 封面
    std::string intro;          ///< 简介
    std::string kind;           ///< 分类
    std::string lastChapter;    ///< 最新章节
    std::string wordCount;      ///< 字数
    std::string tocUrl;         ///< 目录页 URL（关键）
};

// ──────────────────────────────────────────────
// 目录规则
// ──────────────────────────────────────────────
struct CatalogRule {
    std::string chapterList;    ///< 章节列表
    std::string chapterName;    ///< 章节名
    std::string chapterUrl;     ///< 章节链接
    std::string nextPage;       ///< 下一页
    std::string isVip;          ///< VIP判断
    std::string isVolume;       ///< 卷标题判断
    std::string updateTime;     ///< 更新时间/章节附加信息
    std::string formatJs;       ///< 章节标题格式化 JS
};

// ──────────────────────────────────────────────
// 正文规则
// ──────────────────────────────────────────────
struct ContentRule {
    std::string content;        ///< 正文选择器
    std::string nextPage;       ///< 下一页
    std::string replace;        ///< 替换规则
    std::string regex;          ///< 正则提取
};

// ──────────────────────────────────────────────
// 完整书源
// ──────────────────────────────────────────────
struct BookSource {
    // 基本信息
    std::string name;           ///< 书源名称
    std::string url;            ///< 书源URL
    std::string group;          ///< 分组
    std::string icon;           ///< 图标URL
    std::string comment;        ///< 书源说明
    int enabled = 1;            ///< 是否启用
    int enabledExplore = 1;     ///< 启用发现
    int weight = 0;             ///< 权重

    // 有效性标记（C++ 引擎内部维护）
    SourceValidity validity = SourceValidity::Unknown;  ///< 书源有效性
    int latencyMs = -1;                                  ///< 响应延迟（毫秒），-1=未检测

    // 搜索配置
    std::string searchUrl;      ///< 搜索地址，支持 {{key}} 模板
    SearchRule searchRule;

    // 发现配置
    std::string exploreUrl;     ///< 发现地址
    SearchRule exploreRule;

    // 书籍详情配置（用于点击搜索结果后的二次解析）
    BookInfoRule bookInfoRule;

    // 目录配置
    CatalogRule catalogRule;

    // 正文配置
    ContentRule contentRule;

    // HTTP 配置
    std::map<std::string, std::string> headers;  ///< 自定义请求头
    std::string userAgent;      ///< 自定义UA
    std::string loginUrl;       ///< 登录地址
    std::string loginUi;        ///< 登录界面
    std::string loginCheckUrl;  ///< 登录检测
    std::string loginCheckJs;   ///< 登录检测JS
    int timeout = 30000;        ///< 超时(ms)
};

// ──────────────────────────────────────────────
// HTTP 相关类型
// ──────────────────────────────────────────────
struct HttpResponse {
    int statusCode = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string error;
};

struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::map<std::string, std::string> headers;
    std::string body;
    std::string charset;        ///< 编码（gbk/utf-8）
    int timeoutMs = 30000;
    std::size_t maxResponseBytes = 32 * 1024 * 1024; ///< 解压后正文上限，0 使用默认 32 MiB
};

/// HTTP 请求回调（由 UI 层注入，实现跨平台网络）
using HttpRequestFunc = std::function<std::string(
    const std::string& url,
    const std::string& method,
    const std::string& headers,
    const std::string& body
)>;

/// HTTP 客户端回调（完整版）
using HttpClientFunc = std::function<HttpResponse(const HttpRequest&)>;

/// JS 日志回调
using JsLogFunc = std::function<void(const std::string& message)>;

// ──────────────────────────────────────────────
// 书源验证 / 多源搜索 回调类型
// ──────────────────────────────────────────────

/// 书源验证回调（逐源通知）
/// @param sourceIndex 书源索引
/// @param sourceName  书源名称
/// @param validity    当前状态
/// @param detail      附加信息（如超时原因、结果数量）
using SourceValidateCallback = std::function<void(
    size_t sourceIndex,
    const std::string& sourceName,
    SourceValidity validity,
    const std::string& detail
)>;

/// 多源搜索回调（逐源推送结果）
/// @param sourceIndex 书源索引
/// @param sourceName  书源名称
/// @param books       搜索结果（可能为空）
/// @param error       错误信息（空表示成功）
using SearchAllCallback = std::function<void(
    size_t sourceIndex,
    const std::string& sourceName,
    const std::vector<Book>& books,
    const std::string& error
)>;

// ──────────────────────────────────────────────
// 并发控制
// ──────────────────────────────────────────────

/// 取消令牌（线程安全，用于取消并发任务）
class CancelToken {
public:
    CancelToken() : cancelled_(false) {}
    void cancel() { cancelled_.store(true, std::memory_order_release); }
    bool isCancelled() const { return cancelled_.load(std::memory_order_acquire); }
    void reset() { cancelled_.store(false, std::memory_order_release); }
private:
    std::atomic<bool> cancelled_;
};

/// 并发验证回调（含延迟信息，线程安全调用）
/// @param sourceIndex 书源索引
/// @param sourceName  书源名称
/// @param validity    验证结果
/// @param latencyMs   响应延迟（毫秒）
/// @param detail      附加信息
using ConcurrentValidateCallback = std::function<void(
    size_t sourceIndex,
    const std::string& sourceName,
    SourceValidity validity,
    int latencyMs,
    const std::string& detail
)>;

/// 并发搜索回调（逐源推送，线程安全调用）
/// @param sourceIndex 书源索引
/// @param sourceName  书源名称
/// @param books       搜索结果
/// @param latencyMs   响应延迟（毫秒）
/// @param error       错误信息（空表示成功）
using ConcurrentSearchCallback = std::function<void(
    size_t sourceIndex,
    const std::string& sourceName,
    const std::vector<Book>& books,
    int latencyMs,
    const std::string& error
)>;

/// 并发完成回调
/// @param validCount   有效数量
/// @param invalidCount 无效数量
/// @param removedCount 删除数量（仅验证时有效）
using ConcurrentDoneCallback = std::function<void(
    int validCount,
    int invalidCount,
    int removedCount
)>;

// ──────────────────────────────────────────────
// 书架条目（用户收藏的书籍）
// ──────────────────────────────────────────────
struct BookshelfItem {
    int64_t id = 0;                 ///< 数据库主键
    std::string bookName;           ///< 书名
    std::string bookAuthor;         ///< 作者
    std::string coverUrl;           ///< 封面URL
    std::string bookUrl;            ///< 书籍详情页URL（唯一标识）
    std::string sourceName;         ///< 书源名称
    std::string sourceUrl;          ///< 书源URL
    std::string intro;              ///< 简介
    std::string kind;               ///< 分类
    std::string lastChapter;        ///< 最新章节标题
    int totalChapters = 0;          ///< 总章节数
    bool hasUpdate = false;         ///< 是否有更新
    int sortOrder = 0;              ///< 排序权重
    int64_t createdAt = 0;          ///< 加入时间（Unix 时间戳）
    int64_t updatedAt = 0;          ///< 更新时间
};

// ──────────────────────────────────────────────
// 阅读进度
// ──────────────────────────────────────────────
struct ReadProgress {
    std::string bookUrl;            ///< 关联书籍 URL（主键）
    int chapterIndex = 0;           ///< 章节索引
    std::string chapterUrl;         ///< 章节 URL
    std::string chapterTitle;       ///< 章节标题
    int pageOffset = 0;             ///< 页内偏移（预留）
    double readPercent = 0.0;       ///< 阅读百分比
    int64_t lastReadAt = 0;         ///< 最后阅读时间
};
// ──────────────────────────────────────────────
// 书架条目详情（含缓存状态和阅读进度，一次性查询）
// ──────────────────────────────────────────────
struct BookshelfDetail {
    BookshelfItem item;             ///< 书架条目
    int catalogCached = 0;          ///< 已缓存目录章节数
    int contentCached = 0;          ///< 已缓存正文章节数
    ReadProgress progress;          ///< 阅读进度
};

// ──────────────────────────────────────────────
// 单本书阅读状态（引擎内 per-book state 的公开快照）
// ──────────────────────────────────────────────
struct BookReadingState {
    std::string bookUrl;            ///< 书籍URL
    std::string sourceUrl;          ///< 书源URL
    std::string sourceName;         ///< 书源名称
    int sourceIndex = -1;           ///< 书源索引（未知=-1）
    std::string kind;               ///< 分类/连载状态原始文本
    bool isFinished = false;        ///< 是否已完结
    int catalogCached = 0;          ///< 已缓存目录章节数
    int contentCached = 0;          ///< 已缓存正文章节数
    int totalChapters = 0;          ///< 当前已知总章节数
    bool fullyCached = false;       ///< 是否已缓存全部正文
    bool hasUpdate = false;         ///< 是否有更新
    bool refreshQueued = false;     ///< 是否已进入后台刷新队列
    bool refreshInFlight = false;   ///< 是否正在后台刷新
    int64_t lastOpenedAt = 0;       ///< 最近一次打开时间
    int64_t lastRefreshAt = 0;      ///< 最近一次刷新完成时间
    std::string lastError;          ///< 最近一次刷新错误
    ReadProgress progress;          ///< 当前阅读进度
};

// ──────────────────────────────────────────────
// 书源摘要（供列表展示，已过滤/格式化）
// ──────────────────────────────────────────────
struct SourceSummary {
    std::string name;           ///< 书源名称
    std::string url;            ///< 书源URL
    std::string group;          ///< 分组
    std::string searchUrl;      ///< 搜索地址
    std::string exploreUrl;     ///< 发现地址
    std::string validity;       ///< 有效性字符串（"unknown"/"excellent"/"good"/"poor"/"invalid"）
    int latencyMs = -1;         ///< 响应延迟（毫秒），-1=未检测
};

/// 书源各状态统计
struct SourceStats {
    int excellent = 0;  ///< 优
    int good = 0;       ///< 良
    int poor = 0;       ///< 差
    int invalid = 0;    ///< 无效
    int unknown = 0;    ///< 未知
};

/// 书源列表查询结果
struct SourceListResult {
    std::vector<SourceSummary> sources;  ///< 过滤后的书源列表（不含无效源）
    int validCount = 0;                   ///< 有效书源数量
    int totalCount = 0;                   ///< 总书源数量（含无效）
    SourceStats stats;                    ///< 各状态数量统计（只统计有 searchUrl 的书源）
};

// ──────────────────────────────────────────────
// D8: RSS 订阅源
// ──────────────────────────────────────────────
struct RssSource {
    std::string sourceUrl;       ///< RSS 源 URL（主键）
    std::string sourceName;      ///< 源名称
    std::string sourceIcon;      ///< 图标 URL
    std::string sourceGroup;     ///< 分组
    int enabled = 1;             ///< 是否启用
    std::string sortUrl;         ///< 排序 URL
    int articleStyle = 0;        ///< 文章样式
    int customOrder = 0;         ///< 自定义排序
    int64_t lastUpdateTime = 0;  ///< 最后更新时间（Unix 时间戳）
    std::string sourceJson;      ///< 原始 JSON
    int64_t createdAt = 0;       ///< 创建时间
    int64_t updatedAt = 0;       ///< 更新时间

    // ── 第三方订阅源兼容字段 ──
    std::string sourceComment;   ///< 注释
    std::string variableComment; ///< 自定义变量说明
    std::string jsLib;           ///< js 库
    int enabledCookieJar = 1;    ///< 启用 okhttp CookieJar
    std::string concurrentRate;  ///< 并发率
    std::string header;          ///< 请求头（JSON 字符串）
    std::string loginUrl;        ///< 登录地址
    std::string loginUi;         ///< 登录 UI
    std::string loginCheckJs;    ///< 登录检测 js
    std::string coverDecodeJs;   ///< 封面解密 js
    int singleUrl = 0;           ///< 是否单 url 源
    std::string ruleArticles;    ///< 列表规则
    std::string ruleNextPage;    ///< 下一页规则
    std::string ruleTitle;       ///< 标题规则
    std::string rulePubDate;     ///< 发布日期规则
    std::string ruleDescription; ///< 描述规则
    std::string ruleImage;       ///< 图片规则
    std::string ruleLink;        ///< 链接规则
    std::string ruleContent;     ///< 正文规则
    std::string contentWhitelist;///< 正文 url 白名单
    std::string contentBlacklist;///< 正文 url 黑名单
    std::string shouldOverrideUrlLoading; ///< 跳转 url 拦截
    std::string style;           ///< webView 样式
    int enableJs = 1;            ///< 启用 js
    int loadWithBaseUrl = 1;     ///< 使用 baseUrl 加载
    std::string injectJs;        ///< 注入 js

    // ── 检测评级（OpenRead 内部维护，序列化进 source_json extras）──
    std::string validity = "unknown";  ///< 有效性："excellent"/"good"/"poor"/"invalid"/"unknown"
    int latencyMs = -1;                ///< 检测延迟（毫秒），-1=未检测
};

struct RssArticle {
    int64_t id = 0;              ///< 数据库主键
    std::string sourceUrl;       ///< 所属 RSS 源 URL
    std::string title;           ///< 文章标题
    std::string link;            ///< 文章链接
    int64_t pubDate = 0;         ///< 发布时间（Unix 时间戳）
    std::string description;     ///< 摘要
    std::string content;         ///< 正文内容
    std::string image;           ///< 首图 URL
    int64_t order = 0;           ///< 抓取序号（对齐 legado order，用于稳定排序）
    int64_t createdAt = 0;       ///< 入库时间
};

struct RssFetchResult {
    int count = 0;       ///< 解析到的文章总数
    int inserted = 0;    ///< 新插入数量
    int skipped = 0;     ///< 跳过（重复）数量
    std::string error;   ///< 错误信息（空表示成功）
};

struct RssArticleListResult {
    std::vector<RssArticle> articles;  ///< 文章列表
    int total = 0;                      ///< 总数
    int page = 1;                       ///< 当前页
    int pageSize = 50;                  ///< 每页大小
};

// ──────────────────────────────────────────────
// 辅助函数
// ──────────────────────────────────────────────

/// SourceValidity 枚举 → 字符串
inline std::string validityToString(SourceValidity v) {
    switch (v) {
        case SourceValidity::Unknown:   return "unknown";
        case SourceValidity::Excellent: return "excellent";
        case SourceValidity::Good:      return "good";
        case SourceValidity::Poor:      return "poor";
        case SourceValidity::Invalid:   return "invalid";
    }
    return "unknown";
}

/// 判断 SourceValidity 是否为有效状态（Poor 视为无效，不计入有效）
inline bool isValidValidity(SourceValidity v) {
    return v == SourceValidity::Excellent ||
           v == SourceValidity::Good;
}

} // namespace openread

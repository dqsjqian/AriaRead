#pragma once
/// @file engine_impl.h
/// @brief BookSourceEngine 内部实现头文件（仅供 engine_*.cpp 使用）

#include "ariaread/engine.h"
#include "ariaread/js_runtime.h"
#include "ariaread/selector.h"
#include "ariaread/source_parser.h"
#include "ariaread/analyze_url.h"
#include "ariaread/http_client.h"
#include "ariaread/rule_analyzer.h"
#include "ariaread/rule_cache.h"
#include "ariaread/inline_rule.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include <functional>
#include <exception>
#include <regex>
#include <deque>
#include <unordered_set>

namespace ariaread {

using json = nlohmann::json;

// ──────────────────────────────────────────────
// 简单令牌桶速率限制器（线程安全）
// 用于控制对同一书源网站的请求速率，防止触发反爬
// ──────────────────────────────────────────────
class RateLimiter {
public:
    /// @param maxConcurrent 最大同时在飞请求数
    /// @param minIntervalMs 两次请求之间的最小间隔（毫秒）
    RateLimiter(int maxConcurrent = 3, int minIntervalMs = 100)
        : maxConcurrent_(maxConcurrent), minIntervalMs_(minIntervalMs), inFlight_(0) {}

    /// 获取令牌（阻塞直到可以发起请求）
    void acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        // 等待并发数降到限制以下
        cv_.wait(lock, [this]() { return inFlight_ < maxConcurrent_; });
        ++inFlight_;
        // 确保与上次请求有最小间隔
        if (minIntervalMs_ > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRequestTime_).count();
            if (elapsed < minIntervalMs_) {
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(minIntervalMs_ - elapsed));
            }
        }
    }

    /// 释放令牌（请求完成后调用）
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --inFlight_;
            lastRequestTime_ = std::chrono::steady_clock::now();
        }
        cv_.notify_one();
    }

private:
    int maxConcurrent_;
    int minIntervalMs_;
    int inFlight_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::chrono::steady_clock::time_point lastRequestTime_{};
};

// ──────────────────────────────────────────────
// 工具函数（匿名命名空间改为 detail 命名空间，供多文件共享）
// ──────────────────────────────────────────────
namespace detail {

/// 文本归一化（去除 HTML 标签、空白、分隔符，小写化）
std::string normalizeText(const std::string& text);

/// 计算书籍与关键词的匹配分数
int bookMatchScore(const Book& book,
                   const std::string& keyword,
                   bool matchName,
                   bool matchAuthor,
                   bool matchIntro);

/// 判断字符串是否为空白
bool isBlank(const std::string& s);

/// 解析 URL（支持相对路径、@js、options）
std::string resolveUrlWithBase(const std::string& rawUrl,
                               const std::string& baseUrl,
                               JsRuntime* js);

/// 从 HTML 片段中提取 href 属性作为兜底
std::string extractHrefFallback(const std::string& htmlItem,
                                const std::string& baseUrl,
                                JsRuntime* js);

/// 去除首尾空白
std::string trimCopy(const std::string& s);

/// 剥离 XML/RSS 中的 CDATA 包裹（<![CDATA[ ... ]]>），返回内部原文。
/// 大量 CMS（36氪、WordPress 等）把 title/link/description/content 写成 CDATA，
/// 不剥离会导致 link 开头变成 "<!"、标题前后出现乱码、原文按钮失效、正文取不到。
/// 支持一段文本中含多个 CDATA 段；无 CDATA 时原样返回。
std::string stripCdata(const std::string& text);

/// 解码 HTML 实体（&nbsp; &amp; &lt; &gt; &quot; &#xxx; &#xHHH; 等）。
/// 内部会先剥离 CDATA 包裹，再解码实体。
std::string decodeHtmlEntities(const std::string& text);

/// 清理正文内容（去除残留 HTML 标签、解码实体、规范化空白）
std::string cleanContent(const std::string& rawText);

/// 清理正文但保留 <img> 标签（对齐 legado HtmlFormatter.formatKeepImg）。
/// <br>/<p>/<div> 转换行，其它标签去除，<img ... src=...> 转为占位/绝对化 src 后保留。
/// @param baseUrl 用于把相对 img src 转绝对（为空则不转换）
std::string formatKeepImg(const std::string& rawText, const std::string& baseUrl = "");

/// 应用 legado 风格的 ##正则替换 规则串（content 级别全文替换）。
/// 规则形如  regex##replacement  或  regex（删除）  或  regex##repl###（仅首个）。
/// 多组以换行分隔。
std::string applyContentReplaceRule(const std::string& text, const std::string& replaceRule);

/// 检查是否为合法 UTF-8
bool isValidUtf8(const std::string& s);

/// 清理非法 UTF-8 字节
std::string sanitizeUtf8(const std::string& s);

/// 检测并转换编码到 UTF-8（从 HTTP Content-Type 或 XML 声明中提取 charset）
std::string ensureUtf8(const std::string& data, const std::string& declaredCharset = "");

/// 安全 JSON 序列化：非法 UTF-8 字节替换为 U+FFFD，不会抛异常。
/// 必须用于所有 HTTP 响应的 dump 路径，防止书源返回的 GBK/乱码导致 type_error.316 崩溃。
inline std::string safeDump(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

/// 渲染模板规则（替换 {{...}} 占位符）
std::string renderTemplateRule(const std::string& ruleTemplate,
                               const std::function<std::string(const std::string&)>& resolver);

/// 提取字段（支持模板回退）
std::string extractFieldWithTemplateFallback(
    const std::string& item,
    const std::string& fieldRule,
    const std::function<std::vector<std::string>(const std::string&, const std::string&)>& applyRule
);

/// 统一规则解析（与 legado AnalyzeByJSoup.getStringList 语义对齐）。
/// 负责：内嵌规则替换({{js}}/@get:{}/$n)、## 正则替换、RuleAnalyzer 三路分隔符
/// (&&/||/%%) 切分与组合、选择器执行、规则/正则缓存。
/// 抽成自由函数，供 Impl::applyRule 以及各 worker 线程（搜索/下载/校验）统一复用，
/// 避免某些路径直接走 createOrChain().select() 而丢失索引/排除/分隔符/正则替换处理。
/// @param js      可选 JS 运行时；非空时启用 {{js}} 内嵌替换与 @js:/<js> 规则执行。
/// @param baseUrl 当前页面 URL（注入 JS 上下文 baseUrl）。
std::vector<std::string> applyRuleStatic(const std::string& content,
                                         const std::string& rule,
                                         JsRuntime* js = nullptr,
                                         const std::string& baseUrl = "");

/// 清洗作者字段：去除常见的"作者："、"作者:"、"著者："、"著者:"、"文："等前缀，
/// 以及前后空白。书源解析出的 author 经常自带这类前缀，需要统一剔除。
std::string cleanAuthorField(const std::string& author);

} // namespace detail

// ──────────────────────────────────────────────
// 引擎内部实现类
// ──────────────────────────────────────────────
class BookSourceEngine::Impl {
public:
    struct RefreshJob {
        std::string key;
        std::string bookUrl;
        std::string sourceUrl;
        std::string sourceName;
        std::string kind;
        int sourceIndex = -1;
    };

    struct BookStateEntry {
        BookReadingState snapshot;
    };

    std::unique_ptr<JsRuntime> js;
    HttpRequestFunc httpFunc;
    HttpClientFunc httpClientFunc;
    std::function<void()> operationCheck;
    mutable std::exception_ptr operationFailure;
    JsLogFunc logFunc;       ///< JS 日志回调
    JsLogFunc logCallback;   ///< 通用日志回调（引擎内部警告/错误）
    std::vector<BookSource> sources;
    size_t currentSourceIndex = 0;
    std::string lastError;
    std::unique_ptr<SourceDatabase> db;
    std::string dbPath;
    int defaultConcurrency = 16;
    int validateMaxTaskSec = 35;  ///< E2: 验证单任务最长耗时（秒），由 setValidateMaxTaskSec 设置
    int jsPoolMaxSize = 32;       ///< C4: JsRuntime 池上限
    std::mutex sourcesMutex;

    mutable std::mutex bookStateMutex;
    std::unordered_map<std::string, BookStateEntry> bookStates;
    std::deque<RefreshJob> refreshQueue;
    std::unordered_set<std::string> queuedRefreshKeys;
    std::condition_variable refreshCv;
    std::thread refreshWorker;
    bool stopRefreshWorker = false;

    /// engine 操作锁：保护 currentSourceIndex 和所有依赖它的操作
    /// 使用 recursive_mutex 因为方法之间有嵌套调用
    std::recursive_mutex engineMutex;

    // JsRuntime 对象池（避免每次验证都重新创建）
    std::mutex jsPoolMutex;
    std::vector<std::unique_ptr<JsRuntime>> jsPool;
    bool jsPoolWarmed = false;

    /// A5: 用于向 detach 工作线程传递 "Impl 还活着吗" 的信号量。
    /// 验证线程超时后会被 detach，若 Impl 随引擎析构而死，这些线程仍可能在 HTTP
    /// 卡死后苏醒，此时访问 implPtr->sources 会 UAF。它们持有本 shared_ptr
    /// 的副本，每次访问 Impl 成员前先检查 *aliveFlag，为 false 就 no-op 退出。
    std::shared_ptr<std::atomic<bool>> aliveFlag;

    BookSourceEngine* owner = nullptr;  ///< 指向外层 Engine，供工作线程调用其方法

    Impl() : js(std::make_unique<JsRuntime>()),
             httpClientFunc(createDefaultHttpClient()),
             aliveFlag(std::make_shared<std::atomic<bool>>(true)) {
        js->setMemoryLimit(256 * 1024 * 1024);
        js->setStackSize(4 * 1024 * 1024);
        // 注意：owner 必须在 startRefreshWorker 之前设置，由 BookSourceEngine 构造函数负责
    }

    ~Impl() {
        // A5: 立即通知所有 detach 的 worker 线程：别再访问本 Impl
        if (aliveFlag) aliveFlag->store(false, std::memory_order_release);
        stopWorker();
    }

    static std::string makeBookStateKey(const std::string& bookUrl, const std::string& sourceUrl) {
        return bookUrl + "|" + sourceUrl;
    }

    static bool isFinishedKind(const std::string& kind) {
        return kind.find("完结") != std::string::npos ||
               kind.find("完本") != std::string::npos ||
               kind.find("已完结") != std::string::npos ||
               kind.find("已完本") != std::string::npos ||
               kind.find("全本") != std::string::npos;
    }

    static int64_t nowUnix() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void startRefreshWorker();
    void stopWorker();

    /// 从数据库填充 BookReadingState 的缓存统计、进度、书架信息
    void populateBookReadingStateFromDb(BookReadingState& state);

    /// 预热 JsRuntime 对象池（在后台线程中调用）
    void warmUpJsPool(int count) {
        std::lock_guard<std::mutex> lock(jsPoolMutex);
        if (jsPoolWarmed) return;
        // C4: 严格上限保护，避免悪意参数导致内存爆炸
        int actual = std::min(count, jsPoolMaxSize);
        if (actual < 1) actual = 1;
        jsPool.reserve(actual);
        for (int i = 0; i < actual; ++i) {
            auto rt = std::make_unique<JsRuntime>();
            rt->setMemoryLimit(64 * 1024 * 1024);
            rt->setStackSize(2 * 1024 * 1024);
            bindJsSelector(rt.get());
            jsPool.push_back(std::move(rt));
        }
        jsPoolWarmed = true;
    }

    /// 从对象池获取一个 JsRuntime（池空则新建）
    std::unique_ptr<JsRuntime> acquireJsRuntime() {
        std::lock_guard<std::mutex> lock(jsPoolMutex);
        if (!jsPool.empty()) {
            auto rt = std::move(jsPool.back());
            jsPool.pop_back();
            return rt;
        }
        // 池空，新建一个
        auto rt = std::make_unique<JsRuntime>();
        rt->setMemoryLimit(64 * 1024 * 1024);
        rt->setStackSize(2 * 1024 * 1024);
        bindJsSelector(rt.get());
        return rt;
    }

    /// 归还 JsRuntime 到对象池
    /// C4: 上限保护，超过 jsPoolMaxSize 的直接析构，避免突发高并发后池无限增长
    void releaseJsRuntime(std::unique_ptr<JsRuntime> rt) {
        if (!rt) return;
        std::lock_guard<std::mutex> lock(jsPoolMutex);
        if (static_cast<int>(jsPool.size()) >= jsPoolMaxSize) {
            // 池已满，丢弃（rt 在函数返回时自动析构）
            return;
        }
        jsPool.push_back(std::move(rt));
    }

    BookSource& currentSource() {
        if (sources.empty()) {
            static BookSource empty;
            return empty;
        }
        return sources[currentSourceIndex];
    }

    const BookSource& currentSource() const {
        if (sources.empty()) {
            static const BookSource empty;
            return empty;
        }
        return sources[currentSourceIndex];
    }

    /// 模板变量替换
    static std::string replaceTemplate(const std::string& tpl,
                                        const std::map<std::string, std::string>& vars) {
        std::string result = tpl;
        for (const auto& [key, value] : vars) {
            std::string placeholder = "{{" + key + "}}";
            size_t pos = 0;
            while ((pos = result.find(placeholder, pos)) != std::string::npos) {
                result.replace(pos, placeholder.length(), value);
                pos += value.length();
            }
        }
        return result;
    }

    /// 发送 HTTP 请求
    void checkOperation() const {
        if (operationFailure) std::rethrow_exception(operationFailure);
        try {
            if (operationCheck) operationCheck();
        } catch (...) {
            operationFailure = std::current_exception();
            throw;
        }
    }

    std::string httpRequest(const std::string& url,
                            const std::string& method = "GET",
                            const std::string& headers = "{}",
                            const std::string& body = "") {
        checkOperation();
        if (httpClientFunc) {
            HttpRequest req;
            req.url = url;
            req.method = method;
            req.body = body;
            req.timeoutMs = currentSource().timeout;

            try {
                auto h = json::parse(headers);
                for (auto it = h.begin(); it != h.end(); ++it) {
                    req.headers[it.key()] = it.value().get<std::string>();
                }
            } catch (...) {}

            for (const auto& [k, v] : currentSource().headers) {
                req.headers[k] = v;
            }

            auto resp = httpClientFunc(req);
            checkOperation();
            if (resp.statusCode >= 200 && resp.statusCode < 400) {
                return resp.body;
            }
            lastError = "HTTP " + std::to_string(resp.statusCode) + ": " + resp.error;
            return "";
        }

        if (httpFunc) {
            auto response = httpFunc(url, method, headers, body);
            checkOperation();
            return response;
        }

        lastError = "HTTP callback not set";
        return "";
    }

    /// 执行选择器规则，提取数据
    /// 统一委托给 detail::applyRuleStatic，确保与各 worker 线程（搜索/下载/校验）
    /// 走完全一致的解析路径（三路分隔符、索引/排除、内嵌规则、JS、正则替换、缓存）。
    std::vector<std::string> applyRule(const std::string& content,
                                        const std::string& rule) {
        checkOperation();
        auto result = detail::applyRuleStatic(content, rule, js.get(),
                                       sources.empty() ? std::string()
                                                       : currentSource().url);
        checkOperation();
        return result;
    }

    /// 日志输出
    void log(const std::string& msg) {
        if (logFunc) {
            logFunc(msg);
        }
    }

    /// 把引擎的 HTTP 能力桥接给 JsRuntime，使书源 JS 中的
    /// java.ajax/get/post/getString 能真正发起请求（对齐 legado JsExtensions）。
    /// 在每次 setHttpClient/setHttpRequest 后调用。
    void bindJsHttp() {
        if (!js) return;
        js->setHttpFunc([this](const std::string& url,
                               const std::string& method,
                               const std::string& headersJson,
                               const std::string& body) -> std::string {
            // 复用 currentSource 的 header/timeout
            return this->httpRequest(url, method.empty() ? "GET" : method,
                                     headersJson.empty() ? "{}" : headersJson, body);
        });
        bindJsSelector(js.get());
    }

    /// 把选择器引擎桥接给指定 JsRuntime，使书源 JS 中的
    /// java.getString(rule)/getElements(rule) 能用 AriaRead 选择器解析当前内容。
    /// 关键：回调内调用 applyRuleStatic 时传 js=nullptr，避免 JS→选择器→JS 无限递归。
    static void bindJsSelector(JsRuntime* rt) {
        if (!rt) return;
        rt->setSelectorFunc([](const std::string& content,
                               const std::string& rule) -> std::vector<std::string> {
            return detail::applyRuleStatic(content, rule, nullptr, "");
        });
    }
};

} // namespace ariaread

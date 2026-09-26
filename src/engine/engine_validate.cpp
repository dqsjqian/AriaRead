/// @file engine_validate.cpp
/// @brief 书源验证：串行 + 并发。评级算法独立成可单测的小函数。
///
/// 评级语义（千万别再搞错）—— 并行视角，最短成功路径耗时：
///
///   1. 随机抽 N 个不同关键词（默认 N=3，可调），并发搜索
///   2. 每个 kw 各看前 maxBooksToTry 本（默认 5），任一本验证通过即该 kw 路径成功
///   3. 路径耗时 = 该 kw 的 search 耗时 + 成功那本 book 的 verifyBook 耗时
///   4. 评级 = min(成功路径耗时)；失败路径不计入（"白等"分支）
///   5. 阈值：< 2000 优 / ≤ 5000 良 / ≤ 9000 差 / > 9000 或全失败 无效
///
/// 设计意图：
///   * **N≥2 个 kw 并行** 是为了**容错**——某个 kw 在某站 0 命中（站内未收录），
///     不能因此判源差。要给源公平的「我用别的关键词试试」机会。
///   * **失败分支不计入** 因为评级是"用户走最快路径"的体验估计，不是测最差耗时。
///   * **9000ms 兜底** 即使有成功路径，体感不可用就判 Invalid。
///   * **随机偏移遍历前 N 本** 避开置顶广告/付费推广位的影响。

#include "ariaread/engine_impl.h"
#include "ariaread/parallel.h"

#include <cstring>
#include <climits>
#include <algorithm>
#include <thread>
#include <chrono>
#include <future>
#include <random>
#include <array>
#include <vector>

namespace ariaread {

using json = nlohmann::json;

// ──────────────────────────────────────────────
// 评级策略：所有 magic number 集中在这里，可单测注入
// ──────────────────────────────────────────────
struct GradePolicy {
    int excellentMs   = 2000;   ///< < 2000ms 评 Excellent
    int goodMs        = 5000;   ///< ≤ 5000ms 评 Good
    int poorMs        = 9000;   ///< ≤ 9000ms 评 Poor；超过判 Invalid
    int numKeywords   = 3;      ///< 并发尝试的关键词个数（容偶发出错）
    int maxBooksToTry = 5;      ///< 每个 kw 最多看前 N 本搜索结果
    int minChapters   = 3;      ///< 目录至少要有几章才算"有目录"
    int minContentBytes = 50;   ///< 第 1 章正文至少多少字节才算"有正文"
    bool randomBookOffset = true; ///< 随机偏移遍历起点，避开置顶广告位
    int sourceBudgetMs = 12000; ///< 单源总预算（含 search+verify 全过程）。
                                ///< 超过即立刻 bail-out 判 Invalid，避免最后一个源
                                ///< 的 HTTP 半死不活拖住整个验证进度（主要价值是
                                ///< UI 不再卡 30s+，而不是评级更准）。
                                ///< 必须 > poorMs，否则会出现"评级用阈值合法 < 9000，
                                ///< 但被预算干掉"的怪事。
};

namespace {

// 候选随机关键字（常见高频字，几乎任何小说站都能搜到结果）
const std::array<std::string, 15> kRandomKeywords = {
    "我", "的", "记", "传", "天", "人", "神", "界", "爱", "你", "她", "他", "王", "个", "不"
};

/// 线程局部 RNG —— 给评级使用；并发场景下避免互锁。
std::mt19937& threadLocalRng() {
    static thread_local std::mt19937 rng(
        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())
        ^ static_cast<unsigned>(reinterpret_cast<uintptr_t>(&rng)));
    return rng;
}

/// 从候选词池随机不重复地挑 N 个关键词（N ≤ pool size）。
std::vector<std::string> pickRandomKeywords(int n) {
    n = std::clamp(n, 1, static_cast<int>(kRandomKeywords.size()));
    std::vector<size_t> idx(kRandomKeywords.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::shuffle(idx.begin(), idx.end(), threadLocalRng());
    std::vector<std::string> out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) out.push_back(kRandomKeywords[idx[i]]);
    return out;
}

// ──────────────────────────────────────────────
// 评级所需的轻量上下文
// ──────────────────────────────────────────────
struct ValidateCtx {
    const BookSource& snapshot;
    JsRuntime* js;                     ///< 调用本上下文内 search/verify 必须独占的 JS 运行时
    HttpRequestFunc httpFunc;          ///< 旧式 (url,method,headers,body) -> body
    HttpClientFunc httpClientFunc;     ///< 新式 HttpRequest -> HttpResponse
    int timeoutMs;                     ///< 用户传入的总超时（搜索/抓取上限取 min）
    const GradePolicy* policy;
    std::chrono::steady_clock::time_point deadline;  ///< 整个评级的 hard deadline，
                                                     ///< 任何 HTTP 调用前都 check，
                                                     ///< 已超期就直接放弃（避免最后阶段
                                                     ///< 半死站点把多个 30s timeout 串起来）。
};

/// 计算本次 HTTP 应该用的真实 timeoutMs：取 (用户配置, 书源配置, deadline 剩余) 三者最小。
/// 已超 deadline 返回 0，调用方应直接放弃。
int computeRequestTimeoutMs(const ValidateCtx& ctx) {
    int budget = std::min(ctx.timeoutMs, ctx.snapshot.timeout);
    auto now = std::chrono::steady_clock::now();
    if (now >= ctx.deadline) return 0;
    int remain = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(ctx.deadline - now).count());
    return std::max(50, std::min(budget, remain));  // 至少 50ms，避免 0/极小超时
}

bool deadlineExceeded(const ValidateCtx& ctx) {
    return std::chrono::steady_clock::now() >= ctx.deadline;
}

struct SearchOutcome {
    std::vector<std::string> bookItems;
    std::string baseUrl;
    bool gotResponse = false;
    int searchLatencyMs = 0;           ///< 该次搜索本身耗时（含 bookList 解析）
    bool analyzeFailed = false;        ///< AnalyzeUrl 解析失败（书源 URL 模板有问题，
                                       ///< 通常是 {{js}} 表达式异常，不是网络错）
    std::string error;                 ///< 失败原因（解析或抓取层面）
};

/// 统一 HTTP GET（兼容 client / old func 两条路径），耗时回填到 outLatencyMs。
/// **respect deadline**: 已超 deadline 直接返回 ""，不再发请求。
std::string httpGet(const ValidateCtx& ctx, const std::string& rawUrl, int& outLatencyMs) {
    outLatencyMs = 0;
    if (rawUrl.empty()) return "";
    if (deadlineExceeded(ctx)) return "";

    int reqTimeout = computeRequestTimeoutMs(ctx);
    if (reqTimeout <= 0) return "";

    auto t0 = std::chrono::steady_clock::now();
    try {
        AnalyzeUrl a(rawUrl, ctx.snapshot.url, "", 1, ctx.js);
        auto an = a.result();

        std::string body;
        if (ctx.httpClientFunc) {
            HttpRequest req;
            req.url = an.url;
            req.method = an.method;
            req.body = an.body;
            req.timeoutMs = reqTimeout;
            for (auto& [k, v] : an.headers) req.headers[k] = v;
            for (auto& [k, v] : ctx.snapshot.headers) req.headers[k] = v;
            auto resp = ctx.httpClientFunc(req);
            if (resp.statusCode >= 200 && resp.statusCode < 400) body = resp.body;
        } else if (ctx.httpFunc) {
            std::string h = "{}";
            if (!an.headers.empty()) {
                json hj = json::object();
                for (auto& [k, v] : an.headers) hj[k] = v;
                h = detail::safeDump(hj);
            }
            body = ctx.httpFunc(an.url, an.method, h, an.body);
        }
        auto t1 = std::chrono::steady_clock::now();
        outLatencyMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        return body;
    } catch (...) {
        auto t1 = std::chrono::steady_clock::now();
        outLatencyMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        return "";
    }
}

/// 单关键词搜索：发请求 + 解析 bookList。
/// **整个流程包在 try/catch 里**，AnalyzeUrl 解析异常不会冲到 caller。
/// 失败时 analyzeFailed/error 标志位会带上，便于诊断。
SearchOutcome searchOnce(const ValidateCtx& ctx, const std::string& kw) {
    SearchOutcome out;
    auto t0 = std::chrono::steady_clock::now();

    auto markLatencyAndReturn = [&]() {
        auto t1 = std::chrono::steady_clock::now();
        out.searchLatencyMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        return out;
    };

    if (deadlineExceeded(ctx)) {
        out.error = "deadline before search";
        return markLatencyAndReturn();
    }
    int reqTimeout = computeRequestTimeoutMs(ctx);
    if (reqTimeout <= 0) {
        out.error = "deadline";
        return markLatencyAndReturn();
    }

    std::string response;
    try {
        AnalyzeUrl analyzer(ctx.snapshot.searchUrl, ctx.snapshot.url, kw, 1, ctx.js);
        auto analyzed = analyzer.result();
        out.baseUrl = analyzed.url;

        if (ctx.httpClientFunc) {
            HttpRequest req;
            req.url = analyzed.url;
            req.method = analyzed.method;
            req.body = analyzed.body;
            req.timeoutMs = reqTimeout;
            for (auto& [k, v] : analyzed.headers) req.headers[k] = v;
            for (auto& [k, v] : ctx.snapshot.headers) req.headers[k] = v;
            auto resp = ctx.httpClientFunc(req);
            if (resp.statusCode >= 200 && resp.statusCode < 400) {
                response = resp.body;
            } else if (!resp.error.empty()) {
                out.error = resp.error;
            }
        } else if (ctx.httpFunc) {
            std::string headersJson = "{}";
            if (!analyzed.headers.empty()) {
                json h = json::object();
                for (auto& [k, v] : analyzed.headers) h[k] = v;
                headersJson = detail::safeDump(h);
            }
            response = ctx.httpFunc(analyzed.url, analyzed.method, headersJson, analyzed.body);
        }
    } catch (const std::exception& e) {
        out.analyzeFailed = true;
        out.error = std::string("analyzeUrl: ") + e.what();
        return markLatencyAndReturn();
    } catch (...) {
        out.analyzeFailed = true;
        out.error = "analyzeUrl: unknown exception";
        return markLatencyAndReturn();
    }

    if (!response.empty()) {
        out.gotResponse = true;
        try {
            out.bookItems = detail::applyRuleStatic(
                response, ctx.snapshot.searchRule.bookList, ctx.js, out.baseUrl);
        } catch (...) {
            out.bookItems.clear();
            out.error = "bookList rule failed";
        }
    }
    return markLatencyAndReturn();
}

/// 验证单本书：抓目录(≥policy.minChapters 章) + 第 1 章正文(≥policy.minContentBytes 字节)。
/// outBookLatencyMs 包含本次验证的所有 HTTP 耗时；返回 true 仅当全部满足。
bool verifyOneBook(const ValidateCtx& ctx,
                   const std::string& bookUrl,
                   int& outBookLatencyMs) {
    outBookLatencyMs = 0;
    if (bookUrl.empty()) return false;

    int catLatency = 0;
    std::string catHtml = httpGet(ctx, bookUrl, catLatency);
    outBookLatencyMs += catLatency;
    if (catHtml.empty()) return false;

    AnalyzeUrl ca(bookUrl, ctx.snapshot.url, "", 1, ctx.js);
    std::string catBase = ca.result().url;
    auto& catRule = ctx.snapshot.catalogRule;

    auto chapterItems = detail::applyRuleStatic(
        catHtml, catRule.chapterList, ctx.js, catBase);

    // tocUrl 回退：详情页指向另一个目录页的场景
    if (chapterItems.empty() && !ctx.snapshot.bookInfoRule.tocUrl.empty()) {
        auto tocCands = detail::applyRuleStatic(
            catHtml, ctx.snapshot.bookInfoRule.tocUrl, ctx.js, catBase);
        if (!tocCands.empty()) {
            std::string tocUrl = detail::resolveUrlWithBase(tocCands[0], catBase, ctx.js);
            if (!tocUrl.empty()) {
                int tocLatency = 0;
                std::string tocHtml = httpGet(ctx, tocUrl, tocLatency);
                outBookLatencyMs += tocLatency;
                if (!tocHtml.empty()) {
                    catBase = tocUrl;
                    catHtml = tocHtml;
                    chapterItems = detail::applyRuleStatic(
                        catHtml, catRule.chapterList, ctx.js, catBase);
                }
            }
        }
    }

    if (static_cast<int>(chapterItems.size()) < ctx.policy->minChapters) return false;

    // 取第 1 章 URL
    std::string rawChUrl = detail::extractFieldWithTemplateFallback(
        chapterItems[0], catRule.chapterUrl,
        [&](const std::string& c, const std::string& r) {
            return detail::applyRuleStatic(c, r, ctx.js, catBase);
        });
    std::string chapterUrl = detail::resolveUrlWithBase(rawChUrl, catBase, ctx.js);
    if (chapterUrl.empty()) {
        chapterUrl = detail::extractHrefFallback(chapterItems[0], catBase, ctx.js);
    }
    if (chapterUrl.empty()) return false;

    int contentLatency = 0;
    std::string contentHtml = httpGet(ctx, chapterUrl, contentLatency);
    outBookLatencyMs += contentLatency;
    if (contentHtml.empty()) return false;

    auto extracted = detail::applyRuleStatic(
        contentHtml, ctx.snapshot.contentRule.content, ctx.js, chapterUrl);
    int totalLen = 0;
    for (const auto& part : extracted) totalLen += static_cast<int>(part.size());
    return totalLen >= ctx.policy->minContentBytes;
}

/// 遍历某次搜索结果的前 N 本书，任一通过即返回 true。
/// 起点随机偏移，避开置顶广告/付费推广。outBestSuccessLatency 只记成功那本的 verify 耗时。
bool tryBooksOfList(const ValidateCtx& ctx,
                    const std::vector<std::string>& items,
                    const std::string& searchBase,
                    int& outBestSuccessLatency,
                    int& outTried) {
    auto& rule = ctx.snapshot.searchRule;
    int totalItems = static_cast<int>(items.size());
    if (totalItems == 0) return false;

    int maxTry = std::min(totalItems, ctx.policy->maxBooksToTry);
    int offset = 0;
    if (ctx.policy->randomBookOffset && totalItems > 1) {
        std::uniform_int_distribution<int> dist(0, totalItems - 1);
        offset = dist(threadLocalRng());
    }

    for (int k = 0; k < maxTry; ++k) {
        if (deadlineExceeded(ctx)) break;  // 超期立即放弃后续书

        int bi = (offset + k) % totalItems;
        std::string rawBookUrl = detail::extractFieldWithTemplateFallback(
            items[bi], rule.bookUrl,
            [&](const std::string& content, const std::string& r) -> std::vector<std::string> {
                if (r.empty()) return {};
                return detail::applyRuleStatic(content, r, ctx.js, searchBase);
            });
        std::string bookUrl = detail::resolveUrlWithBase(rawBookUrl, searchBase, ctx.js);
        if (bookUrl.empty()) {
            bookUrl = detail::extractHrefFallback(items[bi], searchBase, ctx.js);
        }
        if (bookUrl.empty()) continue;

        ++outTried;
        int bookLatency = 0;
        try {
            if (verifyOneBook(ctx, bookUrl, bookLatency)) {
                outBestSuccessLatency = bookLatency;
                return true;
            }
        } catch (...) { /* 这本失败，下一本 */ }
    }
    return false;
}

// ──────────────────────────────────────────────
// 单 kw 路径的产出（async 子任务的返回值）
// ──────────────────────────────────────────────
struct KwPathResult {
    std::string keyword;
    bool gotResponse = false;
    int totalResults = 0;
    bool success = false;          ///< 该 kw 路径是否最终通过
    int pathLatencyMs = INT_MAX;   ///< success=true 时 = searchMs + bookMs
    int searchLatencyMs = 0;       ///< 该 kw 的 search 段耗时（含解析/异常）
    int triedBooks = 0;
    bool analyzeFailed = false;    ///< AnalyzeUrl 解析失败（书源 URL 模板坏）
    std::string error;             ///< 失败的诊断信息
};

/// 跑单个关键词的完整路径：search → tryBooksOfList。**纯函数**。
KwPathResult runKwPath(const ValidateCtx& ctx, const std::string& kw) {
    KwPathResult r;
    r.keyword = kw;
    SearchOutcome s = searchOnce(ctx, kw);
    r.gotResponse = s.gotResponse;
    r.totalResults = static_cast<int>(s.bookItems.size());
    r.searchLatencyMs = s.searchLatencyMs;
    r.analyzeFailed = s.analyzeFailed;
    r.error = s.error;
    if (s.bookItems.empty()) return r;

    int succLat = 0;
    if (tryBooksOfList(ctx, s.bookItems, s.baseUrl, succLat, r.triedBooks)) {
        r.success = true;
        r.pathLatencyMs = s.searchLatencyMs + succLat;
    }
    return r;
}

// ──────────────────────────────────────────────
// 评级结果三元组
// ──────────────────────────────────────────────
struct GradeResult {
    SourceValidity grade = SourceValidity::Invalid;
    int latencyMs = 0;
    std::string detail;
};

} // anonymous namespace

// ──────────────────────────────────────────────
// 评级核心：N 个 kw 真并行（std::async），最短成功路径取胜
//   js 池的获取 / 释放交给 jsProvider/jsReleaser —— 测试可注入假 provider，
//   生产用时由 worker 提供"从 Impl::acquireJsRuntime 拿 / release 回去"。
//
//   返回 grade + latencyMs（成功=最短路径；失败=并发墙钟）+ detail
//
//   暴露在 namespace ariaread 但不在 .h —— 头文件里 forward declare 给 test.
// ──────────────────────────────────────────────
struct JsHolder {
    std::function<std::unique_ptr<JsRuntime>()> acquire;
    std::function<void(std::unique_ptr<JsRuntime>)> release;
};

namespace {

GradeResult gradeOneSource(const BookSource& snapshot,
                           HttpRequestFunc httpFunc,
                           HttpClientFunc httpClientFunc,
                           int timeoutMs,
                           const GradePolicy& policy,
                           JsHolder& jsHolder,
                           std::chrono::steady_clock::time_point startTime) {
    GradeResult r;

    // 1. 抽 N 个不重复关键词
    auto keywords = pickRandomKeywords(policy.numKeywords);
    if (keywords.empty()) {
        r.grade = SourceValidity::Invalid;
        r.detail = "no keyword candidates";
        return r;
    }

    // 单源 hard deadline：startTime + sourceBudgetMs。
    // 所有 HTTP 调用都看 deadline，超过即立刻放弃，不再发请求。
    // 这是修复"最后几个源卡几十秒"的关键 —— 评级用最短成功路径（快路径
    // 的源 200~3000ms 就过完了），慢的死站点最坏也只阻塞 sourceBudgetMs。
    auto deadline = startTime + std::chrono::milliseconds(policy.sourceBudgetMs);

    // 2. 每个 kw 起一个 std::async（使用 std::launch::async 强制开线程）
    //    每个 async 任务独占一个 JsRuntime（从池里 acquire / release）。
    std::vector<std::future<KwPathResult>> futures;
    futures.reserve(keywords.size());
    for (const auto& kw : keywords) {
        futures.emplace_back(std::async(std::launch::async, [&, kw]() -> KwPathResult {
            auto js = jsHolder.acquire();
            JsRuntime* jsPtr = js.get();
            KwPathResult kwr;
            try {
                ValidateCtx ctx{snapshot, jsPtr, httpFunc, httpClientFunc,
                                timeoutMs, &policy, deadline};
                kwr = runKwPath(ctx, kw);
            } catch (...) {
                // 该 kw 路径异常 → 视为失败，但保留 keyword 以便 detail 输出
                kwr.keyword = kw;
            }
            jsHolder.release(std::move(js));
            return kwr;
        }));
    }

    // 3. 等待所有 future（必须等齐，因为我们要"最短成功路径"，提前退出会偏向快但失败的）
    //    注意：所有 kw 的 HTTP 都在并发跑；真实墙钟 ≈ 最慢那个 kw 的耗时
    std::vector<KwPathResult> results;
    results.reserve(futures.size());
    for (auto& f : futures) results.push_back(f.get());

    // 4. 选最短成功路径
    int bestPathMs = INT_MAX;
    std::string bestKw;
    bool anyResponse = false;
    bool allAnalyzeFailed = !results.empty();  // 所有 kw 都死在 AnalyzeUrl 阶段
    int totalResults = 0;
    int triedBooks = 0;
    int maxSearchLatency = 0;
    std::string firstError;
    for (auto& kr : results) {
        anyResponse = anyResponse || kr.gotResponse;
        totalResults += kr.totalResults;
        triedBooks += kr.triedBooks;
        if (kr.searchLatencyMs > maxSearchLatency) maxSearchLatency = kr.searchLatencyMs;
        if (!kr.analyzeFailed) allAnalyzeFailed = false;
        if (firstError.empty() && !kr.error.empty()) firstError = kr.error;
        if (kr.success && kr.pathLatencyMs < bestPathMs) {
            bestPathMs = kr.pathLatencyMs;
            bestKw = kr.keyword;
        }
    }

    bool anyOk = (bestPathMs != INT_MAX);
    int displayMs;
    if (anyOk) {
        displayMs = bestPathMs;
    } else {
        // 失败时 displayMs = 真实墙钟（避免出现 "0ms invalid" 这种诈尸态）
        auto nowT = std::chrono::steady_clock::now();
        int wallMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(nowT - startTime).count());
        // 如果墙钟 < searchLatency（理论上不会，但防御一下），用 searchLatency 兜
        displayMs = std::max(wallMs, maxSearchLatency);
        // 至少给 1ms，让 UI 能区分"0 = 没跑"和"~1ms = 立即失败"
        if (displayMs <= 0) displayMs = 1;
    }
    r.latencyMs = displayMs;

    // 5. 拼出 kws 列表（给 detail 用）
    std::string kwsList;
    for (size_t i = 0; i < keywords.size(); ++i) {
        if (i > 0) kwsList += "/";
        kwsList += "'" + keywords[i] + "'";
    }

    if (anyOk) {
        if (displayMs < policy.excellentMs) {
            r.grade = SourceValidity::Excellent;
            r.detail = "excellent (" + std::to_string(displayMs) + "ms, kw='" + bestKw + "')";
        } else if (displayMs <= policy.goodMs) {
            r.grade = SourceValidity::Good;
            r.detail = "good (" + std::to_string(displayMs) + "ms, kw='" + bestKw + "')";
        } else if (displayMs <= policy.poorMs) {
            r.grade = SourceValidity::Poor;
            r.detail = "poor (" + std::to_string(displayMs) + "ms, kw='" + bestKw + "')";
        } else {
            r.grade = SourceValidity::Invalid;
            r.detail = "too slow (" + std::to_string(displayMs) + "ms, kw='" + bestKw + "')";
        }
    } else {
        r.grade = SourceValidity::Invalid;
        if (allAnalyzeFailed) {
            // 所有 kw 都在 AnalyzeUrl 阶段抛异常 → 通常是 searchUrl 模板坏
            // （JS 表达式异常 / @js: / cookie.removeCookie 等）
            r.detail = "rule error: " + (firstError.empty() ? std::string("URL template failed")
                                                            : firstError)
                       + " (" + std::to_string(displayMs) + "ms)";
        } else if (!anyResponse) {
            r.detail = "no response from server (" + std::to_string(displayMs) + "ms)";
        } else if (totalResults == 0) {
            r.detail = "no search results for " + kwsList
                       + " (" + std::to_string(displayMs) + "ms)";
        } else {
            r.detail = "no book passed catalog+content (tried " + std::to_string(triedBooks)
                       + " books across " + std::to_string(keywords.size())
                       + " keywords, " + std::to_string(displayMs) + "ms)";
        }
    }
    return r;
}

// ──────────────────────────────────────────────
// 任务结果落盘：sources[i] / DB / callback —— 三件套统一封装。
// 用 templated 是因为 BookSourceEngine::Impl 是 private —— 模板实例化点延迟到
// 类成员可见处。编译期决定，无运行时代价。
// ──────────────────────────────────────────────
template <typename ImplT>
struct CommitArgs {
    ImplT* implPtr;
    std::shared_ptr<std::atomic<bool>> aliveFlag;
    std::shared_ptr<std::mutex> callbackMutex;
    ConcurrentValidateCallback callback;
    size_t taskIndex;
    std::string taskName;
    std::string taskUrl;
};

template <typename ImplT>
void commitSourceResult(const CommitArgs<ImplT>& a,
                        SourceValidity grade,
                        int latencyMs,
                        const std::string& detail) {
    if (!a.aliveFlag->load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(a.implPtr->sourcesMutex);
        if (a.taskIndex < a.implPtr->sources.size()) {
            a.implPtr->sources[a.taskIndex].validity = grade;
            a.implPtr->sources[a.taskIndex].latencyMs = latencyMs;
        }
    }
    if (a.implPtr->db) {
        try { a.implPtr->db->updateValidity(a.taskUrl, grade, latencyMs); } catch (...) {}
    }
    if (a.callback) {
        std::lock_guard<std::mutex> lock(*a.callbackMutex);
        a.callback(a.taskIndex, a.taskName, grade, latencyMs, detail);
    }
}

} // anonymous namespace

// ──────────────────────────────────────────────
// 串行版本（保留兼容）
// ──────────────────────────────────────────────
std::pair<int, int> BookSourceEngine::validateSources(
    const std::string& testQuery,
    int /*timeoutMs*/,
    SourceValidateCallback callback
) {
    int validCount = 0;
    int invalidCount = 0;

    for (size_t i = 0; i < pImpl->sources.size(); ++i) {
        auto& source = pImpl->sources[i];
        if (source.searchUrl.empty()) continue;

        source.validity = SourceValidity::Unknown;
        if (callback) callback(i, source.name, SourceValidity::Unknown, "");

        try {
            pImpl->currentSourceIndex = i;
            // 串行版：任意挑 1 个关键词即可，主要做兼容性兜底
            std::string q = testQuery.empty() ? pickRandomKeywords(1)[0] : testQuery;
            auto results = search(q);
            source.validity = SourceValidity::Good;
            ++validCount;
            if (callback) {
                callback(i, source.name, SourceValidity::Good,
                         std::to_string(results.size()) + " results");
            }
        } catch (const std::exception& e) {
            source.validity = SourceValidity::Invalid;
            ++invalidCount;
            if (callback) callback(i, source.name, SourceValidity::Invalid, e.what());
        } catch (...) {
            source.validity = SourceValidity::Invalid;
            ++invalidCount;
            if (callback) callback(i, source.name, SourceValidity::Invalid, "unknown error");
        }
    }
    return {validCount, invalidCount};
}

// ──────────────────────────────────────────────
// 并发版本：仅做线程池调度 + 超时兜底；评级算法在 gradeOneSource()。
// ──────────────────────────────────────────────
void BookSourceEngine::validateSourcesConcurrent(
    const std::string& /*testQuery*/,
    int timeoutMs,
    int concurrency,
    ConcurrentValidateCallback callback,
    ConcurrentDoneCallback doneCallback,
    std::shared_ptr<CancelToken> cancelToken
) {
    if (concurrency <= 0) concurrency = pImpl->defaultConcurrency;

    GradePolicy policy;  // 默认策略；未来可通过 setter 接受外部注入

    // 1. 收集任务 + 快照
    struct SourceTask {
        size_t index;
        std::string name;
        std::string url;
        BookSource snapshot;
    };
    std::vector<SourceTask> tasks;
    for (size_t i = 0; i < pImpl->sources.size(); ++i) {
        const auto& s = pImpl->sources[i];
        if (s.searchUrl.empty()) continue;
        tasks.push_back({i, s.name, s.url, s});
    }
    if (tasks.empty()) {
        if (doneCallback) doneCallback(0, 0, 0);
        return;
    }

    // 2. 共享状态
    auto validCount     = std::make_shared<std::atomic<int>>(0);
    auto invalidCount   = std::make_shared<std::atomic<int>>(0);
    auto completedCount = std::make_shared<std::atomic<int>>(0);
    auto callbackMutex  = std::make_shared<std::mutex>();

    auto httpFunc = pImpl->httpFunc;
    auto httpClientFunc = pImpl->httpClientFunc;

    // 3. 预热 JsRuntime 池：单 worker 同时跑 N 个 kw 子任务，每个子任务独占 1 个 js
    //    所以池容量需要 ~ concurrency × policy.numKeywords。
    pImpl->warmUpJsPool(concurrency * policy.numKeywords);
    auto implPtr = pImpl.get();
    auto aliveFlag = pImpl->aliveFlag;

    // 4. 单任务执行：起 N 个 async kw 路径，最短成功胜
    auto runOneTask = [=, &policy](const SourceTask& task) {
        auto startTime = std::chrono::steady_clock::now();

        struct CompleteGuard {
            std::shared_ptr<std::atomic<int>> counter;
            ~CompleteGuard() { counter->fetch_add(1); }
        } completeGuard{completedCount};

        CommitArgs<BookSourceEngine::Impl> cargs{
            implPtr, aliveFlag, callbackMutex, callback,
            task.index, task.name, task.url};

        try {
            JsHolder jsHolder{
                [implPtr]() { return implPtr->acquireJsRuntime(); },
                [implPtr, aliveFlag](std::unique_ptr<JsRuntime> rt) {
                    if (aliveFlag->load(std::memory_order_acquire)) {
                        implPtr->releaseJsRuntime(std::move(rt));
                    } else {
                        rt.reset();
                    }
                }
            };
            GradeResult gr = gradeOneSource(task.snapshot, httpFunc, httpClientFunc,
                                            timeoutMs, policy, jsHolder, startTime);
            if (gr.grade == SourceValidity::Invalid) invalidCount->fetch_add(1);
            else validCount->fetch_add(1);
            commitSourceResult(cargs, gr.grade, gr.latencyMs, gr.detail);
        } catch (const std::exception& e) {
            int lat = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count());
            invalidCount->fetch_add(1);
            commitSourceResult(cargs, SourceValidity::Invalid, lat, e.what());
        } catch (...) {
            int lat = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count());
            invalidCount->fetch_add(1);
            commitSourceResult(cargs, SourceValidity::Invalid, lat, "unknown error");
        }
    };

    // 5. 计算总超时（用于 watchdog 取消 + 兜底标记）
    int threadCount = std::min(concurrency, static_cast<int>(tasks.size()));
    const int SINGLE_TASK_MAX_SEC = pImpl->validateMaxTaskSec;
    const int AVG_TASK_SEC = 8;
    int rounds = (static_cast<int>(tasks.size()) + threadCount - 1) / threadCount;
    int totalTimeoutSec = std::max(45, rounds * AVG_TASK_SEC + SINGLE_TASK_MAX_SEC);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(totalTimeoutSec);

    // 6. 并发执行。取消谓词同时覆盖：引擎析构 / 外部取消 / 总超时。
    //    超时后仅停止"领取新任务"，已在执行的卡死 HTTP 由其自身 timeoutMs 收口；
    //    不再 detach 线程（消除 UAF 风险），parallelForEach 阻塞到全部 worker 收尾。
    auto cancelPred = [aliveFlag, cancelToken, deadline]() -> bool {
        if (!aliveFlag->load(std::memory_order_acquire)) return true;
        if (cancelToken && cancelToken->isCancelled()) return true;
        return std::chrono::steady_clock::now() >= deadline;
    };

    detail::parallelForEach(
        tasks.size(), concurrency,
        [&](std::size_t i) { runOneTask(tasks[i]); },
        cancelPred);

    // 7. 兜底：把因取消/超时而仍为 Unknown 的源强制标记 Invalid。
    //    （正常完成的源已在 commitSourceResult 中落地，这里只补未完成的。）
    if (completedCount->load() < static_cast<int>(tasks.size())) {
        for (size_t i = 0; i < tasks.size(); ++i) {
            const auto& task = tasks[i];
            bool needForce = false;
            {
                std::lock_guard<std::mutex> lock(implPtr->sourcesMutex);
                if (task.index < implPtr->sources.size()
                    && implPtr->sources[task.index].validity == SourceValidity::Unknown) {
                    implPtr->sources[task.index].validity = SourceValidity::Invalid;
                    implPtr->sources[task.index].latencyMs = totalTimeoutSec * 1000;
                    needForce = true;
                }
            }
            if (needForce) {
                if (implPtr->db) {
                    try {
                        implPtr->db->updateValidity(task.url, SourceValidity::Invalid,
                                                    totalTimeoutSec * 1000);
                    } catch (...) {}
                }
                invalidCount->fetch_add(1);
                if (callback) {
                    std::lock_guard<std::mutex> lock(*callbackMutex);
                    callback(task.index, task.name, SourceValidity::Invalid,
                             totalTimeoutSec * 1000, "timeout (stuck in HTTP)");
                }
            }
        }
    }

    if (doneCallback) {
        doneCallback(validCount->load(), invalidCount->load(), 0);
    }
}

} // namespace ariaread

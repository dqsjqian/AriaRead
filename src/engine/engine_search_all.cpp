/// @file engine_search_all.cpp
/// @brief 多源搜索：串行多源搜索 + 并发多源搜索

#include "ariaread/engine_impl.h"
#include "ariaread/parallel.h"

#include <cstring>
#include <algorithm>
#include <unordered_set>

namespace ariaread {

using json = nlohmann::json;

// ──────────────────────────────────────────────
// 多源搜索（串行版本，保留兼容）
// ──────────────────────────────────────────────
BookSourceEngine::SearchAllResult BookSourceEngine::searchAll(
    const std::string& keyword,
    SearchAllCallback callback
) {
    SearchAllResult result;

    std::vector<size_t> validIdx;
    std::vector<size_t> unknownIdx;

    for (size_t i = 0; i < pImpl->sources.size(); ++i) {
        const auto& s = pImpl->sources[i];
        if (s.searchUrl.empty()) continue;

        // 只从优、良、未知中搜索
        if (s.validity == SourceValidity::Excellent ||
            s.validity == SourceValidity::Good) {
            validIdx.push_back(i);
        } else if (s.validity == SourceValidity::Unknown) {
            unknownIdx.push_back(i);
        }
        // 差、无效跳过
    }

    std::vector<size_t> searchOrder;
    searchOrder.reserve(validIdx.size() + unknownIdx.size());
    searchOrder.insert(searchOrder.end(), validIdx.begin(), validIdx.end());
    searchOrder.insert(searchOrder.end(), unknownIdx.begin(), unknownIdx.end());

    result.totalSources = static_cast<int>(searchOrder.size());

    for (size_t idx : searchOrder) {
        const auto& source = pImpl->sources[idx];

        try {
            pImpl->currentSourceIndex = idx;
            auto books = search(keyword);

            pImpl->sources[idx].validity = SourceValidity::Good;
            result.totalBooks += static_cast<int>(books.size());

            if (callback) {
                callback(idx, source.name, books, "");
            }
        } catch (const std::exception& e) {
            ++result.totalErrors;
            if (callback) {
                callback(idx, source.name, {}, e.what());
            }
        } catch (...) {
            ++result.totalErrors;
            if (callback) {
                callback(idx, source.name, {}, "unknown error");
            }
        }
    }

    return result;
}

// ──────────────────────────────────────────────
// 并发多源搜索
// 设计：线程池并发搜索，结果通过回调逐源推送
// ──────────────────────────────────────────────
void BookSourceEngine::searchAllConcurrent(
    const std::string& keyword,
    int concurrency,
    ConcurrentSearchCallback callback,
    ConcurrentDoneCallback doneCallback,
    std::shared_ptr<CancelToken> cancelToken,
    bool matchName,
    bool matchAuthor,
    bool matchIntro,
    const std::vector<std::string>& sourceNames
) {
    if (concurrency <= 0) concurrency = pImpl->defaultConcurrency;

    // 1. 收集搜索任务：有效源优先，无效源跳过
    struct SearchTask {
        size_t index;
        std::string name;
        BookSource snapshot;
    };

    // 如果指定了 sourceNames，构建快速查找集合
    std::unordered_set<std::string> nameFilter(sourceNames.begin(), sourceNames.end());
    const bool filterByName = !nameFilter.empty();

    std::vector<SearchTask> tasks;
    for (size_t i = 0; i < pImpl->sources.size(); ++i) {
        const auto& s = pImpl->sources[i];
        if (s.searchUrl.empty()) continue;
        // 如果指定了书源名称列表，只搜索列表中的书源
        if (filterByName) {
            if (nameFilter.find(s.name) == nameFilter.end()) continue;
        } else {
            // 未指定名称列表时，只从优、良、未知中搜索（差和无效跳过）
            if (s.validity == SourceValidity::Invalid || s.validity == SourceValidity::Poor) continue;
        }
        tasks.push_back({i, s.name, s});
    }

    // 按分级排序：优 > 良 > 未知（差和无效已被跳过）
    std::stable_sort(tasks.begin(), tasks.end(), [](const SearchTask& a, const SearchTask& b) {
        auto gradeOrder = [](SourceValidity v) -> int {
            switch (v) {
                case SourceValidity::Excellent: return 0;
                case SourceValidity::Good:      return 1;
                case SourceValidity::Unknown:   return 2;
                default:                         return 3;
            }
        };
        int ga = gradeOrder(a.snapshot.validity);
        int gb = gradeOrder(b.snapshot.validity);
        if (ga != gb) return ga < gb;
        int la = a.snapshot.latencyMs >= 0 ? a.snapshot.latencyMs : 999999;
        int lb = b.snapshot.latencyMs >= 0 ? b.snapshot.latencyMs : 999999;
        return la < lb;
    });

    if (tasks.empty()) {
        if (doneCallback) doneCallback(0, 0, 0);
        return;
    }

    // 2. 共享状态
    auto totalBooks = std::make_shared<std::atomic<int>>(0);
    auto totalErrors = std::make_shared<std::atomic<int>>(0);
    auto callbackMutex = std::make_shared<std::mutex>();

    auto httpFunc = pImpl->httpFunc;
    auto httpClientFunc = pImpl->httpClientFunc;

    // 3. 单任务体（在 worker 线程并发执行）。每个 worker 线程持有自己的
    //    JsRuntime（thread_local，首次用到时惰性创建），对齐原"每线程一个 js"语义。
    auto runTask = [&](std::size_t taskIdx) {
        thread_local std::unique_ptr<JsRuntime> threadJsTls;
        if (!threadJsTls) {
            threadJsTls = std::make_unique<JsRuntime>();
            threadJsTls->setMemoryLimit(64 * 1024 * 1024);
            threadJsTls->setStackSize(2 * 1024 * 1024);
        }
        // thread_local 不能被 lambda 按引用捕获，取裸指针供内部闭包使用。
        JsRuntime* threadJs = threadJsTls.get();

        const auto& task = tasks[taskIdx];
        auto startTime = std::chrono::steady_clock::now();

        try {
                AnalyzeUrl analyzer(task.snapshot.searchUrl, task.snapshot.url,
                                    keyword, 1, threadJs);
                auto analyzed = analyzer.result();

                std::string response;
                if (httpClientFunc) {
                    HttpRequest req;
                    req.url = analyzed.url;
                    req.method = analyzed.method;
                    req.body = analyzed.body;
                    req.timeoutMs = task.snapshot.timeout;
                    for (auto& [k, v] : analyzed.headers) req.headers[k] = v;
                    for (auto& [k, v] : task.snapshot.headers) req.headers[k] = v;
                    auto resp = httpClientFunc(req);
                    if (resp.statusCode >= 200 && resp.statusCode < 400) {
                        response = resp.body;
                    }
                } else if (httpFunc) {
                    std::string headersJson = "{}";
                    if (!analyzed.headers.empty()) {
                        json h = json::object();
                        for (auto& [k, v] : analyzed.headers) h[k] = v;
                        headersJson = detail::safeDump(h);
                    }
                    response = httpFunc(analyzed.url, analyzed.method,
                                        headersJson, analyzed.body);
                }

                auto endTime = std::chrono::steady_clock::now();
                int latencyMs = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());

                if (response.empty()) {
                    totalErrors->fetch_add(1);
                    if (callback) {
                        std::lock_guard<std::mutex> lock(*callbackMutex);
                        callback(task.index, task.name, {}, latencyMs, "empty response");
                    }
                    return;
                }

                // 解析搜索结果
                auto& rule = task.snapshot.searchRule;
                std::vector<Book> books;

                std::vector<std::string> bookItems = detail::applyRuleStatic(
                    response, rule.bookList, threadJs, analyzed.url);

                for (auto& item : bookItems) {
                    Book book;
                    auto extractRule = [&threadJs, &analyzed](const std::string& content, const std::string& fieldRule) -> std::vector<std::string> {
                        if (fieldRule.empty()) return {};
                        return detail::applyRuleStatic(content, fieldRule, threadJs, analyzed.url);
                    };

                    auto extractField = [&](const std::string& content, const std::string& fieldRule) -> std::string {
                        auto vals = extractRule(content, fieldRule);
                        return vals.empty() ? std::string() : vals[0];
                    };

                    book.name = extractField(item, rule.name);
                    book.author = detail::cleanAuthorField(extractField(item, rule.author));
                    book.coverUrl = extractField(item, rule.coverUrl);
                    std::string rawBookUrl = detail::extractFieldWithTemplateFallback(
                        item,
                        rule.bookUrl,
                        [&](const std::string& content, const std::string& r) {
                            return extractRule(content, r);
                        }
                    );
                    book.bookUrl = detail::resolveUrlWithBase(rawBookUrl, analyzed.url, threadJs);
                    if (book.bookUrl.empty()) {
                        book.bookUrl = detail::extractHrefFallback(item, analyzed.url, threadJs);
                    }
                    book.lastChapter = extractField(item, rule.lastChapter);
                    book.intro = extractField(item, rule.intro);
                    book.kind = extractField(item, rule.kind);

                    if (!book.name.empty()) {
                        const bool effectiveMatchName = (matchName || matchAuthor || matchIntro) ? matchName : true;
                        int score = detail::bookMatchScore(book, keyword, effectiveMatchName, matchAuthor, matchIntro);
                        if (score <= 0) {
                            continue;
                        }
                        book.matchScore = score;
                        books.push_back(std::move(book));
                    }
                }

                totalBooks->fetch_add(static_cast<int>(books.size()));

                if (callback) {
                    std::lock_guard<std::mutex> lock(*callbackMutex);
                    callback(task.index, task.name, books, latencyMs, "");
                }

        } catch (const std::exception& e) {
            auto endTime = std::chrono::steady_clock::now();
            int latencyMs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());

            totalErrors->fetch_add(1);
            if (callback) {
                std::lock_guard<std::mutex> lock(*callbackMutex);
                callback(task.index, task.name, {}, latencyMs, e.what());
            }
        } catch (...) {
            auto endTime = std::chrono::steady_clock::now();
            int latencyMs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());

            totalErrors->fetch_add(1);
            if (callback) {
                std::lock_guard<std::mutex> lock(*callbackMutex);
                callback(task.index, task.name, {}, latencyMs, "unknown error");
            }
        }
    };

    // 4. 并发执行所有任务（取消由 cancelToken 驱动）
    detail::parallelForEach(
        tasks.size(), concurrency, runTask,
        cancelToken ? std::function<bool()>([cancelToken]() { return cancelToken->isCancelled(); })
                    : std::function<bool()>{});

    // 5. 完成回调
    if (doneCallback) {
        doneCallback(totalBooks->load(), static_cast<int>(tasks.size()), totalErrors->load());
    }
}

} // namespace ariaread

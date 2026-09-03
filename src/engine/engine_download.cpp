/// @file engine_download.cpp
/// @brief 书架全量下载缓存（B4 拆分：从 engine_bookshelf.cpp 独立出来）

#include "openread/engine_impl.h"
#include "openread/parallel.h"
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <atomic>

namespace openread {

using json = nlohmann::json;

// ──────────────────────────────────────────────
// 全量下载缓存（增量：跳过已缓存章节，并发下载）
// ──────────────────────────────────────────────
BookSourceEngine::DownloadResult BookSourceEngine::downloadBook(
    const std::string& bookUrl, const std::string& sourceUrl,
    int sourceIndex, const std::string& sourceName,
    DownloadProgressCallback callback, int concurrency) {

    DownloadResult result;
    if (concurrency <= 0) concurrency = 4;

    // 第1步：先拷贝 BookSource 快照（短暂持有 engineMutex，避免阻塞其他下载）
    BookSource sourceSnapshot;
    {
        std::lock_guard<std::recursive_mutex> lock(pImpl->engineMutex);
        bool found = false;
        if (!sourceName.empty()) {
            for (size_t i = 0; i < pImpl->sources.size(); ++i) {
                if (pImpl->sources[i].name == sourceName) {
                    sourceSnapshot = pImpl->sources[i];
                    found = true;
                    break;
                }
            }
        }
        if (!found && sourceIndex >= 0 && sourceIndex < static_cast<int>(pImpl->sources.size())) {
            sourceSnapshot = pImpl->sources[sourceIndex];
            found = true;
        }
        if (!found && !pImpl->sources.empty()) {
            sourceSnapshot = pImpl->currentSource();
        }
    }
    // engineMutex 已释放

    // 第2步：获取目录（优先缓存，带质量校验）——不持有 engineMutex
    std::vector<Chapter> chapters;
    if (pImpl->db) {
        chapters = pImpl->db->getCachedCatalog(bookUrl);
        if (chapters.size() == 1 && detail::isBlank(chapters[0].title)) {
            try { pImpl->db->removeCachedCatalog(bookUrl); } catch (...) {}
            chapters.clear();
        }
    }
    if (chapters.empty()) {
        // 使用快照获取目录，避免持有 engineMutex 做网络请求
        chapters = getCatalogForSource(bookUrl, sourceIndex, sourceName);
        if (!chapters.empty() && pImpl->db &&
            !(chapters.size() == 1 && detail::isBlank(chapters[0].title))) {
            pImpl->db->cacheBookCatalog(bookUrl, chapters);
        }
    }

    result.total = static_cast<int>(chapters.size());
    if (result.total == 0) return result;

    auto httpClientFunc = pImpl->httpClientFunc;
    auto httpFunc = pImpl->httpFunc;

    // 第3步：区分已缓存和需要下载的章节
    struct DownloadTask {
        int originalIndex;
        Chapter chapter;
    };
    std::vector<DownloadTask> tasks;
    int preCached = 0;

    for (int i = 0; i < result.total; ++i) {
        const auto& ch = chapters[i];
        std::string existing;
        if (pImpl->db) {
            existing = pImpl->db->getCachedContent(bookUrl, ch.index);
        }
        if (!existing.empty()) {
            preCached++;
        } else {
            tasks.push_back({i, ch});
        }
    }
    result.cached = preCached;

    // 如果全部已缓存，直接返回
    if (tasks.empty()) {
        if (callback) {
            callback(result.total, result.total, result.cached, 0, chapters.back().title, "cached");
        }
        if (pImpl->db && !chapters.empty()) {
            pImpl->db->updateBookshelfLastChapter(
                bookUrl, chapters.back().title, result.total, false);
        }
        {
            std::lock_guard<std::mutex> lock(pImpl->bookStateMutex);
            auto key = Impl::makeBookStateKey(bookUrl, "");
            auto& entry = pImpl->bookStates[key];
            if (entry.snapshot.bookUrl.empty()) {
                entry.snapshot.bookUrl = bookUrl;
                entry.snapshot.sourceUrl = sourceUrl;
                entry.snapshot.sourceName = sourceName;
                entry.snapshot.sourceIndex = sourceIndex;
            }
            entry.snapshot.catalogCached = result.total;
            entry.snapshot.contentCached = result.cached;
            entry.snapshot.totalChapters = result.total;
            entry.snapshot.hasUpdate = false;
            entry.snapshot.fullyCached = true;
            entry.snapshot.lastRefreshAt = Impl::nowUnix();
            entry.snapshot.lastError.clear();
        }
        return result;
    }

    if (callback && preCached > 0) {
        callback(preCached, result.total, result.cached, 0, "跳过已缓存章节", "cached");
    }

    // 第4步：并行下载（线程池，默认 4 并发）
    if (concurrency > 8) concurrency = 8;
    if (concurrency < 1) concurrency = 1;
    int actualConcurrency = std::min(concurrency, static_cast<int>(tasks.size()));

    std::atomic<int> done{preCached};
    std::atomic<int> atomicCached{preCached};
    std::atomic<int> atomicFailed{0};
    std::atomic<bool> debugFileWritten{false};
    std::mutex callbackMutex;
    auto dbPtr = pImpl->db.get();
    // A5: worker 持有 aliveFlag 副本，引擎析构后安全退出
    auto aliveFlag = pImpl->aliveFlag;

    // 预解析 ## 正则替换规则（所有线程共享，只读）
    const auto& rule = sourceSnapshot.contentRule;
    std::string selectorRule = rule.content;
    std::string regexReplace;
    size_t replacePos = rule.content.find("##");
    if (replacePos != std::string::npos) {
        selectorRule = rule.content.substr(0, replacePos);
        regexReplace = rule.content.substr(replacePos + 2);
    }
    std::vector<std::string> regexPatterns;
    if (!regexReplace.empty()) {
        std::string rem = regexReplace;
        size_t ppos;
        while ((ppos = rem.find('|')) != std::string::npos) {
            std::string pat = rem.substr(0, ppos);
            if (!pat.empty()) regexPatterns.push_back(pat);
            rem = rem.substr(ppos + 1);
        }
        if (!rem.empty()) regexPatterns.push_back(rem);
    }

    auto runTask = [&](std::size_t taskIdx) {
        // 线程局部 JsRuntime（每 worker 线程一个），供正文规则中的 @js:/{{js}} 使用
        thread_local std::unique_ptr<JsRuntime> threadJs;
        if (!threadJs) {
            try {
                threadJs = std::make_unique<JsRuntime>();
                threadJs->setMemoryLimit(64 * 1024 * 1024);
                threadJs->setStackSize(2 * 1024 * 1024);
            } catch (...) {
                threadJs.reset();
            }
        }
        {
            // A5: 引擎已析构则放弃
            if (!aliveFlag->load(std::memory_order_acquire)) return;

            const auto& task = tasks[taskIdx];
            const auto& ch = task.chapter;
            std::string status;

            try {
                std::string response;
                int httpStatus = 0;
                for (int attempt = 0; attempt < 2; ++attempt) {
                    if (attempt > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                        log("[WARN][download] 重试第" + std::to_string(attempt) + "次: " + ch.title);
                    }

                    response.clear();
                    httpStatus = 0;

                    if (httpClientFunc) {
                        HttpRequest req;
                        req.url = ch.url;
                        req.method = "GET";
                        req.timeoutMs = sourceSnapshot.timeout;
                        for (const auto& [k, v] : sourceSnapshot.headers) {
                            req.headers[k] = v;
                        }
                        auto resp = httpClientFunc(req);
                        httpStatus = resp.statusCode;
                        if (resp.statusCode >= 200 && resp.statusCode < 400) {
                            response = resp.body;
                        } else {
                            log("[WARN][download] HTTP " + std::to_string(resp.statusCode) + " | url=" + ch.url.substr(0, 80) + " | error=" + resp.error.substr(0, 100));
                        }
                    } else if (httpFunc) {
                        std::string headersJson = "{}";
                        if (!sourceSnapshot.headers.empty()) {
                            json h = json::object();
                            for (const auto& [k, v] : sourceSnapshot.headers) h[k] = v;
                            headersJson = h.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                        }
                        response = httpFunc(ch.url, "GET", headersJson, "");
                        httpStatus = response.empty() ? 0 : 200;
                    }

                    if (!response.empty()) break;
                    if (httpStatus == 0 || httpStatus == 429 || httpStatus == 503) continue;
                    break;
                }

                if (response.empty()) {
                    atomicFailed++;
                    status = "empty";
                    log("[WARN][download] 章节内容为空 | HTTP " + std::to_string(httpStatus) + " | " + ch.title + " | url=" + ch.url.substr(0, 80));
                } else {
                    // 选择器执行统一走 applyRuleStatic（含三路分隔符、索引/排除、内嵌规则）。
                    // selectorRule 已去除 ## 部分，正则替换仍由下方 regexPatterns 多正则循环处理，
                    // 保持正文规则 "##a|b|c" 多正则语义不变。
                    std::vector<std::string> contents = detail::applyRuleStatic(
                        response, selectorRule, threadJs.get(), ch.url);

                    if (!contents.empty() && !regexPatterns.empty()) {
                        for (auto& item : contents) {
                            for (const auto& pat : regexPatterns) {
                                try {
                                    std::regex re(pat);
                                    std::string replaced = std::regex_replace(item, re, "");
                                    if (detail::isValidUtf8(replaced)) {
                                        item = std::move(replaced);
                                    }
                                } catch (...) {}
                            }
                        }
                    }

                    if (contents.empty()) {
                        atomicFailed++;
                        status = "empty";
                        log("[WARN][download] 正文规则匹配为空 | " + ch.title + " | bodyLen=" + std::to_string(response.size()) + " | rule=" + rule.content.substr(0, 120));
                        // 调试转储：仅当显式设置环境变量 OPENREAD_DEBUG_DUMP=<path> 时启用，
                        // 生产默认关闭，不再硬编码写 /tmp（避免无意的磁盘副作用）。
                        bool expected = false;
                        if (debugFileWritten.compare_exchange_strong(expected, true)) {
                            const char* dumpPath = std::getenv("OPENREAD_DEBUG_DUMP");
                            if (dumpPath && *dumpPath) {
                                try {
                                    std::ofstream ofs(dumpPath);
                                    if (ofs.is_open()) {
                                        ofs << "<!-- chapter: " << ch.title << " -->\n";
                                        ofs << "<!-- url: " << ch.url << " -->\n";
                                        ofs << "<!-- contentRule: " << rule.content << " -->\n";
                                        ofs << "<!-- sourceName: " << sourceSnapshot.name << " -->\n";
                                        ofs << "<!-- bodyLen: " << response.size() << " -->\n\n";
                                        ofs << response;
                                        ofs.close();
                                        log(std::string("[WARN][download] 已写入调试文件: ") + dumpPath);
                                    }
                                } catch (...) {}
                            }
                        }
                    } else {
                        std::string text = detail::cleanContent(contents[0]);
                        if (!text.empty()) {
                            // A5: 写入缓存前再次确认 Impl 还活着
                            if (dbPtr && aliveFlag->load(std::memory_order_acquire)) {
                                dbPtr->cacheChapterContent(bookUrl, ch.index, ch.url, text);
                            }
                            atomicCached++;
                            status = "ok";
                        } else {
                            atomicFailed++;
                            status = "empty";
                            log("[WARN][download] cleanContent后为空 | " + ch.title);
                        }
                    }
                }
            } catch (const std::exception& e) {
                atomicFailed++;
                status = "error";
                log("[ERROR][download] 异常: " + std::string(e.what()) + " | " + ch.title);
            } catch (...) {
                atomicFailed++;
                status = "error";
                log("[ERROR][download] 未知异常 | " + ch.title);
            }

            int curDone = done.fetch_add(1) + 1;
            if (callback && aliveFlag->load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(callbackMutex);
                callback(curDone, result.total, atomicCached.load(), atomicFailed.load(), ch.title, status);
            }
        }
    };

    // 引擎析构（aliveFlag 翻转）即停止领取新章节，已在执行的任务自然收尾。
    detail::parallelForEach(
        tasks.size(), actualConcurrency, runTask,
        [aliveFlag]() { return !aliveFlag->load(std::memory_order_acquire); });

    result.cached = atomicCached.load();
    result.failed = atomicFailed.load();

    // 下载完成后：重置 has_update，更新 totalChapters
    if (pImpl->db && !chapters.empty()) {
        pImpl->db->updateBookshelfLastChapter(
            bookUrl, chapters.back().title, result.total, false);
    }

    {
        std::lock_guard<std::mutex> lock(pImpl->bookStateMutex);
        auto key = Impl::makeBookStateKey(bookUrl, "");
        auto& entry = pImpl->bookStates[key];
        if (entry.snapshot.bookUrl.empty()) {
            entry.snapshot.bookUrl = bookUrl;
            entry.snapshot.sourceUrl = sourceUrl;
            entry.snapshot.sourceName = sourceName;
            entry.snapshot.sourceIndex = sourceIndex;
        }
        entry.snapshot.catalogCached = result.total;
        entry.snapshot.contentCached = result.cached;
        entry.snapshot.totalChapters = result.total;
        entry.snapshot.hasUpdate = false;
        entry.snapshot.fullyCached = result.total > 0 && result.cached >= result.total;
        entry.snapshot.lastRefreshAt = Impl::nowUnix();
        entry.snapshot.lastError.clear();
    }

    return result;
}

} // namespace openread

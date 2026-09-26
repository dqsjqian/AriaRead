/// @file routes_search.cpp
/// @brief 搜索路由：/api/search（单源）、/api/search/all（SSE 全源流式）、
///        /api/search/selected（SSE 指定源流式）。

#include "routes_internal.h"
#include "http_helpers.h"
#include "ariaread/engine_impl.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace ariaread::web {

using ariaread::detail::sanitizeUtf8;

namespace {

/// 把"逐源并发搜索 → SSE 推送"这套在 all/selected 两处重复的逻辑收敛成一个函数。
/// @param sourceNames 为空表示搜全部可用源；非空表示只搜指定名称的源。
void stream_concurrent_search(ariaread::BookSourceEngine& engine,
                              Response& res,
                              const std::string& keyword,
                              bool matchName, bool matchAuthor, bool matchIntro,
                              const std::vector<std::string>& sourceNames) {
    res.set_chunked_content_provider(
        "text/event-stream",
        [&engine, keyword, matchName, matchAuthor, matchIntro, sourceNames](
            std::size_t /*offset*/, DataSink& sink) -> bool {
            auto sources = engine.sources();

            // 统计可搜索源数量
            int searchableCount = 0;
            if (sourceNames.empty()) {
                for (const auto& s : sources) {
                    if (!s.searchUrl.empty() &&
                        s.validity != ariaread::SourceValidity::Invalid &&
                        s.validity != ariaread::SourceValidity::Poor) {
                        ++searchableCount;
                    }
                }
            } else {
                for (const auto& sn : sourceNames) {
                    for (const auto& s : sources) {
                        if (s.name == sn && !s.searchUrl.empty()) { ++searchableCount; break; }
                    }
                }
            }

            auto write_sse = [&sink](const std::string& event, const json& data) {
                std::string sse = "event: " + event + "\ndata: " + safeDump(data) + "\n\n";
                sink.write(sse.data(), sse.size());
            };

            write_sse("search_start", {{"totalSources", searchableCount}});

            if (searchableCount == 0) {
                write_sse("search_done", {{"totalBooks", 0}, {"totalSources", 0}});
                return true;
            }

            std::atomic<int> totalBooks{0};
            std::atomic<int> totalErrors{0};
            std::atomic<int> doneCount{0};
            std::mutex sink_mu;
            std::condition_variable done_cv;

            engine.searchAllConcurrent(
                keyword, 8,
                [&](size_t idx, const std::string& name,
                    const std::vector<ariaread::Book>& books,
                    int latencyMs, const std::string& error) {
                    if (!error.empty()) {
                        totalErrors.fetch_add(1);
                        std::lock_guard<std::mutex> lk(sink_mu);
                        write_sse("source_error", {
                            {"index", static_cast<int>(idx)},
                            {"name", sanitizeUtf8(name)},
                            {"url", idx < sources.size() ? sanitizeUtf8(sources[idx].url) : ""},
                            {"error", sanitizeUtf8(error)},
                            {"latency", latencyMs},
                        });
                    } else {
                        json bookArr = json::array();
                        const std::string srcUrl = idx < sources.size() ? sources[idx].url : "";
                        for (const auto& b : books)
                            bookArr.push_back(book_to_json_with_source(b, idx, name, srcUrl));
                        totalBooks.fetch_add(static_cast<int>(books.size()));
                        std::lock_guard<std::mutex> lk(sink_mu);
                        write_sse("source_result", {
                            {"index", static_cast<int>(idx)},
                            {"name", sanitizeUtf8(name)},
                            {"url", sanitizeUtf8(srcUrl)},
                            {"books", bookArr},
                            {"latency", latencyMs},
                        });
                    }
                    doneCount.fetch_add(1);
                },
                [&](int, int, int) {
                    {
                        std::lock_guard<std::mutex> lk(sink_mu);
                        write_sse("search_done", {
                            {"totalBooks", totalBooks.load()},
                            {"totalSources", searchableCount},
                            {"totalErrors", totalErrors.load()},
                        });
                    }
                    done_cv.notify_one();
                },
                nullptr,
                matchName, matchAuthor, matchIntro, sourceNames);

            {
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                std::unique_lock<std::mutex> lk(sink_mu);
                while (g_running.load() &&
                       doneCount.load() < searchableCount &&
                       std::chrono::steady_clock::now() < deadline) {
                    done_cv.wait_for(lk, std::chrono::seconds(1), [&] {
                        return doneCount.load() >= searchableCount || !g_running.load();
                    });
                }
            }
            return true;
        });
}

}  // namespace

void register_search_routes(Server& svr, ariaread::BookSourceEngine& engine) {

    // ── 单源搜索 ──────────────────────────────────────────────────────
    svr.Get("/api/search",
        [&engine](const Request& req, Response& res) {
            std::string keyword = param(req, "q");
            if (keyword.empty()) { json_error(res, 400, "Missing query parameter: q"); return; }
            with_error_handling(res, [&] {
                auto sourceRaw = param(req, "source");
                auto sourceName = param(req, "source_name");
                if (!sourceName.empty()) {
                    engine.selectSourceByName(sourceName);
                } else if (!sourceRaw.empty()) {
                    try {
                        engine.selectSource(std::stoi(sourceRaw));
                    } catch (...) {
                        json_error(res, 400, "Invalid query parameter: source");
                        return;
                    }
                }
                auto books = engine.search(keyword);

                const auto& sources = engine.sources();
                size_t curIdx = 0;
                std::string curName, curUrl;
                for (size_t i = 0; i < sources.size(); ++i) {
                    if (&sources[i] == &engine.currentSource()) {
                        curIdx = i;
                        curName = sources[i].name;
                        curUrl = sources[i].url;
                        break;
                    }
                }
                json arr = json::array();
                for (const auto& b : books)
                    arr.push_back(book_to_json_with_source(b, curIdx, curName, curUrl));
                json_ok(res, arr);
            });
        });

    // ── 全源并发搜索（SSE 流式）───────────────────────────────────────
    svr.Get("/api/search/all",
        [&engine](const Request& req, Response& res) {
            std::string keyword = param(req, "q");
            if (keyword.empty()) { json_error(res, 400, "Missing query parameter: q"); return; }

            bool matchName = param(req, "match_name") != "0";
            bool matchAuthor = param(req, "match_author") == "1";
            bool matchIntro = param(req, "match_intro") == "1";
            if (!matchName && !matchAuthor && !matchIntro) matchName = true;

            stream_concurrent_search(engine, res, keyword,
                                     matchName, matchAuthor, matchIntro, {});
        });

    // ── 指定源并发搜索（SSE 流式）─────────────────────────────────────
    svr.Get("/api/search/selected",
        [&engine](const Request& req, Response& res) {
            std::string keyword = param(req, "q");
            if (keyword.empty()) { json_error(res, 400, "Missing query parameter: q"); return; }

            bool matchName = param(req, "match_name") != "0";
            bool matchAuthor = param(req, "match_author") == "1";
            bool matchIntro = param(req, "match_intro") == "1";
            if (!matchName && !matchAuthor && !matchIntro) matchName = true;

            std::vector<std::string> sourceNames;
            std::istringstream ss(param(req, "source_names"));
            std::string token;
            while (std::getline(ss, token, ',')) {
                if (!token.empty()) sourceNames.push_back(token);
            }

            stream_concurrent_search(engine, res, keyword,
                                     matchName, matchAuthor, matchIntro, sourceNames);
        });
}

}  // namespace ariaread::web

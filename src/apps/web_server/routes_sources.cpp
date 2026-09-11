/// @file routes_sources.cpp
/// @brief 书源路由：/api/sources/*（列表、加载、校验 SSE、导出、清理、移除）。

#include "routes_internal.h"
#include "http_helpers.h"
#include "source_debug.h"
#include "openread/engine_impl.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace openread::web {

using openread::detail::sanitizeUtf8;

void register_sources_routes(httplib::Server& svr, openread::BookSourceEngine& engine,
                             aria::async::IExecutor& /*worker*/) {

    // ── 列表 ──────────────────────────────────────────────────────────
    svr.Get("/api/sources",
        [&engine](const httplib::Request&, httplib::Response& res) {
            with_error_handling(res, [&] {
                auto result = engine.getSourceList();
                json arr = json::array();
                for (const auto& s : result.sources) arr.push_back(source_summary_to_json(s));
                json stats = {
                    {"excellent", result.stats.excellent},
                    {"good", result.stats.good},
                    {"poor", result.stats.poor},
                    {"invalid", result.stats.invalid},
                    {"unknown", result.stats.unknown},
                };
                json_ok(res, {
                    {"sources", arr},
                    {"validCount", result.validCount},
                    {"totalCount", result.totalCount},
                    {"stats", stats},
                });
            });
        });

    // ── 单源调试（SSE 分阶段输出，不修改全局选源/数据库）─────────────
    svr.Get("/api/source/debug",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                const auto url = param(req, "source_url");
                const auto name = param(req, "source_name");
                auto keyword = param(req, "q");
                if (keyword.empty()) keyword = "我";
                if (url.empty() && name.empty()) {
                    json_error(res, 400, "Missing source_url or source_name");
                    return;
                }
                const auto& sources = engine.sources();
                auto found = std::find_if(sources.begin(), sources.end(), [&](const BookSource& source) {
                    return !url.empty() ? source.url == url : source.name == name;
                });
                if (found == sources.end()) { json_error(res, 404, "Source not found"); return; }
                if (keyword.size() > 1024) { json_error(res, 400, "Query exceeds 1024 bytes"); return; }
                res.set_header("Cache-Control", "no-cache");
                res.set_header("X-Accel-Buffering", "no");
                res.set_chunked_content_provider("text/event-stream",
                    [source = *found, keyword](std::size_t, httplib::DataSink& sink) {
                        runSourceDebug(source, keyword,
                            [&](const std::string& event, const json& data) {
                                const auto text = "event: " + event + "\ndata: " + safeDump(data) + "\n\n";
                                return sink.write(text.data(), text.size());
                            },
                            [&] { return g_running.load() && sink.is_writable(); });
                        sink.done();
                        return true;
                    });
            });
        });

    // ── 从文件加载 ────────────────────────────────────────────────────
    svr.Post("/api/sources/load",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                std::string filePath = body.value("path", "");
                if (filePath.empty()) { json_error(res, 400, "Missing path"); return; }
                int count = engine.loadSourcesFromFile(filePath);
                json_ok(res, {{"loaded", count}, {"total", count}, {"ok", true}});
            });
        });

    // ── 从原始 JSON 文本加载 ──────────────────────────────────────────
    svr.Post("/api/sources/raw",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                std::string raw = body.value("json", "");
                if (raw.empty()) { json_error(res, 400, "Missing json"); return; }
                int count = engine.loadSources(raw);
                json_ok(res, {{"loaded", count}, {"total", count}, {"ok", true}});
            });
        });

    // ── 校验全部书源（SSE 流式）───────────────────────────────────────
    svr.Get("/api/sources/validate",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            std::string testQuery = param(req, "q");
            if (testQuery.empty()) testQuery = "\xe6\x88\x91";

            res.set_chunked_content_provider(
                "text/event-stream",
                [&engine, testQuery](
                    std::size_t /*offset*/, httplib::DataSink& sink) -> bool {
                    auto sources = engine.sources();
                    int totalCount = static_cast<int>(sources.size());

                    auto write_sse = [&sink](const std::string& event, const json& data) {
                        std::string sse = "event: " + event + "\ndata: " + safeDump(data) + "\n\n";
                        sink.write(sse.data(), sse.size());
                    };

                    write_sse("validate_start", {{"total", totalCount}});

                    std::atomic<int> doneCount{0};
                    std::atomic<int> validCount{0};
                    std::atomic<int> invalidCount{0};
                    std::atomic<int> poorCount{0};
                    std::mutex sink_mu;
                    std::condition_variable done_cv;

                    auto gradeStr = [](openread::SourceValidity v) -> std::string {
                        switch (v) {
                            case openread::SourceValidity::Excellent: return "excellent";
                            case openread::SourceValidity::Good:      return "good";
                            case openread::SourceValidity::Poor:      return "poor";
                            default:                                   return "invalid";
                        }
                    };

                    engine.validateSourcesConcurrent(
                        testQuery, 10000, 8,
                        [&](size_t idx, const std::string& name,
                            openread::SourceValidity validity, int latencyMs,
                            const std::string& detail) {
                            doneCount.fetch_add(1);
                            int done = doneCount.load();
                            std::string grade = gradeStr(validity);
                            std::string url = idx < sources.size() ? sanitizeUtf8(sources[idx].url) : "";

                            std::lock_guard<std::mutex> lk(sink_mu);
                            if (validity == openread::SourceValidity::Excellent ||
                                validity == openread::SourceValidity::Good) {
                                validCount.fetch_add(1);
                                write_sse("source_valid", {
                                    {"index", static_cast<int>(idx)},
                                    {"name", sanitizeUtf8(name)},
                                    {"url", url},
                                    {"grade", grade},
                                    {"latency", latencyMs},
                                    {"detail", sanitizeUtf8(detail)},
                                });
                            } else if (validity == openread::SourceValidity::Poor) {
                                poorCount.fetch_add(1);
                                write_sse("source_removed", {
                                    {"index", static_cast<int>(idx)},
                                    {"name", sanitizeUtf8(name)},
                                    {"url", url}, {"grade", grade}, {"reason", "poor"},
                                });
                            } else {
                                invalidCount.fetch_add(1);
                                write_sse("source_removed", {
                                    {"index", static_cast<int>(idx)},
                                    {"name", sanitizeUtf8(name)},
                                    {"url", url}, {"grade", grade}, {"reason", "invalid"},
                                });
                            }
                            write_sse("validate_progress", {{"done", done}, {"total", totalCount}});
                        },
                        [&](int valid, int invalid, int removed) {
                            {
                                std::lock_guard<std::mutex> lk(sink_mu);
                                write_sse("validate_done",
                                          {{"valid", valid}, {"invalid", invalid}, {"removed", removed}});
                            }
                            done_cv.notify_one();
                        },
                        nullptr);

                    {
                        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
                        std::unique_lock<std::mutex> lk(sink_mu);
                        while (g_running.load() &&
                               doneCount.load() < totalCount &&
                               std::chrono::steady_clock::now() < deadline) {
                            done_cv.wait_for(lk, std::chrono::seconds(1), [&] {
                                return doneCount.load() >= totalCount || !g_running.load();
                            });
                        }
                    }
                    return true;
                });
        });

    // ── 导出优质源 ────────────────────────────────────────────────────
    svr.Get("/api/sources/export",
        [&engine](const httplib::Request&, httplib::Response& res) {
            with_error_handling(res, [&] {
                res.set_content(engine.exportGoodSources(), "application/json");
            });
        });

    // ── 清空全部源 ────────────────────────────────────────────────────
    svr.Delete("/api/sources/clear",
        [&engine](const httplib::Request&, httplib::Response& res) {
            with_error_handling(res, [&] {
                int count = engine.clearAllSources();
                json_ok(res, {{"count", count}, {"ok", true}});
            });
        });

    // ── 清空无效源 ────────────────────────────────────────────────────
    svr.Delete("/api/sources/invalid",
        [&engine](const httplib::Request&, httplib::Response& res) {
            with_error_handling(res, [&] {
                int count = engine.removeInvalidSources();
                json_ok(res, {{"count", count}, {"ok", true}});
            });
        });

    // ── 从 URL 加载 ───────────────────────────────────────────────────
    svr.Post("/api/sources/url",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                std::string url = body.value("url", "");
                if (url.empty()) { json_error(res, 400, "Missing url"); return; }
                auto content = httpDownload(url);
                int count = engine.loadSources(content);
                json_ok(res, {{"loaded", count}, {"total", count}, {"ok", true}});
            });
        });

    // ── 移除单个源（按 url 或 name）───────────────────────────────────
    svr.Delete("/api/sources/remove",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            std::string url = param(req, "url");
            std::string name = param(req, "name");
            with_error_handling(res, [&] {
                bool removed = false;
                if (!url.empty()) {
                    removed = engine.removeSourceByUrl(url);
                } else if (!name.empty()) {
                    auto& sources = engine.sources();
                    for (size_t i = 0; i < sources.size(); ++i) {
                        if (sources[i].name == name) { removed = engine.removeSource(i); break; }
                    }
                }
                json_ok(res, {{"ok", removed}});
            });
        });
}

}  // namespace openread::web

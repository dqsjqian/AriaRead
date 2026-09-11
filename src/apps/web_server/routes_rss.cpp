/// @file routes_rss.cpp
/// @brief RSS 路由：/api/rss/*（订阅源增删查、导入、文章列表/详情、抓取、评级 SSE、清理）。

#include "routes_internal.h"
#include "http_helpers.h"
#include "openread/engine_impl.h"

#include <atomic>
#include <mutex>
#include <string>

namespace openread::web {

using openread::detail::sanitizeUtf8;

void register_rss_routes(httplib::Server& svr, openread::BookSourceEngine& engine) {

    // ── 订阅源列表 ────────────────────────────────────────────────────
    svr.Get("/api/rss/sources",
        [&engine](const httplib::Request&, httplib::Response& res) {
            with_error_handling(res, [&] {
                auto sources = engine.getRssSources();
                json arr = json::array();
                for (const auto& s : sources) {
                    arr.push_back({
                        {"sourceName", sanitizeUtf8(s.sourceName)},
                        {"sourceUrl", sanitizeUtf8(s.sourceUrl)},
                        {"sourceIcon", sanitizeUtf8(s.sourceIcon)},
                        {"sourceGroup", sanitizeUtf8(s.sourceGroup)},
                        {"enabled", s.enabled},
                        {"lastUpdateTime", s.lastUpdateTime},
                        {"singleUrl", s.singleUrl},
                        {"hasRule", !s.ruleArticles.empty()},
                        {"validity", s.validity},
                        {"latencyMs", s.latencyMs},
                    });
                }
                json_ok(res, {{"sources", arr}});
            });
        });

    // ── 新增/更新订阅源 ───────────────────────────────────────────────
    svr.Post("/api/rss/sources",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                openread::RssSource src;
                // 兼容两种字段命名（前端发 sourceUrl/sourceName/sourceGroup）
                src.sourceName  = body.value("sourceName", body.value("name", ""));
                src.sourceUrl   = body.value("sourceUrl",  body.value("url", ""));
                src.sourceIcon  = body.value("sourceIcon", body.value("iconUrl", ""));
                src.sourceGroup = body.value("sourceGroup", body.value("group", ""));
                if (src.sourceUrl.empty()) { json_error(res, 400, "Missing sourceUrl"); return; }
                if (src.sourceName.empty()) src.sourceName = src.sourceUrl;
                engine.upsertRssSource(src);
                json_ok(res, {{"ok", true}});
            });
        });

    // ── 删除订阅源 ────────────────────────────────────────────────────
    svr.Delete("/api/rss/sources",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                std::string sourceUrl = body.value("sourceUrl", "");
                if (sourceUrl.empty()) { json_error(res, 400, "Missing sourceUrl"); return; }
                bool ok = engine.removeRssSource(sourceUrl);
                json_ok(res, {{"ok", ok}});
            });
        });

    // ── 导入（JSON 文本）──────────────────────────────────────────────
    svr.Post("/api/rss/import/json",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                std::string content = body.value("json", "");
                if (content.empty()) { json_error(res, 400, "Missing json content"); return; }
                auto [count, err] = engine.importRssSourcesFromJson(content);
                json j = {{"count", count}, {"ok", err.empty()}};
                if (!err.empty()) j["error"] = err;
                json_ok(res, j);
            });
        });

    // ── 导入（URL）────────────────────────────────────────────────────
    svr.Post("/api/rss/import/url",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                std::string url = body.value("url", "");
                if (url.empty()) { json_error(res, 400, "Missing url"); return; }
                auto [count, err] = engine.importRssSourcesFromUrl(url);
                json j = {{"count", count}, {"ok", err.empty()}};
                if (!err.empty()) j["error"] = err;
                json_ok(res, j);
            });
        });

    // ── 文章列表（分页）───────────────────────────────────────────────
    svr.Get("/api/rss/articles",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            std::string sourceUrl = param(req, "source_url");
            int page = int_param(req, "page", 1);
            int pageSize = int_param(req, "page_size", 50);
            with_error_handling(res, [&] {
                auto result = param(req, "load") == "1"
                    ? engine.loadRssArticles(sourceUrl, page, pageSize)
                    : engine.getRssArticles(sourceUrl, page, pageSize);
                json arr = json::array();
                for (const auto& a : result.articles) {
                    // 占位 link（源URL#item-N，原文无真实外链）对外输出为空，
                    // 前端据此只展示内联内容、不显示跳转。
                    std::string outLink =
                        (a.link.find("#item-") != std::string::npos) ? "" : a.link;
                    arr.push_back({
                        {"id", a.id},
                        {"title", sanitizeUtf8(a.title)},
                        {"url", sanitizeUtf8(outLink)},
                        {"link", sanitizeUtf8(outLink)},
                        {"description", sanitizeUtf8(a.description)},
                        {"pubDate", a.pubDate},
                        {"sourceUrl", sanitizeUtf8(a.sourceUrl)},
                        {"image", sanitizeUtf8(a.image)},
                    });
                }
                json_ok(res, {
                    {"articles", arr},
                    {"total", result.total},
                    {"error", sanitizeUtf8(result.error)},
                    {"page", page},
                    {"pageSize", pageSize},
                });
            });
        });

    // ── 文章详情（懒加载正文）─────────────────────────────────────────
    svr.Get("/api/rss/article",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            std::string idRaw = param(req, "id");
            if (idRaw.empty()) { json_error(res, 400, "Missing query parameter: id"); return; }
            with_error_handling(res, [&] {
                int64_t id = std::stoll(idRaw);
                auto a = engine.getRssArticle(id);
                if (!a.id) { json_error(res, 404, "文章不存在"); return; }
                auto result = engine.getRssArticleContentResult(id);
                const auto& content = result.content;
                const auto& outLink = result.originalUrl;
                json_ok(res, {
                    {"id", a.id},
                    {"title", sanitizeUtf8(a.title)},
                    {"url", sanitizeUtf8(outLink)},
                    {"link", sanitizeUtf8(outLink)},
                    {"content", sanitizeUtf8(content)},
                    {"contentError", sanitizeUtf8(result.error)},
                    {"description", sanitizeUtf8(a.description)},
                    {"pubDate", a.pubDate},
                    {"sourceUrl", sanitizeUtf8(a.sourceUrl)},
                    {"image", sanitizeUtf8(a.image)},
                });
            });
        });

    // ── 抓取单个订阅源 ────────────────────────────────────────────────
    svr.Post("/api/rss/fetch",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                std::string sourceUrl = body.value("sourceUrl", "");
                if (sourceUrl.empty()) { json_error(res, 400, "Missing sourceUrl"); return; }
                auto result = engine.fetchRssSource(sourceUrl);
                json j = {
                    {"count", result.count},
                    {"inserted", result.inserted},
                    {"skipped", result.skipped},
                    {"ok", result.error.empty()},
                };
                if (!result.error.empty()) j["error"] = result.error;
                json_ok(res, j);
            });
        });

    // ── 评级检测（SSE 流式）───────────────────────────────────────────
    svr.Get("/api/rss/check/stream",
        [&engine](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider(
                "text/event-stream",
                [&engine](std::size_t /*offset*/, httplib::DataSink& sink) -> bool {
                    std::mutex sink_mu;
                    std::atomic<int> excellent{0}, good{0}, poor{0}, invalid{0};
                    try {
                        engine.checkRssSourcesRated(
                            [&](int done, int total, const std::string& current,
                                const std::string& validity, int latencyMs) {
                                if (validity == "excellent") ++excellent;
                                else if (validity == "good") ++good;
                                else if (validity == "poor") ++poor;
                                else ++invalid;
                                bool ok = (validity != "invalid");
                                json ev = {
                                    {"done", done}, {"total", total},
                                    {"current", sanitizeUtf8(current)}, {"ok", ok},
                                    {"validity", validity}, {"latencyMs", latencyMs},
                                    {"excellent", excellent.load()}, {"good", good.load()},
                                    {"poor", poor.load()}, {"invalid", invalid.load()},
                                };
                                std::string sse = "event: check_progress\ndata: " + safeDump(ev) + "\n\n";
                                std::lock_guard<std::mutex> lk(sink_mu);
                                sink.write(sse.data(), sse.size());
                            });
                    } catch (const std::exception& e) {
                        json err_ev = {{"error", std::string(e.what())}};
                        std::string sse = "event: check_progress\ndata: " + safeDump(err_ev) + "\n\n";
                        std::lock_guard<std::mutex> lk(sink_mu);
                        sink.write(sse.data(), sse.size());
                    }

                    json done_ev = {
                        {"finished", true},
                        {"excellent", excellent.load()}, {"good", good.load()},
                        {"poor", poor.load()}, {"invalid", invalid.load()},
                    };
                    std::string sse = "event: check_done\ndata: " + safeDump(done_ev) + "\n\n";
                    std::lock_guard<std::mutex> lk(sink_mu);
                    sink.write(sse.data(), sse.size());
                    sink.done();
                    return true;
                });
        });

    // ── 清空全部 RSS ──────────────────────────────────────────────────
    svr.Delete("/api/rss/clear",
        [&engine](const httplib::Request&, httplib::Response& res) {
            with_error_handling(res, [&] {
                engine.clearAllRss();
                json_ok(res, {{"ok", true}});
            });
        });

    // ── 清空无效 RSS ──────────────────────────────────────────────────
    svr.Delete("/api/rss/clear-invalid",
        [&engine](const httplib::Request&, httplib::Response& res) {
            with_error_handling(res, [&] {
                int removed = engine.clearInvalidRssSources();
                json_ok(res, {{"ok", true}, {"removed", removed}});
            });
        });
}

}  // namespace openread::web

/// @file routes_bookshelf.cpp
/// @brief 书架路由：/api/bookshelf/*（列表、增删、进度、换源、缓存、下载、导出、自动换源）。

#include "routes_internal.h"
#include "http_helpers.h"
#include "openread/engine_impl.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace openread::web {

using openread::detail::sanitizeUtf8;

void register_bookshelf_routes(httplib::Server& svr, openread::BookSourceEngine& engine,
                               aria::async::IExecutor& worker) {

    // ── 列表 ──────────────────────────────────────────────────────────
    svr.Get("/api/bookshelf",
        [&engine](const httplib::Request&, httplib::Response& res) {
            with_error_handling(res, [&] {
                auto details = engine.getBookshelfWithDetails();
                json arr = json::array();
                for (const auto& d : details) arr.push_back(bookshelf_detail_to_json(d));
                json_ok(res, {{"books", arr}, {"count", arr.size()}});
            });
        });

    // ── 加入书架 ──────────────────────────────────────────────────────
    svr.Post("/api/bookshelf/add",
        [&engine, &worker](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                openread::BookshelfItem item;
                item.bookName = body.value("bookName", "");
                item.bookAuthor = body.value("bookAuthor", "");
                item.coverUrl = body.value("coverUrl", "");
                item.bookUrl = body.value("bookUrl", "");
                item.sourceName = body.value("sourceName", "");
                item.sourceUrl = body.value("sourceUrl", "");
                item.intro = body.value("intro", "");
                item.kind = body.value("kind", "");
                item.lastChapter = body.value("lastChapter", "");

                if (item.bookUrl.empty()) { json_error(res, 400, "bookUrl is required"); return; }

                auto newId = engine.addToBookshelf(item);
                if (newId < 0) {
                    res.status = 409;
                    json_ok(res, {{"error", "Already in bookshelf"}, {"exists", true}});
                    return;
                }

                std::string bookUrl = item.bookUrl;
                int sourceIndex = body.value("sourceIndex", -1);
                std::string sourceName = item.sourceName;
                worker.post([&engine, bookUrl, sourceIndex, sourceName]() {
                    try { engine.refreshCatalog(bookUrl, "", sourceIndex, sourceName); } catch (...) {}
                });

                json_ok(res, {{"id", newId}, {"ok", true}});
            });
        });

    // ── 移除（支持 query 或 body）─────────────────────────────────────
    svr.Delete("/api/bookshelf/remove",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            std::string bookUrl = param(req, "bookUrl");
            if (bookUrl.empty() && !req.body.empty()) {
                try {
                    json body = json::parse(req.body);
                    bookUrl = body.value("bookUrl", "");
                } catch (...) {}
            }
            if (bookUrl.empty()) { json_error(res, 400, "Missing bookUrl"); return; }
            with_error_handling(res, [&] {
                engine.removeFromBookshelf(bookUrl);
                json_ok(res, {{"ok", true}});
            });
        });

    // ── 阅读进度：读 ──────────────────────────────────────────────────
    svr.Get("/api/bookshelf/progress",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            std::string bookUrl = param(req, {"book_url", "bookUrl"});
            if (bookUrl.empty()) { json_error(res, 400, "Missing query parameter: bookUrl"); return; }
            with_error_handling(res, [&] {
                auto p = engine.getReadProgress(bookUrl);
                json_ok(res, {
                    {"chapterIndex", p.chapterIndex},
                    {"chapterTitle", sanitizeUtf8(p.chapterTitle)},
                    {"chapterUrl", sanitizeUtf8(p.chapterUrl)},
                    {"readPercent", p.readPercent},
                    {"lastReadAt", p.lastReadAt},
                });
            });
        });

    // ── 阅读进度：写 ──────────────────────────────────────────────────
    svr.Put("/api/bookshelf/progress",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                openread::ReadProgress p;
                p.bookUrl = body.value("bookUrl", "");
                p.chapterIndex = body.value("chapterIndex", 0);
                p.chapterTitle = body.value("chapterTitle", "");
                p.chapterUrl = body.value("chapterUrl", "");
                p.pageOffset = body.value("pageOffset", 0);
                p.readPercent = body.value("readPercent", 0.0);
                engine.saveReadProgress(p);
                json_ok(res, {{"ok", true}});
            });
        });

    // ── 换源 ──────────────────────────────────────────────────────────
    svr.Put("/api/bookshelf/change_source",
        [&engine, &worker](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                std::string bookUrl = body.value("bookUrl", "");
                std::string oldSourceUrl = body.value("oldSourceUrl", "");
                std::string newSourceName = body.value("newSourceName", "");
                std::string newSourceUrl = body.value("newSourceUrl", "");
                std::string newBookUrl = body.value("newBookUrl", "");

                if (bookUrl.empty() || newSourceUrl.empty()) {
                    json_error(res, 400, "bookUrl and newSourceUrl are required");
                    return;
                }

                engine.changeBookSource(bookUrl, oldSourceUrl, newSourceName, newSourceUrl, newBookUrl);

                int sourceIndex = body.value("newSourceIndex", body.value("sourceIndex", -1));
                std::string bookUrl2 = bookUrl;
                worker.post([&engine, bookUrl2, sourceIndex, newSourceName, newSourceUrl]() {
                    try {
                        engine.refreshCatalog(bookUrl2, newSourceUrl, sourceIndex, newSourceName);
                    } catch (...) {}
                });

                json_ok(res, {{"ok", true}});
            });
        });

    // ── 缓存状态 ──────────────────────────────────────────────────────
    svr.Get("/api/bookshelf/cache_status",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            std::string bookUrl = param(req, {"book_url", "bookUrl"});
            if (bookUrl.empty()) { json_error(res, 400, "Missing query parameter: book_url"); return; }
            with_error_handling(res, [&] {
                json_ok(res, {
                    {"catalogCount", engine.getCachedCatalogCount(bookUrl)},
                    {"contentCount", engine.getCachedContentCount(bookUrl)},
                });
            });
        });

    // ── 清缓存 ────────────────────────────────────────────────────────
    svr.Delete("/api/bookshelf/clear_cache",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                std::string bookUrl = body.value("bookUrl", "");
                std::string sourceUrl = body.value("sourceUrl", "");
                if (bookUrl.empty()) { json_error(res, 400, "bookUrl is required"); return; }
                engine.clearBookCache(bookUrl, sourceUrl);
                json_ok(res, {{"ok", true}});
            });
        });

    // ── 更新检查状态（轮询用）─────────────────────────────────────────
    svr.Get("/api/bookshelf/check_status",
        [&engine](const httplib::Request&, httplib::Response& res) {
            with_error_handling(res, [&] {
                auto details = engine.getBookshelfWithDetails();
                int hasUpdate = 0;
                for (const auto& d : details) if (d.item.hasUpdate) ++hasUpdate;
                json_ok(res, {
                    {"running", false},
                    {"checked", static_cast<int>(details.size())},
                    {"total", static_cast<int>(details.size())},
                    {"updated", hasUpdate},
                    {"done", true},
                });
            });
        });

    // ── 批量更新检查 ──────────────────────────────────────────────────
    svr.Get("/api/bookshelf/check_update",
        [&engine](const httplib::Request&, httplib::Response& res) {
            with_error_handling(res, [&] {
                int count = engine.checkAllUpdates();
                json_ok(res, {{"updatedCount", count}, {"ok", true}});
            });
        });

    // ── 全量下载（SSE）────────────────────────────────────────────────
    svr.Get("/api/bookshelf/download",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            std::string bookUrl = param(req, {"book_url", "bookUrl"});
            if (bookUrl.empty()) { json_error(res, 400, "Missing query parameter: book_url"); return; }
            std::string sourceUrl = param(req, {"source_url", "sourceUrl"});
            int sourceIndex = int_param(req, {"source_index", "sourceIndex"}, -1);
            std::string sourceName = param(req, {"source_name", "sourceName"});

            res.set_chunked_content_provider(
                "text/event-stream",
                [&engine, bookUrl, sourceUrl, sourceIndex, sourceName](
                    std::size_t /*offset*/, httplib::DataSink& sink) -> bool {
                    std::mutex sink_mu;

                    {
                        json start_ev = {{"bookUrl", sanitizeUtf8(bookUrl)}};
                        std::string sse = "event: download_start\ndata: " + safeDump(start_ev) + "\n\n";
                        std::lock_guard<std::mutex> lk(sink_mu);
                        sink.write(sse.data(), sse.size());
                    }

                    auto result = engine.downloadBook(
                        bookUrl, sourceUrl, sourceIndex, sourceName,
                        [&](int done, int total, int cached, int failed,
                            const std::string& title, const std::string& status) {
                            json ev = {
                                {"done", done}, {"total", total},
                                {"cached", cached}, {"failed", failed},
                                {"chapterTitle", sanitizeUtf8(title)}, {"status", sanitizeUtf8(status)},
                            };
                            std::string sse = "event: download_progress\ndata: " + safeDump(ev) + "\n\n";
                            std::lock_guard<std::mutex> lk(sink_mu);
                            sink.write(sse.data(), sse.size());
                        });

                    json done_ev = {
                        {"total", result.total},
                        {"cached", result.cached},
                        {"failed", result.failed},
                    };
                    std::string sse = "event: download_done\ndata: " + safeDump(done_ev) + "\n\n";
                    std::lock_guard<std::mutex> lk(sink_mu);
                    sink.write(sse.data(), sse.size());
                    return true;
                });
        });

    // ── 导出 TXT ──────────────────────────────────────────────────────
    svr.Get("/api/bookshelf/export_txt",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            std::string bookUrl = param(req, {"book_url", "bookUrl"});
            std::string sourceUrl = param(req, {"source_url", "sourceUrl"});
            if (bookUrl.empty()) { json_error(res, 400, "Missing query parameter: bookUrl"); return; }
            with_error_handling(res, [&] {
                auto txt = engine.exportBookToTxt(bookUrl, sourceUrl);
                if (txt.empty()) {
                    json_error(res, 404, "Book not fully cached or export failed");
                    return;
                }
                res.set_content(txt, "text/plain; charset=utf-8");
            });
        });

    // ── 自动换源（SSE）────────────────────────────────────────────────
    svr.Get("/api/bookshelf/auto_source",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            std::string bookName = param(req, {"book_name", "bookName"});
            std::string bookAuthor = param(req, {"book_author", "bookAuthor"});
            std::string excludeSourceUrl = param(req, {"exclude_source_url", "excludeSourceUrl"});
            if (bookName.empty()) { json_error(res, 400, "Missing query parameter: book_name"); return; }

            res.set_chunked_content_provider(
                "text/event-stream",
                [&engine, bookName, bookAuthor, excludeSourceUrl](
                    std::size_t /*offset*/, httplib::DataSink& sink) -> bool {
                    std::mutex sink_mu;
                    engine.findAlternativeSources(
                        bookName, bookAuthor, excludeSourceUrl, 8,
                        [&](const openread::BookSourceEngine::SourceCandidate& c) {
                            json ev = {
                                {"book", book_to_json(c.book)},
                                {"sourceName", sanitizeUtf8(c.sourceName)},
                                {"sourceUrl", sanitizeUtf8(c.sourceUrl)},
                                {"sourceIndex", c.sourceIndex},
                                {"latencyMs", c.latencyMs},
                                {"matchScore", c.matchScore},
                            };
                            std::string sse = "event: auto_source_candidate\ndata: " + safeDump(ev) + "\n\n";
                            std::lock_guard<std::mutex> lk(sink_mu);
                            sink.write(sse.data(), sse.size());
                        },
                        [&](int total) {
                            json ev = {{"totalCandidates", total}};
                            std::string sse = "event: auto_source_done\ndata: " + safeDump(ev) + "\n\n";
                            std::lock_guard<std::mutex> lk(sink_mu);
                            sink.write(sse.data(), sse.size());
                        });

                    for (int i = 0; i < 300 && g_running.load(); ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    return true;
                });
        });
}

}  // namespace openread::web

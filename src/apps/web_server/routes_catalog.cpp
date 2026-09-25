/// @file routes_catalog.cpp
/// @brief 目录/正文路由：/api/catalog、/api/content、/api/catalog/cached、
///        /api/catalog/refresh。

#include "routes_internal.h"
#include "http_helpers.h"
#include "openread/engine_impl.h"

#include <string>

namespace openread::web {

using openread::detail::sanitizeUtf8;

void register_catalog_routes(Server& svr, openread::BookSourceEngine& engine) {

    // ── Catalog（带缓存抓取）─────────────────────────────────────────
    svr.Get("/api/catalog",
        [&engine](const Request& req, Response& res) {
            std::string url = param(req, "url");
            if (url.empty()) { json_error(res, 400, "Missing query parameter: url"); return; }
            std::string sourceUrl = param(req, "source_url");
            std::string sourceName = param(req, "source_name");
            int sourceIndex = int_param(req, "source_index", -1);
            with_error_handling(res, [&] {
                auto chapters = engine.getCatalogWithCache(url, sourceUrl, sourceIndex, sourceName);
                json arr = json::array();
                for (const auto& c : chapters) arr.push_back(chapter_to_json(c));
                json_ok(res, arr);
            });
        });

    // ── Content（带缓存抓取，兼容 camelCase 别名）────────────────────
    svr.Get("/api/content",
        [&engine](const Request& req, Response& res) {
            std::string url = param(req, "url");
            if (url.empty()) { json_error(res, 400, "Missing query parameter: url"); return; }
            std::string bookUrl = param(req, {"book_url", "bookUrl"});
            std::string sourceUrl = param(req, {"source_url", "sourceUrl"});
            int chapterIndex = int_param(req, {"chapter_index", "chapterIndex"}, -1);
            int sourceIndex = int_param(req, {"source_index", "sourceIndex"}, -1);
            std::string sourceName = param(req, {"source_name", "sourceName"});
            with_error_handling(res, [&] {
                auto text = engine.getContentWithCache(url, bookUrl, chapterIndex,
                                                       sourceUrl, sourceIndex, sourceName);
                json_ok(res, {{"content", sanitizeUtf8(text)}});
            });
        });

    // ── Catalog: cached（仅取已缓存目录 + 本地完整性标记）────────────
    svr.Get("/api/catalog/cached",
        [&engine](const Request& req, Response& res) {
            std::string url = param(req, "url");
            if (url.empty()) { json_error(res, 400, "Missing query parameter: url"); return; }
            std::string sourceUrl = param(req, "source_url");
            std::string sourceName = param(req, "source_name");
            int sourceIndex = int_param(req, "source_index", -1);
            std::string kind = param(req, "kind");
            with_error_handling(res, [&] {
                auto chapters = engine.getCachedCatalog(url, sourceUrl);
                bool isFullyLocal = false;
                try {
                    auto state = engine.openBookSession(url, sourceUrl, sourceIndex, sourceName, kind);
                    isFullyLocal = state.isFinished && state.fullyCached;
                } catch (...) {}
                json chArr = json::array();
                for (const auto& c : chapters) chArr.push_back(chapter_to_json(c));
                json_ok(res, {{"chapters", chArr}, {"isFullyLocal", isFullyLocal}});
            });
        });

    // ── Catalog: refresh（强制重新抓取目录）──────────────────────────
    svr.Get("/api/catalog/refresh",
        [&engine](const Request& req, Response& res) {
            std::string url = param(req, "url");
            if (url.empty()) { json_error(res, 400, "Missing query parameter: url"); return; }
            std::string sourceUrl = param(req, "source_url");
            std::string sourceName = param(req, "source_name");
            int sourceIndex = int_param(req, "source_index", -1);
            with_error_handling(res, [&] {
                auto chapters = engine.refreshCatalog(url, sourceUrl, sourceIndex, sourceName);
                json arr = json::array();
                for (const auto& c : chapters) arr.push_back(chapter_to_json(c));
                json_ok(res, arr);
            });
        });
}

}  // namespace openread::web

/// @file routes_misc.cpp
/// @brief 杂项路由：health / shutdown / eval / url_history。

#include "routes_internal.h"
#include "http_helpers.h"
#include "openread/version.h"

#include <string>

namespace openread::web {

void register_misc_routes(httplib::Server& svr, openread::BookSourceEngine& engine) {

    // ── Health ────────────────────────────────────────────────────────
    svr.Get("/api/health",
        [&engine](const httplib::Request&, httplib::Response& res) {
            int totalSources = 0, validSources = 0;
            try {
                auto result = engine.getSourceList();
                totalSources = result.totalCount;
                validSources = result.validCount;
            } catch (...) {}
            json_ok(res, {
                {"ok", true},
                {"status", "ok"},
                {"backend", "cpp"},
                {"version", OPENREAD_VERSION},
                {"sources", totalSources},
                {"valid", validSources},
            });
        });

    // ── Shutdown ──────────────────────────────────────────────────────
    svr.Post("/api/shutdown",
        [](const httplib::Request&, httplib::Response& res) {
            json_ok(res, {{"ok", true}});
            g_running.store(false);
        });

    // ── Eval (JS execution，C++ 后端不支持) ──────────────────────────
    svr.Post("/api/eval",
        [](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                std::string code = body.value("code", "");
                if (code.empty()) { json_error(res, 400, "Missing code"); return; }
                json_ok(res, {{"error", "JS eval not supported in C++ backend"}, {"ok", false}});
            });
        });

    // ── URL History ──────────────────────────────────────────────────
    svr.Get("/api/url_history",
        [&engine](const httplib::Request&, httplib::Response& res) {
            with_error_handling(res, [&] {
                auto* db = engine.database();
                if (!db) { json_ok(res, json::array()); return; }
                auto history = db->getUrlHistory();
                json arr = json::array();
                for (const auto& h : history) arr.push_back(h);
                json_ok(res, arr);
            });
        });

    svr.Post("/api/url_history",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                std::string url = body.value("url", "");
                if (url.empty()) { json_error(res, 400, "Missing url"); return; }
                if (auto* db = engine.database()) db->addUrlHistory(url);
                json_ok(res, {{"ok", true}});
            });
        });

    svr.Delete("/api/url_history",
        [&engine](const httplib::Request& req, httplib::Response& res) {
            with_error_handling(res, [&] {
                std::string url = param(req, "url");
                if (auto* db = engine.database(); db && !url.empty()) db->removeUrlHistory(url);
                json_ok(res, {{"ok", true}});
            });
        });
}

}  // namespace openread::web

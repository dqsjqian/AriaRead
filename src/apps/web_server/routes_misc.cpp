/// @file routes_misc.cpp
/// @brief 杂项路由：health / shutdown / eval / url_history。

#include "routes_internal.h"
#include "http_helpers.h"
#include "debug_console.h"
#include "ariaread/version.h"

#include <string>

namespace ariaread::web {

void register_misc_routes(Server& svr, ariaread::BookSourceEngine& engine) {

    // ── Health ────────────────────────────────────────────────────────
    svr.Get("/api/health",
        [&engine](const Request&, Response& res) {
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
                {"version", ARIAREAD_VERSION},
                {"sources", totalSources},
                {"valid", validSources},
            });
        });

    // ── Shutdown ──────────────────────────────────────────────────────
    svr.Post("/api/shutdown",
        [](const Request&, Response& res) {
            json_ok(res, {{"ok", true}});
            g_running.store(false);
        });

    // ── Eval（隔离运行时，无网络/文件桥接）──────────────────────────
    svr.Post("/api/eval",
        [](const Request& req, Response& res) {
            with_error_handling(res, [&] {
                if (req.body.size() > 512 * 1024) {
                    json_error(res, 413, "Request exceeds 512 KiB");
                    return;
                }
                try {
                    json_ok(res, evaluateDebugScript(json::parse(req.body)));
                } catch (const json::exception& error) {
                    json_error(res, 400, error.what());
                } catch (const std::invalid_argument& error) {
                    json_error(res, 400, error.what());
                } catch (const std::length_error& error) {
                    json_error(res, 413, error.what());
                }
            });
        });

    // ── URL History ──────────────────────────────────────────────────
    svr.Get("/api/url_history",
        [&engine](const Request&, Response& res) {
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
        [&engine](const Request& req, Response& res) {
            with_error_handling(res, [&] {
                json body = json::parse(req.body);
                std::string url = body.value("url", "");
                if (url.empty()) { json_error(res, 400, "Missing url"); return; }
                if (auto* db = engine.database()) db->addUrlHistory(url);
                json_ok(res, {{"ok", true}});
            });
        });

    svr.Delete("/api/url_history",
        [&engine](const Request& req, Response& res) {
            with_error_handling(res, [&] {
                std::string url = param(req, "url");
                if (auto* db = engine.database(); db && !url.empty()) db->removeUrlHistory(url);
                json_ok(res, {{"ok", true}});
            });
        });
}

}  // namespace ariaread::web

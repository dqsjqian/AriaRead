/// @file http_helpers.h
/// @brief REST 路由通用辅助：参数解析（含 camelCase/snake_case 别名兜底）、
///        整型参数安全解析、统一错误响应、异常包装器。
///
/// 这些 helper 把 routes.cpp 里重复了几十次的样板收敛掉：
///   - `get_param_value` + `if (x.empty()) x = get_param_value("xCamel")` 别名兜底
///   - `try { stoi(...) } catch(...){}` 整型解析
///   - `res.status=500; json={{"error",...}}; set_content(...)` 错误响应
///   - 路由体 try/catch → 500 的统一包装

#pragma once

#include "json_helpers.h"

#include "continuo_server.h"

#include <initializer_list>
#include <string>
#include <utility>

namespace ariaread::web {

/// 取请求参数；若主名取不到（空），依次回退到别名（如 snake↔camel）。
/// @param names 一个或多个候选参数名，按序尝试，返回首个非空值。
inline std::string param(const Request& req,
                         std::initializer_list<const char*> names) {
    for (const char* n : names) {
        std::string v = req.get_param_value(n);
        if (!v.empty()) return v;
    }
    return {};
}

/// 单名版本：等价 req.get_param_value(name)，统一入口便于阅读。
inline std::string param(const Request& req, const char* name) {
    return req.get_param_value(name);
}

/// 取整型参数（含别名兜底 + 安全解析）。解析失败或缺省时返回 def。
inline int int_param(const Request& req,
                     std::initializer_list<const char*> names,
                     int def = -1) {
    std::string raw = param(req, names);
    if (raw.empty()) return def;
    try {
        return std::stoi(raw);
    } catch (...) {
        return def;
    }
}

/// 单名整型参数。
inline int int_param(const Request& req, const char* name, int def = -1) {
    return int_param(req, {name}, def);
}

/// 统一错误响应：设置状态码 + {"error": msg} JSON 体。
inline void json_error(Response& res, int status, const std::string& msg) {
    res.status = status;
    res.set_content(safeDump(json({{"error", msg}})), "application/json");
}

/// 统一成功响应：直接把 json 写回（application/json）。
inline void json_ok(Response& res, const json& j) {
    res.set_content(safeDump(j), "application/json");
}

/// 路由体异常包装器：执行 fn，捕获 std::exception → 500 {"error": what()}。
/// 用法：
///   svr.Get("/api/xxx", [&](const Request& req, Response& res) {
///       with_error_handling(res, [&] {
///           ... 业务逻辑，直接 throw 或返回 ...
///       });
///   });
template <typename Fn>
inline void with_error_handling(Response& res, Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::exception& e) {
        json_error(res, 500, e.what());
    } catch (...) {
        json_error(res, 500, "unknown error");
    }
}

}  // namespace ariaread::web

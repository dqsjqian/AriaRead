/// @file routes.cpp
/// @brief REST 路由注册总入口：按资源域分组委派到各 register_*_routes()。
///
/// 历史上本文件是 1500+ 行的巨型函数。现已按资源域拆分到 routes_*.cpp：
///   - routes_misc.cpp      health / shutdown / eval / url_history
///   - routes_search.cpp    /api/search*
///   - routes_catalog.cpp   /api/catalog* + /api/content
///   - routes_bookshelf.cpp /api/bookshelf/*
///   - routes_sources.cpp   /api/sources/*
///   - routes_rss.cpp       /api/rss/*
/// 公共 helper 见 http_helpers.h（参数解析 / 错误响应 / 异常包装）。

#include "routes.h"
#include "routes_internal.h"

namespace ariaread::web {

void register_routes(
    Server& svr,
    ariaread::BookSourceEngine& engine,
    aria::async::IExecutor& /*ui*/,
    aria::async::IExecutor& worker,
    ariaread::vm::SearchViewModel& /*svm*/,
    ariaread::vm::BookshelfViewModel& /*bvm*/,
    ariaread::vm::ReaderViewModel& /*rvm*/,
    ariaread::vm::SourceViewModel& /*srcvm*/) {

    register_misc_routes(svr, engine);
    register_search_routes(svr, engine);
    register_catalog_routes(svr, engine);
    register_bookshelf_routes(svr, engine, worker);
    register_sources_routes(svr, engine, worker);
    register_rss_routes(svr, engine);
}

}  // namespace ariaread::web

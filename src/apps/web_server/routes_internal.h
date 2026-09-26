/// @file routes_internal.h
/// @brief routes.cpp 拆分后的内部声明：各路由分组注册函数 + 共享符号。
///
/// register_routes()（routes.cpp）作为总入口，依次调用下列分组注册函数。
/// 每个分组一个 .cpp，按资源域划分，便于定位与维护。

#pragma once

#include "ariaread/engine.h"
#include "ariaread/vm/search_view_model.h"
#include "ariaread/vm/bookshelf_view_model.h"
#include "ariaread/vm/reader_view_model.h"
#include "ariaread/vm/source_view_model.h"

#include "aria/async/executor.hpp"

#include <atomic>
#include "continuo_server.h"

namespace ariaread::web {

/// 全局运行标志（main.cpp 定义；信号处理 + 长任务 SSE 提前退出共享）。
extern std::atomic<bool> g_running;

// ── 分组注册函数 ───────────────────────────────────────────────────
void register_misc_routes(Server& svr, ariaread::BookSourceEngine& engine);

void register_search_routes(Server& svr, ariaread::BookSourceEngine& engine);

void register_catalog_routes(Server& svr, ariaread::BookSourceEngine& engine);

void register_bookshelf_routes(Server& svr, ariaread::BookSourceEngine& engine,
                               aria::async::IExecutor& worker);

void register_sources_routes(Server& svr, ariaread::BookSourceEngine& engine,
                             aria::async::IExecutor& worker);

void register_rss_routes(Server& svr, ariaread::BookSourceEngine& engine);

}  // namespace ariaread::web

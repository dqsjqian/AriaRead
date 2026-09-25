/// @file routes.h
/// @brief REST 路由注册（/api/* → ViewModel 层）。

#pragma once

#include "openread/engine.h"
#include "openread/vm/search_view_model.h"
#include "openread/vm/bookshelf_view_model.h"
#include "openread/vm/reader_view_model.h"
#include "openread/vm/source_view_model.h"

#include "aria/async/executor.hpp"

#include "continuo_server.h"

namespace openread::web {

/// 注册所有 REST 路由到 Server。
///
/// 路由分组：
///   /api/health          — 健康检查
///   /api/search/*        — 搜索（SSE 流式）
///   /api/catalog/*       — 目录/章节
///   /api/content         — 正文内容
///   /api/bookshelf/*     — 书架管理
///   /api/sources/*       — 书源管理
///   /api/rss/*           — RSS 管理
///   /api/url_history     — URL 历史
///   /api/eval            — JS 调试（隔离执行，资源限制）
///   /api/source/debug    — 单源搜索/目录/正文诊断（SSE）
///   /api/shutdown        — 优雅关闭
void register_routes(
    Server& svr,
    openread::BookSourceEngine& engine,
    aria::async::IExecutor& ui,
    aria::async::IExecutor& worker,
    openread::vm::SearchViewModel& svm,
    openread::vm::BookshelfViewModel& bvm,
    openread::vm::ReaderViewModel& rvm,
    openread::vm::SourceViewModel& srcvm);

}  // namespace openread::web

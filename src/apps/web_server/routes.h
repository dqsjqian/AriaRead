/// @file routes.h
/// @brief REST 路由注册（/api/* → ViewModel 层）。

#pragma once

#include "ariaread/engine.h"
#include "ariaread/vm/search_view_model.h"
#include "ariaread/vm/bookshelf_view_model.h"
#include "ariaread/vm/reader_view_model.h"
#include "ariaread/vm/source_view_model.h"

#include "aria/async/executor.hpp"

#include "continuo_server.h"

namespace ariaread::web {

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
    ariaread::BookSourceEngine& engine,
    aria::async::IExecutor& ui,
    aria::async::IExecutor& worker,
    ariaread::vm::SearchViewModel& svm,
    ariaread::vm::BookshelfViewModel& bvm,
    ariaread::vm::ReaderViewModel& rvm,
    ariaread::vm::SourceViewModel& srcvm);

}  // namespace ariaread::web

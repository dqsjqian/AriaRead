#pragma once
/// @file engine_search_adapter.h
/// @brief 把 OpenRead 引擎的 searchAllConcurrent 适配成 SearchViewModel 所需的
///        StreamSearchFn。这是"引擎 ↔ 搜索 ViewModel"之间唯一的桥接点。
///
/// 设计：声明在 .h、实现在 .cpp。这样 engine.h（及其传递的第三方私有依赖
/// sqlite_modern_cpp / nlohmann_json）不会泄漏给所有消费 ViewModel 的 TU，
/// 只在适配器实现文件里出现一次。

#include "openread/vm/search_view_model.h"

namespace openread {
class BookSourceEngine;  // 前向声明，避免在头里包含 engine.h
}

namespace openread::vm {

/// 构造一个驱动真实引擎并发搜索的 StreamSearchFn。
///
/// @param engine       引擎实例（调用方保证其生命周期长于返回的函数被使用期间）
/// @param concurrency  并发线程数（透传给 searchAllConcurrent）
/// @param matchName    是否按书名过滤
/// @param matchAuthor  是否按作者过滤
/// @param matchIntro   是否按简介过滤
/// @return 可直接交给 SearchViewModel 的流式搜索函数
StreamSearchFn make_engine_stream_search(
    openread::BookSourceEngine& engine,
    int concurrency = 8,
    bool matchName = true,
    bool matchAuthor = false,
    bool matchIntro = false);

}  // namespace openread::vm

/// @file engine_search_adapter.cpp
/// @brief make_engine_stream_search 的实现——把引擎 searchAllConcurrent 的
///        逐源回调桥接到 SearchViewModel 的 StreamSearchFn。

#include "openread/vm/engine_search_adapter.h"

#include "openread/engine.h"   // 仅此 TU 引入 engine.h（含其私有第三方传递依赖）

#include <atomic>
#include <memory>

namespace openread::vm {

StreamSearchFn make_engine_stream_search(
    openread::BookSourceEngine& engine,
    int concurrency,
    bool matchName,
    bool matchAuthor,
    bool matchIntro) {

    return [&engine, concurrency, matchName, matchAuthor, matchIntro](
               const std::string& keyword,
               const SearchEmit& emit,
               const std::atomic<bool>& cancel) {
        // 引擎用 shared_ptr<CancelToken> 表达取消；ViewModel 用 std::atomic<bool>。
        // 在逐源回调里把外部 atomic 的取消意图传播到引擎 token——逐源回调是
        // 天然的高频检查点，无需额外监视线程。
        auto token = std::make_shared<openread::CancelToken>();

        // 逐源回调（worker 线程触发，线程安全）：
        openread::ConcurrentSearchCallback on_source =
            [&emit, &cancel, token](std::size_t /*sourceIndex*/,
                                    const std::string& /*sourceName*/,
                                    const std::vector<openread::Book>& books,
                                    int /*latencyMs*/,
                                    const std::string& error) {
                // 外部已请求取消 → 传播给引擎 token，使其尽快停止后续源。
                if (cancel.load(std::memory_order_acquire)) {
                    token->cancel();
                    return;
                }
                // 有错误的源跳过（ViewModel 层只关心成功结果的增量推送）。
                if (!error.empty()) return;
                if (!books.empty()) {
                    emit(books);
                }
            };

        // 驱动并发流式搜索，阻塞直到全部源完成或取消。
        engine.searchAllConcurrent(
            keyword,
            concurrency,
            on_source,
            /*doneCallback=*/nullptr,
            token,
            matchName,
            matchAuthor,
            matchIntro,
            /*sourceNames=*/{});
    };
}

}  // namespace openread::vm

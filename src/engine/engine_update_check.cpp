/// @file engine_update_check.cpp
/// @brief 书架批量更新检测（B4 拆分：从 engine_bookshelf.cpp 独立出来）

#include "ariaread/engine_impl.h"
#include <thread>
#include <chrono>

namespace ariaread {

// ──────────────────────────────────────────────
// 批量更新检测（全书架）
// ──────────────────────────────────────────────
int BookSourceEngine::checkAllUpdates(CheckUpdateCallback callback) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return 0;
    }

    auto items = pImpl->db->getBookshelf();
    int total = static_cast<int>(items.size());
    int updatedCount = 0;
    // A5: aliveFlag 副本，引擎析构时安全早退
    auto aliveFlag = pImpl->aliveFlag;

    for (int i = 0; i < total; ++i) {
        if (!aliveFlag->load(std::memory_order_acquire)) break;
        const auto& item = items[i];
        bool hasUpdate = false;

        try {
            hasUpdate = checkBookUpdate(item.bookUrl, item.sourceUrl, -1, item.sourceName);
        } catch (...) {}
        if (hasUpdate) ++updatedCount;
        if (callback) {
            callback(i + 1, total, item.bookName, hasUpdate);
        }

        // 每本书检查完后短暂让步，给 Web 请求线程机会获取 engineMutex
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return updatedCount;
}

} // namespace ariaread

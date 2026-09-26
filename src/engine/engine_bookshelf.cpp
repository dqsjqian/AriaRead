/// @file engine_bookshelf.cpp
/// @brief 引擎书架功能：收藏管理、阅读进度、换源、追更检查

#include "ariaread/engine_impl.h"
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <fstream>
#include <thread>
#include <mutex>
#include <atomic>

namespace ariaread {

namespace {

bool isCatalogValidForState(const std::vector<Chapter>& chapters) {
    if (chapters.empty()) return false;
    if (chapters.size() == 1 && detail::isBlank(chapters[0].title)) return false;
    for (const auto& ch : chapters) {
        if (!detail::isBlank(ch.title)) return true;
    }
    return false;
}

/// 防退化保护：新目录是否可以安全覆盖旧目录
/// 规则：如果旧目录有效且章节数 > 10，新目录章节数不能少于旧目录的 50%
/// 这样可以防止网络异常/解析失败时用垃圾数据覆盖好的缓存
bool canSafelyReplaceCatalog(int oldCount, const std::vector<Chapter>& newChapters) {
    int newCount = static_cast<int>(newChapters.size());
    // 旧目录为空或很少，任何有效新目录都可以覆盖
    if (oldCount <= 10) return true;
    // 新目录为空或只有1章空标题，明显是垃圾数据
    if (newChapters.empty()) return false;
    if (newChapters.size() == 1 && detail::isBlank(newChapters[0].title)) return false;
    // 新目录章节数不能少于旧目录的 50%（防止退化）
    if (newCount < oldCount / 2) return false;
    return true;
}

} // namespace

void BookSourceEngine::Impl::populateBookReadingStateFromDb(BookReadingState& state) {
    if (!db) return;
    state.catalogCached = db->getCachedCatalogCount(state.bookUrl);
    state.contentCached = db->getCachedContentCount(state.bookUrl);
    state.progress = db->getReadProgress(state.bookUrl);

    auto shelf = db->getBookshelf();
    for (const auto& item : shelf) {
        if (item.bookUrl == state.bookUrl) {
            state.totalChapters = item.totalChapters;
            state.hasUpdate = item.hasUpdate;
            if (state.kind.empty()) state.kind = item.kind;
            if (state.sourceName.empty()) state.sourceName = item.sourceName;
            break;
        }
    }

    state.isFinished = isFinishedKind(state.kind);
    if (state.totalChapters <= 0) state.totalChapters = state.catalogCached;
    state.fullyCached = state.totalChapters > 0 && state.contentCached >= state.totalChapters;
}

bool shouldAutoRefreshState(const BookReadingState& state) {
    return !(state.isFinished && state.fullyCached);
}

void BookSourceEngine::Impl::startRefreshWorker() {
    refreshWorker = std::thread([this]() {
        for (;;) {
            RefreshJob job;
            {
                std::unique_lock<std::mutex> lock(bookStateMutex);
                refreshCv.wait(lock, [this]() { return stopRefreshWorker || !refreshQueue.empty(); });
                if (stopRefreshWorker && refreshQueue.empty()) return;
                job = refreshQueue.front();
                refreshQueue.pop_front();
                queuedRefreshKeys.erase(job.key);
                auto it = bookStates.find(job.key);
                if (it != bookStates.end()) {
                    it->second.snapshot.refreshQueued = false;
                    it->second.snapshot.refreshInFlight = true;
                    it->second.snapshot.lastError.clear();
                }
            }

            // A5: 析构已开始（aliveFlag=false）时不再访问 owner——job 直接
            // 丢弃并退出。把 refresh worker 与引擎析构的赛跑窗口收窄到
            // "正在进行的调用"，后者由 stopWorker 的 join 覆盖。
            // （macOS CI 曾稳定出现 openBookSession 用例 SIGSEGV：worker
            // 无同步地读主线程栈上 engine 对象的成员，与栈回收竞态。）
            if (!aliveFlag->load(std::memory_order_acquire)) return;

            std::vector<Chapter> chapters;
            std::string error;
            try {
                // 尝试获取 engineMutex（非阻塞），避免与 Web 请求线程死锁
                // getCatalogForSource 内部会持有 engineMutex 做网络请求（可能很慢），
                // 如果此时 Web 请求线程也在等 engineMutex，会导致 UI 卡死。
                // 使用 try_lock：如果获取不到，把 job 放回队列稍后重试。
                // 注意经 this（Impl 自身成员）而非 owner->pImpl —— 后者是
                // 对主线程栈上对象的无锁跨线程读（TSan 报 data race）。
                if (engineMutex.try_lock()) {
                    engineMutex.unlock();
                    // engineMutex 可用，正常调用（getCatalogForSource 内部会再次获取锁）
                    chapters = owner->getCatalogForSource(job.bookUrl, job.sourceIndex, job.sourceName);
                } else {
                    // engineMutex 被占用（可能有 Web 请求正在执行），延迟重试
                    {
                        std::lock_guard<std::mutex> lock(bookStateMutex);
                        auto it = bookStates.find(job.key);
                        if (it != bookStates.end()) {
                            it->second.snapshot.refreshInFlight = false;
                            it->second.snapshot.refreshQueued = true;
                        }
                        // 放回队列尾部
                        queuedRefreshKeys.insert(job.key);
                        refreshQueue.push_back(job);
                    }
                    // 短暂等待后重试，避免忙等
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
            } catch (const std::exception& e) {
                error = e.what();
            } catch (...) {
                error = "unknown refresh error";
            }

            int cachedCatalog = 0;
            int cachedContent = 0;
            int totalChapters = 0;
            bool hasUpdate = false;
            {
                std::lock_guard<std::mutex> lock(bookStateMutex);
                auto it = bookStates.find(job.key);
                if (it == bookStates.end()) continue;
                auto& state = it->second.snapshot;
                state.refreshInFlight = false;
                state.lastRefreshAt = nowUnix();

                if (!error.empty()) {
                    state.lastError = error;
                    continue;
                }

                if (isCatalogValidForState(chapters) && db) {
                    // 防退化保护：检查旧目录章节数，避免用垃圾数据覆盖好的缓存
                    int oldCatalogCount = db->getCachedCatalogCount(job.bookUrl);
                    if (canSafelyReplaceCatalog(oldCatalogCount, chapters)) {
                        try {
                            db->cacheBookCatalog(job.bookUrl, chapters);
                        } catch (...) {}
                    } else {
                        // 目录退化被拒绝，使用旧的章节数（避免 totalChapters 被错误降低）
                        if (oldCatalogCount > static_cast<int>(chapters.size())) {
                            chapters.clear(); // 清空，后续不更新 totalChapters
                        }
                    }
                }

                totalChapters = static_cast<int>(chapters.size());
                if (totalChapters > 0) {
                    std::string latestChapter = chapters.back().title;
                    // 重新计算 hasUpdate：不累加旧值，每次刷新后完全基于当前状态判断
                    // 条件：用户有阅读进度 + 用户读到的章节不是最新章节
                    hasUpdate = !state.progress.chapterTitle.empty() &&
                                state.progress.chapterTitle != latestChapter;
                    if (db) {
                        try {
                            db->updateBookshelfLastChapter(job.bookUrl, latestChapter, totalChapters, hasUpdate);
                        } catch (...) {}
                    }
                }

                populateBookReadingStateFromDb(state);
                if (totalChapters > 0) state.totalChapters = totalChapters;
                state.hasUpdate = hasUpdate;
                state.fullyCached = state.totalChapters > 0 && state.contentCached >= state.totalChapters;
                state.lastError.clear();
                cachedCatalog = state.catalogCached;
                cachedContent = state.contentCached;
            }

            (void)cachedCatalog;
            (void)cachedContent;
        }
    });
}

void BookSourceEngine::Impl::stopWorker() {
    {
        std::lock_guard<std::mutex> lock(bookStateMutex);
        stopRefreshWorker = true;
    }
    refreshCv.notify_all();
    if (refreshWorker.joinable()) refreshWorker.join();
}

// ──────────────────────────────────────────────
// 书架管理
// ──────────────────────────────────────────────

int64_t BookSourceEngine::addToBookshelf(const BookshelfItem& item) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return -1;
    }
    return pImpl->db->addToBookshelf(item);
}

bool BookSourceEngine::removeFromBookshelf(const std::string& bookUrl) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return false;
    }
    return pImpl->db->removeFromBookshelf(bookUrl);
}

std::vector<BookshelfItem> BookSourceEngine::getBookshelf() {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return {};
    }
    return pImpl->db->getBookshelf();
}

std::vector<BookshelfDetail> BookSourceEngine::getBookshelfWithDetails() {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return {};
    }
    auto items = pImpl->db->getBookshelf();
    if (items.empty()) return {};

    // 构建 bookUrl → index 映射
    std::unordered_map<std::string, size_t> keyMap;
    std::vector<BookshelfDetail> result(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        keyMap[items[i].bookUrl] = i;
        result[i].item = std::move(items[i]);
    }

    // 批量查询目录缓存数量
    auto catalogCounts = pImpl->db->batchGetCachedCatalogCounts();
    for (const auto& [bookUrl, count] : catalogCounts) {
        auto it = keyMap.find(bookUrl);
        if (it != keyMap.end()) result[it->second].catalogCached = count;
    }

    // 批量查询正文缓存数量
    auto contentCounts = pImpl->db->batchGetCachedContentCounts();
    for (const auto& [bookUrl, count] : contentCounts) {
        auto it = keyMap.find(bookUrl);
        if (it != keyMap.end()) result[it->second].contentCached = count;
    }

    // 批量查询阅读进度
    auto progresses = pImpl->db->batchGetReadProgress();
    for (const auto& progress : progresses) {
        auto it = keyMap.find(progress.bookUrl);
        if (it != keyMap.end()) result[it->second].progress = progress;
    }

    return result;
}

bool BookSourceEngine::isInBookshelf(const std::string& bookUrl) {
    if (!pImpl->db) return false;
    return pImpl->db->isInBookshelf(bookUrl);
}

// ──────────────────────────────────────────────
// 阅读进度
// ──────────────────────────────────────────────

void BookSourceEngine::saveReadProgress(const ReadProgress& progress) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return;
    }
    pImpl->db->saveReadProgress(progress);

    std::lock_guard<std::mutex> lock(pImpl->bookStateMutex);
    auto key = Impl::makeBookStateKey(progress.bookUrl, "");
    auto& entry = pImpl->bookStates[key];
    if (entry.snapshot.bookUrl.empty()) {
        entry.snapshot.bookUrl = progress.bookUrl;
    }
    entry.snapshot.progress = progress;
    entry.snapshot.lastOpenedAt = Impl::nowUnix();
}

ReadProgress BookSourceEngine::getReadProgress(const std::string& bookUrl) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return {};
    }
    return pImpl->db->getReadProgress(bookUrl);
}

// ──────────────────────────────────────────────
// 换源
// ──────────────────────────────────────────────

void BookSourceEngine::changeBookSource(const std::string& bookUrl, const std::string& oldSourceUrl,
                                         const std::string& newSourceName, const std::string& newSourceUrl,
                                         const std::string& newBookUrl) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return;
    }
    // 换源：删除旧 book_url 的所有缓存，更新 bookshelf 主键为新 book_url
    pImpl->db->updateBookshelfSource(bookUrl, newSourceName, newSourceUrl, newBookUrl);

    // 同步清理内存中旧 key 的状态
    std::lock_guard<std::mutex> lock(pImpl->bookStateMutex);
    auto oldKey = Impl::makeBookStateKey(bookUrl, "");
    pImpl->bookStates.erase(oldKey);
}

// ──────────────────────────────────────────────
// 追更检查（单本）
// ──────────────────────────────────────────────

bool BookSourceEngine::checkBookUpdate(const std::string& bookUrl, const std::string& sourceUrl,
                                        int sourceIndex, const std::string& sourceName) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return false;
    }

    // 获取当前书架记录
    auto shelf = pImpl->db->getBookshelf();
    std::string oldLastChapter;
    for (const auto& item : shelf) {
        if (item.bookUrl == bookUrl) {
            oldLastChapter = item.lastChapter;
            break;
        }
    }

    // 获取最新目录（原样使用，不做任何去重处理）
    auto chapters = getCatalogForSource(bookUrl, sourceIndex, sourceName);
    if (chapters.empty()) return false;

    // 对比最新章节
    const auto& latestChapter = chapters.back().title;
    int totalChapters = static_cast<int>(chapters.size());
    bool hasUpdate = !oldLastChapter.empty() && latestChapter != oldLastChapter;

    // 同步更新目录缓存表（带防退化保护：避免用垃圾数据覆盖好的缓存）
    int oldCatalogCount = pImpl->db->getCachedCatalogCount(bookUrl);
    bool catalogUpdated = false;
    try {
        if (canSafelyReplaceCatalog(oldCatalogCount, chapters)) {
            pImpl->db->cacheBookCatalog(bookUrl, chapters);
            catalogUpdated = true;
        }
    } catch (...) {}

    // 更新书架表（如果目录退化被拒绝，totalChapters 取旧值的 max，避免被错误降低）
    if (!catalogUpdated && oldCatalogCount > totalChapters) {
        totalChapters = oldCatalogCount;
    }
    pImpl->db->updateBookshelfLastChapter(bookUrl, latestChapter, totalChapters, hasUpdate);

    return hasUpdate;
}

// ──────────────────────────────────────────────
// 更新书架条目的最新章节信息
// ──────────────────────────────────────────────

void BookSourceEngine::updateBookshelfLastChapter(const std::string& bookUrl, const std::string& sourceUrl,
                                                   const std::string& lastChapter, int totalChapters, bool hasUpdate) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return;
    }
    pImpl->db->updateBookshelfLastChapter(bookUrl, lastChapter, totalChapters, hasUpdate);

    std::lock_guard<std::mutex> lock(pImpl->bookStateMutex);
    auto key = Impl::makeBookStateKey(bookUrl, "");
    auto it = pImpl->bookStates.find(key);
    if (it != pImpl->bookStates.end()) {
        it->second.snapshot.totalChapters = totalChapters;
        it->second.snapshot.hasUpdate = hasUpdate;
        it->second.snapshot.fullyCached = totalChapters > 0 && it->second.snapshot.contentCached >= totalChapters;
        it->second.snapshot.lastRefreshAt = Impl::nowUnix();
    }
}

// ──────────────────────────────────────────────
// 单本书状态管理（per-book state）
// ──────────────────────────────────────────────

BookReadingState BookSourceEngine::openBookSession(const std::string& bookUrl,
                                                   const std::string& sourceUrl,
                                                   int sourceIndex,
                                                   const std::string& sourceName,
                                                   const std::string& kind) {
    BookReadingState snapshot;
    snapshot.bookUrl = bookUrl;
    snapshot.sourceUrl = sourceUrl;
    snapshot.sourceIndex = sourceIndex;
    snapshot.sourceName = sourceName;
    snapshot.kind = kind;
    snapshot.lastOpenedAt = Impl::nowUnix();
    pImpl->populateBookReadingStateFromDb(snapshot);

    {
        std::lock_guard<std::mutex> lock(pImpl->bookStateMutex);
        auto key = Impl::makeBookStateKey(bookUrl, "");
        auto& entry = pImpl->bookStates[key];
        if (!entry.snapshot.bookUrl.empty()) {
            if (snapshot.sourceName.empty()) snapshot.sourceName = entry.snapshot.sourceName;
            if (snapshot.kind.empty()) snapshot.kind = entry.snapshot.kind;
            if (snapshot.sourceIndex < 0) snapshot.sourceIndex = entry.snapshot.sourceIndex;
        }
        entry.snapshot = snapshot;
    }

    if (shouldAutoRefreshState(snapshot)) {
        requestBookRefresh(bookUrl, sourceUrl, sourceIndex, sourceName, kind);
    }
    return getBookReadingState(bookUrl, sourceUrl);
}

BookReadingState BookSourceEngine::getBookReadingState(const std::string& bookUrl,
                                                       const std::string& sourceUrl) const {
    std::lock_guard<std::mutex> lock(pImpl->bookStateMutex);
    auto key = Impl::makeBookStateKey(bookUrl, "");
    auto it = pImpl->bookStates.find(key);
    if (it != pImpl->bookStates.end()) return it->second.snapshot;
    BookReadingState empty;
    empty.bookUrl = bookUrl;
    empty.sourceUrl = sourceUrl;
    return empty;
}

std::vector<BookReadingState> BookSourceEngine::listBookReadingStates() const {
    std::lock_guard<std::mutex> lock(pImpl->bookStateMutex);
    std::vector<BookReadingState> result;
    result.reserve(pImpl->bookStates.size());
    for (const auto& [_, entry] : pImpl->bookStates) {
        result.push_back(entry.snapshot);
    }
    std::sort(result.begin(), result.end(), [](const BookReadingState& a, const BookReadingState& b) {
        return a.lastOpenedAt > b.lastOpenedAt;
    });
    return result;
}

bool BookSourceEngine::requestBookRefresh(const std::string& bookUrl,
                                          const std::string& sourceUrl,
                                          int sourceIndex,
                                          const std::string& sourceName,
                                          const std::string& kind) {
    if (bookUrl.empty()) return false;
    auto key = Impl::makeBookStateKey(bookUrl, "");
    {
        std::lock_guard<std::mutex> lock(pImpl->bookStateMutex);
        auto& entry = pImpl->bookStates[key];
        if (entry.snapshot.bookUrl.empty()) {
            entry.snapshot.bookUrl = bookUrl;
            entry.snapshot.sourceUrl = sourceUrl;
        }
        if (!sourceName.empty()) entry.snapshot.sourceName = sourceName;
        if (!kind.empty()) {
            entry.snapshot.kind = kind;
            entry.snapshot.isFinished = Impl::isFinishedKind(kind);
        }
        if (sourceIndex >= 0) entry.snapshot.sourceIndex = sourceIndex;
        entry.snapshot.lastOpenedAt = Impl::nowUnix();
        if (entry.snapshot.refreshInFlight || pImpl->queuedRefreshKeys.count(key)) {
            return true;
        }
        entry.snapshot.refreshQueued = true;
        pImpl->queuedRefreshKeys.insert(key);
        pImpl->refreshQueue.push_back({ key, bookUrl, sourceUrl, entry.snapshot.sourceName, entry.snapshot.kind, entry.snapshot.sourceIndex });
    }
    pImpl->refreshCv.notify_one();
    return true;
}

// ──────────────────────────────────────────────
// 缓存管理（目录 + 正文）
// ──────────────────────────────────────────────

void BookSourceEngine::cacheBookCatalog(const std::string& bookUrl, const std::string& sourceUrl,
                                         const std::vector<Chapter>& chapters) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return;
    }
    pImpl->db->cacheBookCatalog(bookUrl, chapters);

    std::lock_guard<std::mutex> lock(pImpl->bookStateMutex);
    auto key = Impl::makeBookStateKey(bookUrl, "");
    auto& entry = pImpl->bookStates[key];
    if (entry.snapshot.bookUrl.empty()) entry.snapshot.bookUrl = bookUrl;
    entry.snapshot.catalogCached = static_cast<int>(chapters.size());
    entry.snapshot.totalChapters = std::max(entry.snapshot.totalChapters, static_cast<int>(chapters.size()));
    entry.snapshot.fullyCached = entry.snapshot.totalChapters > 0 && entry.snapshot.contentCached >= entry.snapshot.totalChapters;
}

std::vector<Chapter> BookSourceEngine::getCachedCatalog(const std::string& bookUrl, const std::string& sourceUrl) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return {};
    }
    return pImpl->db->getCachedCatalog(bookUrl);
}

int BookSourceEngine::getCachedCatalogCount(const std::string& bookUrl, const std::string& sourceUrl) {
    if (!pImpl->db) return 0;
    return pImpl->db->getCachedCatalogCount(bookUrl);
}

void BookSourceEngine::cacheChapterContent(const std::string& chapterUrl, const std::string& bookUrl,
                                            const std::string& sourceUrl, const std::string& content) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return;
    }
    // 需要 chapter_index，从目录缓存中查找
    auto chapters = pImpl->db->getCachedCatalog(bookUrl);
    int idx = -1;
    for (const auto& ch : chapters) {
        if (ch.url == chapterUrl) { idx = ch.index; break; }
    }
    if (idx < 0) return; // 目录中找不到该 url，无法确定 index
    pImpl->db->cacheChapterContent(bookUrl, idx, chapterUrl, content);

    std::lock_guard<std::mutex> lock(pImpl->bookStateMutex);
    auto key = Impl::makeBookStateKey(bookUrl, "");
    auto& entry = pImpl->bookStates[key];
    if (entry.snapshot.bookUrl.empty()) entry.snapshot.bookUrl = bookUrl;
    entry.snapshot.contentCached = pImpl->db->getCachedContentCount(bookUrl);
    entry.snapshot.fullyCached = entry.snapshot.totalChapters > 0 && entry.snapshot.contentCached >= entry.snapshot.totalChapters;
}

std::string BookSourceEngine::getCachedContent(const std::string& bookUrl, int chapterIndex) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return "";
    }
    return pImpl->db->getCachedContent(bookUrl, chapterIndex);
}

int BookSourceEngine::getCachedContentCount(const std::string& bookUrl, const std::string& sourceUrl) {
    if (!pImpl->db) return 0;
    return pImpl->db->getCachedContentCount(bookUrl);
}

void BookSourceEngine::clearBookCache(const std::string& bookUrl, const std::string& sourceUrl) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return;
    }
    pImpl->db->removeCachedCatalog(bookUrl);
    pImpl->db->removeCachedContent(bookUrl);

    std::lock_guard<std::mutex> lock(pImpl->bookStateMutex);
    auto key = Impl::makeBookStateKey(bookUrl, "");
    auto it = pImpl->bookStates.find(key);
    if (it != pImpl->bookStates.end()) {
        it->second.snapshot.catalogCached = 0;
        it->second.snapshot.contentCached = 0;
        it->second.snapshot.fullyCached = false;
        it->second.snapshot.hasUpdate = false;
        it->second.snapshot.lastRefreshAt = Impl::nowUnix();
    }
}

// ──────────────────────────────────────────────
// 全量下载缓存 → 见 engine_download.cpp（B4 已拆分）
// 批量更新检测   → 见 engine_update_check.cpp（B4 已拆分）
// ──────────────────────────────────────────────

} // namespace ariaread

#pragma once
/// @file wrapper.hpp
/// @brief AriaRead C++ 便捷绑定 —— 供 Qt / 其他 C++ GUI 直接使用
///
/// 不走 C 函数指针，直接用 C++ 类和 std::string/std::vector，
/// 更符合 C++ 项目习惯。底层调用 bridge.h C API。
///
/// 用法：
///   #include <ariaread/wrapper.hpp>
///   ariaread::EngineWrapper engine;
///   engine.loadSourcesFromFile("sources.json");
///   auto books = engine.search("斗破苍穹");

#include "ariaread/bridge.h"
#include "ariaread/types.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <stdexcept>

namespace ariaread {

/// 引擎 RAII 包装 —— 构造即创建，析构即销毁
/// 注意：类名改为 EngineWrapper 以避免与核心 Engine 类冲突
class EngineWrapper {
public:
    EngineWrapper() : handle_(ariaread_engine_create()) {
        if (!handle_) {
            throw std::runtime_error("Failed to create AriaRead engine");
        }
    }

    ~EngineWrapper() {
        if (handle_) {
            ariaread_engine_destroy(handle_);
        }
    }

    // 禁止拷贝，允许移动
    EngineWrapper(const EngineWrapper&) = delete;
    EngineWrapper& operator=(const EngineWrapper&) = delete;
    EngineWrapper(EngineWrapper&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    EngineWrapper& operator=(EngineWrapper&& other) noexcept {
        if (this != &other) {
            if (handle_) ariaread_engine_destroy(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    // ──────────────────────────────────────────────
    // HTTP 回调（C++ 风格）
    // ──────────────────────────────────────────────
    using HttpCallback = std::function<std::string(
        const std::string& url,
        const std::string& method,
        const std::string& headers,
        const std::string& body
    )>;

    void setHttpCallback(HttpCallback callback) {
        httpCallback_ = std::move(callback);
        if (httpCallback_) {
            ariaread_engine_set_http_callback(
                handle_,
                [](const char* url, const char* method,
                   const char* headers, const char* body,
                   void* userData) -> char* {
                    auto* self = static_cast<EngineWrapper*>(userData);
                    std::string result = self->httpCallback_(
                        url ? url : "",
                        method ? method : "",
                        headers ? headers : "",
                        body ? body : ""
                    );
                    return strdup(result.c_str());
                },
                this
            );
        } else {
            ariaread_engine_set_http_callback(handle_, nullptr, nullptr);
        }
    }

    // ──────────────────────────────────────────────
    // 日志回调（C++ 风格）
    // ──────────────────────────────────────────────
    using LogCallback = std::function<void(const std::string& message)>;

    void setLogCallback(LogCallback callback) {
        logCallback_ = std::move(callback);
        if (logCallback_) {
            ariaread_engine_set_log_callback(
                handle_,
                [](const char* message, void* userData) {
                    auto* self = static_cast<EngineWrapper*>(userData);
                    self->logCallback_(message ? message : "");
                },
                this
            );
        } else {
            ariaread_engine_set_log_callback(handle_, nullptr, nullptr);
        }
    }

    // ──────────────────────────────────────────────
    // 书源管理
    // ──────────────────────────────────────────────
    bool loadSource(const std::string& json) {
        return ariaread_engine_load_source(handle_, json.c_str()) == 0;
    }

    int loadSources(const std::string& jsonArray) {
        return ariaread_engine_load_sources(handle_, jsonArray.c_str());
    }

    int loadSourcesFromFile(const std::string& filePath) {
        return ariaread_engine_load_sources_from_file(handle_, filePath.c_str());
    }

    bool selectSource(int index) {
        return ariaread_engine_select_source(handle_, index) == 0;
    }

    bool selectSourceByName(const std::string& name) {
        return ariaread_engine_select_source_by_name(handle_, name.c_str()) == 0;
    }

    int sourceCount() const {
        return ariaread_engine_source_count(handle_);
    }

    // ──────────────────────────────────────────────
    // 核心操作
    // ──────────────────────────────────────────────
    std::vector<Book> search(const std::string& keyword) {
        char* result = ariaread_engine_search(handle_, keyword.c_str());
        std::vector<Book> books;
        if (result) {
            books = parseBooks(result);
            ariaread_free_string(result);
        }
        return books;
    }

    std::vector<Chapter> getCatalog(const std::string& bookUrl) {
        char* result = ariaread_engine_get_catalog(handle_, bookUrl.c_str());
        std::vector<Chapter> chapters;
        if (result) {
            chapters = parseChapters(result);
            ariaread_free_string(result);
        }
        return chapters;
    }

    std::string getContent(const std::string& chapterUrl) {
        char* result = ariaread_engine_get_content(handle_, chapterUrl.c_str());
        std::string content;
        if (result) {
            content = result;
            ariaread_free_string(result);
        }
        return content;
    }

    std::vector<Chapter> getCatalogForSource(const std::string& bookUrl, int sourceIndex, const std::string& sourceName = "") {
        char* result = ariaread_engine_get_catalog_for_source(handle_, bookUrl.c_str(), sourceIndex, sourceName.c_str());
        std::vector<Chapter> chapters;
        if (result) {
            chapters = parseChapters(result);
            ariaread_free_string(result);
        }
        return chapters;
    }

    std::string getContentForSource(const std::string& chapterUrl, int sourceIndex, const std::string& sourceName = "") {
        char* result = ariaread_engine_get_content_for_source(handle_, chapterUrl.c_str(), sourceIndex, sourceName.c_str());
        std::string content;
        if (result) {
            content = result;
            ariaread_free_string(result);
        }
        return content;
    }

    // ──────────────────────────────────────────────
    // 数据库持久化
    // ──────────────────────────────────────────────
    void setDatabasePath(const std::string& dbPath) {
        ariaread_engine_set_database_path(handle_, dbPath.c_str());
    }

    int loadSourcesFromDatabase() {
        return ariaread_engine_load_sources_from_database(handle_);
    }

    int removeInvalidSources() {
        return ariaread_engine_remove_invalid_sources(handle_);
    }

    int clearAllSources() {
        return ariaread_engine_clear_all_sources(handle_);
    }

    // ──────────────────────────────────────────────
    // 书架管理
    // ──────────────────────────────────────────────

    int64_t addToBookshelf(const std::string& bookJson) {
        return ariaread_bookshelf_add(handle_, bookJson.c_str());
    }

    bool removeFromBookshelf(const std::string& bookUrl, const std::string& sourceUrl = "") {
        return ariaread_bookshelf_remove(handle_, bookUrl.c_str(), sourceUrl.c_str()) == 0;
    }

    std::string getBookshelfJson() {
        char* result = ariaread_bookshelf_list(handle_);
        std::string json;
        if (result) {
            json = result;
            ariaread_free_string(result);
        }
        return json;
    }

    bool isInBookshelf(const std::string& bookUrl, const std::string& sourceUrl = "") {
        return ariaread_bookshelf_is_in(handle_, bookUrl.c_str(), sourceUrl.c_str()) != 0;
    }

    bool saveReadProgress(const std::string& progressJson) {
        return ariaread_bookshelf_progress_save(handle_, progressJson.c_str()) == 0;
    }

    std::string getReadProgressJson(const std::string& bookUrl, const std::string& sourceUrl = "") {
        char* result = ariaread_bookshelf_progress_get(handle_, bookUrl.c_str(), sourceUrl.c_str());
        std::string json;
        if (result) {
            json = result;
            ariaread_free_string(result);
        }
        return json;
    }

    bool changeBookSource(const std::string& bookUrl, const std::string& oldSourceUrl,
                           const std::string& newSourceName, const std::string& newSourceUrl,
                           const std::string& newBookUrl) {
        return ariaread_bookshelf_change_source(handle_,
            bookUrl.c_str(), oldSourceUrl.c_str(),
            newSourceName.c_str(), newSourceUrl.c_str(), newBookUrl.c_str()) == 0;
    }

    int checkBookUpdate(const std::string& bookUrl, const std::string& sourceUrl,
                         int sourceIndex = -1, const std::string& sourceName = "") {
        return ariaread_bookshelf_check_update(handle_,
            bookUrl.c_str(), sourceUrl.c_str(), sourceIndex, sourceName.c_str());
    }

    int updateBookshelfLastChapter(const std::string& bookUrl, const std::string& sourceUrl,
                                    const std::string& lastChapter, int totalChapters, bool hasUpdate) {
        return ariaread_bookshelf_update_last_chapter(handle_,
            bookUrl.c_str(), sourceUrl.c_str(), lastChapter.c_str(), totalChapters, hasUpdate ? 1 : 0);
    }

    std::string downloadBook(const std::string& bookUrl, const std::string& sourceUrl,
                              int sourceIndex = -1, const std::string& sourceName = "", int sleepMs = 300) {
        char* result = ariaread_bookshelf_download(handle_,
            bookUrl.c_str(), sourceUrl.c_str(), sourceIndex, sourceName.c_str(), sleepMs);
        std::string json;
        if (result) { json = result; ariaread_free_string(result); }
        return json;
    }

    int checkAllUpdates() {
        return ariaread_bookshelf_check_all_updates(handle_);
    }

    void setConcurrency(int n) {
        ariaread_engine_set_concurrency(handle_, n);
    }

    // ──────────────────────────────────────────────
    // 并发操作（C++ 风格，通过 C API 桥接）
    // ──────────────────────────────────────────────

    using ValidateCallback = std::function<void(
        int sourceIndex, const std::string& sourceName,
        int validity, int latencyMs, const std::string& detail
    )>;

    using SearchCallback = std::function<void(
        int sourceIndex, const std::string& sourceName,
        const std::string& booksJson, int latencyMs, const std::string& error
    )>;

    using DoneCallback = std::function<void(int c1, int c2, int c3)>;

    void validateConcurrent(
        const std::string& testQuery = "\xe6\x88\x91",
        int timeoutMs = 10000,
        int concurrency = 8,
        ValidateCallback callback = nullptr,
        DoneCallback doneCallback = nullptr
    ) {
        validateCallback_ = std::move(callback);
        validateDoneCallback_ = std::move(doneCallback);

        ariaread_engine_validate_concurrent(
            handle_, testQuery.c_str(), timeoutMs, concurrency,
            validateCallback_ ? [](int idx, const char* name, int validity,
                                    int latencyMs, const char* detail, void* ud) {
                auto* self = static_cast<EngineWrapper*>(ud);
                self->validateCallback_(idx, name ? name : "", validity,
                                         latencyMs, detail ? detail : "");
            } : nullptr,
            validateDoneCallback_ ? [](int c1, int c2, int c3, void* ud) {
                auto* self = static_cast<EngineWrapper*>(ud);
                self->validateDoneCallback_(c1, c2, c3);
            } : nullptr,
            this, nullptr
        );
    }

    void searchAllConcurrent(
        const std::string& keyword,
        int concurrency = 8,
        SearchCallback callback = nullptr,
        DoneCallback doneCallback = nullptr
    ) {
        searchCallback_ = std::move(callback);
        searchDoneCallback_ = std::move(doneCallback);

        ariaread_engine_search_all_concurrent(
            handle_, keyword.c_str(), concurrency,
            searchCallback_ ? [](int idx, const char* name, const char* booksJson,
                                  int latencyMs, const char* error, void* ud) {
                auto* self = static_cast<EngineWrapper*>(ud);
                self->searchCallback_(idx, name ? name : "",
                                       booksJson ? booksJson : "[]",
                                       latencyMs, error ? error : "");
            } : nullptr,
            searchDoneCallback_ ? [](int c1, int c2, int c3, void* ud) {
                auto* self = static_cast<EngineWrapper*>(ud);
                self->searchDoneCallback_(c1, c2, c3);
            } : nullptr,
            this, nullptr,
            1, 0, 0
        );
    }

    // ──────────────────────────────────────────────
    // 状态查询
    // ──────────────────────────────────────────────
    std::string getSourceInfo() {
        char* result = ariaread_engine_get_source_info(handle_);
        std::string info;
        if (result) {
            info = result;
            ariaread_free_string(result);
        }
        return info;
    }

    std::string getLastError() const {
        return ariaread_engine_get_last_error(handle_);
    }

    bool isAvailable() const {
        return ariaread_engine_is_available(handle_) != 0;
    }

    static const char* version() {
        return ariaread_version();
    }

    AriaReadEngine nativeHandle() const {
        return handle_;
    }

private:
    AriaReadEngine handle_;
    HttpCallback httpCallback_;
    LogCallback logCallback_;
    ValidateCallback validateCallback_;
    DoneCallback validateDoneCallback_;
    SearchCallback searchCallback_;
    DoneCallback searchDoneCallback_;

    // ──────────────────────────────────────────────
    // JSON 解析（使用 nlohmann/json，可靠且安全）
    // ──────────────────────────────────────────────
    static std::vector<Book> parseBooks(const char* jsonStr) {
        std::vector<Book> books;
        try {
            auto arr = nlohmann::json::parse(jsonStr);
            if (!arr.is_array()) return books;
            for (const auto& j : arr) {
                Book b;
                b.name = j.value("name", "");
                b.author = j.value("author", "");
                b.coverUrl = j.value("coverUrl", "");
                b.bookUrl = j.value("bookUrl", "");
                b.lastChapter = j.value("lastChapter", "");
                b.intro = j.value("intro", "");
                b.kind = j.value("kind", "");
                b.wordCount = j.value("wordCount", "");
                b.matchScore = j.value("matchScore", 0);
                books.push_back(std::move(b));
            }
        } catch (...) {}
        return books;
    }

    static std::vector<Chapter> parseChapters(const char* jsonStr) {
        std::vector<Chapter> chapters;
        try {
            auto arr = nlohmann::json::parse(jsonStr);
            if (!arr.is_array()) return chapters;
            for (const auto& j : arr) {
                Chapter c;
                c.title = j.value("title", "");
                c.url = j.value("url", "");
                c.index = j.value("index", 0);
                c.isVip = j.value("isVip", false);
                chapters.push_back(std::move(c));
            }
        } catch (...) {}
        return chapters;
    }
};

// 向后兼容：保留 Engine 别名
using Engine = EngineWrapper;

}  // namespace ariaread

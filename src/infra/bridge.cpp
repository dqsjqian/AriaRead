#include "openread/bridge.h"
#include "openread/engine.h"
#include "openread/version.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <memory>
#include <thread>

using namespace openread;
using json = nlohmann::json;

// ──────────────────────────────────────────────
// 辅助：Book → JSON
// ──────────────────────────────────────────────
static std::string bookToJson(const Book& b) {
    json j;
    j["name"] = b.name;
    j["author"] = b.author;
    j["coverUrl"] = b.coverUrl;
    j["bookUrl"] = b.bookUrl;
    j["lastChapter"] = b.lastChapter;
    j["intro"] = b.intro;
    j["kind"] = b.kind;
    j["wordCount"] = b.wordCount;
    j["matchScore"] = b.matchScore;
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// ──────────────────────────────────────────────
// 辅助：Chapter → JSON
// ──────────────────────────────────────────────
static std::string chapterToJson(const Chapter& c) {
    json j;
    j["title"] = c.title;
    j["url"] = c.url;
    j["index"] = c.index;
    j["isVip"] = c.isVip;
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// ──────────────────────────────────────────────
// 生命周期
// ──────────────────────────────────────────────
OpenReadEngine openread_engine_create() {
    try {
        auto* engine = new BookSourceEngine();
        return static_cast<OpenReadEngine>(engine);
    } catch (...) {
        return nullptr;
    }
}

void openread_engine_destroy(OpenReadEngine engine) {
    if (engine) {
        delete static_cast<BookSourceEngine*>(engine);
    }
}

// ──────────────────────────────────────────────
// 配置
// ──────────────────────────────────────────────
void openread_engine_set_http_callback(
    OpenReadEngine engine,
    OpenReadHttpCallback callback,
    void* userData
) {
    if (!engine || !callback) return;

    auto* e = static_cast<BookSourceEngine*>(engine);

    // 包装 C 回调为 C++ lambda
    auto httpFunc = [callback, userData](
        const std::string& url,
        const std::string& method,
        const std::string& headers,
        const std::string& body
    ) -> std::string {
        char* result = callback(url.c_str(), method.c_str(),
                                headers.c_str(), body.c_str(), userData);
        if (result) {
            std::string str(result);
            openread_free_string(result);
            return str;
        }
        return "";
    };

    e->setHttpRequest(httpFunc);
}

void openread_engine_set_log_callback(
    OpenReadEngine engine,
    OpenReadLogCallback callback,
    void* userData
) {
    if (!engine || !callback) return;

    auto* e = static_cast<BookSourceEngine*>(engine);

    auto logFunc = [callback, userData](const std::string& msg) {
        callback(msg.c_str(), userData);
    };

    e->setJsLogCallback(logFunc);
}

// ──────────────────────────────────────────────
// 书源管理
// ──────────────────────────────────────────────
int openread_engine_load_source(OpenReadEngine engine, const char* jsonStr) {
    if (!engine || !jsonStr) return -1;
    auto* e = static_cast<BookSourceEngine*>(engine);
    return e->loadSource(jsonStr) ? 0 : -1;
}

int openread_engine_load_sources(OpenReadEngine engine, const char* jsonArray) {
    if (!engine || !jsonArray) return -1;
    auto* e = static_cast<BookSourceEngine*>(engine);
    return e->loadSources(jsonArray);
}

int openread_engine_load_sources_from_file(OpenReadEngine engine, const char* filePath) {
    if (!engine || !filePath) return -1;
    auto* e = static_cast<BookSourceEngine*>(engine);
    return e->loadSourcesFromFile(filePath);
}

int openread_engine_select_source(OpenReadEngine engine, int index) {
    if (!engine) return -1;
    auto* e = static_cast<BookSourceEngine*>(engine);
    return e->selectSource(static_cast<size_t>(index)) ? 0 : -1;
}

int openread_engine_select_source_by_name(OpenReadEngine engine, const char* name) {
    if (!engine || !name) return -1;
    auto* e = static_cast<BookSourceEngine*>(engine);
    return e->selectSourceByName(name) ? 0 : -1;
}

int openread_engine_source_count(OpenReadEngine engine) {
    if (!engine) return 0;
    auto* e = static_cast<BookSourceEngine*>(engine);
    return static_cast<int>(e->sources().size());
}

// ──────────────────────────────────────────────
// 核心操作
// ──────────────────────────────────────────────
char* openread_engine_search(OpenReadEngine engine, const char* keyword) {
    if (!engine || !keyword) return strdup("[]");

    auto* e = static_cast<BookSourceEngine*>(engine);
    auto books = e->search(keyword);

    json arr = json::array();
    for (const auto& b : books) {
        arr.push_back(json::parse(bookToJson(b)));
    }
    return strdup(arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
}

char* openread_engine_get_catalog(OpenReadEngine engine, const char* bookUrl) {
    if (!engine || !bookUrl) return strdup("[]");

    auto* e = static_cast<BookSourceEngine*>(engine);
    auto chapters = e->getCatalog(bookUrl);

    json arr = json::array();
    for (const auto& c : chapters) {
        arr.push_back(json::parse(chapterToJson(c)));
    }
    return strdup(arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
}

char* openread_engine_get_content(OpenReadEngine engine, const char* chapterUrl) {
    if (!engine || !chapterUrl) return strdup("");

    auto* e = static_cast<BookSourceEngine*>(engine);
    std::string content = e->getContent(chapterUrl);
    return strdup(content.c_str());
}

char* openread_engine_get_catalog_for_source(
    OpenReadEngine engine,
    const char* bookUrl,
    int sourceIndex,
    const char* sourceName
) {
    if (!engine || !bookUrl) return strdup("[]");

    auto* e = static_cast<BookSourceEngine*>(engine);
    auto chapters = e->getCatalogForSource(bookUrl, sourceIndex, sourceName ? sourceName : "");

    json arr = json::array();
    for (const auto& c : chapters) {
        arr.push_back(json::parse(chapterToJson(c)));
    }
    return strdup(arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
}

char* openread_engine_get_content_for_source(
    OpenReadEngine engine,
    const char* chapterUrl,
    int sourceIndex,
    const char* sourceName
) {
    if (!engine || !chapterUrl) return strdup("");

    auto* e = static_cast<BookSourceEngine*>(engine);
    std::string content = e->getContentForSource(chapterUrl, sourceIndex, sourceName ? sourceName : "");
    return strdup(content.c_str());
}

int openread_engine_clear_all_sources(OpenReadEngine engine) {
    if (!engine) return 0;
    auto* e = static_cast<BookSourceEngine*>(engine);
    return e->clearAllSources();
}

// ──────────────────────────────────────────────
// 书架管理
// ──────────────────────────────────────────────

int64_t openread_bookshelf_add(OpenReadEngine engine, const char* bookJson) {
    if (!engine || !bookJson) return -1;
    auto* e = static_cast<BookSourceEngine*>(engine);
    try {
        auto j = json::parse(bookJson);
        BookshelfItem item;
        item.bookName = j.value("bookName", "");
        item.bookAuthor = j.value("bookAuthor", "");
        item.coverUrl = j.value("coverUrl", "");
        item.bookUrl = j.value("bookUrl", "");
        item.sourceName = j.value("sourceName", "");
        item.sourceUrl = j.value("sourceUrl", "");
        item.intro = j.value("intro", "");
        item.kind = j.value("kind", "");
        item.lastChapter = j.value("lastChapter", "");
        return e->addToBookshelf(item);
    } catch (...) {
        return -1;
    }
}

int openread_bookshelf_remove(OpenReadEngine engine, const char* bookUrl, const char* sourceUrl) {
    if (!engine || !bookUrl) return -1;
    auto* e = static_cast<BookSourceEngine*>(engine);
    return e->removeFromBookshelf(bookUrl) ? 0 : -1;
}

char* openread_bookshelf_list(OpenReadEngine engine) {
    if (!engine) return strdup("[]");
    auto* e = static_cast<BookSourceEngine*>(engine);
    auto items = e->getBookshelf();
    json arr = json::array();
    for (const auto& item : items) {
        json j;
        j["id"] = item.id;
        j["bookName"] = item.bookName;
        j["bookAuthor"] = item.bookAuthor;
        j["coverUrl"] = item.coverUrl;
        j["bookUrl"] = item.bookUrl;
        j["sourceName"] = item.sourceName;
        j["sourceUrl"] = item.sourceUrl;
        j["intro"] = item.intro;
        j["kind"] = item.kind;
        j["lastChapter"] = item.lastChapter;
        j["totalChapters"] = item.totalChapters;
        j["hasUpdate"] = item.hasUpdate;
        j["createdAt"] = item.createdAt;
        j["updatedAt"] = item.updatedAt;
        arr.push_back(std::move(j));
    }
    return strdup(arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
}

int openread_bookshelf_is_in(OpenReadEngine engine, const char* bookUrl, const char* sourceUrl) {
    if (!engine || !bookUrl) return 0;
    auto* e = static_cast<BookSourceEngine*>(engine);
    return e->isInBookshelf(bookUrl) ? 1 : 0;
}

int openread_bookshelf_progress_save(OpenReadEngine engine, const char* progressJson) {
    if (!engine || !progressJson) return -1;
    auto* e = static_cast<BookSourceEngine*>(engine);
    try {
        auto j = json::parse(progressJson);
        ReadProgress progress;
        progress.bookUrl = j.value("bookUrl", "");
        progress.chapterIndex = j.value("chapterIndex", 0);
        progress.chapterUrl = j.value("chapterUrl", "");
        progress.chapterTitle = j.value("chapterTitle", "");
        progress.pageOffset = j.value("pageOffset", 0);
        progress.readPercent = j.value("readPercent", 0.0);
        e->saveReadProgress(progress);
        return 0;
    } catch (...) {
        return -1;
    }
}

char* openread_bookshelf_progress_get(OpenReadEngine engine, const char* bookUrl, const char* sourceUrl) {
    if (!engine || !bookUrl) return strdup("{}");
    auto* e = static_cast<BookSourceEngine*>(engine);
    auto progress = e->getReadProgress(bookUrl);
    json j;
    j["bookUrl"] = progress.bookUrl;
    j["chapterIndex"] = progress.chapterIndex;
    j["chapterUrl"] = progress.chapterUrl;
    j["chapterTitle"] = progress.chapterTitle;
    j["pageOffset"] = progress.pageOffset;
    j["readPercent"] = progress.readPercent;
    j["lastReadAt"] = progress.lastReadAt;
    return strdup(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
}

int openread_bookshelf_change_source(OpenReadEngine engine,
    const char* bookUrl, const char* oldSourceUrl,
    const char* newSourceName, const char* newSourceUrl, const char* newBookUrl) {
    if (!engine || !bookUrl) return -1;
    auto* e = static_cast<BookSourceEngine*>(engine);
    try {
        e->changeBookSource(bookUrl,
            oldSourceUrl ? oldSourceUrl : "",
            newSourceName ? newSourceName : "",
            newSourceUrl ? newSourceUrl : "",
            newBookUrl ? newBookUrl : "");
        return 0;
    } catch (...) {
        return -1;
    }
}

int openread_bookshelf_check_update(OpenReadEngine engine,
    const char* bookUrl, const char* sourceUrl,
    int sourceIndex, const char* sourceName) {
    if (!engine || !bookUrl) return -1;
    auto* e = static_cast<BookSourceEngine*>(engine);
    try {
        return e->checkBookUpdate(bookUrl,
            sourceUrl ? sourceUrl : "",
            sourceIndex,
            sourceName ? sourceName : "") ? 1 : 0;
    } catch (...) {
        return -1;
    }
}

int openread_bookshelf_update_last_chapter(OpenReadEngine engine,
    const char* bookUrl, const char* sourceUrl,
    const char* lastChapter, int totalChapters, int hasUpdate) {
    if (!engine || !bookUrl) return -1;
    auto* e = static_cast<BookSourceEngine*>(engine);
    try {
        e->updateBookshelfLastChapter(
            bookUrl,
            sourceUrl ? sourceUrl : "",
            lastChapter ? lastChapter : "",
            totalChapters,
            hasUpdate != 0);
        return 0;
    } catch (...) {
        return -1;
    }
}

// ──────────────────────────────────────────────
// 全量下载缓存（同步，无回调版本）
// ──────────────────────────────────────────────
char* openread_bookshelf_download(OpenReadEngine engine,
    const char* bookUrl, const char* sourceUrl,
    int sourceIndex, const char* sourceName) {
    if (!engine || !bookUrl) return strdup("{\"error\":\"invalid params\"}");
    auto* e = static_cast<BookSourceEngine*>(engine);
    try {
        auto result = e->downloadBook(
            bookUrl, sourceUrl ? sourceUrl : "",
            sourceIndex, sourceName ? sourceName : "");
        nlohmann::json j;
        j["total"] = result.total;
        j["cached"] = result.cached;
        j["failed"] = result.failed;
        return strdup(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
    } catch (const std::exception& ex) {
        nlohmann::json j;
        j["error"] = ex.what();
        return strdup(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
    }
}

// ──────────────────────────────────────────────
// 批量更新检测（同步，无回调版本）
// ──────────────────────────────────────────────
int openread_bookshelf_check_all_updates(OpenReadEngine engine) {
    if (!engine) return -1;
    auto* e = static_cast<BookSourceEngine*>(engine);
    try {
        return e->checkAllUpdates(nullptr);
    } catch (...) {
        return -1;
    }
}

// ──────────────────────────────────────────────
// 状态查询
// ──────────────────────────────────────────────
char* openread_engine_get_source_info(OpenReadEngine engine) {
    if (!engine) return strdup("{}");

    auto* e = static_cast<BookSourceEngine*>(engine);
    std::string info = e->getSourceInfo();
    return strdup(info.c_str());
}

char* openread_engine_get_source_list(OpenReadEngine engine) {
    if (!engine) return strdup("{\"sources\":[],\"validCount\":0,\"totalCount\":0}");

    auto* e = static_cast<BookSourceEngine*>(engine);
    auto result = e->getSourceList();

    json j;
    json arr = json::array();
    for (const auto& s : result.sources) {
        json item;
        item["name"] = s.name;
        item["url"] = s.url;
        item["group"] = s.group;
        item["searchUrl"] = s.searchUrl;
        item["exploreUrl"] = s.exploreUrl;
        item["validity"] = s.validity;
        if (s.latencyMs >= 0) {
            item["latency"] = s.latencyMs;
        } else {
            item["latency"] = nullptr;
        }
        arr.push_back(std::move(item));
    }
    j["sources"] = std::move(arr);
    j["validCount"] = result.validCount;
    j["totalCount"] = result.totalCount;

    return strdup(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
}

char* openread_engine_export_good_sources(OpenReadEngine engine) {
    if (!engine) return strdup("[]");
    auto* e = static_cast<BookSourceEngine*>(engine);
    return strdup(e->exportGoodSources().c_str());
}

const char* openread_engine_get_last_error(OpenReadEngine engine) {
    if (!engine) return "Invalid engine";

    auto* e = static_cast<BookSourceEngine*>(engine);
    static thread_local std::string error;
    error = e->getLastError();
    return error.c_str();
}

int openread_engine_is_available(OpenReadEngine engine) {
    if (!engine) return 0;
    auto* e = static_cast<BookSourceEngine*>(engine);
    return e->isAvailable() ? 1 : 0;
}

// ──────────────────────────────────────────────
// 并发操作
// ──────────────────────────────────────────────
OpenReadCancelToken openread_cancel_token_create() {
    auto* token = new std::shared_ptr<CancelToken>(std::make_shared<CancelToken>());
    return static_cast<OpenReadCancelToken>(token);
}

void openread_cancel_token_cancel(OpenReadCancelToken token) {
    if (token) {
        auto* ptr = static_cast<std::shared_ptr<CancelToken>*>(token);
        (*ptr)->cancel();
    }
}

void openread_cancel_token_destroy(OpenReadCancelToken token) {
    if (token) {
        delete static_cast<std::shared_ptr<CancelToken>*>(token);
    }
}

void openread_engine_validate_concurrent(
    OpenReadEngine engine,
    const char* testQuery,
    int timeoutMs,
    int concurrency,
    OpenReadValidateCallback callback,
    OpenReadDoneCallback doneCallback,
    void* userData,
    OpenReadCancelToken cancelToken
) {
    if (!engine) return;
    auto* e = static_cast<BookSourceEngine*>(engine);

    // 包装 C 回调为 C++ lambda
    ConcurrentValidateCallback cppCallback = nullptr;
    if (callback) {
        cppCallback = [callback, userData](size_t idx, const std::string& name,
                                            SourceValidity validity, int latencyMs,
                                            const std::string& detail) {
            int v = 0;
            switch (validity) {
                case SourceValidity::Unknown:   v = 0; break;
                case SourceValidity::Invalid:   v = 3; break;
                case SourceValidity::Excellent: v = 4; break;
                case SourceValidity::Good:      v = 5; break;
                case SourceValidity::Poor:      v = 6; break;
            }
            callback(static_cast<int>(idx), name.c_str(), v, latencyMs,
                     detail.c_str(), userData);
        };
    }

    ConcurrentDoneCallback cppDone = nullptr;
    if (doneCallback) {
        cppDone = [doneCallback, userData](int c1, int c2, int c3) {
            doneCallback(c1, c2, c3, userData);
        };
    }

    std::shared_ptr<CancelToken> token = nullptr;
    if (cancelToken) {
        token = *static_cast<std::shared_ptr<CancelToken>*>(cancelToken);
    }

    e->validateSourcesConcurrent(
        testQuery ? testQuery : "\xe6\x88\x91",
        timeoutMs, concurrency, cppCallback, cppDone, token
    );
}

void openread_engine_search_all_concurrent(
    OpenReadEngine engine,
    const char* keyword,
    int concurrency,
    OpenReadSearchCallback callback,
    OpenReadDoneCallback doneCallback,
    void* userData,
    OpenReadCancelToken cancelToken,
    int matchName,
    int matchAuthor,
    int matchIntro
) {
    if (!engine || !keyword) return;
    auto* e = static_cast<BookSourceEngine*>(engine);

    ConcurrentSearchCallback cppCallback = nullptr;
    if (callback) {
        cppCallback = [callback, userData](size_t idx, const std::string& name,
                                            const std::vector<Book>& books,
                                            int latencyMs, const std::string& error) {
            // 序列化 books 为 JSON
            json arr = json::array();
            for (const auto& b : books) {
                json j;
                j["name"] = b.name;
                j["author"] = b.author;
                j["coverUrl"] = b.coverUrl;
                j["bookUrl"] = b.bookUrl;
                j["lastChapter"] = b.lastChapter;
                j["intro"] = b.intro;
                j["kind"] = b.kind;
                j["wordCount"] = b.wordCount;
                arr.push_back(j);
            }
            std::string booksStr = arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            callback(static_cast<int>(idx), name.c_str(), booksStr.c_str(),
                     latencyMs, error.c_str(), userData);
        };
    }

    ConcurrentDoneCallback cppDone = nullptr;
    if (doneCallback) {
        cppDone = [doneCallback, userData](int c1, int c2, int c3) {
            doneCallback(c1, c2, c3, userData);
        };
    }

    std::shared_ptr<CancelToken> token = nullptr;
    if (cancelToken) {
        token = *static_cast<std::shared_ptr<CancelToken>*>(cancelToken);
    }

    e->searchAllConcurrent(
        keyword,
        concurrency,
        cppCallback,
        cppDone,
        token,
        matchName != 0,
        matchAuthor != 0,
        matchIntro != 0
    );
}

void openread_engine_set_database_path(OpenReadEngine engine, const char* dbPath) {
    if (!engine) return;
    auto* e = static_cast<BookSourceEngine*>(engine);
    e->setDatabasePath(dbPath ? dbPath : "");
}

int openread_engine_load_sources_from_database(OpenReadEngine engine) {
    if (!engine) return 0;
    auto* e = static_cast<BookSourceEngine*>(engine);
    return e->loadSourcesFromDatabase();
}

int openread_engine_remove_invalid_sources(OpenReadEngine engine) {
    if (!engine) return 0;
    auto* e = static_cast<BookSourceEngine*>(engine);
    return e->removeInvalidSources();
}

void openread_engine_set_concurrency(OpenReadEngine engine, int n) {
    if (!engine) return;
    auto* e = static_cast<BookSourceEngine*>(engine);
    e->setConcurrency(n);
}

// ──────────────────────────────────────────────
// 内存管理
// ──────────────────────────────────────────────
void openread_free_string(char* str) {
    if (str) {
        free(str);
    }
}

// ──────────────────────────────────────────────
// 版本信息
// ──────────────────────────────────────────────
const char* openread_version() {
    return OPENREAD_VERSION;
}

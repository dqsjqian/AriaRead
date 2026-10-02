/// @file json_helpers.cpp
/// @brief JSON 序列化辅助函数实现。

#include "json_helpers.h"

#include "ariaread/engine.h"
#include "ariaread/engine_impl.h"
#include "ariaread/http_client.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace ariaread::web {

using ariaread::detail::sanitizeUtf8;

json book_to_json(const ariaread::Book& b) {
    return {
        {"name", sanitizeUtf8(b.name)},
        {"url", sanitizeUtf8(b.bookUrl)},
        {"author", sanitizeUtf8(b.author)},
        {"coverUrl", sanitizeUtf8(b.coverUrl)},
        {"cover_url", sanitizeUtf8(b.coverUrl)},
        {"intro", sanitizeUtf8(b.intro)},
        {"lastChapter", sanitizeUtf8(b.lastChapter)},
        {"last_chapter", sanitizeUtf8(b.lastChapter)},
        {"kind", sanitizeUtf8(b.kind)},
        {"matchScore", b.matchScore},
    };
}

json book_to_json_with_source(const ariaread::Book& b,
                              size_t sourceIndex,
                              const std::string& sourceName,
                              const std::string& sourceUrl) {
    json j = book_to_json(b);
    j["sourceName"] = sanitizeUtf8(sourceName);
    j["sourceIndex"] = static_cast<int>(sourceIndex);
    j["sourceUrl"] = sanitizeUtf8(sourceUrl);
    j["source_url"] = sanitizeUtf8(sourceUrl);
    return j;
}

json chapter_to_json(const ariaread::Chapter& c) {
    return {
        {"title", sanitizeUtf8(c.title)},
        {"url", sanitizeUtf8(c.url)},
        {"index", c.index},
        {"isVip", c.isVip},
    };
}

json source_summary_to_json(const ariaread::SourceSummary& s) {
    json j = {
        {"name", sanitizeUtf8(s.name)},
        {"url", sanitizeUtf8(s.url)},
        {"group", sanitizeUtf8(s.group)},
        {"search_url", sanitizeUtf8(s.searchUrl)},
        {"explore_url", sanitizeUtf8(s.exploreUrl)},
        {"validity", sanitizeUtf8(s.validity)},
    };
    j["latency"] = (s.latencyMs >= 0) ? json(s.latencyMs) : json(nullptr);
    return j;
}

json bookshelf_detail_to_json(const ariaread::BookshelfDetail& d) {
    const auto& item = d.item;
    const auto& progress = d.progress;
    json j = {
        {"id", item.id},
        {"bookName", sanitizeUtf8(item.bookName)},
        {"bookAuthor", sanitizeUtf8(item.bookAuthor)},
        {"coverUrl", sanitizeUtf8(item.coverUrl)},
        {"bookUrl", sanitizeUtf8(item.bookUrl)},
        {"sourceName", sanitizeUtf8(item.sourceName)},
        {"sourceUrl", sanitizeUtf8(item.sourceUrl)},
        {"intro", sanitizeUtf8(item.intro)},
        {"kind", sanitizeUtf8(item.kind)},
        {"lastChapter", sanitizeUtf8(item.lastChapter)},
        {"totalChapters", item.totalChapters},
        {"hasUpdate", item.hasUpdate},
        {"createdAt", item.createdAt},
        {"updatedAt", item.updatedAt},
        {"catalogCached", d.catalogCached},
        {"contentCached", d.contentCached},
    };
    if (progress.lastReadAt > 0) {
        j["progress"] = {
            {"chapterIndex", progress.chapterIndex},
            {"chapterTitle", sanitizeUtf8(progress.chapterTitle)},
            {"chapterUrl", sanitizeUtf8(progress.chapterUrl)},
            {"readPercent", progress.readPercent},
            {"lastReadAt", progress.lastReadAt},
        };
    } else {
        j["progress"] = nullptr;
    }
    return j;
}

std::string httpDownload(const std::string& url, int timeoutSec) {
    auto client = createDefaultHttpClient();
    if (!client) throw std::runtime_error("Failed to initialize HTTP client");
    if (timeoutSec < 0) throw std::invalid_argument("Download timeout must not be negative");
    HttpRequest request;
    request.url = url;
    request.headers["User-Agent"] = "AriaRead/1.0";
    request.timeoutMs = timeoutSec > std::numeric_limits<int>::max() / 1000
                            ? std::numeric_limits<int>::max() : timeoutSec * 1000;
    // Share redirects, TLS verification, decompression and the 32 MiB decoded
    // response limit with book/RSS requests.
    auto response = client(request);
    if (!response.error.empty())
        throw std::runtime_error("Download failed: " + response.error);
    if (response.statusCode >= 400)
        throw std::runtime_error("HTTP error " + std::to_string(response.statusCode));
    return std::move(response.body);
}

}  // namespace ariaread::web

/// @file json_helpers.cpp
/// @brief JSON 序列化辅助函数实现。

#include "json_helpers.h"

#include "ariaread/engine.h"
#include "ariaread/engine_impl.h"

#include <curl/curl.h>
#include <stdexcept>

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
    std::string result;
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Failed to initialize curl");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AriaRead/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* buf = static_cast<std::string*>(userdata);
            buf->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        throw std::runtime_error("Download failed: " + std::string(curl_easy_strerror(res)));
    if (httpCode >= 400)
        throw std::runtime_error("HTTP error " + std::to_string(httpCode));
    return result;
}

}  // namespace ariaread::web

#pragma once

#include "debug_console.h"
#include "ariaread/engine.h"
#include "ariaread/http_client.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>

namespace ariaread::web {

using DebugEventSink = std::function<bool(const std::string&, const nlohmann::json&)>;

inline void runSourceDebug(const BookSource& source, const std::string& keyword,
                           const DebugEventSink& emit, const std::function<bool()>& active,
                           HttpClientFunc client = createDefaultHttpClient(),
                           std::chrono::milliseconds budget = std::chrono::seconds(60)) {
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    struct Cancelled {};
    const auto deadline = Clock::now() + budget;
    bool connected = true;
    std::exception_ptr failure;
    auto check = [&] {
        if (failure) std::rethrow_exception(failure);
        if (!connected || !active()) throw Cancelled{};
        if (Clock::now() >= deadline) {
            throw std::runtime_error("调试超过 " + std::to_string(budget.count()) + " 毫秒限制");
        }
    };
    auto send = [&](const std::string& event, const Json& data) {
        check();
        connected = emit(event, data);
        if (!connected) throw Cancelled{};
    };
    auto elapsed = [](Clock::time_point started) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    };
    std::string stage = "debug_search";
    size_t requests = 0;
    try {
        check();
        if (!client) throw std::runtime_error("HTTP 客户端不可用");
        BookSourceEngine local;
        local.setOperationCheck(check);
        if (!local.loadSource(SourceParser::serialize(source))) {
            throw std::runtime_error("无法加载调试书源");
        }
        local.setHttpClient([&](const HttpRequest& request) {
            // JS 的 java.ajax 桥接会捕获 C++ 异常；保存首次失败，规则退出后仍须终止调试。
            try {
                check();
                if (++requests > 20) throw std::runtime_error("调试请求超过 20 次限制");
                if (request.url.rfind("http://", 0) != 0 && request.url.rfind("https://", 0) != 0) {
                    throw std::runtime_error("调试只支持 HTTP/HTTPS 请求");
                }
                auto bounded = request;
                bounded.maxResponseBytes = std::min(request.maxResponseBytes > 0 ? request.maxResponseBytes : size_t{32 * 1024 * 1024},
                                                    size_t{4 * 1024 * 1024});
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
                bounded.timeoutMs = std::max(1, std::min({request.timeoutMs > 0 ? request.timeoutMs : 10000,
                                                         10000, static_cast<int>(remaining)}));
                const auto started = Clock::now();
                auto response = client(bounded);
                send("debug_http", {{"stage", stage}, {"url", request.url}, {"method", request.method},
                                    {"status", response.statusCode}, {"elapsedMs", elapsed(started)},
                                    {"bytes", response.body.size()}, {"error", response.error}});
                return response;
            } catch (...) {
                if (!failure) failure = std::current_exception();
                throw;
            }
        });
        send("debug_info", {{"sourceName", source.name}, {"sourceUrl", source.url},
                            {"searchUrl", source.searchUrl}});
        const auto searchStarted = Clock::now();
        auto books = local.search(keyword);
        check();
        if (books.empty()) {
            throw std::runtime_error(local.getLastError().empty() ? "搜索未找到书籍，请检查关键词与搜索规则" : local.getLastError());
        }
        Json sample = Json::array();
        for (size_t i = 0; i < std::min<size_t>(books.size(), 5); ++i) {
            sample.push_back({{"name", books[i].name}, {"author", books[i].author}, {"url", books[i].bookUrl}});
        }
        send(stage, {{"count", books.size()}, {"sample", sample}, {"elapsedMs", elapsed(searchStarted)}});
        stage = "debug_catalog";
        auto book = std::find_if(books.begin(), books.end(), [](const Book& item) { return !item.bookUrl.empty(); });
        if (book == books.end()) throw std::runtime_error("搜索结果没有可用书籍链接");
        check();
        const auto catalogStarted = Clock::now();
        auto chapters = local.getCatalog(book->bookUrl);
        check();
        if (chapters.empty()) {
            throw std::runtime_error(local.getLastError().empty() ? "未解析到目录，请检查详情页与目录规则" : local.getLastError());
        }
        sample = Json::array();
        for (size_t i = 0; i < std::min<size_t>(chapters.size(), 5); ++i) {
            sample.push_back({{"index", chapters[i].index}, {"title", chapters[i].title}, {"url", chapters[i].url}});
        }
        send(stage, {{"bookName", book->name}, {"count", chapters.size()}, {"sample", sample},
                     {"elapsedMs", elapsed(catalogStarted)}});
        stage = "debug_content";
        auto chapter = std::find_if(chapters.begin(), chapters.end(), [](const Chapter& item) {
            return !item.isVolume && !item.url.empty();
        });
        if (chapter == chapters.end()) throw std::runtime_error("目录中没有可读取的正文章节");
        check();
        const auto contentStarted = Clock::now();
        auto content = local.getContent(chapter->url);
        check();
        if (content.empty()) {
            throw std::runtime_error(local.getLastError().empty() ? "正文为空，请检查正文规则" : local.getLastError());
        }
        send(stage, {{"chapterTitle", chapter->title}, {"length", content.size()},
                     {"preview", debugTextPrefix(content, 4096)}, {"truncated", content.size() > 4096},
                     {"elapsedMs", elapsed(contentStarted)}});
        send("debug_done", {{"ok", true}, {"requests", requests}});
    } catch (const Cancelled&) {
    } catch (const std::exception& error) {
        if (connected && active()) emit("debug_error", {{"stage", stage}, {"error", error.what()}});
    }
}

}  // namespace ariaread::web

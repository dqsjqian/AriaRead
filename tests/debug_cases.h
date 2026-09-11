#pragma once

#include <doctest/doctest.h>
#include "../src/apps/web_server/debug_console.h"
#include "../src/apps/web_server/source_debug.h"

namespace {

TEST_CASE("DebugConsole - 输出日志结果与隔离状态") {
    using openread::web::evaluateDebugScript;
    auto result = evaluateDebugScript({{"code", "console.log('hello'); 6 * 7"}});
    CHECK(result["ok"] == true);
    CHECK(result["result"] == "42");
    CHECK(result["logs"][0] == "hello");
    CHECK(result["elapsedMs"].get<int>() >= 0);
    evaluateDebugScript({{"code", "var privateValue = 1"}});
    CHECK(evaluateDebugScript({{"code", "typeof privateValue"}})["result"] == "undefined");
    CHECK(evaluateDebugScript({{"code", "typeof fetch + ':' + typeof require"}})["result"] == "undefined:undefined");
}

TEST_CASE("DebugConsole - 错误超时与输入限制") {
    using openread::web::evaluateDebugScript;
    auto result = evaluateDebugScript({{"code", "while (true) {}"}, {"timeoutMs", 20}});
    CHECK(result["ok"] == false);
    CHECK(result["error"].get<std::string>().find("timed out") != std::string::npos);
    CHECK(evaluateDebugScript({{"code", "throw new Error('bad')"}})["ok"] == false);
    CHECK_THROWS_AS(evaluateDebugScript(nlohmann::json::array()), std::invalid_argument);
    CHECK_THROWS_AS(evaluateDebugScript({{"code", 42}}), std::invalid_argument);
    CHECK_THROWS_AS(evaluateDebugScript({{"code", ""}}), std::invalid_argument);
    CHECK_THROWS_AS(evaluateDebugScript({{"code", std::string(65537, 'x')}}), std::length_error);
    for (const auto& timeout : {nlohmann::json(0), nlohmann::json(-1), nlohmann::json(5001),
                               nlohmann::json(1.5), nlohmann::json("10")}) {
        CHECK_THROWS_AS(evaluateDebugScript({{"code", "1"}, {"timeoutMs", timeout}}), std::invalid_argument);
    }
}

TEST_CASE("DebugConsole - 输出上限与 UTF8 边界") {
    using openread::web::evaluateDebugScript;
    auto result = evaluateDebugScript({{"code", "for (var i=0;i<120;i++) console.log(i); 'ok'"}});
    CHECK(result["logs"].size() == 100);
    CHECK(result["logsTruncated"] == true);
    result = evaluateDebugScript({{"code", "console.log('中'.repeat(10000)); '中'.repeat(30000)"}});
    CHECK(result["logs"][0].get<std::string>().size() <= 16384);
    CHECK(result["result"].get<std::string>().size() <= 65536);
    CHECK(result["resultTruncated"] == true);
    CHECK_NOTHROW(result.dump());
}

TEST_CASE("DebugConsole - 异常输出上限与 UTF8 边界") {
    using openread::web::evaluateDebugScript;
    const auto result = evaluateDebugScript({{"code", "throw '中'.repeat(30000)"}});
    CHECK(result["ok"] == false);
    CHECK(result["result"] == "");
    const auto error = result["error"].get<std::string>();
    CHECK(error.size() <= 64 * 1024);
    CHECK(error.size() > 65000);
    CHECK(result["errorTruncated"] == true);
    CHECK_NOTHROW(result.dump());
    CHECK(evaluateDebugScript({{"code", "throw new Error('short')"}})["errorTruncated"] == false);
}

openread::BookSource debugTestSource() {
    return openread::SourceParser::parse(R"({
        "bookSourceName":"诊断源", "bookSourceUrl":"https://debug.test",
        "searchUrl":"https://debug.test/search?q={{key}}",
        "ruleSearch":{"bookList":"$.books[*]","name":"$.name","bookUrl":"$.url"},
        "ruleToc":{"chapterList":"$.chapters[*]","chapterName":"$.title","chapterUrl":"$.url","isVolume":"$.volume"},
        "ruleContent":{"content":"$.text"}
    })");
}

openread::HttpResponse debugScenarioResponse(const openread::HttpRequest& request) {
    if (request.url.find("/search") != std::string::npos) {
        return {200, R"({"books":[{"name":"书籍","url":"https://debug.test/book"}]})", {}, ""};
    }
    if (request.url == "https://debug.test/book") {
        return {200, R"({"chapters":[{"title":"第一章","url":"https://debug.test/chapter","volume":false}]})", {}, ""};
    }
    return {200, R"({"text":"这是正文内容。"})", {}, ""};
}

TEST_CASE("SourceDebug - 搜索目录正文及 HTTP 事件") {
    std::vector<std::pair<std::string, nlohmann::json>> events;
    std::vector<std::string> urls;
    auto source = debugTestSource();
    openread::web::runSourceDebug(source, "test",
        [&](const std::string& event, const nlohmann::json& data) { events.emplace_back(event, data); return true; },
        [] { return true; },
        [&](const openread::HttpRequest& request) {
            urls.push_back(request.url);
            CHECK(request.timeoutMs > 0);
            CHECK(request.timeoutMs <= 10000);
            openread::HttpResponse response;
            response.statusCode = 200;
            if (request.url.find("/search") != std::string::npos) {
                response.body = R"({"books":[{"name":"书籍","url":"https://debug.test/book"}]})";
            } else if (request.url == "https://debug.test/book") {
                response.body = R"({"chapters":[{"title":"卷一","url":"https://debug.test/volume","volume":true},{"title":"第一章","url":"https://debug.test/chapter","volume":false}]})";
            } else {
                response.body = R"({"text":"这是正文内容，用于诊断测试。"})";
            }
            return response;
        });
    REQUIRE(!events.empty());
    CHECK(events.front().first == "debug_info");
    CHECK(events.back().first == "debug_done");
    CHECK(urls.size() == 3);
    CHECK(urls.back() == "https://debug.test/chapter");
    CHECK(source.validity == openread::SourceValidity::Unknown);
    std::vector<std::string> stages;
    for (const auto& [event, data] : events) {
        if (event != "debug_http") stages.push_back(event);
        if (event == "debug_content") {
            CHECK(data["preview"].get<std::string>().find("正文") != std::string::npos);
            CHECK(data["truncated"] == false);
        }
    }
    CHECK(stages == std::vector<std::string>{"debug_info", "debug_search", "debug_catalog", "debug_content", "debug_done"});
}

TEST_CASE("SourceDebug - 空搜索结果报告阶段失败") {
    std::vector<std::pair<std::string, nlohmann::json>> events;
    openread::web::runSourceDebug(debugTestSource(), "test",
        [&](const std::string& event, const nlohmann::json& data) { events.emplace_back(event, data); return true; },
        [] { return true; },
        [](const openread::HttpRequest&) { return openread::HttpResponse{200, R"({"books":[]})", {}, ""}; });
    REQUIRE(!events.empty());
    CHECK(events.back().first == "debug_error");
    CHECK(events.back().second["stage"] == "debug_search");
}

TEST_CASE("SourceDebug - 断连后不再发起请求") {
    int requests = 0;
    int events = 0;
    openread::web::runSourceDebug(debugTestSource(), "test",
        [&](const std::string&, const nlohmann::json&) { ++events; return false; },
        [] { return true; },
        [&](const openread::HttpRequest&) { ++requests; return openread::HttpResponse{}; });
    CHECK(events == 1);
    CHECK(requests == 0);
}

TEST_CASE("SourceDebug - 多项慢规则共享整体时间预算") {
    using Clock = std::chrono::steady_clock;
    auto source = debugTestSource();
    source.searchRule.name =
        "@js:var until = Date.now() + 50; while (Date.now() < until) {} JSON.parse(result).name;";
    nlohmann::json books = nlohmann::json::array();
    for (int i = 0; i < 30; ++i) {
        books.push_back({{"name", "书籍" + std::to_string(i)}, {"url", "https://debug.test/book"}});
    }
    const auto responseBody = nlohmann::json{{"books", books}}.dump();
    int requests = 0;
    std::vector<std::pair<std::string, nlohmann::json>> events;
    const auto started = Clock::now();
    openread::web::runSourceDebug(source, "test",
        [&](const std::string& event, const nlohmann::json& data) { events.emplace_back(event, data); return true; },
        [] { return true; },
        [&](const openread::HttpRequest&) {
            ++requests;
            return openread::HttpResponse{200, responseBody, {}, ""};
        }, std::chrono::milliseconds(100));
    const auto duration = Clock::now() - started;
    CHECK(duration < std::chrono::seconds(1));
    CHECK(requests == 1);
    REQUIRE(!events.empty());
    CHECK(events.back().first == "debug_error");
    CHECK(events.back().second["stage"] == "debug_search");
    CHECK(events.back().second["error"].get<std::string>().find("超过") != std::string::npos);
    for (const auto& event : events) CHECK(event.first != "debug_search");
}

TEST_CASE("SourceDebug - JS 执行中断连立即取消且不再发起 HTTP") {
    using Clock = std::chrono::steady_clock;
    auto source = debugTestSource();
    source.searchRule.name =
        "@js:java.ajax('https://debug.test/js-started');"
        "var until = Date.now() + 2000; while (Date.now() < until) {}"
        "java.ajax('https://debug.test/after-cancel'); JSON.parse(result).name;";
    bool scriptStarted = false;
    Clock::time_point cancelAt;
    std::vector<std::string> urls;
    std::vector<std::string> events;
    const auto started = Clock::now();
    openread::web::runSourceDebug(source, "test",
        [&](const std::string& event, const nlohmann::json&) { events.push_back(event); return true; },
        [&] { return !scriptStarted || Clock::now() < cancelAt; },
        [&](const openread::HttpRequest& request) {
            urls.push_back(request.url);
            if (request.url == "https://debug.test/js-started") {
                scriptStarted = true;
                cancelAt = Clock::now() + std::chrono::milliseconds(30);
            }
            return debugScenarioResponse(request);
        });
    CHECK(Clock::now() - started < std::chrono::seconds(1));
    CHECK(scriptStarted);
    REQUIRE(urls.size() == 2);
    CHECK(urls.back() == "https://debug.test/js-started");
    for (const auto& event : events) {
        CHECK(event != "debug_search");
        CHECK(event != "debug_error");
        CHECK(event != "debug_done");
    }
}

TEST_CASE("SourceDebug - JS HTTP 桥接不能吞掉请求上限错误") {
    auto source = debugTestSource();
    source.contentRule.content =
        "@js:for (var i = 0; i < 25; ++i) java.ajax('https://debug.test/probe?i=' + i); '正常正文';";
    int requests = 0;
    std::vector<std::pair<std::string, nlohmann::json>> events;
    openread::web::runSourceDebug(source, "test",
        [&](const std::string& event, const nlohmann::json& data) { events.emplace_back(event, data); return true; },
        [] { return true; },
        [&](const openread::HttpRequest& request) { ++requests; return debugScenarioResponse(request); });
    CHECK(requests == 20);
    REQUIRE(!events.empty());
    CHECK(events.back().first == "debug_error");
    CHECK(events.back().second["stage"] == "debug_content");
    CHECK(events.back().second["error"].get<std::string>().find("20") != std::string::npos);
    for (const auto& event : events) {
        CHECK(event.first != "debug_content");
        CHECK(event.first != "debug_done");
    }
}

TEST_CASE("SourceDebug - 引擎取消检查清空后可重新使用") {
    auto source = debugTestSource();
    source.searchRule.name =
        "@js:java.log('begin'); var until = Date.now() + 20;"
        "while (Date.now() < until) {} JSON.parse(result).name;";
    openread::BookSourceEngine engine;
    REQUIRE(engine.loadSource(openread::SourceParser::serialize(source)));
    engine.setHttpClient(debugScenarioResponse);
    bool cancelled = false;
    engine.setJsLogCallback([&](const std::string& message) {
        if (message == "begin") cancelled = true;
    });
    engine.setOperationCheck([&] {
        if (cancelled) throw std::runtime_error("operation cancelled");
    });
    CHECK_THROWS_AS(engine.search("test"), std::runtime_error);
    CHECK(cancelled);
    engine.setJsLogCallback({});
    engine.setOperationCheck({});
    const auto books = engine.search("test");
    REQUIRE(books.size() == 1);
    CHECK(books[0].name == "书籍");
}

TEST_CASE("SourceDebug - 指定书源读取取消后恢复原先选择") {
    auto sourceA = debugTestSource();
    sourceA.name = "源 A";
    auto sourceB = sourceA;
    sourceB.name = "源 B";
    sourceB.url = "https://second-debug.test";
    openread::BookSourceEngine engine;
    REQUIRE(engine.loadSource(openread::SourceParser::serialize(sourceA)));
    REQUIRE(engine.loadSource(openread::SourceParser::serialize(sourceB)));
    REQUIRE(engine.selectSource(0));
    int requests = 0;
    engine.setHttpClient([&](const openread::HttpRequest& request) {
        ++requests;
        return debugScenarioResponse(request);
    });
    engine.setOperationCheck([] { throw std::runtime_error("operation cancelled"); });
    SUBCASE("目录按名称切换") {
        CHECK_THROWS_AS(engine.getCatalogForSource("https://debug.test/book", -1, sourceB.name),
                        std::runtime_error);
        CHECK(engine.currentSource().name == sourceA.name);
        CHECK(requests == 0);
        engine.setOperationCheck({});
        CHECK(engine.getCatalogForSource("https://debug.test/book", -1, sourceB.name).size() == 1);
    }
    SUBCASE("正文按索引切换") {
        CHECK_THROWS_AS(engine.getContentForSource("https://debug.test/chapter", 1), std::runtime_error);
        CHECK(engine.currentSource().name == sourceA.name);
        CHECK(requests == 0);
        engine.setOperationCheck({});
        CHECK_FALSE(engine.getContentForSource("https://debug.test/chapter", 1).empty());
    }
    CHECK(engine.currentSource().name == sourceA.name);
    CHECK(engine.sources().size() == 2);
}

TEST_CASE("SourceDebug - RSS 读取取消后移除临时书源并恢复选择") {
    auto source = debugTestSource();
    openread::BookSourceEngine engine;
    REQUIRE(engine.loadSource(openread::SourceParser::serialize(source)));
    bool cancelled = true;
    bool cancelDuringHttp = false;
    SUBCASE("HTTP 前取消") {}
    SUBCASE("HTTP 返回时取消") {
        cancelled = false;
        cancelDuringHttp = true;
    }
    int requests = 0;
    engine.setHttpClient([&](const openread::HttpRequest& request) {
        ++requests;
        if (cancelDuringHttp) cancelled = true;
        return debugScenarioResponse(request);
    });
    engine.setOperationCheck([&] {
        if (cancelled) throw std::runtime_error("operation cancelled");
    });
    openread::RssSource rss;
    rss.sourceName = "临时 RSS 源";
    rss.sourceUrl = "https://rss-debug.test";
    rss.ruleContent = "$.text";
    CHECK_THROWS_AS(engine.getContentForRssSource("https://debug.test/chapter", rss), std::runtime_error);
    CHECK(requests == (cancelDuringHttp ? 1 : 0));
    REQUIRE(engine.sources().size() == 1);
    CHECK(engine.sources()[0].url == source.url);
    CHECK(engine.currentSource().url == source.url);
    engine.setOperationCheck({});
    CHECK_FALSE(engine.getContentForRssSource("https://debug.test/chapter", rss).empty());
    CHECK(engine.sources().size() == 1);
    CHECK(engine.currentSource().url == source.url);
}

TEST_CASE("SourceDebug - JS HTTP 桥接不能吞掉一次性取消检查异常") {
    auto source = debugTestSource();
    source.searchRule.name =
        "@js:java.log('before-ajax'); java.ajax('https://debug.test/probe'); JSON.parse(result).name;";
    openread::BookSourceEngine engine;
    REQUIRE(engine.loadSource(openread::SourceParser::serialize(source)));
    bool readyToCancel = false;
    bool threwOnce = false;
    int probes = 0;
    engine.setJsLogCallback([&](const std::string& message) {
        if (message == "before-ajax") readyToCancel = true;
    });
    engine.setHttpClient([&](const openread::HttpRequest& request) {
        if (request.url == "https://debug.test/probe") ++probes;
        return debugScenarioResponse(request);
    });
    engine.setOperationCheck([&] {
        if (readyToCancel && !threwOnce) {
            threwOnce = true;
            throw std::runtime_error("one-shot cancellation");
        }
    });
    CHECK_THROWS_WITH_AS(engine.search("test"), "one-shot cancellation", std::runtime_error);
    CHECK(threwOnce);
    CHECK(probes == 0);
    // The callback now succeeds, but the first cancellation remains latched until cleared.
    CHECK_THROWS_WITH_AS(engine.search("test"), "one-shot cancellation", std::runtime_error);
    engine.setOperationCheck({});
    const auto books = engine.search("test");
    REQUIRE(books.size() == 1);
    CHECK(books[0].name == "书籍");
    CHECK(probes == 1);
}

}  // namespace

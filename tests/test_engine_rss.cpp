#include <doctest/doctest.h>
#include "ariaread/engine.h"
#include <nlohmann/json.hpp>

using namespace ariaread;
using Json = nlohmann::json;

namespace {
RssSource ruleSource(std::string url = "https://rss.test") {
    RssSource s;
    s.sourceUrl = url; s.sourceName = "Fixture"; s.ruleArticles = "$.items[*]";
    s.ruleTitle = "$.title"; s.ruleLink = "$.link"; s.ruleContent = "article@html";
    return s;
}
HttpResponse ok(std::string body, std::string effective = "") {
    HttpResponse r; r.statusCode = 200; r.body = std::move(body); r.effectiveUrl = effective; return r;
}
std::string grade(BookSourceEngine& e) { e.checkRssSourcesRated(nullptr); return e.getRssSources().at(0).validity; }
}

TEST_CASE("rss: imported boolean and null extras preserve rules after database reload") {
    BookSourceEngine e; e.setDatabasePath(":memory:");
    auto imported = e.importRssSourcesFromJson(R"({"sourceUrl":"https://rss.test","sourceName":"test","sourceComment":null,"enabledCookieJar":true,"singleUrl":false,"enableJs":true,"ruleArticles":"$.items[*]","ruleTitle":"$.title","ruleContent":"article@html"})");
    REQUIRE(imported.first == 1);
    const auto s = e.getRssSources().at(0);
    CHECK(s.ruleArticles == "$.items[*]"); CHECK(s.ruleContent == "article@html"); CHECK(s.enabledCookieJar == 1);
}

TEST_CASE("rss: fast raw fragments without valid articles are never healthy") {
    BookSourceEngine e; e.setDatabasePath(":memory:"); e.upsertRssSource(ruleSource());
    e.setHttpClient([](const HttpRequest&) { return ok(R"({"items":[{}, {"title":" "}]})"); });
    const auto result = e.fetchRssSource("https://rss.test");
    CHECK(result.count == 0); CHECK_FALSE(result.error.empty()); CHECK(grade(e) == "invalid");
}

TEST_CASE("rss: list and content requests preserve POST bodies headers and URL options") {
    BookSourceEngine e; e.setDatabasePath(":memory:"); auto src = ruleSource();
    src.header = R"({"X-Source":"shared","X-Override":"source"})";
    src.sortUrl = R"(List::/list,{"method":"POST","body":"page={{page}}","headers":{"X-Override":"url"}})";
    e.upsertRssSource(src);
    int posts = 0;
    e.setHttpClient([&](const HttpRequest& req) {
        CHECK(req.headers.at("X-Source") == "shared");
        REQUIRE(req.method == "POST"); ++posts;
        if (req.url == "https://rss.test/list") {
            CHECK(req.body == "page=1"); CHECK(req.headers.at("X-Override") == "url");
            return ok(Json{{"items", Json::array({{{"title","Article"},{"link",R"(/article,{"method":"POST","body":"id=7"})"}}})}}.dump());
        }
        CHECK(req.url == "https://rss.test/article"); CHECK(req.body == "id=7");
        return ok("<article><p>Short valid article.</p></article>");
    });
    REQUIRE(e.fetchRssSource(src.sourceUrl).count == 1);
    const auto a = e.getRssArticles(src.sourceUrl,1,10).articles.at(0);
    CHECK(a.link.find("method") != std::string::npos);
    auto content = e.getRssArticleContentResult(a.id);
    CHECK(content.error.empty()); CHECK(content.content.find("Short valid article") != std::string::npos);
    CHECK(content.originalUrl == "https://rss.test/article");
    CHECK(grade(e) != "invalid"); CHECK(posts >= 4);
}

TEST_CASE("rss: content AJAX uses source headers and isolated JavaScript state") {
    BookSourceEngine e; e.setDatabasePath(":memory:");
    for (const auto* host : {"https://one.test", "https://two.test"}) {
        auto src = ruleSource(host); src.header = Json{{"X-Source",host}}.dump();
        src.ruleContent = "@js: if (typeof leaked !== 'undefined') throw Error('leaked'); var leaked = 1; java.ajax(baseUrl + '/body')";
        e.upsertRssSource(src);
    }
    e.setHttpClient([](const HttpRequest& req) {
        const auto& host = req.headers.at("X-Source");
        CHECK(req.url.rfind(host, 0) == 0);
        if (req.url == host) return ok(R"({"items":[{"title":"Article","link":"/article"}]})");
        if (req.url == host + "/article") return ok("<html></html>");
        CHECK(req.url == host + "/article/body"); return ok("<p>AJAX content</p>");
    });
    for (const auto& src : e.getRssSources()) {
        REQUIRE(e.fetchRssSource(src.sourceUrl).count == 1);
        const auto id = e.getRssArticles(src.sourceUrl,1,10).articles.at(0).id;
        for (int n=0;n<2;++n) { auto content=e.getRssArticleContentResult(id); CHECK(content.error.empty()); CHECK(content.content == "<p>AJAX content</p>"); }
    }
}

TEST_CASE("rss: feed CDATA and Atom alternate links resolve against redirected feed URL") {
    BookSourceEngine e; e.setDatabasePath(":memory:"); RssSource s; s.sourceUrl="https://rss.test/feed"; e.upsertRssSource(s);
    e.setHttpClient([](const HttpRequest&) {
        return ok(R"(<feed xmlns="http://www.w3.org/2005/Atom"><entry><title><![CDATA[Article &amp; Test]]></title><link rel="self" href="entry.xml"/><link rel="alternate" href="../article?a=1&amp;b=2"/><content><![CDATA[<p>Inline text</p>]]></content></entry></feed>)", "https://final.test/news/feed.xml");
    });
    REQUIRE(e.fetchRssSource(s.sourceUrl).count == 1);
    const auto a=e.getRssArticles(s.sourceUrl,1,10).articles.at(0);
    CHECK(a.title == "Article & Test"); CHECK(a.link == "https://final.test/article?a=1&b=2");
    CHECK(a.sourceUrl == s.sourceUrl); CHECK(a.content == "<p>Inline text</p>");
}

TEST_CASE("rss: inline feed content remains readable when original is unavailable") {
    BookSourceEngine e; e.setDatabasePath(":memory:"); RssSource s; s.sourceUrl="https://rss.test/feed"; e.upsertRssSource(s);
    e.setHttpClient([&](const HttpRequest& req) {
        CHECK(req.url == s.sourceUrl);
        return ok("<rss><channel><item><title>Short</title><link>https://blocked.test</link><description><![CDATA[<p>Hi</p>]]></description></item></channel></rss>");
    });
    REQUIRE(e.fetchRssSource(s.sourceUrl).count == 1);
    auto a=e.getRssArticles(s.sourceUrl,1,10).articles.at(0);
    CHECK(e.getRssArticleContentResult(a.id).content == "<p>Hi</p>");
    CHECK(grade(e) == "excellent");
}

TEST_CASE("rss: unreadable content is diagnosed and failed refresh keeps cached articles") {
    BookSourceEngine e; e.setDatabasePath(":memory:"); auto s=ruleSource(); e.upsertRssSource(s);
    bool fail=false;
    e.setHttpClient([&](const HttpRequest& req) {
        if (fail) { HttpResponse r; r.statusCode=403; return r; }
        if (req.url == s.sourceUrl) return ok(R"({"items":[{"title":"Article","link":"/article"}]})");
        return ok("<html><body><div id='root'></div><script>console.log('loading')</script></body></html>");
    });
    REQUIRE(e.fetchRssSource(s.sourceUrl).count == 1);
    auto id=e.getRssArticles(s.sourceUrl,1,10).articles.at(0).id;
    auto content=e.getRssArticleContentResult(id);
    CHECK(content.content.empty()); CHECK_FALSE(content.error.empty()); CHECK(grade(e) == "poor");
    fail=true;
    CHECK_FALSE(e.fetchRssSource(s.sourceUrl).error.empty());
    CHECK(e.getRssArticles(s.sourceUrl,1,10).total == 1);
    CHECK(e.getRssArticleContentResult(id).error.find("403") != std::string::npos);
}

TEST_CASE("rss: URL import accepts JSON instead of requiring feed markers") {
    BookSourceEngine e; e.setDatabasePath(":memory:");
    e.setHttpClient([](const HttpRequest&) { return ok(R"([{"sourceUrl":"https://rss.test/feed","sourceName":"Fixture"}])"); });
    auto result=e.importRssSourcesFromUrl("https://rss.test/sources.json");
    CHECK(result.first == 1); CHECK(result.second.empty());
}

TEST_CASE("rss: JavaScript channels arrays and persistent source variables") {
    BookSourceEngine e; e.setDatabasePath(":memory:"); auto s=ruleSource();
    s.sortUrl = "@js: 'List::/list?q={{source.setVariable(\"test\")}}{{source.getVariable()}}'";
    s.ruleArticles = "@js: JSON.parse(result).items";
    e.upsertRssSource(s);
    e.setHttpClient([](const HttpRequest& req) {
        CHECK(req.url == "https://rss.test/list?q=test");
        return ok(R"({"items":[{"title":"One","link":"/one"},{"title":"Two","link":"/two"}]})");
    });
    auto result=e.fetchRssSource(s.sourceUrl);
    CHECK(result.error.empty()); CHECK(result.count == 2);
}

TEST_CASE("rss: content rule failure preserves readable fallback but not a healthy rating") {
    BookSourceEngine e; e.setDatabasePath(":memory:"); auto s=ruleSource(); s.ruleContent=".obsolete@html"; e.upsertRssSource(s);
    e.setHttpClient([&](const HttpRequest& req) {
        if (req.url == s.sourceUrl) return ok(R"({"items":[{"title":"Article","link":"/article"}]})");
        return ok("<article><p>Actual text remains readable</p></article>");
    });
    REQUIRE(e.fetchRssSource(s.sourceUrl).count == 1);
    auto id=e.getRssArticles(s.sourceUrl,1,10).articles.at(0).id;
    auto content=e.getRssArticleContentResult(id);
    CHECK_FALSE(content.error.empty()); CHECK(content.content.find("Actual text") != std::string::npos);
    CHECK(grade(e) == "poor");
}

TEST_CASE("rss: checking content has the same isolated state as the reader") {
    BookSourceEngine e; e.setDatabasePath(":memory:"); auto s=ruleSource();
    s.ruleArticles = "@js: source.put('ready','yes'); JSON.parse(result).items";
    s.ruleContent = "@js: if(source.get('ready')!=='yes') throw Error('missing'); '<p>OK</p>'";
    e.upsertRssSource(s);
    e.setHttpClient([&](const HttpRequest& req) {
        if (req.url == s.sourceUrl) return ok(R"({"items":[{"title":"Article","link":"/article"}]})");
        return ok("<html><body></body></html>");
    });
    REQUIRE(e.fetchRssSource(s.sourceUrl).count == 1);
    auto id=e.getRssArticles(s.sourceUrl,1,10).articles.at(0).id;
    CHECK_FALSE(e.getRssArticleContentResult(id).error.empty()); CHECK(grade(e) == "poor");
}

TEST_CASE("rss: lazy images are normalized and empty image URLs are not readable") {
    BookSourceEngine e; e.setDatabasePath(":memory:"); RssSource s; s.sourceUrl="https://rss.test/feed.xml"; e.upsertRssSource(s);
    std::string image = "<img data-src='/photo.jpg'>";
    e.setHttpClient([&](const HttpRequest&) {
        return ok("<rss><channel><item><title>Photo</title><description><![CDATA[" + image + "]]></description></item></channel></rss>");
    });
    REQUIRE(e.fetchRssSource(s.sourceUrl).count == 1);
    auto id=e.getRssArticles(s.sourceUrl,1,10).articles.at(0).id;
    auto content=e.getRssArticleContentResult(id);
    CHECK(content.error.empty()); CHECK(content.content.find(" src=\"https://rss.test/photo.jpg\"") != std::string::npos);
    CHECK(grade(e) == "excellent");
    image = "<img src=/unquoted.jpg>";
    REQUIRE(e.fetchRssSource(s.sourceUrl).count == 1);
    content = e.getRssArticleContentResult(id);
    CHECK(content.error.empty()); CHECK(content.content.find("https://rss.test/unquoted.jpg") != std::string::npos);
    image = "<img src=''>";
    REQUIRE(e.fetchRssSource(s.sourceUrl).count == 1);
    CHECK_FALSE(e.getRssArticleContentResult(id).error.empty()); CHECK(grade(e) == "poor");
}

TEST_CASE("rss: image-only rule articles retain their visible content") {
    BookSourceEngine e; e.setDatabasePath(":memory:"); auto s=ruleSource();
    s.ruleLink=""; s.ruleContent=""; s.ruleImage="$.image"; e.upsertRssSource(s);
    e.setHttpClient([](const HttpRequest&) { return ok(R"({"items":[{"title":"Photo","image":"/photo.jpg"}]})"); });
    REQUIRE(e.fetchRssSource(s.sourceUrl).count == 1);
    auto id=e.getRssArticles(s.sourceUrl,1,10).articles.at(0).id;
    auto content=e.getRssArticleContentResult(id);
    CHECK(content.error.empty()); CHECK(content.content.find("https://rss.test/photo.jpg") != std::string::npos);
    CHECK(grade(e) == "excellent");
}

TEST_CASE("rss: public articles remain readable beside login widgets") {
    BookSourceEngine e; e.setDatabasePath(":memory:"); auto s=ruleSource(); e.upsertRssSource(s);
    bool loginOnly = false;
    e.setHttpClient([&](const HttpRequest& req) {
        if (req.url == s.sourceUrl) return ok(R"({"items":[{"title":"Article","link":"/article"}]})");
        return ok(std::string("<body><h1>Sign in</h1><p>Please sign in to continue.</p><form><input type=password><button>Login</button></form>") +
            (loginOnly ? "" : "<article><p>Public article</p></article>") + "</body>");
    });
    REQUIRE(e.fetchRssSource(s.sourceUrl).count == 1);
    auto id=e.getRssArticles(s.sourceUrl,1,10).articles.at(0).id;
    CHECK(e.getRssArticleContentResult(id).content.find("Public article") != std::string::npos);
    CHECK(grade(e) == "excellent");
    loginOnly = true;
    CHECK(e.getRssArticleContentResult(id).content.empty());
    CHECK(e.getRssArticleContentResult(id).error.find("登录") != std::string::npos);
    CHECK(grade(e) == "poor");
    s.ruleContent = ""; e.upsertRssSource(s);
    CHECK(e.getRssArticleContentResult(id).content.empty()); CHECK(grade(e) == "poor");
    s.ruleContent = "body@html"; e.upsertRssSource(s);
    CHECK(e.getRssArticleContentResult(id).content.empty()); CHECK(grade(e) == "poor");
}

TEST_CASE("rss: portable list loading fetches empty cache despite imported timestamp") {
    BookSourceEngine e; e.setDatabasePath(":memory:"); auto s=ruleSource();
    s.lastUpdateTime=123456; e.upsertRssSource(s);
    int requests=0;
    e.setHttpClient([&](const HttpRequest&) { ++requests; return ok(R"({"items":[{"title":"Article","link":"/article"}]})"); });
    auto result=e.loadRssArticles(s.sourceUrl);
    CHECK(result.total == 1); CHECK(result.error.empty()); CHECK(requests == 1);
    CHECK(e.loadRssArticles(s.sourceUrl).articles.size() == 1); CHECK(requests == 1);
    CHECK(e.loadRssArticles(s.sourceUrl,2,50).articles.empty()); CHECK(requests == 1);
    auto broken=ruleSource("https://broken.test"); e.upsertRssSource(broken);
    e.setHttpClient([](const HttpRequest&) { HttpResponse r; r.statusCode=503; return r; });
    CHECK(e.loadRssArticles(broken.sourceUrl).error.find("503") != std::string::npos);
    CHECK_FALSE(e.loadRssArticles("https://missing.test").error.empty());
}

#include <doctest/doctest.h>
#include "openread/selector.h"
#include "openread/engine_impl.h"
#include "openread/js_runtime.h"

using namespace openread;
using namespace openread::detail;

TEST_CASE("JsonPathSelector - basic") {
    std::string json = R"({
        "data": {
            "list": [
                {"title": "Book1", "author": "Author1"},
                {"title": "Book2", "author": "Author2"}
            ]
        }
    })";

    SUBCASE("extract array") {
        JsonPathSelector sel("$.data.list");
        auto results = sel.select(json);
        CHECK(results.size() == 2);
    }

    SUBCASE("extract first item") {
        JsonPathSelector sel("$.data.list[0].title");
        auto results = sel.select(json);
        REQUIRE(results.size() == 1);
        CHECK(results[0] == "Book1");
    }

    SUBCASE("extract author") {
        JsonPathSelector sel("$.data.list[1].author");
        auto results = sel.select(json);
        REQUIRE(results.size() == 1);
        CHECK(results[0] == "Author2");
    }
}

TEST_CASE("JsonPathSelector - nested") {
    std::string json = R"({
        "result": {
            "data": {
                "books": [
                    {"name": "A", "info": {"author": "X"}}
                ]
            }
        }
    })";

    JsonPathSelector sel("$.result.data.books[0].info.author");
    auto results = sel.select(json);
    REQUIRE(results.size() == 1);
    CHECK(results[0] == "X");
}

TEST_CASE("RegexSelector - basic") {
    std::string html = "<title>Hello World</title><p>Content</p>";

    SUBCASE("with capture group") {
        RegexSelector sel("<title>(.*?)</title>");
        auto results = sel.select(html);
        CHECK(results.size() == 1);
        CHECK(results[0] == "Hello World");
    }

    SUBCASE("no capture group") {
        RegexSelector sel("<title>.*?</title>");
        auto results = sel.select(html);
        CHECK(results.size() == 1);
        CHECK(results[0] == "<title>Hello World</title>");
    }
}

TEST_CASE("SelectorFactory - detectType") {
    CHECK(SelectorFactory::detectType("$.data.list") == SelectorType::JsonPath);
    CHECK(SelectorFactory::detectType("@css:.item > h2") == SelectorType::Css);
    CHECK(SelectorFactory::detectType("@xpath://div[@class]") == SelectorType::XPath);
    CHECK(SelectorFactory::detectType("@regex:pattern") == SelectorType::Regex);
    CHECK(SelectorFactory::detectType("@js:code") == SelectorType::JsEval);
}

TEST_CASE("SelectorFactory - create") {
    SUBCASE("JSONPath") {
        auto sel = SelectorFactory::create("$.data.list");
        CHECK(sel != nullptr);
        CHECK(sel->type() == SelectorType::JsonPath);
    }

    SUBCASE("Regex") {
        auto sel = SelectorFactory::create("@regex:<title>(.*?)</title>");
        CHECK(sel != nullptr);
        CHECK(sel->type() == SelectorType::Regex);
    }

    SUBCASE("empty rule") {
        auto sel = SelectorFactory::create("");
        CHECK(sel == nullptr);
    }
}

// ──────────────────────────────────────────────
// applyRuleStatic 链式 JS 测试
// ──────────────────────────────────────────────

TEST_CASE("applyRuleStatic - chain @js: selector@js:code") {
    JsRuntime js;
    std::string html = R"(<html><body>
        <div class="list">
            <a href="/book/123">Book1</a>
            <a href="/book/456">Book2</a>
        </div>
    </body></html>)";

    SUBCASE("class.list@tag.a@href@js: prepend base URL") {
        auto results = applyRuleStatic(html, "class.list@tag.a@href@js:'https://example.com'+result", &js, "");
        REQUIRE(results.size() == 2);
        CHECK(results[0] == "https://example.com/book/123");
        CHECK(results[1] == "https://example.com/book/456");
    }

    SUBCASE("tag.a@href@js: transform result") {
        auto results = applyRuleStatic(html, "tag.a@href@js:'PREFIX:'+result", &js, "");
        REQUIRE(results.size() == 2);
        CHECK(results[0] == "PREFIX:/book/123");
        CHECK(results[1] == "PREFIX:/book/456");
    }

    SUBCASE("tag.a@text@js:result.replace(...)") {
        auto results = applyRuleStatic(html, "tag.a@text@js:result.replace('Book','Novel')", &js, "");
        REQUIRE(results.size() == 2);
        CHECK(results[0] == "Novel1");
        CHECK(results[1] == "Novel2");
    }
}

TEST_CASE("applyRuleStatic - chain <js> selector<js>code</js>") {
    JsRuntime js;
    std::string html = R"(<html><body>
        <a href="/read/1">Ch1</a>
        <a href="/read/2">Ch2</a>
    </body></html>)";

    SUBCASE("tag.a@href<js>...</js> transform") {
        auto results = applyRuleStatic(html, "tag.a@href<js>'https://base.com'+result</js>", &js, "");
        REQUIRE(results.size() == 2);
        CHECK(results[0] == "https://base.com/read/1");
        CHECK(results[1] == "https://base.com/read/2");
    }
}

TEST_CASE("applyRuleStatic - pure @js: (not chain)") {
    JsRuntime js;
    std::string html = "<html><body>hello</body></html>";

    SUBCASE("@js: at start - not chain, direct JS") {
        auto results = applyRuleStatic(html, "@js:result.toUpperCase()", &js, "");
        REQUIRE(results.size() == 1);
        CHECK(results[0] == "<HTML><BODY>HELLO</BODY></HTML>");
    }
}

TEST_CASE("applyRuleStatic - no JS, pure selector") {
    JsRuntime js;
    std::string html = R"(<html><body>
        <a href="/link1">Text1</a>
        <a href="/link2">Text2</a>
    </body></html>)";

    SUBCASE("tag.a@text without JS") {
        auto results = applyRuleStatic(html, "tag.a@text", &js, "");
        REQUIRE(results.size() == 2);
        CHECK(results[0] == "Text1");
        CHECK(results[1] == "Text2");
    }

    SUBCASE("tag.a@href without JS") {
        auto results = applyRuleStatic(html, "tag.a@href", &js, "");
        REQUIRE(results.size() == 2);
        CHECK(results[0] == "/link1");
        CHECK(results[1] == "/link2");
    }
}

TEST_CASE("applyRuleStatic - JSONPath rule not affected by chain JS detection") {
    JsRuntime js;
    std::string json = R"({"data":{"url":"http://example.com/page1"}})";

    SUBCASE("$.data.url should not be treated as JSoup chain") {
        auto results = applyRuleStatic(json, "$.data.url", &js, "");
        REQUIRE(results.size() == 1);
        CHECK(results[0] == "http://example.com/page1");
    }
}

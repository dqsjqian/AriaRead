#include <doctest/doctest.h>
#include "ariaread/selector.h"
#include "ariaread/engine_impl.h"
#include "ariaread/js_runtime.h"
#include <memory>
#include <stdexcept>

using namespace ariaread;
using namespace ariaread::detail;

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

TEST_CASE("JsEvalSelector - borrows the supplied runtime and rule context") {
    JsRuntime runtime;
    runtime.eval("var prefix = 'owned';");
    JsEvalSelector selector("prefix + ':' + src + ':' + result + ':' + page");
    selector.setJsContext(runtime.rawContext());
    CHECK(selector.select("content") == std::vector<std::string>{"owned:content:content:1"});
    CHECK(runtime.getCurrentContent() == "content");
    CHECK(runtime.getLastError().empty());

    JsRuntime other;
    other.eval("var prefix = 'other';");
    selector.setJsContext(other.rawContext());
    CHECK(selector.select("next") == std::vector<std::string>{"other:next:next:1"});
    CHECK(runtime.getCurrentContent() == "content");
}

TEST_CASE("JsEvalSelector - shares rule result conversion and java bindings") {
    JsRuntime runtime;
    SUBCASE("arrays are serialized as one rule result") {
        JsEvalSelector selector("[result, 'second']");
        selector.setJsContext(runtime.rawContext());
        CHECK(selector.select("first") == std::vector<std::string>{R"(["first","second"])"});
    }
    SUBCASE("java.put result fallback") {
        JsEvalSelector selector("java.put('result', result.toUpperCase()); undefined;");
        selector.setJsContext(runtime.rawContext());
        CHECK(selector.select("input") == std::vector<std::string>{"INPUT"});
    }
    SUBCASE("undefined falls back to the input") {
        JsEvalSelector selector("undefined");
        selector.setJsContext(runtime.rawContext());
        CHECK(selector.select("input") == std::vector<std::string>{"input"});
        CHECK(selector.select("").empty());
    }
    SUBCASE("factory-created selector executes an injected function") {
        runtime.injectFunction("transform", [](const std::vector<std::string>& args) {
            return "native:" + args.at(0);
        });
        auto created = SelectorFactory::create("@js:transform(result)");
        auto* selector = dynamic_cast<JsEvalSelector*>(created.get());
        REQUIRE(selector != nullptr);
        selector->setJsContext(runtime.rawContext());
        CHECK(selector->select("input") == std::vector<std::string>{"native:input"});
    }
}

TEST_CASE("JsEvalSelector - missing or unowned contexts do not execute") {
    JsEvalSelector selector("'executed'");
    CHECK(selector.select("input").empty());
    int foreignContext = 0;
    selector.setJsContext(&foreignContext);
    CHECK(selector.select("input").empty());
    {
        JsRuntime runtime;
        selector.setJsContext(runtime.rawContext());
        CHECK(selector.select("input") == std::vector<std::string>{"executed"});
    }
    // No context dereference after the borrowed runtime has been destroyed.
    CHECK(selector.select("input").empty());
}

TEST_CASE("JsEvalSelector - exceptions and interruption use runtime error handling") {
    JsRuntime runtime;
    SUBCASE("script exception") {
        JsEvalSelector selector("throw new Error('selector failure')");
        selector.setJsContext(runtime.rawContext());
        CHECK(selector.select("input").empty());
        CHECK(runtime.getLastError().find("selector failure") != std::string::npos);
    }
    SUBCASE("syntax error") {
        JsEvalSelector selector(")");
        selector.setJsContext(runtime.rawContext());
        CHECK(selector.select("input").empty());
        CHECK_FALSE(runtime.getLastError().empty());
    }
    SUBCASE("timeout") {
        runtime.setExecutionTimeout(20);
        JsEvalSelector selector("while (true) {}");
        selector.setJsContext(runtime.rawContext());
        CHECK(selector.select("input").empty());
        CHECK(runtime.getLastError().find("timed out") != std::string::npos);
    }
    SUBCASE("cancelled before execution") {
        runtime.setInterruptCallback([] { return true; });
        JsEvalSelector selector("'ignored'");
        selector.setJsContext(runtime.rawContext());
        CHECK(selector.select("input").empty());
        CHECK(runtime.getLastError() == "JavaScript execution interrupted");
        runtime.setInterruptCallback({});
    }
    JsEvalSelector recovered("result.toUpperCase()");
    recovered.setJsContext(runtime.rawContext());
    CHECK(recovered.select("recovered") == std::vector<std::string>{"RECOVERED"});
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - injected functions convert arguments and preserve NUL") {
    JsRuntime runtime;
    std::vector<std::string> received;
    runtime.injectFunction("capture", [&](const std::vector<std::string>& args) {
        received = args;
        return std::string("x\0y", 3);
    });
    CHECK(runtime.getLastError().empty());
    CHECK(runtime.eval("typeof capture") == "function");
    CHECK(runtime.eval(R"(capture('a\u0000b', 7, true, null, undefined, {x:1}, [2,3]))")
          == std::string("x\0y", 3));
    REQUIRE(received.size() == 7);
    CHECK(received[0] == std::string("a\0b", 3));
    CHECK(received[1] == "7");
    CHECK(received[2] == "true");
    CHECK(received[3].empty());
    CHECK(received[4].empty());
    CHECK(received[5] == R"({"x":1})");
    CHECK(received[6] == "[2,3]");
    CHECK(runtime.eval("capture()") == std::string("x\0y", 3));
    CHECK(received.empty());
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - injected function exceptions and conversion failure recover") {
    JsRuntime runtime;
    runtime.injectFunction("fail", [](const std::vector<std::string>&) -> std::string {
        throw std::runtime_error("callback failure");
    });
    CHECK(runtime.eval("fail()").empty());
    CHECK(runtime.getLastError().find("callback failure") != std::string::npos);
    CHECK(runtime.eval("try { fail(); } catch (e) { e.message; }") == "callback failure");
    CHECK(runtime.getLastError().empty());
    runtime.injectFunction("fail", [](const std::vector<std::string>&) -> std::string {
        throw 7;
    });
    CHECK(runtime.eval("fail()").empty());
    CHECK(runtime.getLastError().find("unknown exception") != std::string::npos);

    int calls = 0;
    runtime.injectFunction("count", [&](const std::vector<std::string>&) {
        ++calls;
        return "ok";
    });
    CHECK(runtime.eval("var cycle = {}; cycle.self = cycle; count(cycle);").empty());
    CHECK_FALSE(runtime.getLastError().empty());
    CHECK(calls == 0);
    CHECK(runtime.eval("count('recovered')") == "ok");
    CHECK(calls == 1);
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - function replacement retains aliases and releases captures") {
    std::weak_ptr<int> oldLifetime;
    std::weak_ptr<int> newLifetime;
    {
        JsRuntime runtime;
        auto oldToken = std::make_shared<int>(1);
        oldLifetime = oldToken;
        runtime.injectFunction("version", [oldToken](const std::vector<std::string>&) {
            return std::to_string(*oldToken);
        });
        oldToken.reset();
        runtime.eval("var savedVersion = version;");
        auto newToken = std::make_shared<int>(2);
        newLifetime = newToken;
        runtime.injectFunction("version", [newToken](const std::vector<std::string>&) {
            return std::to_string(*newToken);
        });
        newToken.reset();
        CHECK(runtime.eval("version() + ':' + savedVersion()") == "2:1");
        CHECK_FALSE(oldLifetime.expired());
        runtime.eval("savedVersion = undefined;");
        CHECK(oldLifetime.expired());
        CHECK_FALSE(newLifetime.expired());
    }
    CHECK(newLifetime.expired());
}

TEST_CASE("JsRuntime - injected functions can reinject during their own call") {
    JsRuntime runtime;
    auto originalCallback = [&](const std::vector<std::string>& args) {
        runtime.injectFunction("replaceSelf", [](const std::vector<std::string>& values) {
            return "new:" + values.at(0);
        });
        // Reenter JS after replacement while this original C++ call is active.
        return "old:" + args.at(0) + ":" + runtime.eval("replaceSelf('nested')");
    };
    runtime.injectFunction("replaceSelf", originalCallback);
    // No saved JS alias keeps the original function alive during this replacement.
    CHECK(runtime.eval("replaceSelf('first')") == "old:first:new:nested");
    CHECK(runtime.eval("replaceSelf('second')") == "new:second");
    runtime.injectFunction("replaceSelf", originalCallback);
    runtime.eval("var original = replaceSelf;");
    runtime.injectFunction("replaceSelf", [](const std::vector<std::string>&) {
        return std::string("temporary replacement");
    });
    CHECK(runtime.eval("original('alias')") == "old:alias:new:nested");
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - injected mutable callbacks retain state and aliases") {
    JsRuntime runtime;
    runtime.injectFunction("next", [counter = 0](const std::vector<std::string>&) mutable {
        return std::to_string(++counter);
    });
    CHECK(runtime.eval("next()") == "1");
    CHECK(runtime.eval("next()") == "2");
    runtime.eval("var savedNext = next;");
    runtime.injectFunction("next", [counter = 100](const std::vector<std::string>&) mutable {
        return std::to_string(++counter);
    });
    CHECK(runtime.eval("next() + ':' + savedNext()") == "101:3");
    CHECK(runtime.eval("savedNext() + ':' + next()") == "4:102");
    runtime.injectFunction("nested", [&runtime, counter = 0](const std::vector<std::string>& args) mutable {
        ++counter;
        if (!args.empty()) return runtime.eval("nested()");
        return std::to_string(counter);
    });
    CHECK(runtime.eval("nested('reenter')") == "2");
    CHECK(runtime.eval("nested()") == "3");
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - invalid injection and protected globals preserve existing functions") {
    JsRuntime runtime;
    auto callback = [](const std::vector<std::string>&) { return std::string("kept"); };
    runtime.injectFunction("kept", callback);
    runtime.injectFunction("kept", {});
    CHECK_FALSE(runtime.getLastError().empty());
    CHECK(runtime.eval("kept()") == "kept");
    runtime.injectFunction("", callback);
    CHECK_FALSE(runtime.getLastError().empty());
    runtime.injectFunction(std::string("bad\0name", 8), callback);
    CHECK_FALSE(runtime.getLastError().empty());
    CHECK(runtime.eval("typeof bad") == "undefined");
    runtime.eval("Object.defineProperty(globalThis, 'locked', {value: 17, configurable: false});");
    runtime.injectFunction("locked", callback);
    CHECK_FALSE(runtime.getLastError().empty());
    CHECK(runtime.eval("locked") == "17");
    CHECK(runtime.getLastError().empty());
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

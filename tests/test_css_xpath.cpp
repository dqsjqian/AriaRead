/// @file test_css_xpath.cpp
/// @brief CSS3 / XPath 引擎 + JS 真实绑定 的单元测试
#include <doctest/doctest.h>
#include "ariaread/css_engine.h"
#include "ariaread/xpath_engine.h"
#include "ariaread/selector.h"
#include "ariaread/js_runtime.h"
#include "ariaread/engine_impl.h"
#include <gumbo.h>

using namespace ariaread;
using namespace ariaread::detail;

namespace {
std::string htmlDoc = R"(<html><body>
  <div id="content" class="main wrap">
    <ul class="chapter-list">
      <li class="item"><a href="/c/1" target="_blank">第一章</a></li>
      <li class="item vip"><a href="/c/2">第二章</a></li>
      <li class="item"><a href="/c/3">第三章</a></li>
    </ul>
    <p class="intro">简介内容</p>
    <span data-id="42">meta</span>
  </div>
</body></html>)";
}

// ──────────────────────────────────────────────
// CSS 引擎
// ──────────────────────────────────────────────
TEST_CASE("CssEngine - 基础类型/class/id 选择") {
    GumboOutput* out = gumbo_parse(htmlDoc.c_str());

    SUBCASE("tag 选择") {
        CssEngine e("li");
        CHECK(e.valid());
        CHECK(e.select(out->root).size() == 3);
    }
    SUBCASE("class 选择") {
        CssEngine e(".item");
        CHECK(e.select(out->root).size() == 3);
    }
    SUBCASE("多 class 连写") {
        CssEngine e(".item.vip");
        CHECK(e.select(out->root).size() == 1);
    }
    SUBCASE("id 选择") {
        CssEngine e("#content");
        CHECK(e.select(out->root).size() == 1);
    }
    gumbo_destroy_output(&kGumboDefaultOptions, out);
}

TEST_CASE("CssEngine - 组合器") {
    GumboOutput* out = gumbo_parse(htmlDoc.c_str());

    SUBCASE("后代选择 ul a") {
        CssEngine e("ul a");
        CHECK(e.select(out->root).size() == 3);
    }
    SUBCASE("子选择 ul > li") {
        CssEngine e("ul > li");
        CHECK(e.select(out->root).size() == 3);
    }
    SUBCASE("子选择不跨层 div > a 应为空") {
        CssEngine e("div > a");
        CHECK(e.select(out->root).empty());
    }
    SUBCASE("后代多级 #content .item a") {
        CssEngine e("#content .item a");
        CHECK(e.select(out->root).size() == 3);
    }
    gumbo_destroy_output(&kGumboDefaultOptions, out);
}

TEST_CASE("CssEngine - 属性选择器") {
    GumboOutput* out = gumbo_parse(htmlDoc.c_str());

    SUBCASE("[attr] 存在") {
        CssEngine e("a[target]");
        CHECK(e.select(out->root).size() == 1);
    }
    SUBCASE("[attr=v]") {
        CssEngine e("a[target=_blank]");
        CHECK(e.select(out->root).size() == 1);
    }
    SUBCASE("[attr^=v] 前缀") {
        CssEngine e("a[href^='/c/']");
        CHECK(e.select(out->root).size() == 3);
    }
    SUBCASE("[attr$=v] 后缀") {
        CssEngine e("a[href$='2']");
        CHECK(e.select(out->root).size() == 1);
    }
    SUBCASE("[attr*=v] 包含") {
        CssEngine e("span[data-id*='4']");
        CHECK(e.select(out->root).size() == 1);
    }
    gumbo_destroy_output(&kGumboDefaultOptions, out);
}

TEST_CASE("CssEngine - 伪类") {
    GumboOutput* out = gumbo_parse(htmlDoc.c_str());

    SUBCASE(":first-child") {
        CssEngine e("li:first-child");
        CHECK(e.select(out->root).size() == 1);
    }
    SUBCASE(":last-child") {
        CssEngine e("li:last-child");
        CHECK(e.select(out->root).size() == 1);
    }
    SUBCASE(":nth-child(2)") {
        CssEngine e("li:nth-child(2)");
        CHECK(e.select(out->root).size() == 1);
    }
    SUBCASE(":not(.vip)") {
        CssEngine e("li:not(.vip)");
        CHECK(e.select(out->root).size() == 2);
    }
    gumbo_destroy_output(&kGumboDefaultOptions, out);
}

TEST_CASE("CssSelector - @CSS: 规则取值") {
    SUBCASE("取 text") {
        CssSelector sel(".chapter-list a@text");
        auto r = sel.select(htmlDoc);
        REQUIRE(r.size() == 3);
        CHECK(r[0] == "第一章");
        CHECK(r[2] == "第三章");
    }
    SUBCASE("取 href 属性") {
        CssSelector sel("li a@href");
        auto r = sel.select(htmlDoc);
        REQUIRE(r.size() == 3);
        CHECK(r[0] == "/c/1");
    }
}

// ──────────────────────────────────────────────
// XPath 引擎
// ──────────────────────────────────────────────
TEST_CASE("XPathEngine - 路径与属性") {
    SUBCASE("//a/@href") {
        XPathSelector sel("//a/@href");
        auto r = sel.select(htmlDoc);
        REQUIRE(r.size() == 3);
        CHECK(r[0] == "/c/1");
    }
    SUBCASE("//a/text()") {
        XPathSelector sel("//a/text()");
        auto r = sel.select(htmlDoc);
        REQUIRE(r.size() == 3);
        CHECK(r[1] == "第二章");
    }
    SUBCASE("//li[@class] 带属性谓词") {
        XPathSelector sel("//li[@class]/a/@href");
        auto r = sel.select(htmlDoc);
        CHECK(r.size() == 3);
    }
    SUBCASE("//li[2]/a/@href 索引谓词") {
        XPathSelector sel("//li[2]/a/@href");
        auto r = sel.select(htmlDoc);
        REQUIRE(r.size() == 1);
        CHECK(r[0] == "/c/2");
    }
    SUBCASE("contains(@class,'vip')") {
        XPathSelector sel("//li[contains(@class,'vip')]/a/@href");
        auto r = sel.select(htmlDoc);
        REQUIRE(r.size() == 1);
        CHECK(r[0] == "/c/2");
    }
}

TEST_CASE("SelectorFactory - 新引擎可创建并工作") {
    CHECK(SelectorFactory::detectType("@css:.item") == SelectorType::Css);
    CHECK(SelectorFactory::detectType("//div") == SelectorType::XPath);

    auto css = SelectorFactory::create("@css:li a@href");
    REQUIRE(css != nullptr);
    auto r = css->select(htmlDoc);
    CHECK(r.size() == 3);
}

// ──────────────────────────────────────────────
// JS 真实绑定：@put/@get 变量表 + java
// ──────────────────────────────────────────────
TEST_CASE("JsRuntime - @put/@get 变量表") {
    JsRuntime js;
    js.putVariable("foo", "bar");
    CHECK(js.getVariable("foo") == "bar");

    SUBCASE("java.put / java.get 经由 JS 写读变量表") {
        std::string r = js.evalRuleJs("java.put('k','v123'); java.get('k')", "", "");
        CHECK(r == "v123");
        CHECK(js.getVariable("k") == "v123");
    }
}

TEST_CASE("JsRuntime - java.ajax 经 HttpFunc 真实回调") {
    JsRuntime js;
    bool called = false;
    js.setHttpFunc([&](const std::string& url, const std::string&,
                       const std::string&, const std::string&) -> std::string {
        called = true;
        return "RESP:" + url;
    });
    std::string r = js.evalRuleJs("java.ajax('http://x.com/api')", "", "");
    CHECK(called);
    CHECK(r == "RESP:http://x.com/api");
}

TEST_CASE("applyRuleStatic - @put 写入 + @get 读取") {
    JsRuntime js;
    std::string html = R"(<html><body><span id="t">HELLO</span></body></html>)";
    // @put 先把 id=t 的文本存进变量 g，再用 @get 取出拼接
    auto r = applyRuleStatic(html, "@put:{\"g\":\"id.t@text\"}@get:{g}", &js, "");
    REQUIRE(r.size() == 1);
    CHECK(r[0] == "HELLO");
}

// ──────────────────────────────────────────────
// DefaultJSoup → CSS 引擎兜底（带连字符 class / 属性选择器）
// ──────────────────────────────────────────────
TEST_CASE("DefaultJSoup - CSS 兜底处理复杂选择器") {
    std::string html = R"(<html><body>
      <div class="book-list">
        <a class="book-item" href="/b/1">书1</a>
        <a class="book-item" href="/b/2">书2</a>
      </div>
    </body></html>)";

    SUBCASE("属性前缀选择 a[href^=/b/]@href 经 CSS 兜底") {
        JsRuntime js;
        auto r = applyRuleStatic(html, "@css:a[href^='/b/']@href", &js, "");
        REQUIRE(r.size() == 2);
        CHECK(r[0] == "/b/1");
    }
    SUBCASE("带连字符 class 选择 class.book-item@text") {
        JsRuntime js;
        auto r = applyRuleStatic(html, "class.book-item@text", &js, "");
        REQUIRE(r.size() == 2);
        CHECK(r[0] == "书1");
        CHECK(r[1] == "书2");
    }
}

// ──────────────────────────────────────────────
// && 合并语义（对齐 legado）——修复「正文取到也点不开/空白」核心根因。
// && 必须是「各子规则独立作用于同一 content 再拼接」，而非「前一个输出喂后一个」（管道）。
// ──────────────────────────────────────────────
TEST_CASE("applyRuleStatic - && 为合并而非管道") {
    JsRuntime js;
    std::string html = R"(<html><body>
      <div class="title">标题文字</div>
      <div class="content">正文内容</div>
    </body></html>)";

    // 管道语义会先取 .title 文本，再在该片段里找 .content（必然找不到）→ 空。
    // 合并语义应分别取出 .title 和 .content 再拼接 → 2 个结果。
    auto r = applyRuleStatic(html, "class.title@text&&class.content@text", &js, "");
    REQUIRE(r.size() == 2);
    CHECK(r[0] == "标题文字");
    CHECK(r[1] == "正文内容");
}

TEST_CASE("applyRuleStatic - 模板内嵌 {{选择器&&选择器}} 多结果 join") {
    JsRuntime js;
    std::string html = R"(<html><body>
      <div class="title">甲</div>
      <div class="content">乙</div>
    </body></html>)";
    // {{@@A&&B}} 应把 A、B 两个结果用 \n 连接为单串，而非只取第一个。
    auto r = applyRuleStatic(html, "前缀-{{@@class.title@text&&class.content@text}}", &js, "");
    REQUIRE(r.size() == 1);
    CHECK(r[0] == "前缀-甲\n乙");
}

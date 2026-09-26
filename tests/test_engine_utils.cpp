/// @file test_engine_utils.cpp
/// @brief 工具函数单元测试（normalizeText、bookMatchScore、cleanContent 等）

#include <doctest/doctest.h>
#include "ariaread/engine_impl.h"

using namespace ariaread;
using namespace ariaread::detail;

// ──────────────────────────────────────────────
// normalizeText 文本归一化
// ──────────────────────────────────────────────

TEST_CASE("normalizeText - 基本归一化") {
    SUBCASE("英文小写化") {
        CHECK(normalizeText("Hello World") == "helloworld");
    }

    SUBCASE("去除空白和分隔符") {
        CHECK(normalizeText("hello - world") == "helloworld");
        CHECK(normalizeText("hello_world") == "helloworld");
        CHECK(normalizeText("hello.world") == "helloworld");
        CHECK(normalizeText("hello|world") == "helloworld");
        CHECK(normalizeText("hello[world]") == "helloworld");
        CHECK(normalizeText("hello(world)") == "helloworld");
    }

    SUBCASE("去除 HTML 标签") {
        CHECK(normalizeText("<b>hello</b>") == "hello");
        CHECK(normalizeText("<div class='x'>text</div>") == "text");
    }

    SUBCASE("中文保留") {
        CHECK(normalizeText("红楼梦") == "红楼梦");
        CHECK(normalizeText("  红楼梦  ") == "红楼梦");
    }

    SUBCASE("空字符串") {
        CHECK(normalizeText("") == "");
        CHECK(normalizeText("   ") == "");
    }
}

// ──────────────────────────────────────────────
// bookMatchScore 搜索匹配评分
// ──────────────────────────────────────────────

TEST_CASE("bookMatchScore - 只勾选书名") {
    Book book;
    book.name = "红楼梦";
    book.author = "曹雪芹";
    book.intro = "四大名著之一";

    SUBCASE("书名完全匹配 → 1000") {
        int score = bookMatchScore(book, "红楼梦", true, false, false);
        CHECK(score == 1000);
    }

    SUBCASE("书名前缀匹配 → 850") {
        book.name = "红楼梦续集";
        int score = bookMatchScore(book, "红楼梦", true, false, false);
        CHECK(score == 850);
    }

    SUBCASE("书名包含匹配 → 700") {
        book.name = "新红楼梦传";
        int score = bookMatchScore(book, "红楼梦", true, false, false);
        CHECK(score == 700);
    }

    SUBCASE("书名不匹配 → 0（被过滤）") {
        int score = bookMatchScore(book, "西游记", true, false, false);
        CHECK(score == 0);
    }

    SUBCASE("无关书籍被过滤") {
        book.name = "春花满画楼";
        int score = bookMatchScore(book, "红楼梦", true, false, false);
        CHECK(score == 0);
    }
}

TEST_CASE("bookMatchScore - 只勾选作者") {
    Book book;
    book.name = "斗破苍穹";
    book.author = "天蚕土豆";

    SUBCASE("作者完全匹配 → 520") {
        int score = bookMatchScore(book, "天蚕土豆", false, true, false);
        CHECK(score == 520);
    }

    SUBCASE("作者包含匹配 → 420") {
        int score = bookMatchScore(book, "天蚕", false, true, false);
        CHECK(score == 420);
    }

    SUBCASE("作者不匹配 → 0") {
        int score = bookMatchScore(book, "耳根", false, true, false);
        CHECK(score == 0);
    }

    SUBCASE("书名匹配但只勾选作者 → 0（不看书名）") {
        int score = bookMatchScore(book, "斗破苍穹", false, true, false);
        CHECK(score == 0);
    }
}

TEST_CASE("bookMatchScore - 同时勾选书名+作者（或关系）") {
    Book book;
    book.name = "天蚕变";
    book.author = "某作者";

    SUBCASE("书名匹配，作者不匹配 → 取书名分") {
        int score = bookMatchScore(book, "天蚕", true, true, false);
        CHECK(score > 0);  // 书名包含"天蚕"
    }

    SUBCASE("作者匹配，书名不匹配 → 取作者分") {
        book.name = "斗破苍穹";
        book.author = "天蚕土豆";
        int score = bookMatchScore(book, "天蚕土豆", true, true, false);
        CHECK(score >= 520);  // 作者完全匹配
    }

    SUBCASE("都不匹配 → 0") {
        int score = bookMatchScore(book, "耳根", true, true, false);
        CHECK(score == 0);
    }
}

TEST_CASE("bookMatchScore - 勾选简介") {
    Book book;
    book.name = "某书";
    book.author = "某人";
    book.intro = "这是一本关于修仙的小说";

    SUBCASE("简介包含关键词 → 220") {
        int score = bookMatchScore(book, "修仙", false, false, true);
        CHECK(score == 220);
    }

    SUBCASE("简介不包含 → 0") {
        int score = bookMatchScore(book, "都市", false, false, true);
        CHECK(score == 0);
    }
}

TEST_CASE("bookMatchScore - 全部不勾选 → 不过滤") {
    Book book;
    book.name = "红楼梦";
    int score = bookMatchScore(book, "红楼梦", false, false, false);
    CHECK(score == 0);  // 全部 false 时不做任何匹配，返回 0
}

TEST_CASE("bookMatchScore - 空关键词") {
    Book book;
    book.name = "红楼梦";
    int score = bookMatchScore(book, "", true, false, false);
    CHECK(score == 0);
}

// ──────────────────────────────────────────────
// isBlank 空白判断
// ──────────────────────────────────────────────

TEST_CASE("isBlank") {
    CHECK(isBlank("") == true);
    CHECK(isBlank("   ") == true);
    CHECK(isBlank("\t\n") == true);
    CHECK(isBlank("hello") == false);
    CHECK(isBlank(" a ") == false);
}

// ──────────────────────────────────────────────
// decodeHtmlEntities HTML 实体解码
// ──────────────────────────────────────────────

TEST_CASE("decodeHtmlEntities - 命名实体") {
    CHECK(decodeHtmlEntities("&amp;") == "&");
    CHECK(decodeHtmlEntities("&lt;") == "<");
    CHECK(decodeHtmlEntities("&gt;") == ">");
    CHECK(decodeHtmlEntities("&quot;") == "\"");
    CHECK(decodeHtmlEntities("&nbsp;") == " ");
    CHECK(decodeHtmlEntities("&apos;") == "'");
}

TEST_CASE("decodeHtmlEntities - 数字实体") {
    CHECK(decodeHtmlEntities("&#65;") == "A");
    CHECK(decodeHtmlEntities("&#x41;") == "A");
    CHECK(decodeHtmlEntities("&#x4e2d;") == "中");  // 中文"中"
}

TEST_CASE("decodeHtmlEntities - 混合文本") {
    CHECK(decodeHtmlEntities("hello &amp; world") == "hello & world");
    CHECK(decodeHtmlEntities("a&lt;b&gt;c") == "a<b>c");
}

TEST_CASE("decodeHtmlEntities - 无实体") {
    CHECK(decodeHtmlEntities("hello world") == "hello world");
    CHECK(decodeHtmlEntities("") == "");
}

// ──────────────────────────────────────────────
// stripCdata — CDATA 剥离（修复「评优质却点不开」根因）
// ──────────────────────────────────────────────
TEST_CASE("stripCdata - 基本剥离") {
    // 36氪等 CMS 的真实 link 形态
    CHECK(stripCdata("<![CDATA[https://36kr.com/p/123?f=rss]]>")
          == "https://36kr.com/p/123?f=rss");
    CHECK(stripCdata("<![CDATA[标题文字]]>") == "标题文字");
}

TEST_CASE("stripCdata - 无 CDATA 原样返回") {
    CHECK(stripCdata("https://example.com/a") == "https://example.com/a");
    CHECK(stripCdata("") == "");
    CHECK(stripCdata("纯文本标题") == "纯文本标题");
}

TEST_CASE("stripCdata - 前后混合文本与多段") {
    CHECK(stripCdata("前缀<![CDATA[中间]]>后缀") == "前缀中间后缀");
    CHECK(stripCdata("<![CDATA[A]]> <![CDATA[B]]>") == "A B");
}

TEST_CASE("stripCdata - 未闭合容错") {
    // 没有 ]]> 时，保留 open 之后的剩余原文，不丢内容
    CHECK(stripCdata("<![CDATA[https://x.com/a") == "https://x.com/a");
}

TEST_CASE("decodeHtmlEntities - 自动剥离 CDATA 后再解码") {
    // 关键回归：link 被 CDATA 包裹后能还原为可用 http 链接
    std::string r = decodeHtmlEntities("<![CDATA[https://36kr.com/p/123?a=1&amp;b=2]]>");
    CHECK(r == "https://36kr.com/p/123?a=1&b=2");
    // 验证前端 /^https?:\/\// 判定可通过
    CHECK(r.rfind("https://", 0) == 0);
}

// ──────────────────────────────────────────────
// cleanContent 正文清理
// ──────────────────────────────────────────────

TEST_CASE("cleanContent - 基本清理") {
    SUBCASE("去除 HTML 标签") {
        CHECK(cleanContent("<p>hello</p>") == "hello");
    }

    SUBCASE("br 转换行") {
        std::string result = cleanContent("line1<br>line2");
        CHECK(result.find('\n') != std::string::npos);
    }

    SUBCASE("解码实体") {
        CHECK(cleanContent("hello&amp;world") == "hello&world");
    }

    SUBCASE("合并连续空格") {
        CHECK(cleanContent("hello   world") == "hello world");
    }

    SUBCASE("去除首尾空白") {
        CHECK(cleanContent("\n\n  hello  \n\n") == "hello");
    }

    SUBCASE("空字符串") {
        CHECK(cleanContent("") == "");
    }
}

TEST_CASE("cleanContent - 复杂 HTML") {
    std::string html = "<div class='content'><p>第一段</p><p>第二段</p></div>";
    std::string result = cleanContent(html);
    CHECK(result.find("第一段") != std::string::npos);
    CHECK(result.find("第二段") != std::string::npos);
    CHECK(result.find("<") == std::string::npos);  // 无残留标签
}

// ──────────────────────────────────────────────
// isValidUtf8 / sanitizeUtf8
// ──────────────────────────────────────────────

TEST_CASE("isValidUtf8") {
    CHECK(isValidUtf8("hello") == true);
    CHECK(isValidUtf8("你好世界") == true);
    CHECK(isValidUtf8("") == true);

    // 非法 UTF-8 字节
    std::string bad;
    bad.push_back(static_cast<char>(0xFF));
    CHECK(isValidUtf8(bad) == false);
}

TEST_CASE("sanitizeUtf8") {
    SUBCASE("合法 UTF-8 原样返回") {
        CHECK(sanitizeUtf8("hello") == "hello");
        CHECK(sanitizeUtf8("你好") == "你好");
    }

    SUBCASE("非法字节被清除") {
        std::string bad = "hello";
        bad.push_back(static_cast<char>(0xFF));
        bad += "world";
        std::string result = sanitizeUtf8(bad);
        CHECK(result.find("hello") != std::string::npos);
        CHECK(result.find("world") != std::string::npos);
        CHECK(isValidUtf8(result) == true);
    }
}

// ──────────────────────────────────────────────
// trimCopy 去除首尾空白
// ──────────────────────────────────────────────

TEST_CASE("trimCopy") {
    CHECK(trimCopy("  hello  ") == "hello");
    CHECK(trimCopy("hello") == "hello");
    CHECK(trimCopy("") == "");
    CHECK(trimCopy("   ") == "");
    CHECK(trimCopy("\thello\n") == "hello");
}

// ──────────────────────────────────────────────
// AnalyzeUrl::getAbsoluteURL —— URL 绝对化（对齐 legado URL(base, rel)）
// 修复「正文取到也点不开」次因之一：link 拼接畸形（host 重复 / 协议相对 // 未处理）
// ──────────────────────────────────────────────
#include "ariaread/analyze_url.h"

TEST_CASE("getAbsoluteURL - 协议相对 // 继承 scheme，不重复 host") {
    // 修复前：base=https://m.zol.com.cn + rel=//m.zol.com.cn/a.html
    //         → https://m.zol.com.cn//m.zol.com.cn/a.html（host 重复，抓错页）
    CHECK(AnalyzeUrl::getAbsoluteURL("https://m.zol.com.cn", "//m.zol.com.cn/a.html")
          == "https://m.zol.com.cn/a.html");
    CHECK(AnalyzeUrl::getAbsoluteURL("http://x.com/p/1", "//cdn.x.com/img.jpg")
          == "http://cdn.x.com/img.jpg");
}

TEST_CASE("getAbsoluteURL - 绝对路径 /") {
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com/b/c.html", "/d/e.html")
          == "https://a.com/d/e.html");
}

TEST_CASE("getAbsoluteURL - 书源作者标记和查询参数不属于主机或路径") {
    CHECK(AnalyzeUrl::getAbsoluteURL("https://www.shubl.com#乃星", "/index/get_search_book_list/我")
          == "https://www.shubl.com/index/get_search_book_list/我");
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com?from=/wrong/dir#作者", "search")
          == "https://a.com/search");
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com/books/page?next=/wrong/dir#作者", "../chapter")
          == "https://a.com/chapter");
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com#作者/目录", "search")
          == "https://a.com/search");
    AnalyzeUrl analyzer("/search?q={{key}}", "https://a.com#作者", "我");
    CHECK(analyzer.result().url == "https://a.com/search?q=%E6%88%91");
}

TEST_CASE("getAbsoluteURL - 查询与锚点单独解析且不规范化参数内容") {
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com/books/page?old=1#old", "?page=2")
          == "https://a.com/books/page?page=2");
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com/books/page?old=1#old", "#new")
          == "https://a.com/books/page?old=1#new");
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com/books/page", "../search?next=/a/../b#c/../d")
          == "https://a.com/search?next=/a/../b#c/../d");
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com#作者", "/search?next=/a/../b")
          == "https://a.com/search?next=/a/../b");
}

TEST_CASE("getAbsoluteURL - 相对路径与 ../") {
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com/b/c/page.html", "img.jpg")
          == "https://a.com/b/c/img.jpg");
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com/b/c/page.html", "../x.html")
          == "https://a.com/b/x.html");
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com/b/c/page.html", "../../y.html")
          == "https://a.com/y.html");
}

TEST_CASE("getAbsoluteURL - 已是绝对/特殊 URL 原样") {
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com", "https://b.com/x")
          == "https://b.com/x");
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com", "data:image/png;base64,AAA")
          == "data:image/png;base64,AAA");
    CHECK(AnalyzeUrl::getAbsoluteURL("https://a.com", "javascript:void(0)") == "");
}

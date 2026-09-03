#include <doctest/doctest.h>
#include "openread/source_parser.h"

using namespace openread;

TEST_CASE("SourceParser - parse single source") {
    std::string json = R"({
        "bookSourceName": "测试书源",
        "bookSourceUrl": "https://example.com",
        "bookSourceGroup": "测试",
        "searchUrl": "https://example.com/search?q={{key}}",
        "ruleSearch": {
            "bookList": "$.data.list",
            "name": "$.title",
            "author": "$.author"
        },
        "ruleToc": {
            "chapterList": "$.chapters",
            "chapterName": "$.title",
            "chapterUrl": "$.url"
        },
        "ruleContent": {
            "content": "$.content"
        }
    })";

    auto source = SourceParser::parse(json);

    CHECK(source.name == "测试书源");
    CHECK(source.url == "https://example.com");
    CHECK(source.group == "测试");
    CHECK(source.searchUrl == "https://example.com/search?q={{key}}");
    CHECK(source.searchRule.bookList == "$.data.list");
    CHECK(source.searchRule.name == "$.title");
    CHECK(source.searchRule.author == "$.author");
    CHECK(source.catalogRule.chapterList == "$.chapters");
    CHECK(source.contentRule.content == "$.content");
}

TEST_CASE("SourceParser - validate") {
    SUBCASE("valid source with searchUrl") {
        BookSource source;
        source.name = "Test";
        source.url = "https://example.com";
        source.searchUrl = "https://example.com/search?q={{key}}";
        CHECK(SourceParser::validate(source));
    }

    SUBCASE("valid source with exploreUrl only") {
        BookSource source;
        source.name = "Test";
        source.url = "https://example.com";
        source.exploreUrl = "https://example.com/explore";
        CHECK(SourceParser::validate(source));
    }

    SUBCASE("valid source with legado JS expression") {
        BookSource source;
        source.name = "文学吧";
        source.url = "https://www.wenxue88.com";
        source.searchUrl = R"(https://www.wenxue88.com#{{java.put("key",key)}})";
        CHECK(SourceParser::validate(source));
    }

    SUBCASE("missing name") {
        BookSource source;
        source.url = "https://example.com";
        source.searchUrl = "https://example.com/search?q={{key}}";
        CHECK_FALSE(SourceParser::validate(source));
    }

    SUBCASE("missing url") {
        BookSource source;
        source.name = "Test";
        source.searchUrl = "https://example.com/search?q={{key}}";
        CHECK_FALSE(SourceParser::validate(source));
    }

    SUBCASE("reject placeholder searchUrl '-'") {
        BookSource source;
        source.name = "Test";
        source.url = "https://example.com";
        source.searchUrl = "-";
        CHECK_FALSE(SourceParser::validate(source));
    }

    SUBCASE("reject empty searchUrl + empty exploreUrl") {
        BookSource source;
        source.name = "Test";
        source.url = "https://example.com";
        CHECK_FALSE(SourceParser::validate(source));
    }

    SUBCASE("reject static searchUrl without placeholder") {
        BookSource source;
        source.name = "Test";
        source.url = "https://example.com";
        source.searchUrl = "files/writer/3671.html";  // 静态死链
        CHECK_FALSE(SourceParser::validate(source));
    }
}

TEST_CASE("SourceParser - parse array") {
    std::string json = R"([
        {
            "bookSourceName": "源1",
            "bookSourceUrl": "https://one.com",
            "searchUrl": "https://one.com/search?q={{key}}"
        },
        {
            "bookSourceName": "源2",
            "bookSourceUrl": "https://two.com",
            "searchUrl": "https://two.com/search?q={{key}}"
        }
    ])";

    auto sources = SourceParser::parseArray(json);
    CHECK(sources.size() == 2);
    CHECK(sources[0].name == "源1");
    CHECK(sources[1].name == "源2");
}

TEST_CASE("SourceParser - serialize") {
    BookSource source;
    source.name = "测试";
    source.url = "https://example.com";
    source.searchUrl = "https://example.com/search?q={{key}}";
    source.searchRule.bookList = "$.list";
    source.searchRule.name = "$.title";

    std::string json = SourceParser::serialize(source);
    CHECK(json.find("测试") != std::string::npos);
    CHECK(json.find("example.com") != std::string::npos);
}

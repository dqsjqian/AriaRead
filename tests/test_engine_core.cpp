/// @file test_engine_core.cpp
/// @brief 引擎核心逻辑单元测试（书源管理、有效性过滤、列表过滤、导出等）

#include <doctest/doctest.h>
#include "ariaread/engine.h"
#include "ariaread/types.h"
#include "ariaread/source_parser.h"
#include "startup_runtime_cases.h"
#include "debug_cases.h"

#include <nlohmann/json.hpp>

using namespace ariaread;
using json = nlohmann::json;

// ──────────────────────────────────────────────
// 辅助：构建测试书源 JSON
// ──────────────────────────────────────────────
static std::string makeSourceJson(const std::string& name, const std::string& url,
                                   const std::string& searchUrl = "https://example.com/search?q={{key}}") {
    json j;
    j["bookSourceName"] = name;
    j["bookSourceUrl"] = url;
    j["searchUrl"] = searchUrl;
    j["ruleSearch"] = {{"bookList", "$.list"}, {"name", "$.title"}, {"author", "$.author"}};
    j["ruleToc"] = {{"chapterList", "$.chapters"}, {"chapterName", "$.title"}, {"chapterUrl", "$.url"}};
    j["ruleContent"] = {{"content", "$.content"}};
    return j.dump();
}

// ──────────────────────────────────────────────
// SourceValidity 枚举工具函数
// ──────────────────────────────────────────────

TEST_CASE("validityToString - 枚举转字符串") {
    CHECK(validityToString(SourceValidity::Unknown) == "unknown");
    CHECK(validityToString(SourceValidity::Excellent) == "excellent");
    CHECK(validityToString(SourceValidity::Good) == "good");
    CHECK(validityToString(SourceValidity::Poor) == "poor");
    CHECK(validityToString(SourceValidity::Invalid) == "invalid");
}

TEST_CASE("isValidValidity - 有效性判断") {
    CHECK(isValidValidity(SourceValidity::Excellent) == true);
    CHECK(isValidValidity(SourceValidity::Good) == true);
    CHECK(isValidValidity(SourceValidity::Poor) == false);    // 差不算有效
    CHECK(isValidValidity(SourceValidity::Invalid) == false);
    CHECK(isValidValidity(SourceValidity::Unknown) == false);
}

// ──────────────────────────────────────────────
// 书源加载与管理
// ──────────────────────────────────────────────

TEST_CASE("BookSourceEngine - 加载书源") {
    BookSourceEngine engine;

    SUBCASE("加载单个书源") {
        bool ok = engine.loadSource(makeSourceJson("测试源", "https://test.com"));
        CHECK(ok == true);
        CHECK(engine.sources().size() == 1);
        CHECK(engine.sources()[0].name == "测试源");
    }

    SUBCASE("加载书源数组") {
        json arr = json::array();
        arr.push_back(json::parse(makeSourceJson("源1", "https://one.com")));
        arr.push_back(json::parse(makeSourceJson("源2", "https://two.com")));
        int count = engine.loadSources(arr.dump());
        CHECK(count == 2);
        CHECK(engine.sources().size() == 2);
    }

    SUBCASE("加载无效 JSON") {
        bool ok = engine.loadSource("not json");
        CHECK(ok == false);
    }
}

// ──────────────────────────────────────────────
// 书源有效性设置
// ──────────────────────────────────────────────

TEST_CASE("BookSourceEngine - 设置书源有效性") {
    BookSourceEngine engine;
    engine.loadSource(makeSourceJson("源A", "https://a.com"));
    engine.loadSource(makeSourceJson("源B", "https://b.com"));
    engine.loadSource(makeSourceJson("源C", "https://c.com"));

    engine.setSourceValidity(0, SourceValidity::Excellent, 100);
    engine.setSourceValidity(1, SourceValidity::Poor, 3000);
    engine.setSourceValidity(2, SourceValidity::Invalid, 5000);

    CHECK(engine.sources()[0].validity == SourceValidity::Excellent);
    CHECK(engine.sources()[0].latencyMs == 100);
    CHECK(engine.sources()[1].validity == SourceValidity::Poor);
    CHECK(engine.sources()[2].validity == SourceValidity::Invalid);
}

// ──────────────────────────────────────────────
// validSourceCount 只统计优+良
// ──────────────────────────────────────────────

TEST_CASE("BookSourceEngine - validSourceCount 只统计优和良") {
    BookSourceEngine engine;
    engine.loadSource(makeSourceJson("优", "https://excellent.com"));
    engine.loadSource(makeSourceJson("良", "https://good.com"));
    engine.loadSource(makeSourceJson("差", "https://poor.com"));
    engine.loadSource(makeSourceJson("无效", "https://invalid.com"));
    engine.loadSource(makeSourceJson("待检测", "https://unknown.com"));

    engine.setSourceValidity(0, SourceValidity::Excellent);
    engine.setSourceValidity(1, SourceValidity::Good);
    engine.setSourceValidity(2, SourceValidity::Poor);
    engine.setSourceValidity(3, SourceValidity::Invalid);
    // 第4个保持 Unknown

    CHECK(engine.validSourceCount() == 2);  // 只有优+良
}

// ──────────────────────────────────────────────
// getSourceList 过滤逻辑（核心测试）
// ──────────────────────────────────────────────

TEST_CASE("BookSourceEngine - getSourceList 只返回 Unknown/Excellent/Good") {
    BookSourceEngine engine;
    engine.loadSource(makeSourceJson("优源", "https://excellent.com"));
    engine.loadSource(makeSourceJson("良源", "https://good.com"));
    engine.loadSource(makeSourceJson("差源", "https://poor.com"));
    engine.loadSource(makeSourceJson("无效源", "https://invalid.com"));
    engine.loadSource(makeSourceJson("待检测源", "https://unknown.com"));

    engine.setSourceValidity(0, SourceValidity::Excellent, 50);
    engine.setSourceValidity(1, SourceValidity::Good, 200);
    engine.setSourceValidity(2, SourceValidity::Poor, 3000);
    engine.setSourceValidity(3, SourceValidity::Invalid, 5000);
    // 第4个保持 Unknown

    auto result = engine.getSourceList();

    SUBCASE("列表只包含 3 个书源（优、良、待检测）") {
        CHECK(result.sources.size() == 3);
    }

    SUBCASE("差和无效不在列表中") {
        for (const auto& s : result.sources) {
            CHECK(s.validity != "poor");
            CHECK(s.validity != "invalid");
        }
    }

    SUBCASE("列表中包含优、良、待检测") {
        bool hasExcellent = false, hasGood = false, hasUnknown = false;
        for (const auto& s : result.sources) {
            if (s.validity == "excellent") hasExcellent = true;
            if (s.validity == "good") hasGood = true;
            if (s.validity == "unknown") hasUnknown = true;
        }
        CHECK(hasExcellent);
        CHECK(hasGood);
        CHECK(hasUnknown);
    }

    SUBCASE("stats 统计包含所有状态") {
        CHECK(result.stats.excellent == 1);
        CHECK(result.stats.good == 1);
        CHECK(result.stats.poor == 1);
        CHECK(result.stats.invalid == 1);
        CHECK(result.stats.unknown == 1);
    }

    SUBCASE("totalCount 包含所有有 searchUrl 的书源") {
        CHECK(result.totalCount == 5);
    }

    SUBCASE("validCount 只统计优+良") {
        CHECK(result.validCount == 2);
    }
}

TEST_CASE("BookSourceEngine - getSourceList 无 searchUrl 的书源不计入") {
    BookSourceEngine engine;
    // 有 searchUrl 的
    engine.loadSource(makeSourceJson("有搜索", "https://a.com", "https://a.com/search?q={{key}}"));
    // 无 searchUrl 的
    engine.loadSource(makeSourceJson("无搜索", "https://b.com", ""));

    auto result = engine.getSourceList();
    CHECK(result.totalCount == 1);  // 只统计有 searchUrl 的
    CHECK(result.sources.size() == 1);
}

// ──────────────────────────────────────────────
// removeInvalidSources 删除差+无效
// ──────────────────────────────────────────────

TEST_CASE("BookSourceEngine - removeInvalidSources 删除差和无效") {
    BookSourceEngine engine;
    engine.loadSource(makeSourceJson("优", "https://a.com"));
    engine.loadSource(makeSourceJson("差", "https://b.com"));
    engine.loadSource(makeSourceJson("无效", "https://c.com"));
    engine.loadSource(makeSourceJson("良", "https://d.com"));

    engine.setSourceValidity(0, SourceValidity::Excellent);
    engine.setSourceValidity(1, SourceValidity::Poor);
    engine.setSourceValidity(2, SourceValidity::Invalid);
    engine.setSourceValidity(3, SourceValidity::Good);

    int removed = engine.removeInvalidSources();
    CHECK(removed == 2);  // 差+无效
    CHECK(engine.sources().size() == 2);

    // 剩余的应该是优和良
    bool hasExcellent = false, hasGood = false;
    for (const auto& s : engine.sources()) {
        if (s.validity == SourceValidity::Excellent) hasExcellent = true;
        if (s.validity == SourceValidity::Good) hasGood = true;
    }
    CHECK(hasExcellent);
    CHECK(hasGood);
}

// ──────────────────────────────────────────────
// exportGoodSources 只导出优+良
// ──────────────────────────────────────────────

TEST_CASE("BookSourceEngine - exportGoodSources 只导出优和良") {
    BookSourceEngine engine;
    engine.loadSource(makeSourceJson("优源", "https://excellent.com"));
    engine.loadSource(makeSourceJson("良源", "https://good.com"));
    engine.loadSource(makeSourceJson("差源", "https://poor.com"));
    engine.loadSource(makeSourceJson("无效源", "https://invalid.com"));
    engine.loadSource(makeSourceJson("待检测源", "https://unknown.com"));

    engine.setSourceValidity(0, SourceValidity::Excellent);
    engine.setSourceValidity(1, SourceValidity::Good);
    engine.setSourceValidity(2, SourceValidity::Poor);
    engine.setSourceValidity(3, SourceValidity::Invalid);

    std::string exported = engine.exportGoodSources();
    auto arr = json::parse(exported);

    CHECK(arr.is_array());
    CHECK(arr.size() == 2);

    // 验证只包含优和良
    bool hasExcellent = false, hasGood = false;
    for (const auto& item : arr) {
        std::string name = item.value("bookSourceName", "");
        if (name == "优源") hasExcellent = true;
        if (name == "良源") hasGood = true;
        // 不应该包含差、无效、待检测
        CHECK(name != "差源");
        CHECK(name != "无效源");
        CHECK(name != "待检测源");
    }
    CHECK(hasExcellent);
    CHECK(hasGood);
}

// ──────────────────────────────────────────────
// clearAllSources 清空所有书源
// ──────────────────────────────────────────────

TEST_CASE("BookSourceEngine - clearAllSources") {
    BookSourceEngine engine;
    engine.loadSource(makeSourceJson("源1", "https://a.com"));
    engine.loadSource(makeSourceJson("源2", "https://b.com"));
    CHECK(engine.sources().size() == 2);

    int removed = engine.clearAllSources();
    CHECK(removed == 2);
    CHECK(engine.sources().empty());
}

// ──────────────────────────────────────────────
// selectSource 选择书源
// ──────────────────────────────────────────────

TEST_CASE("BookSourceEngine - selectSource") {
    BookSourceEngine engine;
    engine.loadSource(makeSourceJson("源A", "https://a.com"));
    engine.loadSource(makeSourceJson("源B", "https://b.com"));

    SUBCASE("按索引选择") {
        CHECK(engine.selectSource(1) == true);
        CHECK(engine.currentSource().name == "源B");
    }

    SUBCASE("按名称选择") {
        CHECK(engine.selectSourceByName("源A") == true);
        CHECK(engine.currentSource().name == "源A");
    }

    SUBCASE("无效索引") {
        CHECK(engine.selectSource(99) == false);
    }

    SUBCASE("无效名称") {
        CHECK(engine.selectSourceByName("不存在") == false);
    }
}

// ──────────────────────────────────────────────
// 版本号
// ──────────────────────────────────────────────

TEST_CASE("BookSourceEngine - version 不为空") {
    std::string ver = BookSourceEngine::version();
    CHECK(!ver.empty());
}

/// @file test_engine_integration.cpp
/// @brief 引擎集成测试（模拟用户行为的端到端场景）

#include <doctest/doctest.h>
#include "ariaread/engine.h"
#include "ariaread/types.h"
#include "ariaread/source_parser.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <system_error>
#include <atomic>
#include <thread>
#include <chrono>

using namespace ariaread;
using json = nlohmann::json;

// ──────────────────────────────────────────────
// 辅助
// ──────────────────────────────────────────────
// On Windows the SQLite handle is released by sqlite3_close_v2 only after the
// last prepared statement is finalized, and WAL mode keeps the -wal/-shm side
// files mapped while the connection lives. Removing the temp DB can therefore
// fail with a sharing violation, which cannot happen on POSIX (unlink of an
// open file always succeeds there). Cleanup is best-effort: a leftover temp
// file must not fail a test.
static void removeTempDb(const std::string& dbPath) {
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath + "-wal", ec);
    std::filesystem::remove(dbPath + "-shm", ec);
}

static std::string makeTempDbPath(const std::string& suffix = "") {
    auto path = std::filesystem::temp_directory_path() / ("ariaread_integ" + suffix + ".db");
    removeTempDb(path.string());
    return path.string();
}

static std::string makeSourceJson(const std::string& name, const std::string& url) {
    json j;
    j["bookSourceName"] = name;
    j["bookSourceUrl"] = url;
    j["searchUrl"] = url + "/search?q={{key}}";
    j["ruleSearch"] = {{"bookList", "$.list"}, {"name", "$.title"}, {"author", "$.author"}};
    j["ruleToc"] = {{"chapterList", "$.chapters"}, {"chapterName", "$.title"}, {"chapterUrl", "$.url"}};
    j["ruleContent"] = {{"content", "$.content"}};
    return j.dump();
}

// ──────────────────────────────────────────────
// 场景1：用户加载书源 → 检测 → 过滤 → 导出
// ──────────────────────────────────────────────

TEST_CASE("集成测试 - 书源加载、检测、过滤、导出完整流程") {
    auto dbPath = makeTempDbPath("_flow1");
    BookSourceEngine engine;
    engine.setDatabasePath(dbPath);

    // 步骤1：加载 5 个书源
    engine.loadSource(makeSourceJson("优质源", "https://excellent.com"));
    engine.loadSource(makeSourceJson("良好源", "https://good.com"));
    engine.loadSource(makeSourceJson("较差源", "https://poor.com"));
    engine.loadSource(makeSourceJson("无效源", "https://invalid.com"));
    engine.loadSource(makeSourceJson("待检测源", "https://unknown.com"));
    CHECK(engine.sources().size() == 5);

    // 步骤2：模拟检测结果（实际检测需要网络，这里直接设置）
    engine.setSourceValidity(0, SourceValidity::Excellent, 50);
    engine.setSourceValidity(1, SourceValidity::Good, 200);
    engine.setSourceValidity(2, SourceValidity::Poor, 3000);
    engine.setSourceValidity(3, SourceValidity::Invalid, 5000);
    // 第4个保持 Unknown

    // 步骤3：获取书源列表（模拟前端刷新）
    auto list = engine.getSourceList();

    // 验证：列表只包含优、良、待检测
    CHECK(list.sources.size() == 3);
    for (const auto& s : list.sources) {
        CHECK(s.validity != "poor");
        CHECK(s.validity != "invalid");
    }

    // 验证：stats 包含所有状态
    CHECK(list.stats.excellent == 1);
    CHECK(list.stats.good == 1);
    CHECK(list.stats.poor == 1);
    CHECK(list.stats.invalid == 1);
    CHECK(list.stats.unknown == 1);

    // 步骤4：导出优+良书源
    std::string exported = engine.exportGoodSources();
    auto arr = json::parse(exported);
    CHECK(arr.size() == 2);

    // 步骤5：删除无效书源
    int removed = engine.removeInvalidSources();
    CHECK(removed == 2);  // 差+无效
    CHECK(engine.sources().size() == 3);  // 优+良+待检测

    removeTempDb(dbPath);
}

// ──────────────────────────────────────────────
// 场景2：用户加载书源 → 持久化 → 重启 → 恢复
// ──────────────────────────────────────────────

TEST_CASE("集成测试 - 书源持久化和重启恢复") {
    auto dbPath = makeTempDbPath("_flow2");

    // 第一次启动：加载书源并设置有效性
    {
        BookSourceEngine engine;
        engine.setDatabasePath(dbPath);

        engine.loadSource(makeSourceJson("优质源", "https://excellent.com"));
        engine.loadSource(makeSourceJson("良好源", "https://good.com"));
        engine.syncSourcesToDatabase();

        engine.setSourceValidity(0, SourceValidity::Excellent, 50);
        engine.setSourceValidity(1, SourceValidity::Good, 200);
    }

    // 第二次启动：从数据库恢复
    {
        BookSourceEngine engine;
        engine.setDatabasePath(dbPath);
        int loaded = engine.loadSourcesFromDatabase();
        CHECK(loaded == 2);

        // 验证有效性被恢复
        bool hasExcellent = false, hasGood = false;
        for (const auto& s : engine.sources()) {
            if (s.url == "https://excellent.com") {
                CHECK(s.validity == SourceValidity::Excellent);
                CHECK(s.latencyMs == 50);
                hasExcellent = true;
            }
            if (s.url == "https://good.com") {
                CHECK(s.validity == SourceValidity::Good);
                CHECK(s.latencyMs == 200);
                hasGood = true;
            }
        }
        CHECK(hasExcellent);
        CHECK(hasGood);

        // 验证列表过滤正常
        auto list = engine.getSourceList();
        CHECK(list.sources.size() == 2);
        CHECK(list.validCount == 2);
    }

    removeTempDb(dbPath);
}

// ──────────────────────────────────────────────
// 场景3：用户添加书到书架 → 缓存目录 → 缓存正文 → 读取
// ──────────────────────────────────────────────

TEST_CASE("集成测试 - 书架和缓存完整流程") {
    auto dbPath = makeTempDbPath("_flow3");
    BookSourceEngine engine;
    engine.setDatabasePath(dbPath);

    // 步骤1：添加书到书架
    BookshelfItem item;
    item.bookName = "仙逆";
    item.bookAuthor = "耳根";
    item.bookUrl = "book://xianni";
    item.sourceName = "源A";
    item.sourceUrl = "source://alpha";
    int64_t id = engine.addToBookshelf(item);
    CHECK(id > 0);

    // 步骤2：验证书架
    auto shelf = engine.getBookshelf();
    CHECK(shelf.size() == 1);
    CHECK(shelf[0].bookName == "仙逆");

    // 步骤3：缓存目录
    std::vector<Chapter> chapters;
    for (int i = 0; i < 100; ++i) {
        chapters.push_back({"第" + std::to_string(i + 1) + "章", "ch://" + std::to_string(i), i, false});
    }
    engine.cacheBookCatalog("book://xianni", "source://alpha", chapters);

    // 步骤4：验证目录缓存
    auto cached = engine.getCachedCatalog("book://xianni", "source://alpha");
    CHECK(cached.size() == 100);
    CHECK(cached[0].title == "第1章");
    CHECK(cached[99].title == "第100章");

    // 步骤5：缓存正文
    engine.cacheChapterContent("ch://0", "book://xianni", "source://alpha", "第一章正文内容");
    engine.cacheChapterContent("ch://1", "book://xianni", "source://alpha", "第二章正文内容");

    // 步骤6：读取缓存的正文
    std::string content = engine.getCachedContent("book://xianni", 0);
    CHECK(content == "第一章正文内容");

    // 步骤7：验证缓存数量
    CHECK(engine.getCachedCatalogCount("book://xianni") == 100);
    CHECK(engine.getCachedContentCount("book://xianni") == 2);

    // 步骤8：获取书架详情（含缓存状态）
    auto details = engine.getBookshelfWithDetails();
    CHECK(details.size() == 1);
    CHECK(details[0].item.bookName == "仙逆");
    CHECK(details[0].catalogCached == 100);
    CHECK(details[0].contentCached == 2);

    // 步骤9：保存阅读进度
    ReadProgress progress;
    progress.bookUrl = "book://xianni";
    progress.chapterIndex = 42;
    progress.chapterTitle = "第43章";
    progress.readPercent = 0.5;
    engine.saveReadProgress(progress);

    // 步骤10：读取阅读进度
    auto loaded = engine.getReadProgress("book://xianni");
    CHECK(loaded.chapterIndex == 42);
    CHECK(loaded.readPercent == doctest::Approx(0.5));

    // 步骤11：从书架移除（级联删除）
    engine.removeFromBookshelf("book://xianni");
    CHECK(engine.getBookshelf().empty());
    CHECK(engine.getCachedCatalogCount("book://xianni") == 0);
    CHECK(engine.getCachedContentCount("book://xianni") == 0);

    removeTempDb(dbPath);
}

// ──────────────────────────────────────────────
// 场景4：检测过程中实时过滤（模拟前端行为）
// ──────────────────────────────────────────────

TEST_CASE("集成测试 - 检测过程中 getSourceList 实时过滤") {
    BookSourceEngine engine;

    // 加载 3 个书源，初始都是 Unknown
    engine.loadSource(makeSourceJson("源1", "https://s1.com"));
    engine.loadSource(makeSourceJson("源2", "https://s2.com"));
    engine.loadSource(makeSourceJson("源3", "https://s3.com"));

    // 初始状态：全部在列表中
    auto list1 = engine.getSourceList();
    CHECK(list1.sources.size() == 3);

    // 模拟检测：源1 变为优
    engine.setSourceValidity(0, SourceValidity::Excellent, 50);
    auto list2 = engine.getSourceList();
    CHECK(list2.sources.size() == 3);  // 优+Unknown+Unknown

    // 模拟检测：源2 变为无效
    engine.setSourceValidity(1, SourceValidity::Invalid, 5000);
    auto list3 = engine.getSourceList();
    CHECK(list3.sources.size() == 2);  // 优+Unknown（无效被过滤）

    // 模拟检测：源3 变为差
    engine.setSourceValidity(2, SourceValidity::Poor, 4000);
    auto list4 = engine.getSourceList();
    CHECK(list4.sources.size() == 1);  // 只剩优（差也被过滤）

    // 验证 stats 仍然包含所有状态
    CHECK(list4.stats.excellent == 1);
    CHECK(list4.stats.invalid == 1);
    CHECK(list4.stats.poor == 1);
}

// ──────────────────────────────────────────────
// 场景5：检测后重新变为优/良，应重新出现在列表
// ──────────────────────────────────────────────

TEST_CASE("集成测试 - 书源重新检测后恢复到列表") {
    BookSourceEngine engine;
    engine.loadSource(makeSourceJson("源1", "https://s1.com"));

    // 初始：Unknown，在列表中
    CHECK(engine.getSourceList().sources.size() == 1);

    // 变为无效，从列表消失
    engine.setSourceValidity(0, SourceValidity::Invalid);
    CHECK(engine.getSourceList().sources.size() == 0);

    // 重新检测变为良，重新出现
    engine.setSourceValidity(0, SourceValidity::Good, 150);
    auto list = engine.getSourceList();
    CHECK(list.sources.size() == 1);
    CHECK(list.sources[0].validity == "good");
}

// ──────────────────────────────────────────────
// 场景6：多本书缓存隔离
// ──────────────────────────────────────────────

TEST_CASE("集成测试 - 多本书缓存完全隔离") {
    auto dbPath = makeTempDbPath("_isolation");
    BookSourceEngine engine;
    engine.setDatabasePath(dbPath);

    // 添加两本书
    BookshelfItem item1;
    item1.bookName = "仙逆";
    item1.bookUrl = "book://xianni";
    item1.sourceName = "源A";
    item1.sourceUrl = "source://a";
    engine.addToBookshelf(item1);

    BookshelfItem item2;
    item2.bookName = "凡人修仙传";
    item2.bookUrl = "book://fanren";
    item2.sourceName = "源B";
    item2.sourceUrl = "source://b";
    engine.addToBookshelf(item2);

    // 分别缓存目录
    std::vector<Chapter> ch1 = {{"仙逆第1章", "ch://x1", 0, false}};
    std::vector<Chapter> ch2 = {{"凡人第1章", "ch://f1", 0, false}, {"凡人第2章", "ch://f2", 1, false}};
    engine.cacheBookCatalog("book://xianni", "source://a", ch1);
    engine.cacheBookCatalog("book://fanren", "source://b", ch2);

    // 分别缓存正文
    engine.cacheChapterContent("ch://x1", "book://xianni", "source://a", "仙逆正文");
    engine.cacheChapterContent("ch://f1", "book://fanren", "source://b", "凡人正文");

    // 验证隔离
    CHECK(engine.getCachedCatalogCount("book://xianni") == 1);
    CHECK(engine.getCachedCatalogCount("book://fanren") == 2);
    CHECK(engine.getCachedContentCount("book://xianni") == 1);
    CHECK(engine.getCachedContentCount("book://fanren") == 1);

    // 删除仙逆不影响凡人
    engine.removeFromBookshelf("book://xianni");
    CHECK(engine.getCachedCatalogCount("book://xianni") == 0);
    CHECK(engine.getCachedCatalogCount("book://fanren") == 2);  // 不受影响
    CHECK(engine.getCachedContentCount("book://fanren") == 1);  // 不受影响

    removeTempDb(dbPath);
}

// ──────────────────────────────────────────────
// 场景7：downloadBook 接口签名验证（无 sleepMs）
// ──────────────────────────────────────────────

TEST_CASE("集成测试 - downloadBook 无 sleepMs 参数") {
    auto dbPath = makeTempDbPath("_download");
    BookSourceEngine engine;
    engine.setDatabasePath(dbPath);

    // 不设置 HTTP 回调，downloadBook 应该安全返回（目录为空）
    auto result = engine.downloadBook("book://test", "source://test", -1, "源A");
    CHECK(result.total == 0);
    CHECK(result.cached == 0);
    CHECK(result.failed == 0);

    // 验证可以传 concurrency 参数
    auto result2 = engine.downloadBook("book://test", "source://test", -1, "源A", nullptr, 4);
    CHECK(result2.total == 0);

    removeTempDb(dbPath);
}

// ──────────────────────────────────────────────
// 场景8：openBookSession 状态管理
// ──────────────────────────────────────────────

TEST_CASE("集成测试 - openBookSession 状态快照") {
    auto dbPath = makeTempDbPath("_session");
    BookSourceEngine engine;
    engine.setDatabasePath(dbPath);

    // 添加书到书架
    BookshelfItem item;
    item.bookName = "测试书";
    item.bookUrl = "book://session-test";
    item.sourceName = "源";
    item.sourceUrl = "source://test";
    engine.addToBookshelf(item);

    // 缓存一些目录
    std::vector<Chapter> chapters = {
        {"第1章", "ch://1", 0, false},
        {"第2章", "ch://2", 1, false},
    };
    engine.cacheBookCatalog("book://session-test", "source://test", chapters);

    // 打开书本会话
    auto state = engine.openBookSession("book://session-test", "source://test", -1, "源");
    CHECK(state.bookUrl == "book://session-test");
    CHECK(state.catalogCached == 2);

    // 获取状态快照
    auto state2 = engine.getBookReadingState("book://session-test", "source://test");
    CHECK(state2.bookUrl == "book://session-test");

    removeTempDb(dbPath);
}

/// @file test_database.cpp
/// @brief 数据库层单元测试（书源持久化、书架 CRUD、目录缓存、正文缓存、阅读进度）

#include <doctest/doctest.h>
#include "ariaread/database.h"
#include "ariaread/types.h"

#include <filesystem>
#include <string>
#include <system_error>

using namespace ariaread;

// ──────────────────────────────────────────────
// 辅助：创建临时数据库
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
    auto path = std::filesystem::temp_directory_path() / ("ariaread_db_test" + suffix + ".db");
    removeTempDb(path.string());
    return path.string();
}

static BookSource makeTestSource(const std::string& name, const std::string& url,
                                  SourceValidity validity = SourceValidity::Unknown) {
    BookSource s;
    s.name = name;
    s.url = url;
    s.searchUrl = "https://" + name + "/search?q={{key}}";
    s.validity = validity;
    return s;
}

// ──────────────────────────────────────────────
// 书源 CRUD
// ──────────────────────────────────────────────

TEST_CASE("SourceDatabase - 书源插入和查询") {
    auto dbPath = makeTempDbPath("_source_crud");
    SourceDatabase db(dbPath);

    std::vector<BookSource> sources = {
        makeTestSource("源A", "https://a.com"),
        makeTestSource("源B", "https://b.com"),
    };

    SUBCASE("批量插入") {
        int inserted = db.insertSources(sources);
        CHECK(inserted == 2);
        CHECK(db.getSourceCount() == 2);
    }

    SUBCASE("重复插入不增加记录数") {
        CHECK(db.insertSources(sources) == 2);
        CHECK(db.insertSources(sources) == 0);  // INSERT OR IGNORE，不会新增记录
        CHECK(db.getSourceCount() == 2);  // 仍然只有 2 条
    }

    SUBCASE("混合重复和新书源返回实际插入数") {
        CHECK(db.insertSources({sources.front()}) == 1);
        CHECK(db.insertSources(sources) == 1);
        CHECK(db.getSourceCount() == 2);
    }

    SUBCASE("sourceExists") {
        db.insertSources(sources);
        CHECK(db.sourceExists("https://a.com") == true);
        CHECK(db.sourceExists("https://not-exist.com") == false);
    }

    SUBCASE("removeSource") {
        db.insertSources(sources);
        CHECK(db.removeSource("https://a.com") == true);
        CHECK(db.getSourceCount() == 1);
        CHECK(db.sourceExists("https://a.com") == false);
    }

    removeTempDb(dbPath);
}

TEST_CASE("SourceDatabase - RSS wide migration preserves data and indexes") {
    const auto path = makeTempDbPath("_rss_wide");
    { SourceDatabase initial(path); }
    {
        sqlite::database raw(path);
        raw << "ALTER TABLE rss_sources ADD COLUMN legacy_extra TEXT;";
        raw << "INSERT INTO rss_sources(source_url, source_name, source_group, validity, latency_ms, source_json) "
               "VALUES ('https://example.invalid/feed', 'Retained', 'group', 'good', 37, '{}');";
        raw << "INSERT INTO rss_articles(source_url, title, link) "
               "VALUES ('https://example.invalid/feed', 'Retained article', 'https://example.invalid/article');";
    }
    // Reopening twice also verifies that migration is idempotent.
    for (int attempt = 0; attempt < 2; ++attempt) {
        { SourceDatabase migrated(path); }
        sqlite::database raw(path);
        int columns = 0;
        raw << "SELECT count(*) FROM pragma_table_info('rss_sources');" >> columns;
        CHECK(columns == 14);
        int records = 0;
        raw << "SELECT count(*) FROM rss_sources WHERE source_name='Retained' AND validity='good' AND latency_ms=37;" >> records;
        CHECK(records == 1);
        int indexes = 0;
        raw << "SELECT count(*) FROM sqlite_master WHERE type='index' AND tbl_name='rss_sources' "
               "AND name IN ('idx_rss_group', 'idx_rss_enabled', 'idx_rss_validity');" >> indexes;
        CHECK(indexes == 3);
        raw << "SELECT count(*) FROM rss_articles WHERE title='Retained article';" >> records;
        CHECK(records == 1);
    }
    removeTempDb(path);
}

TEST_CASE("SourceDatabase - failed RSS migration restores original schema and rows") {
    const auto path = makeTempDbPath("_rss_rollback");
    { SourceDatabase initial(path); }
    {
        sqlite::database raw(path);
        raw << "ALTER TABLE rss_sources ADD COLUMN legacy_extra TEXT;";
        raw << "INSERT INTO rss_sources(source_url, legacy_extra) VALUES ('https://example.invalid/feed', 'keep');";
        // Force a late failure, after the wide source table was replaced.
        raw << "DROP TABLE rss_articles;";
        raw << "CREATE TABLE rss_articles(id INTEGER);";
    }
    CHECK_THROWS(static_cast<void>(SourceDatabase{path}));
    {
        sqlite::database raw(path);
        int records = 0;
        raw << "SELECT count(*) FROM rss_sources WHERE legacy_extra='keep';" >> records;
        CHECK(records == 1);
        raw << "SELECT count(*) FROM sqlite_master WHERE name='rss_sources_old';" >> records;
        CHECK(records == 0);
        raw << "SELECT count(*) FROM sqlite_master WHERE name='idx_rss_group' AND tbl_name='rss_sources';" >> records;
        CHECK(records == 1);
        // Repair the deliberately broken fixture; the next open can migrate.
        raw << "DROP TABLE rss_articles;";
    }
    CHECK_NOTHROW(static_cast<void>(SourceDatabase{path}));
    removeTempDb(path);
}

TEST_CASE("SourceDatabase - partial RSS schema repairs missing latency independently") {
    const auto path = makeTempDbPath("_rss_partial");
    { SourceDatabase initial(path); }
    {
        sqlite::database raw(path);
        raw << "ALTER TABLE rss_sources DROP COLUMN latency_ms;";
        raw << "INSERT INTO rss_sources(source_url, validity, source_json) "
               "VALUES ('https://example.invalid/valid', 'good', '{\"__latencyMs\":23}'), "
               "('https://example.invalid/malformed', 'poor', 'not JSON');";
    }
    { SourceDatabase migrated(path); }
    {
        sqlite::database raw(path);
        int records = 0;
        raw << "SELECT count(*) FROM rss_sources WHERE validity='good' AND latency_ms=23;" >> records;
        CHECK(records == 1);
        raw << "SELECT count(*) FROM rss_sources WHERE validity='poor' AND latency_ms=-1;" >> records;
        CHECK(records == 1);
    }
    removeTempDb(path);
}

TEST_CASE("SourceDatabase - legacy RSS JSON restores missing status fields") {
    bool wide = false;
    SUBCASE("narrow table without either status field") {}
    SUBCASE("wide table without either status field") { wide = true; }
    const auto path = makeTempDbPath("_rss_json_upgrade");
    { SourceDatabase initial(path); }
    {
        sqlite::database raw(path);
        raw << "DROP INDEX idx_rss_validity;";
        raw << "ALTER TABLE rss_sources DROP COLUMN validity;";
        raw << "ALTER TABLE rss_sources DROP COLUMN latency_ms;";
        if (wide) {
            raw << "ALTER TABLE rss_sources ADD COLUMN legacy_a TEXT;";
            raw << "ALTER TABLE rss_sources ADD COLUMN legacy_b TEXT;";
            raw << "ALTER TABLE rss_sources ADD COLUMN legacy_c TEXT;";
        }
        raw << "DROP INDEX idx_rss_article_order;";
        raw << "ALTER TABLE rss_articles DROP COLUMN article_order;";
        raw << "INSERT INTO rss_sources(source_url, source_json) VALUES "
               "('https://example.invalid/valid', '{\"__validity\":\"good\",\"__latencyMs\":42}'), "
               "('https://example.invalid/malformed', 'not JSON');";
    }
    { SourceDatabase migrated(path); }
    {
        sqlite::database raw(path);
        int records = 0;
        raw << "SELECT count(*) FROM rss_sources WHERE validity='good' AND latency_ms=42;" >> records;
        CHECK(records == 1);
        raw << "SELECT count(*) FROM rss_sources WHERE validity='unknown' AND latency_ms=-1;" >> records;
        CHECK(records == 1);
        raw << "SELECT count(*) FROM pragma_table_info('rss_articles') WHERE name='article_order';" >> records;
        CHECK(records == 1);
        raw << "SELECT count(*) FROM sqlite_master WHERE tbl_name='rss_sources' AND name='idx_rss_validity';" >> records;
        CHECK(records == 1);
    }
    removeTempDb(path);
}

// ──────────────────────────────────────────────
// 书源有效性持久化（核心 bug 修复验证）
// ──────────────────────────────────────────────

TEST_CASE("SourceDatabase - validity 持久化到数据库") {
    auto dbPath = makeTempDbPath("_validity");

    // 第一次：插入书源并设置有效性
    {
        SourceDatabase db(dbPath);
        std::vector<BookSource> sources = {
            makeTestSource("优源", "https://excellent.com"),
            makeTestSource("良源", "https://good.com"),
            makeTestSource("差源", "https://poor.com"),
            makeTestSource("无效源", "https://invalid.com"),
        };
        db.insertSources(sources);

        db.updateValidity("https://excellent.com", SourceValidity::Excellent, 50);
        db.updateValidity("https://good.com", SourceValidity::Good, 200);
        db.updateValidity("https://poor.com", SourceValidity::Poor, 3000);
        db.updateValidity("https://invalid.com", SourceValidity::Invalid, 5000);
    }

    // 第二次：重新打开数据库，验证有效性被持久化
    {
        SourceDatabase db(dbPath);
        auto sources = db.getAllSources();
        CHECK(sources.size() == 4);

        // 构建 url → validity 映射
        std::map<std::string, SourceValidity> validityMap;
        std::map<std::string, int> latencyMap;
        for (const auto& s : sources) {
            validityMap[s.url] = s.validity;
            latencyMap[s.url] = s.latencyMs;
        }

        CHECK(validityMap["https://excellent.com"] == SourceValidity::Excellent);
        CHECK(validityMap["https://good.com"] == SourceValidity::Good);
        CHECK(validityMap["https://poor.com"] == SourceValidity::Poor);
        CHECK(validityMap["https://invalid.com"] == SourceValidity::Invalid);

        CHECK(latencyMap["https://excellent.com"] == 50);
        CHECK(latencyMap["https://good.com"] == 200);
    }

    removeTempDb(dbPath);
}

TEST_CASE("SourceDatabase - removeInvalidSources 删除差和无效") {
    auto dbPath = makeTempDbPath("_remove_invalid");
    SourceDatabase db(dbPath);

    std::vector<BookSource> sources = {
        makeTestSource("优", "https://a.com"),
        makeTestSource("差", "https://b.com"),
        makeTestSource("无效", "https://c.com"),
        makeTestSource("良", "https://d.com"),
    };
    db.insertSources(sources);

    db.updateValidity("https://a.com", SourceValidity::Excellent);
    db.updateValidity("https://b.com", SourceValidity::Poor);
    db.updateValidity("https://c.com", SourceValidity::Invalid);
    db.updateValidity("https://d.com", SourceValidity::Good);

    int removed = db.removeInvalidSources();
    CHECK(removed == 2);
    CHECK(db.getSourceCount() == 2);

    removeTempDb(dbPath);
}

// ──────────────────────────────────────────────
// 书架 CRUD
// ──────────────────────────────────────────────

TEST_CASE("SourceDatabase - 书架操作") {
    auto dbPath = makeTempDbPath("_bookshelf");
    SourceDatabase db(dbPath);

    BookshelfItem item;
    item.bookName = "仙逆";
    item.bookAuthor = "耳根";
    item.bookUrl = "book://xianni";
    item.sourceName = "源A";
    item.sourceUrl = "source://alpha";
    item.totalChapters = 2046;

    SUBCASE("添加到书架") {
        int64_t id = db.addToBookshelf(item);
        CHECK(id > 0);
        CHECK(db.isInBookshelf("book://xianni") == true);
        CHECK(db.getBookshelfCount() == 1);
    }

    SUBCASE("重复添加不增加记录数") {
        db.addToBookshelf(item);
        db.addToBookshelf(item);  // INSERT OR IGNORE
        CHECK(db.getBookshelfCount() == 1);  // 仍然只有 1 条
    }

    SUBCASE("从书架移除") {
        db.addToBookshelf(item);
        CHECK(db.removeFromBookshelf("book://xianni") == true);
        CHECK(db.isInBookshelf("book://xianni") == false);
        CHECK(db.getBookshelfCount() == 0);
    }

    SUBCASE("获取书架列表") {
        db.addToBookshelf(item);

        BookshelfItem item2;
        item2.bookName = "凡人修仙传";
        item2.bookAuthor = "忘语";
        item2.bookUrl = "book://fanren";
        item2.sourceName = "源B";
        item2.sourceUrl = "source://beta";
        db.addToBookshelf(item2);

        auto books = db.getBookshelf();
        CHECK(books.size() == 2);
    }

    SUBCASE("更新最新章节") {
        db.addToBookshelf(item);
        db.updateBookshelfLastChapter("book://xianni", "第2046章 大结局", 2046, false);

        auto books = db.getBookshelf();
        REQUIRE(books.size() == 1);
        CHECK(books[0].lastChapter == "第2046章 大结局");
        CHECK(books[0].totalChapters == 2046);
        CHECK(books[0].hasUpdate == false);
    }

    removeTempDb(dbPath);
}

// ──────────────────────────────────────────────
// 目录缓存
// ──────────────────────────────────────────────

TEST_CASE("SourceDatabase - 目录缓存") {
    auto dbPath = makeTempDbPath("_catalog_cache");
    SourceDatabase db(dbPath);

    // 目录缓存有外键约束，需要先添加书架条目
    BookshelfItem shelf;
    shelf.bookName = "测试书";
    shelf.bookUrl = "book://test";
    shelf.sourceName = "源";
    shelf.sourceUrl = "source://test";
    db.addToBookshelf(shelf);

    std::vector<Chapter> chapters = {
        {"第1章 开始", "ch://1", 0, false},
        {"第2章 修炼", "ch://2", 1, false},
        {"第3章 突破", "ch://3", 2, false},
    };

    SUBCASE("缓存和读取") {
        db.cacheBookCatalog("book://test", chapters);
        auto cached = db.getCachedCatalog("book://test");
        CHECK(cached.size() == 3);
        CHECK(cached[0].title == "第1章 开始");
        CHECK(cached[1].title == "第2章 修炼");
        CHECK(cached[2].title == "第3章 突破");
    }

    SUBCASE("缓存数量查询") {
        db.cacheBookCatalog("book://test", chapters);
        CHECK(db.getCachedCatalogCount("book://test") == 3);
    }

    SUBCASE("全量替换（不是追加）") {
        db.cacheBookCatalog("book://test", chapters);

        std::vector<Chapter> newChapters = {
            {"新第1章", "ch://new1", 0, false},
        };
        db.cacheBookCatalog("book://test", newChapters);

        auto cached = db.getCachedCatalog("book://test");
        CHECK(cached.size() == 1);
        CHECK(cached[0].title == "新第1章");
    }

    SUBCASE("删除缓存") {
        db.cacheBookCatalog("book://test", chapters);
        db.removeCachedCatalog("book://test");
        auto cached = db.getCachedCatalog("book://test");
        CHECK(cached.empty());
    }

    SUBCASE("不同书籍的目录隔离") {
        // 需要为 book://a 和 book://b 分别添加书架条目
        BookshelfItem shelfA;
        shelfA.bookName = "书A";
        shelfA.bookUrl = "book://a";
        shelfA.sourceName = "源";
        shelfA.sourceUrl = "source://a";
        db.addToBookshelf(shelfA);

        BookshelfItem shelfB;
        shelfB.bookName = "书B";
        shelfB.bookUrl = "book://b";
        shelfB.sourceName = "源";
        shelfB.sourceUrl = "source://b";
        db.addToBookshelf(shelfB);

        db.cacheBookCatalog("book://a", chapters);

        std::vector<Chapter> otherChapters = {
            {"其他第1章", "ch://other1", 0, false},
        };
        db.cacheBookCatalog("book://b", otherChapters);

        CHECK(db.getCachedCatalogCount("book://a") == 3);
        CHECK(db.getCachedCatalogCount("book://b") == 1);
    }

    removeTempDb(dbPath);
}

// ──────────────────────────────────────────────
// 正文缓存
// ──────────────────────────────────────────────

TEST_CASE("SourceDatabase - 正文缓存") {
    auto dbPath = makeTempDbPath("_content_cache");
    SourceDatabase db(dbPath);

    // 正文缓存有外键约束，需要先添加书架条目
    BookshelfItem shelf;
    shelf.bookName = "测试书";
    shelf.bookUrl = "book://test";
    shelf.sourceName = "源";
    shelf.sourceUrl = "source://test";
    db.addToBookshelf(shelf);

    SUBCASE("缓存和读取") {
        db.cacheChapterContent("book://test", 0, "ch://1", "这是第一章的正文内容");
        std::string content = db.getCachedContent("book://test", 0);
        CHECK(content == "这是第一章的正文内容");
    }

    SUBCASE("未缓存返回空") {
        std::string content = db.getCachedContent("book://test", 99);
        CHECK(content.empty());
    }

    SUBCASE("缓存数量") {
        db.cacheChapterContent("book://test", 0, "ch://1", "内容1");
        db.cacheChapterContent("book://test", 1, "ch://2", "内容2");
        db.cacheChapterContent("book://test", 2, "ch://3", "内容3");
        CHECK(db.getCachedContentCount("book://test") == 3);
    }

    SUBCASE("删除正文缓存") {
        db.cacheChapterContent("book://test", 0, "ch://1", "内容");
        db.removeCachedContent("book://test");
        CHECK(db.getCachedContentCount("book://test") == 0);
    }

    SUBCASE("不同书籍的正文隔离") {
        BookshelfItem shelfA;
        shelfA.bookName = "书A";
        shelfA.bookUrl = "book://a";
        shelfA.sourceName = "源";
        shelfA.sourceUrl = "source://a";
        db.addToBookshelf(shelfA);

        BookshelfItem shelfB;
        shelfB.bookName = "书B";
        shelfB.bookUrl = "book://b";
        shelfB.sourceName = "源";
        shelfB.sourceUrl = "source://b";
        db.addToBookshelf(shelfB);

        db.cacheChapterContent("book://a", 0, "ch://a1", "A的内容");
        db.cacheChapterContent("book://b", 0, "ch://b1", "B的内容");

        CHECK(db.getCachedContent("book://a", 0) == "A的内容");
        CHECK(db.getCachedContent("book://b", 0) == "B的内容");
    }

    removeTempDb(dbPath);
}

// ──────────────────────────────────────────────
// 阅读进度
// ──────────────────────────────────────────────

TEST_CASE("SourceDatabase - 阅读进度") {
    auto dbPath = makeTempDbPath("_progress");
    SourceDatabase db(dbPath);

    // 需要先添加书架条目（进度关联 bookUrl）
    BookshelfItem item;
    item.bookName = "测试书";
    item.bookUrl = "book://test";
    item.sourceName = "源";
    item.sourceUrl = "source://test";
    db.addToBookshelf(item);

    ReadProgress progress;
    progress.bookUrl = "book://test";
    progress.chapterIndex = 42;
    progress.chapterTitle = "第42章";
    progress.chapterUrl = "ch://42";
    progress.readPercent = 0.75;

    SUBCASE("保存和读取") {
        db.saveReadProgress(progress);
        auto loaded = db.getReadProgress("book://test");
        CHECK(loaded.bookUrl == "book://test");
        CHECK(loaded.chapterIndex == 42);
        CHECK(loaded.chapterTitle == "第42章");
        CHECK(loaded.readPercent == doctest::Approx(0.75));
    }

    SUBCASE("更新进度（覆盖）") {
        db.saveReadProgress(progress);

        progress.chapterIndex = 100;
        progress.chapterTitle = "第100章";
        progress.readPercent = 0.5;
        db.saveReadProgress(progress);

        auto loaded = db.getReadProgress("book://test");
        CHECK(loaded.chapterIndex == 100);
        CHECK(loaded.chapterTitle == "第100章");
    }

    SUBCASE("未保存的进度返回默认值") {
        auto loaded = db.getReadProgress("book://not-exist");
        // getReadProgress 总是设置 bookUrl = 传入的参数，但 chapterIndex 为默认 0
        CHECK(loaded.chapterIndex == 0);
        CHECK(loaded.readPercent == doctest::Approx(0.0));
    }

    removeTempDb(dbPath);
}

// ──────────────────────────────────────────────
// 书架删除级联
// ──────────────────────────────────────────────

TEST_CASE("SourceDatabase - 删除书架条目级联删除缓存和进度") {
    auto dbPath = makeTempDbPath("_cascade");
    SourceDatabase db(dbPath);

    // 添加书架
    BookshelfItem item;
    item.bookName = "测试书";
    item.bookUrl = "book://cascade";
    item.sourceName = "源";
    item.sourceUrl = "source://test";
    db.addToBookshelf(item);

    // 缓存目录
    std::vector<Chapter> chapters = {
        {"第1章", "ch://1", 0, false},
        {"第2章", "ch://2", 1, false},
    };
    db.cacheBookCatalog("book://cascade", chapters);

    // 缓存正文
    db.cacheChapterContent("book://cascade", 0, "ch://1", "正文1");
    db.cacheChapterContent("book://cascade", 1, "ch://2", "正文2");

    // 保存进度
    ReadProgress progress;
    progress.bookUrl = "book://cascade";
    progress.chapterIndex = 1;
    db.saveReadProgress(progress);

    // 验证数据存在
    CHECK(db.getCachedCatalogCount("book://cascade") == 2);
    CHECK(db.getCachedContentCount("book://cascade") == 2);

    // 删除书架条目
    db.removeFromBookshelf("book://cascade");

    // 验证级联删除
    CHECK(db.getCachedCatalogCount("book://cascade") == 0);
    CHECK(db.getCachedContentCount("book://cascade") == 0);
    auto loadedProgress = db.getReadProgress("book://cascade");
    // 级联删除后，进度记录应该被清除，chapterIndex 回到默认 0
    CHECK(loadedProgress.chapterIndex == 0);
    CHECK(loadedProgress.readPercent == doctest::Approx(0.0));

    removeTempDb(dbPath);
}

// ──────────────────────────────────────────────
// 批量查询（避免 N+1）
// ──────────────────────────────────────────────

TEST_CASE("SourceDatabase - 批量查询缓存数量") {
    auto dbPath = makeTempDbPath("_batch");
    SourceDatabase db(dbPath);

    // 需要先添加书架条目（外键约束）
    BookshelfItem shelfA;
    shelfA.bookName = "书A";
    shelfA.bookUrl = "book://a";
    shelfA.sourceName = "源";
    shelfA.sourceUrl = "source://a";
    db.addToBookshelf(shelfA);

    BookshelfItem shelfB;
    shelfB.bookName = "书B";
    shelfB.bookUrl = "book://b";
    shelfB.sourceName = "源";
    shelfB.sourceUrl = "source://b";
    db.addToBookshelf(shelfB);

    // 添加两本书的目录缓存
    std::vector<Chapter> ch1 = {{"第1章", "ch://1", 0, false}, {"第2章", "ch://2", 1, false}};
    std::vector<Chapter> ch2 = {{"第1章", "ch://b1", 0, false}};
    db.cacheBookCatalog("book://a", ch1);
    db.cacheBookCatalog("book://b", ch2);

    // 添加正文缓存
    db.cacheChapterContent("book://a", 0, "ch://1", "内容1");
    db.cacheChapterContent("book://a", 1, "ch://2", "内容2");

    SUBCASE("批量目录缓存数量") {
        auto counts = db.batchGetCachedCatalogCounts();
        CHECK(counts.size() == 2);
    }

    SUBCASE("批量正文缓存数量") {
        auto counts = db.batchGetCachedContentCounts();
        CHECK(counts.size() >= 1);
    }

    removeTempDb(dbPath);
}

// ──────────────────────────────────────────────
// URL 历史记录
// ──────────────────────────────────────────────

TEST_CASE("SourceDatabase - URL 历史记录") {
    auto dbPath = makeTempDbPath("_url_history");
    SourceDatabase db(dbPath);

    SUBCASE("添加和获取") {
        db.addUrlHistory("https://example.com/sources.json");
        auto history = db.getUrlHistory();
        CHECK(history.size() == 1);
        CHECK(history[0] == "https://example.com/sources.json");
    }

    SUBCASE("去重") {
        db.addUrlHistory("https://a.com");
        db.addUrlHistory("https://a.com");
        auto history = db.getUrlHistory();
        CHECK(history.size() == 1);
    }

    SUBCASE("删除") {
        db.addUrlHistory("https://a.com");
        db.removeUrlHistory("https://a.com");
        auto history = db.getUrlHistory();
        CHECK(history.empty());
    }

    removeTempDb(dbPath);
}

/// @file test_engine_bookshelf.cpp
/// @brief E4: engine_bookshelf 书架/缓存/换源/导出关键逻辑单测

#include <doctest/doctest.h>
#include "ariaread/engine.h"
#include "ariaread/types.h"

#include <filesystem>

using namespace ariaread;
namespace fs = std::filesystem;

static std::string tmpDbPath(const std::string& tag) {
    auto dir = fs::temp_directory_path() / "ariaread_ut";
    fs::create_directories(dir);
    auto p = dir / ("test_bookshelf_" + tag + ".db");
    // 每次测试前清理
    std::error_code ec;
    fs::remove(p, ec);
    fs::remove(p.string() + "-wal", ec);
    fs::remove(p.string() + "-shm", ec);
    return p.string();
}

TEST_CASE("bookshelf: add / is_in / remove / get 工作闭环") {
    BookSourceEngine engine;
    engine.setDatabasePath(tmpDbPath("crud"));

    BookshelfItem item;
    item.bookName = "测试书";
    item.bookAuthor = "佚名";
    item.bookUrl = "http://example.com/book/1";
    item.sourceName = "TestSource";
    item.sourceUrl = "http://example.com";

    auto id1 = engine.addToBookshelf(item);
    CHECK(id1 >= 0);
    CHECK(engine.isInBookshelf(item.bookUrl));

    // 同 bookUrl 再次 add 应被拒绝（返回 -1）
    auto id2 = engine.addToBookshelf(item);
    CHECK(id2 == -1);

    auto shelf = engine.getBookshelf();
    CHECK(shelf.size() == 1);
    CHECK(shelf[0].bookName == "测试书");

    CHECK(engine.removeFromBookshelf(item.bookUrl));
    CHECK_FALSE(engine.isInBookshelf(item.bookUrl));
}

TEST_CASE("read progress: save / get") {
    BookSourceEngine engine;
    engine.setDatabasePath(tmpDbPath("progress"));

    BookshelfItem item;
    item.bookUrl = "http://example.com/book/p1";
    item.bookName = "P1";
    item.sourceUrl = "http://example.com";
    engine.addToBookshelf(item);

    ReadProgress prog;
    prog.bookUrl = item.bookUrl;
    prog.chapterIndex = 3;
    prog.chapterTitle = "第三章 开端";
    prog.chapterUrl = "http://example.com/book/p1/ch3";
    prog.readPercent = 0.42;
    engine.saveReadProgress(prog);

    auto got = engine.getReadProgress(item.bookUrl);
    CHECK(got.chapterIndex == 3);
    CHECK(got.chapterTitle == "第三章 开端");
    CHECK(got.readPercent == doctest::Approx(0.42));
}

TEST_CASE("catalog cache: cacheBookCatalog / getCachedCatalog / clearBookCache 闭环") {
    BookSourceEngine engine;
    engine.setDatabasePath(tmpDbPath("catalog"));

    BookshelfItem item;
    item.bookUrl = "http://example.com/book/c1";
    item.bookName = "C1";
    engine.addToBookshelf(item);

    std::vector<Chapter> chapters;
    for (int i = 0; i < 5; ++i) {
        Chapter ch;
        ch.index = i;
        ch.title = "第" + std::to_string(i + 1) + "章";
        ch.url = "http://example.com/book/c1/ch" + std::to_string(i);
        chapters.push_back(ch);
    }
    engine.cacheBookCatalog(item.bookUrl, "", chapters);

    auto cached = engine.getCachedCatalog(item.bookUrl, "");
    CHECK(cached.size() == 5);
    CHECK(engine.getCachedCatalogCount(item.bookUrl, "") == 5);

    engine.clearBookCache(item.bookUrl, "");
    CHECK(engine.getCachedCatalogCount(item.bookUrl, "") == 0);
}

TEST_CASE("exportBookToTxt: 未在书架 → 空串 + lastError") {
    BookSourceEngine engine;
    engine.setDatabasePath(tmpDbPath("export_missing"));
    auto txt = engine.exportBookToTxt("http://example.com/nonexistent", "");
    CHECK(txt.empty());
    CHECK(engine.getLastError().find("not in bookshelf") != std::string::npos);
}

TEST_CASE("exportBookToTxt: 已入书架但无目录缓存 → 空串 + lastError") {
    BookSourceEngine engine;
    engine.setDatabasePath(tmpDbPath("export_no_catalog"));

    BookshelfItem item;
    item.bookUrl = "http://example.com/book/exp1";
    item.bookName = "待导出的书";
    engine.addToBookshelf(item);

    auto txt = engine.exportBookToTxt(item.bookUrl, "");
    CHECK(txt.empty());
    CHECK(engine.getLastError().find("Catalog not cached") != std::string::npos);
}

TEST_CASE("exportBookToTxt: 正常导出（部分章节正文未缓存时也能导出，缺失章节标注）") {
    BookSourceEngine engine;
    engine.setDatabasePath(tmpDbPath("export_ok"));

    BookshelfItem item;
    item.bookUrl = "http://example.com/book/exp2";
    item.bookName = "完本小说";
    item.bookAuthor = "测试作者";
    item.sourceName = "TestSource";
    engine.addToBookshelf(item);

    std::vector<Chapter> chapters;
    for (int i = 0; i < 3; ++i) {
        Chapter ch;
        ch.index = i;
        ch.title = "第" + std::to_string(i + 1) + "章";
        ch.url = "http://example.com/book/exp2/ch" + std::to_string(i);
        chapters.push_back(ch);
    }
    engine.cacheBookCatalog(item.bookUrl, "", chapters);

    // 只缓存前两章正文
    engine.cacheChapterContent(chapters[0].url, item.bookUrl, "", "第一章内容 aaa");
    engine.cacheChapterContent(chapters[1].url, item.bookUrl, "", "第二章内容 bbb");

    auto txt = engine.exportBookToTxt(item.bookUrl, "");
    CHECK(!txt.empty());
    CHECK(txt.find("《完本小说》") != std::string::npos);
    CHECK(txt.find("测试作者") != std::string::npos);
    CHECK(txt.find("第一章内容 aaa") != std::string::npos);
    CHECK(txt.find("第二章内容 bbb") != std::string::npos);
    // 第三章未缓存，应包含占位提示
    CHECK(txt.find("[本章正文尚未缓存]") != std::string::npos);
}

/// @file test_reader_view_model.cpp
/// @brief ReaderViewModel 的 headless 单元测试。
///
/// 用 InlineExecutor 作 ui + worker，注入 FakeReaderBackend，
/// 验证：load_catalog → chapters/chapter_count；open_chapter → content/index；
/// save_progress → 后端收到正确进度。

#include <doctest/doctest.h>

#include "openread/vm/reader_view_model.h"

#include "aria/async/executor.hpp"

#include <string>
#include <vector>

using namespace openread;
using aria::async::InlineExecutor;

namespace {

/// Fake 后端：返回固定目录和正文。
class FakeReaderBackend : public vm::ReaderBackend {
public:
    std::vector<Chapter> load_catalog(const std::string& bookUrl,
                                     const std::string&) override {
        last_load_catalog_book = bookUrl;
        if (fail_catalog_) return {};
        std::vector<Chapter> chs;
        for (int i = 0; i < 3; ++i) {
            Chapter c;
            c.title = "第" + std::to_string(i + 1) + "章";
            c.url = bookUrl + "/ch/" + std::to_string(i);
            chs.push_back(c);
        }
        return chs;
    }

    std::string load_content(const std::string& bookUrl,
                            const std::string& chapterUrl,
                            int chapterIndex,
                            const std::string&) override {
        last_content_chapter_url = chapterUrl;
        if (fail_content_) return "";
        return "正文内容[" + std::to_string(chapterIndex) + "]";
    }

    void save_progress(const ReadProgress& progress) override {
        last_saved_progress = progress;
    }

    ReadProgress load_progress(const std::string&) override {
        return last_saved_progress;
    }

    // 测试操控
    bool fail_catalog_ = false;
    bool fail_content_ = false;
    std::string last_load_catalog_book;
    std::string last_content_chapter_url;
    ReadProgress last_saved_progress;
};

}  // namespace

TEST_CASE("ReaderViewModel: load_catalog 设置 chapters 和 chapter_count") {
    InlineExecutor ui, worker;
    FakeReaderBackend fake;
    vm::ReaderViewModel rvm{ui, worker, fake};

    rvm.book_url.set("http://book.example.com/123");
    rvm.source_url.set("http://src.example.com");
    rvm.load_catalog();

    CHECK(rvm.chapters.size() == 3);
    CHECK(rvm.chapter_count.get() == 3);
    CHECK(rvm.chapters.at(0)->title == "第1章");
    CHECK(rvm.chapters.at(2)->url == "http://book.example.com/123/ch/2");
    CHECK(fake.last_load_catalog_book == "http://book.example.com/123");
}

TEST_CASE("ReaderViewModel: open_chapter 设置 content/index/title/percent") {
    InlineExecutor ui, worker;
    FakeReaderBackend fake;
    vm::ReaderViewModel rvm{ui, worker, fake};

    rvm.book_url.set("http://book.example.com/123");
    rvm.source_url.set("http://src.example.com");
    rvm.load_catalog();

    // 打开第 2 章（index=1）
    rvm.open_chapter(1);
    CHECK(rvm.current_index.get() == 1);
    CHECK(rvm.current_title.get() == "第2章");
    CHECK(rvm.content.get() == "正文内容[1]");
    // 3 章，第 2 章 → 2/3 ≈ 0.667
    CHECK(rvm.read_percent.get() == doctest::Approx(2.0 / 3.0));

    CHECK(fake.last_content_chapter_url == "http://book.example.com/123/ch/1");
}

TEST_CASE("ReaderViewModel: save_progress 传递正确数据到后端") {
    InlineExecutor ui, worker;
    FakeReaderBackend fake;
    vm::ReaderViewModel rvm{ui, worker, fake};

    rvm.book_url.set("http://book.example.com/123");
    rvm.source_url.set("http://src.example.com");
    rvm.load_catalog();
    rvm.open_chapter(2);

    rvm.save_progress();

    CHECK(fake.last_saved_progress.bookUrl == "http://book.example.com/123");
    CHECK(fake.last_saved_progress.chapterIndex == 2);
    CHECK(fake.last_saved_progress.chapterTitle == "第3章");
}

TEST_CASE("ReaderViewModel: 空目录时 open_chapter 不崩溃") {
    InlineExecutor ui, worker;
    FakeReaderBackend fake;
    fake.fail_catalog_ = true;
    vm::ReaderViewModel rvm{ui, worker, fake};

    rvm.book_url.set("http://empty.example.com");
    rvm.load_catalog();

    CHECK(rvm.chapters.size() == 0);
    CHECK(rvm.chapter_count.get() == 0);

    // open_chapter 在空目录下不应崩溃
    rvm.open_chapter(0);
    CHECK(rvm.content.get() == "");
}

/// @file test_bookshelf_view_model.cpp
/// @brief BookshelfViewModel 的 headless 单元测试（fake backend，无真实 DB）。

#include <doctest/doctest.h>

#include "openread/vm/bookshelf_view_model.h"

#include "aria/async/executor.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

using namespace openread;
using aria::async::InlineExecutor;

namespace {

openread::BookshelfDetail make_detail(const std::string& url,
                                      const std::string& name) {
    openread::BookshelfDetail d;
    d.item.bookUrl = url;
    d.item.bookName = name;
    return d;
}

/// 内存版书架后端：模拟引擎的 load/remove/add。
class FakeBookshelfBackend : public vm::BookshelfBackend {
public:
    std::vector<openread::BookshelfDetail> items;

    std::vector<openread::BookshelfDetail> load() override { return items; }

    bool remove(const std::string& bookUrl) override {
        auto before = items.size();
        items.erase(
            std::remove_if(items.begin(), items.end(),
                           [&](const auto& d) { return d.item.bookUrl == bookUrl; }),
            items.end());
        return items.size() < before;
    }

    int64_t add(const openread::BookshelfItem& item) override {
        for (const auto& d : items) {
            if (d.item.bookUrl == item.bookUrl) return -1;  // 已存在
        }
        openread::BookshelfDetail d;
        d.item = item;
        items.push_back(d);
        return static_cast<int64_t>(items.size());
    }
};

}  // namespace

TEST_CASE("书架：refresh 加载全部条目") {
    InlineExecutor ui, worker;
    FakeBookshelfBackend backend;
    backend.items = {make_detail("u1", "书一"), make_detail("u2", "书二")};

    vm::BookshelfViewModel bvm{ui, worker, backend};
    bvm.refresh();

    CHECK(bvm.books.size() == 2);
    CHECK(bvm.count.get() == 2);
    CHECK(bvm.books.at(0)->item.bookName == "书一");
    CHECK_FALSE(bvm.is_loading().get());
    CHECK(bvm.last_error_message().get() == "");
}

TEST_CASE("书架：remove 后列表自动刷新") {
    InlineExecutor ui, worker;
    FakeBookshelfBackend backend;
    backend.items = {make_detail("u1", "书一"),
                     make_detail("u2", "书二"),
                     make_detail("u3", "书三")};

    vm::BookshelfViewModel bvm{ui, worker, backend};
    bvm.refresh();
    CHECK(bvm.books.size() == 3);

    bvm.remove("u2");
    CHECK(bvm.books.size() == 2);
    CHECK(bvm.count.get() == 2);
    // 剩 u1, u3
    CHECK(bvm.books.at(0)->item.bookUrl == "u1");
    CHECK(bvm.books.at(1)->item.bookUrl == "u3");
}

TEST_CASE("书架：add 后列表自动刷新，重复 add 不增加") {
    InlineExecutor ui, worker;
    FakeBookshelfBackend backend;

    vm::BookshelfViewModel bvm{ui, worker, backend};
    bvm.refresh();
    CHECK(bvm.books.size() == 0);

    openread::BookshelfItem item;
    item.bookUrl = "new";
    item.bookName = "新书";
    bvm.add(item);
    CHECK(bvm.books.size() == 1);
    CHECK(bvm.count.get() == 1);
    CHECK(bvm.books.at(0)->item.bookName == "新书");

    // 重复加入同一本：列表本数不变
    bvm.add(item);
    CHECK(bvm.books.size() == 1);
}

TEST_CASE("书架：backend 抛异常时 last_error 被填充") {
    InlineExecutor ui, worker;

    struct ThrowingBackend : vm::BookshelfBackend {
        std::vector<openread::BookshelfDetail> load() override {
            throw std::runtime_error("db locked");
        }
        bool remove(const std::string&) override { return false; }
        int64_t add(const openread::BookshelfItem&) override { return -1; }
    } backend;

    vm::BookshelfViewModel bvm{ui, worker, backend};
    bvm.refresh();

    CHECK(bvm.last_error_message().get() == "db locked");
    CHECK(bvm.books.size() == 0);
    CHECK_FALSE(bvm.is_loading().get());
}

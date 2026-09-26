/// @file test_search_view_model.cpp
/// @brief 流式 SearchViewModel 的 headless 单元测试。
///
/// 用 Aria 的 InlineExecutor（同步执行）作 ui + worker，注入 mock 流式搜索，
/// 全程无真实网络/书源/线程，验证：
///   逐源 emit → 切回 ui 增量 append → results / found_count / 状态 Property 正确。

#include <doctest/doctest.h>

#include "ariaread/vm/search_view_model.h"

#include "aria/async/executor.hpp"

#include <atomic>
#include <string>
#include <vector>

using namespace ariaread;
using aria::async::InlineExecutor;

namespace {

ariaread::Book make_book(const std::string& name, const std::string& author) {
    ariaread::Book b;
    b.name = name;
    b.author = author;
    b.bookUrl = "http://example.com/" + name;
    return b;
}

}  // namespace

TEST_CASE("流式搜索：多个源逐批 append，found_count 实时递增") {
    InlineExecutor ui, worker;

    // mock：模拟 3 个源分别返回 2 / 1 / 2 本（共 5 本），逐源 emit
    vm::StreamSearchFn mock = [](const std::string& kw,
                                 const vm::SearchEmit& emit,
                                 const std::atomic<bool>&) {
        emit({make_book(kw + "-1a", "甲"), make_book(kw + "-1b", "甲")});
        emit({make_book(kw + "-2a", "乙")});
        emit({make_book(kw + "-3a", "丙"), make_book(kw + "-3b", "丙")});
    };

    vm::SearchViewModel svm{ui, worker, mock};
    svm.keyword.set("斗破");
    svm.search();

    CHECK_FALSE(svm.is_searching().get());
    CHECK(svm.results.size() == 5);
    CHECK(svm.found_count.get() == 5);
    CHECK(svm.results.at(0)->name == "斗破-1a");
    CHECK(svm.results.at(4)->name == "斗破-3b");
    REQUIRE(svm.last_total().get().has_value());
    CHECK(*svm.last_total().get() == 5);
    CHECK(svm.last_error_message().get() == "");
}

TEST_CASE("流式搜索：再次搜索先清空旧结果") {
    InlineExecutor ui, worker;

    int call = 0;
    vm::StreamSearchFn mock = [&call](const std::string&,
                                      const vm::SearchEmit& emit,
                                      const std::atomic<bool>&) {
        ++call;
        if (call == 1) {
            emit({make_book("一", "x"), make_book("二", "y"), make_book("三", "z")});
        } else {
            emit({make_book("新", "w")});
        }
    };

    vm::SearchViewModel svm{ui, worker, mock};

    svm.search_with("first");
    CHECK(svm.results.size() == 3);
    CHECK(svm.found_count.get() == 3);

    svm.search_with("second");
    CHECK(svm.results.size() == 1);
    CHECK(svm.found_count.get() == 1);
    CHECK(svm.results.at(0)->name == "新");
    CHECK(*svm.last_total().get() == 1);
}

TEST_CASE("流式搜索：空 emit 不污染列表，总数为 0") {
    InlineExecutor ui, worker;
    vm::StreamSearchFn mock = [](const std::string&,
                                 const vm::SearchEmit& emit,
                                 const std::atomic<bool>&) {
        emit({});          // 空批次应被忽略
    };

    vm::SearchViewModel svm{ui, worker, mock};
    svm.search_with("不存在");

    CHECK(svm.results.size() == 0);
    CHECK(svm.found_count.get() == 0);
    REQUIRE(svm.last_total().get().has_value());
    CHECK(*svm.last_total().get() == 0);
    CHECK_FALSE(svm.is_searching().get());
}

TEST_CASE("流式搜索：进行中 cancel 后实现停止继续 emit") {
    InlineExecutor ui, worker;

    vm::SearchViewModel* svm_ptr = nullptr;

    // mock：每批前检查取消标志。第 2 批后由回调触发 svm.cancel()，
    // 之后的循环检查到 cancel=true 即停止。
    vm::StreamSearchFn mock = [&svm_ptr](const std::string&,
                                         const vm::SearchEmit& emit,
                                         const std::atomic<bool>& cancel) {
        for (int i = 0; i < 5; ++i) {
            if (cancel.load()) return;             // 被取消则不再 emit 后续源
            emit({make_book("book-" + std::to_string(i), "a")});
            if (i == 1 && svm_ptr) svm_ptr->cancel();  // 第 2 批后请求取消
        }
    };

    vm::SearchViewModel svm{ui, worker, mock};
    svm_ptr = &svm;
    svm.search_with("x");

    // 应只 emit 了前 2 批（i=0,1），第 3 轮检查到 cancel 即停。
    CHECK(svm.results.size() == 2);
    CHECK(svm.found_count.get() == 2);
}

TEST_CASE("流式搜索：搜索实现抛异常时 last_error 被填充") {
    InlineExecutor ui, worker;
    vm::StreamSearchFn mock = [](const std::string&,
                                 const vm::SearchEmit&,
                                 const std::atomic<bool>&) {
        throw std::runtime_error("source down");
    };

    vm::SearchViewModel svm{ui, worker, mock};
    svm.search_with("会炸");

    CHECK(svm.last_error_message().get() == "source down");
    CHECK_FALSE(svm.is_searching().get());
    CHECK(svm.results.size() == 0);
}

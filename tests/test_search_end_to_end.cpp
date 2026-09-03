/// @file test_search_end_to_end.cpp
/// @brief 端到端示例 + 测试：SearchViewModel 通过 Aria BindingEngine 驱动一个
///        真实 IViewAdapter（FakeAdapter）。
///
/// 这验证了完整 MVVM 闭环：
///   用户在"输入框"打字 → keyword Property 更新（双向绑定）
///   → 触发 search() → 流式结果回填 ObservableList + found_count
///   → found_count 通过单向绑定自动显示到"计数控件"
///   → is_searching 自动驱动"loading 控件"
///
/// FakeAdapter / FakeView 是 Aria 自带的真实 IViewAdapter 实现（来自 binding
/// 的测试辅助），各端真实适配器（Qt6 / AppKit / UIKit）可无缝替换它。

#include <doctest/doctest.h>

#include "openread/vm/search_view_model.h"

#include "aria/async/executor.hpp"
#include "aria/binding/binding_engine.hpp"
// FakeAdapter/FakeView 是 Aria binding 的测试辅助（modules/binding/tests/）。
// CMake 已把该目录加入本测试的 include 搜索路径。
#include "fake_adapter.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

using namespace openread;
using aria::async::InlineExecutor;
using aria::binding::BindingEngine;
using aria::binding::testing::FakeAdapter;
using aria::binding::testing::FakeView;

namespace {

openread::Book make_book(const std::string& name) {
    openread::Book b;
    b.name = name;
    b.bookUrl = "http://example.com/" + name;
    return b;
}

}  // namespace

TEST_CASE("端到端：用户输入 → 搜索 → 结果数自动显示到控件") {
    InlineExecutor ui, worker;

    // mock 流式搜索：返回 3 本
    vm::StreamSearchFn mock = [](const std::string& kw,
                                 const vm::SearchEmit& emit,
                                 const std::atomic<bool>&) {
        emit({make_book(kw + "-1"), make_book(kw + "-2")});
        emit({make_book(kw + "-3")});
    };

    vm::SearchViewModel svm{ui, worker, mock};

    // 真实 IViewAdapter + BindingEngine
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    FakeView keyword_box;     // 输入框（双向绑 keyword）
    FakeView count_label;     // 计数控件（单向绑 found_count）
    FakeView loading_flag;    // loading 控件（单向绑 is_searching）

    engine.bind_text(svm.keyword, keyword_box);
    engine.bind_int_oneway(svm.found_count, count_label);
    engine.bind_bool_oneway(svm.is_searching(), loading_flag);

    // 1) 模拟用户在输入框打字 → keyword Property 应被更新（双向绑定）
    FakeAdapter::user_type(keyword_box, "斗破");
    CHECK(svm.keyword.get() == "斗破");

    // 2) 触发搜索（InlineExecutor 同步执行整条链路）
    svm.search();

    // 3) 结果数应通过单向绑定自动写到计数控件
    CHECK(svm.found_count.get() == 3);
    CHECK(count_label.integer == 3);

    // 4) 搜索结束后 loading 应回到 false
    CHECK_FALSE(svm.is_searching().get());
    CHECK_FALSE(loading_flag.flag);

    // 5) 列表确实拿到 3 本（含用户输入的 keyword 前缀）
    REQUIRE(svm.results.size() == 3);
    CHECK(svm.results.at(0)->name == "斗破-1");
}

TEST_CASE("端到端：VM→View 反向同步——改 Property 自动刷新控件文本") {
    InlineExecutor ui, worker;
    vm::StreamSearchFn mock = [](const std::string&,
                                 const vm::SearchEmit&,
                                 const std::atomic<bool>&) {};

    vm::SearchViewModel svm{ui, worker, mock};

    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    FakeView keyword_box;
    engine.bind_text(svm.keyword, keyword_box);

    // 程序侧改 Property → 控件文本应自动更新（VM→View 方向）
    svm.keyword.set("程序设定的关键词");
    CHECK(keyword_box.text == "程序设定的关键词");
}

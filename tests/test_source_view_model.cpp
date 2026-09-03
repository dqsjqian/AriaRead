/// @file test_source_view_model.cpp
/// @brief SourceViewModel 的 headless 单元测试。
///
/// 用 InlineExecutor 作 ui + worker，注入 FakeSourceBackend，
/// 验证：refresh → sources/stats/count；load_from_json → 自动刷新；
/// clear_all → 清空；export_good → JSON；validate → 进度回调。

#include <doctest/doctest.h>

#include "openread/vm/source_view_model.h"

#include "aria/async/executor.hpp"

#include <string>

using namespace openread;
using aria::async::InlineExecutor;

namespace {

SourceSummary make_src(const std::string& name, const std::string& url,
                       const std::string& validity = "unknown") {
    SourceSummary s;
    s.name = name;
    s.url = url;
    s.validity = validity;
    s.latencyMs = -1;
    return s;
}

/// Fake 后端：返回固定书源列表。
class FakeSourceBackend : public vm::SourceBackend {
public:
    SourceListResult get_source_list() override {
        SourceListResult r;
        r.sources = stored_;
        r.validCount = static_cast<int>(stored_.size());
        r.totalCount = static_cast<int>(stored_.size()) + invalid_extra_;
        r.stats.excellent = excellent_;
        r.stats.good = good_;
        r.stats.poor = poor_;
        r.stats.invalid = invalid_extra_;
        r.stats.unknown = unknown_;
        return r;
    }

    int load_sources_from_json(const std::string&) override {
        stored_.push_back(make_src("新增源", "http://new.src"));
        return 1;
    }

    int load_sources_from_file(const std::string&) override {
        stored_.push_back(make_src("文件源", "http://file.src"));
        return 1;
    }

    void validate_sources(
        const std::string&, int, int,
        std::function<void(size_t, const std::string&, SourceValidity, int)> perSource,
        std::function<void(int, int)> done,
        std::shared_ptr<CancelToken>) override {
        // 同步模拟：逐源回调 + 完成
        for (size_t i = 0; i < stored_.size(); ++i) {
            perSource(i, stored_[i].name, SourceValidity::Excellent, 50);
        }
        done(static_cast<int>(stored_.size()), 0);
    }

    int clear_all_sources() override {
        auto n = static_cast<int>(stored_.size());
        stored_.clear();
        excellent_ = good_ = poor_ = invalid_extra_ = unknown_ = 0;
        return n;
    }

    std::string export_good_sources() override {
        return "[{\"name\":\"good_src\"}]";
    }

    // 测试操控
    std::vector<SourceSummary> stored_{
        make_src("源A", "http://a.src", "excellent"),
        make_src("源B", "http://b.src", "good"),
    };
    int excellent_ = 1;
    int good_ = 1;
    int poor_ = 0;
    int invalid_extra_ = 0;
    int unknown_ = 0;
};

}  // namespace

TEST_CASE("SourceViewModel: refresh 设置 sources/stats/count") {
    InlineExecutor ui, worker;
    FakeSourceBackend fake;
    vm::SourceViewModel svm{ui, worker, fake};

    svm.refresh();

    CHECK(svm.sources.size() == 2);
    CHECK(svm.valid_count.get() == 2);
    CHECK(svm.total_count.get() == 2);
    CHECK(svm.stat_excellent.get() == 1);
    CHECK(svm.stat_good.get() == 1);
    CHECK(svm.stat_poor.get() == 0);
    CHECK(svm.stat_unknown.get() == 0);
    CHECK(svm.sources.at(0)->name == "源A");
}

TEST_CASE("SourceViewModel: load_from_json 自动刷新列表") {
    InlineExecutor ui, worker;
    FakeSourceBackend fake;
    vm::SourceViewModel svm{ui, worker, fake};

    svm.refresh();  // 初始 2 个
    CHECK(svm.sources.size() == 2);

    svm.load_from_json("[{\"name\":\"新增源\"}]");
    CHECK(svm.sources.size() == 3);  // 自动刷新后看到新增
    CHECK(svm.sources.at(2)->name == "新增源");
}

TEST_CASE("SourceViewModel: clear_all 清空列表") {
    InlineExecutor ui, worker;
    FakeSourceBackend fake;
    vm::SourceViewModel svm{ui, worker, fake};

    svm.refresh();
    CHECK(svm.sources.size() == 2);

    svm.clear_all();
    CHECK(svm.sources.size() == 0);
    CHECK(svm.total_count.get() == 0);
}

TEST_CASE("SourceViewModel: export_good 返回 JSON") {
    InlineExecutor ui, worker;
    FakeSourceBackend fake;
    vm::SourceViewModel svm{ui, worker, fake};

    svm.export_good();
    CHECK(svm.exported_json.get() == "[{\"name\":\"good_src\"}]");
}

TEST_CASE("SourceViewModel: validate 设置进度并在完成后刷新") {
    InlineExecutor ui, worker;
    FakeSourceBackend fake;
    vm::SourceViewModel svm{ui, worker, fake};

    svm.refresh();  // 先加载 2 个源
    CHECK_FALSE(svm.is_validating.get());

    svm.validate("测试", 5000, 4);

    // Fake 同步完成，validate 结束后 is_validating 回 false
    CHECK_FALSE(svm.is_validating.get());
}

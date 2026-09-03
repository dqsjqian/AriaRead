/// @file source_view_model.cpp
/// @brief SourceViewModel 的实现。

#include "openread/vm/source_view_model.h"

#include "aria/async/task.hpp"

#include <memory>

namespace openread::vm {

SourceViewModel::SourceViewModel(aria::async::IExecutor& ui,
                                aria::async::IExecutor& worker,
                                SourceBackend& backend)
    : ui_(ui),
      backend_(backend),
      refresh_cmd_(
          ui, worker,
          [this]() -> aria::async::Task<int> {
              auto r = backend_.get_source_list();
              ui_.post([this, r] { apply_list_(r); });
              co_return r.totalCount;
          }),
      load_json_cmd_(
          ui, worker,
          [this](std::string jsonArray) -> aria::async::Task<int> {
              auto n = backend_.load_sources_from_json(jsonArray);
              // 加载后自动刷新列表
              auto r = backend_.get_source_list();
              ui_.post([this, r] { apply_list_(r); });
              co_return n;
          }),
      load_file_cmd_(
          ui, worker,
          [this](std::string filePath) -> aria::async::Task<int> {
              auto n = backend_.load_sources_from_file(filePath);
              auto r = backend_.get_source_list();
              ui_.post([this, r] { apply_list_(r); });
              co_return n;
          }),
      clear_cmd_(
          ui, worker,
          [this]() -> aria::async::Task<int> {
              auto n = backend_.clear_all_sources();
              auto r = backend_.get_source_list();
              ui_.post([this, r] { apply_list_(r); });
              co_return n;
          }),
      export_cmd_(
          ui, worker,
          [this]() -> aria::async::Task<std::string> {
              auto json = backend_.export_good_sources();
              ui_.post([this, json] { exported_json.set(json); });
              co_return json;
          }),
      cancel_token_(std::make_shared<openread::CancelToken>()) {}

aria::Property<std::string>& SourceViewModel::last_error_message() {
    return refresh_cmd_.last_error_message;
}

void SourceViewModel::refresh() {
    refresh_cmd_.execute();
}

void SourceViewModel::load_from_json(const std::string& jsonArray) {
    load_json_cmd_.execute(jsonArray);
}

void SourceViewModel::load_from_file(const std::string& filePath) {
    load_file_cmd_.execute(filePath);
}

void SourceViewModel::validate(const std::string& testQuery,
                               int timeoutMs, int concurrency) {
    if (is_validating.get()) return;
    is_validating.set(true);
    validation_done_count.set(0);
    validation_total_count.set(total_count.get());

    cancel_token_->reset();

    // 验证是回调驱动，不走 AsyncCommand（引擎自身管理线程池）。
    // 我们在 perSource 回调中递增进度，在 done 回调中刷新列表。
    backend_.validate_sources(
        testQuery, timeoutMs, concurrency,
        /* perSource */
        [this](size_t /*idx*/, const std::string& /*name*/,
               SourceValidity /*validity*/, int /*latency*/) {
            ui_.post([this] {
                int d = validation_done_count.get() + 1;
                validation_done_count.set(d);
            });
        },
        /* done */
        [this](int /*validCount*/, int /*invalidCount*/) {
            ui_.post([this] {
                is_validating.set(false);
                // 验证完毕刷新列表
                refresh();
            });
        },
        cancel_token_);
}

void SourceViewModel::cancel_validate() {
    cancel_token_->cancel();
}

void SourceViewModel::clear_all() {
    clear_cmd_.execute();
}

void SourceViewModel::export_good() {
    export_cmd_.execute();
}

void SourceViewModel::apply_list_(const openread::SourceListResult& result) {
    sources.clear();
    for (const auto& s : result.sources) {
        sources.emplace_back(s);
    }
    stat_excellent.set(result.stats.excellent);
    stat_good.set(result.stats.good);
    stat_poor.set(result.stats.poor);
    stat_invalid.set(result.stats.invalid);
    stat_unknown.set(result.stats.unknown);
    valid_count.set(result.validCount);
    total_count.set(result.totalCount);
}

}  // namespace openread::vm

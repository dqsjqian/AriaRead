/// @file search_view_model.cpp
/// @brief SearchViewModel 的实现。

#include "ariaread/vm/search_view_model.h"

#include "aria/async/task.hpp"

#include <atomic>

namespace ariaread::vm {

SearchViewModel::SearchViewModel(aria::async::IExecutor& ui,
                                 aria::async::IExecutor& worker,
                                 StreamSearchFn search)
    : ui_(ui),
      search_(std::move(search)),
      search_command_(
          ui, worker,
          [this](std::string kw) -> aria::async::Task<int> {
              // 1) 搜索开始：清空旧结果、归零计数（marshal 回 ui 线程）。
              ui_.post([this] {
                  results.clear();
                  found_count.set(0);
              });

              // 2) 在 worker 线程驱动流式搜索。每个源返回时把该批结果
              //    post 回 ui 线程 append（保证 list 信号在图线程触发）。
              std::atomic<int> total{0};
              if (search_) {
                  SearchEmit emit = [this, &total](
                      const std::vector<ariaread::Book>& books) {
                      if (books.empty()) return;
                      total.fetch_add(static_cast<int>(books.size()),
                                      std::memory_order_relaxed);
                      ui_.post([this, books] { append_results_(books); });
                  };
                  search_(kw, emit, cancel_flag_);
              }

              co_return total.load(std::memory_order_relaxed);
          }) {}

aria::Property<bool>& SearchViewModel::is_searching() {
    return search_command_.is_executing;
}

aria::Property<std::string>& SearchViewModel::last_error_message() {
    return search_command_.last_error_message;
}

aria::Property<std::optional<int>>& SearchViewModel::last_total() {
    return search_command_.last_result;
}

void SearchViewModel::search() {
    start_(keyword.get());
}

void SearchViewModel::search_with(const std::string& kw) {
    start_(kw);
}

void SearchViewModel::cancel() {
    cancel_flag_.store(true, std::memory_order_release);
}

void SearchViewModel::start_(const std::string& kw) {
    cancel_flag_.store(false, std::memory_order_release);
    search_command_.execute(kw);
}

void SearchViewModel::append_results_(const std::vector<ariaread::Book>& books) {
    for (const auto& b : books) {
        results.emplace_back(b);
    }
    found_count.set(static_cast<int>(results.size()));
}

}  // namespace ariaread::vm

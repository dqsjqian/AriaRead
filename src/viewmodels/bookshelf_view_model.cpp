/// @file bookshelf_view_model.cpp
/// @brief BookshelfViewModel 的实现。

#include "ariaread/vm/bookshelf_view_model.h"

#include "aria/async/task.hpp"

namespace ariaread::vm {

BookshelfViewModel::BookshelfViewModel(aria::async::IExecutor& ui,
                                       aria::async::IExecutor& worker,
                                       BookshelfBackend& backend)
    : ui_(ui),
      backend_(backend),
      // load：worker 加载 → 切回 ui 写 list。返回本数。
      load_command_(
          ui, worker,
          [this]() -> aria::async::Task<int> {
              auto items = backend_.load();
              auto n = static_cast<int>(items.size());
              ui_.post([this, items] { reload_into_list_(items); });
              co_return n;
          }),
      // remove：worker 执行移除 + 重新加载 → 切回 ui 写 list。返回是否成功。
      remove_command_(
          ui, worker,
          [this](std::string bookUrl) -> aria::async::Task<bool> {
              bool ok = backend_.remove(bookUrl);
              auto items = backend_.load();
              ui_.post([this, items] { reload_into_list_(items); });
              co_return ok;
          }),
      // add：worker 执行加入 + 重新加载 → 切回 ui 写 list。返回新 id。
      add_command_(
          ui, worker,
          [this](ariaread::BookshelfItem item) -> aria::async::Task<int64_t> {
              int64_t id = backend_.add(item);
              auto items = backend_.load();
              ui_.post([this, items] { reload_into_list_(items); });
              co_return id;
          }) {}

aria::Property<bool>& BookshelfViewModel::is_loading() {
    // 任一命令在执行都算"加载中"——这里以 load 命令为主指示。
    return load_command_.is_executing;
}

aria::Property<std::string>& BookshelfViewModel::last_error_message() {
    return load_command_.last_error_message;
}

void BookshelfViewModel::refresh() {
    load_command_.execute();
}

void BookshelfViewModel::remove(const std::string& bookUrl) {
    remove_command_.execute(bookUrl);
}

void BookshelfViewModel::add(const ariaread::BookshelfItem& item) {
    add_command_.execute(item);
}

void BookshelfViewModel::reload_into_list_(
    const std::vector<ariaread::BookshelfDetail>& items) {
    books.clear();
    for (const auto& d : items) {
        books.emplace_back(d);
    }
    count.set(static_cast<int>(books.size()));
}

}  // namespace ariaread::vm

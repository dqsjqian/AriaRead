/// @file reader_view_model.cpp
/// @brief ReaderViewModel 的实现。

#include "ariaread/vm/reader_view_model.h"

#include "aria/async/task.hpp"

#include <memory>

namespace ariaread::vm {

ReaderViewModel::ReaderViewModel(aria::async::IExecutor& ui,
                                 aria::async::IExecutor& worker,
                                 ReaderBackend& backend)
    : ui_(ui),
      backend_(backend),
      // 加载目录：worker 拉目录 → ui 写 list。
      catalog_command_(
          ui, worker,
          [this]() -> aria::async::Task<int> {
              auto bu = book_url.get();
              auto su = source_url.get();
              auto chs = backend_.load_catalog(bu, su);
              auto n = static_cast<int>(chs.size());
              ui_.post([this, chs] { apply_catalog_(chs); });
              co_return n;
          }),
      // 打开章节：参数 index。worker 取正文 → ui 写 content。
      content_command_(
          ui, worker,
          [this](int index) -> aria::async::Task<int> {
              // 章节元信息（url/title）需在 worker 前从 list 取出。
              // ObservableList 内部带锁，at() 跨线程读安全；但为稳妥，
              // 调用方（open_chapter）已确保 index 合法。
              std::string chapterUrl, title;
              int total = 0;
              {
                  total = static_cast<int>(chapters.size());
                  if (index >= 0 && index < total) {
                      auto ch = chapters.at(static_cast<std::size_t>(index));
                      chapterUrl = ch->url;
                      title = ch->title;
                  }
              }
              if (chapterUrl.empty()) co_return 0;

              auto bu = book_url.get();
              auto su = source_url.get();
              auto text = backend_.load_content(bu, chapterUrl, index, su);
              double percent = (total > 0)
                  ? static_cast<double>(index + 1) / static_cast<double>(total)
                  : 0.0;

              ui_.post([this, index, title, text, percent] {
                  apply_content_(index, title, text, percent);
              });
              co_return static_cast<int>(text.size());
          }),
      // 保存进度：worker 写 DB。
      save_command_(
          ui, worker,
          [this]() -> aria::async::Task<void> {
              ariaread::ReadProgress p;
              p.bookUrl = book_url.get();
              p.chapterIndex = current_index.get();
              p.chapterTitle = current_title.get();
              p.readPercent = read_percent.get();
              backend_.save_progress(p);
              co_return;
          }) {}

aria::Property<bool>& ReaderViewModel::is_busy() {
    return catalog_command_.is_executing;
}

aria::Property<std::string>& ReaderViewModel::last_error_message() {
    return catalog_command_.last_error_message;
}

void ReaderViewModel::load_catalog() {
    catalog_command_.execute();
}

void ReaderViewModel::open_chapter(int index) {
    content_command_.execute(index);
}

void ReaderViewModel::save_progress() {
    save_command_.execute();
}

void ReaderViewModel::apply_catalog_(const std::vector<ariaread::Chapter>& chs) {
    chapters.clear();
    for (const auto& c : chs) {
        chapters.emplace_back(c);
    }
    chapter_count.set(static_cast<int>(chapters.size()));
}

void ReaderViewModel::apply_content_(int index, const std::string& title,
                                     const std::string& text, double percent) {
    current_index.set(index);
    current_title.set(title);
    content.set(text);
    read_percent.set(percent);
}

}  // namespace ariaread::vm

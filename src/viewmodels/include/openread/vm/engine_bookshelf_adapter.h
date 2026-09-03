#pragma once
/// @file engine_bookshelf_adapter.h
/// @brief 把 OpenRead 引擎的书架 API 适配成 BookshelfBackend。
///        引擎 ↔ 书架 ViewModel 之间唯一的桥接点（engine.h 只在 .cpp 出现）。

#include "openread/vm/bookshelf_view_model.h"

namespace openread {
class BookSourceEngine;  // 前向声明
}

namespace openread::vm {

/// 用真实引擎实现的书架后端。
class EngineBookshelfBackend : public BookshelfBackend {
public:
    explicit EngineBookshelfBackend(openread::BookSourceEngine& engine);

    std::vector<openread::BookshelfDetail> load() override;
    bool remove(const std::string& bookUrl) override;
    int64_t add(const openread::BookshelfItem& item) override;

private:
    openread::BookSourceEngine& engine_;
};

}  // namespace openread::vm

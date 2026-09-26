/// @file engine_bookshelf_adapter.cpp
/// @brief EngineBookshelfBackend 的实现（唯一 include engine.h 的书架 TU）。

#include "ariaread/vm/engine_bookshelf_adapter.h"

#include "ariaread/engine.h"

namespace ariaread::vm {

EngineBookshelfBackend::EngineBookshelfBackend(ariaread::BookSourceEngine& engine)
    : engine_(engine) {}

std::vector<ariaread::BookshelfDetail> EngineBookshelfBackend::load() {
    return engine_.getBookshelfWithDetails();
}

bool EngineBookshelfBackend::remove(const std::string& bookUrl) {
    return engine_.removeFromBookshelf(bookUrl);
}

int64_t EngineBookshelfBackend::add(const ariaread::BookshelfItem& item) {
    return engine_.addToBookshelf(item);
}

}  // namespace ariaread::vm

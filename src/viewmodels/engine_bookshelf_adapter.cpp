/// @file engine_bookshelf_adapter.cpp
/// @brief EngineBookshelfBackend 的实现（唯一 include engine.h 的书架 TU）。

#include "openread/vm/engine_bookshelf_adapter.h"

#include "openread/engine.h"

namespace openread::vm {

EngineBookshelfBackend::EngineBookshelfBackend(openread::BookSourceEngine& engine)
    : engine_(engine) {}

std::vector<openread::BookshelfDetail> EngineBookshelfBackend::load() {
    return engine_.getBookshelfWithDetails();
}

bool EngineBookshelfBackend::remove(const std::string& bookUrl) {
    return engine_.removeFromBookshelf(bookUrl);
}

int64_t EngineBookshelfBackend::add(const openread::BookshelfItem& item) {
    return engine_.addToBookshelf(item);
}

}  // namespace openread::vm

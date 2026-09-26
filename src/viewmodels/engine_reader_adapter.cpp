/// @file engine_reader_adapter.cpp
/// @brief EngineReaderBackend 的实现（唯一 include engine.h 的阅读器 TU）。

#include "ariaread/vm/engine_reader_adapter.h"

#include "ariaread/engine.h"

namespace ariaread::vm {

EngineReaderBackend::EngineReaderBackend(ariaread::BookSourceEngine& engine)
    : engine_(engine) {}

std::vector<ariaread::Chapter> EngineReaderBackend::load_catalog(
    const std::string& bookUrl, const std::string& sourceUrl) {
    // 优先缓存，未命中则网络请求并自动缓存。
    return engine_.getCatalogWithCache(bookUrl, sourceUrl);
}

std::string EngineReaderBackend::load_content(
    const std::string& bookUrl, const std::string& chapterUrl,
    int chapterIndex, const std::string& sourceUrl) {
    return engine_.getContentWithCache(chapterUrl, bookUrl, chapterIndex,
                                       sourceUrl);
}

void EngineReaderBackend::save_progress(const ariaread::ReadProgress& progress) {
    engine_.saveReadProgress(progress);
}

ariaread::ReadProgress EngineReaderBackend::load_progress(
    const std::string& bookUrl) {
    return engine_.getReadProgress(bookUrl);
}

}  // namespace ariaread::vm

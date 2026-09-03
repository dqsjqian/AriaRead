/// @file engine_reader_adapter.cpp
/// @brief EngineReaderBackend 的实现（唯一 include engine.h 的阅读器 TU）。

#include "openread/vm/engine_reader_adapter.h"

#include "openread/engine.h"

namespace openread::vm {

EngineReaderBackend::EngineReaderBackend(openread::BookSourceEngine& engine)
    : engine_(engine) {}

std::vector<openread::Chapter> EngineReaderBackend::load_catalog(
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

void EngineReaderBackend::save_progress(const openread::ReadProgress& progress) {
    engine_.saveReadProgress(progress);
}

openread::ReadProgress EngineReaderBackend::load_progress(
    const std::string& bookUrl) {
    return engine_.getReadProgress(bookUrl);
}

}  // namespace openread::vm

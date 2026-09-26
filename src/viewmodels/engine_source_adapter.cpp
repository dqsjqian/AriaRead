/// @file engine_source_adapter.cpp
/// @brief EngineSourceBackend 的实现（唯一 include engine.h 的书源管理 TU）。

#include "ariaread/vm/engine_source_adapter.h"

#include "ariaread/engine.h"

namespace ariaread::vm {

EngineSourceBackend::EngineSourceBackend(ariaread::BookSourceEngine& engine)
    : engine_(engine) {}

ariaread::SourceListResult EngineSourceBackend::get_source_list() {
    return engine_.getSourceList();
}

int EngineSourceBackend::load_sources_from_json(const std::string& jsonArray) {
    return engine_.loadSources(jsonArray);
}

int EngineSourceBackend::load_sources_from_file(const std::string& filePath) {
    return engine_.loadSourcesFromFile(filePath);
}

void EngineSourceBackend::validate_sources(
    const std::string& testQuery, int timeoutMs, int concurrency,
    std::function<void(size_t, const std::string&, SourceValidity, int)> perSource,
    std::function<void(int, int)> done,
    std::shared_ptr<ariaread::CancelToken> cancelToken) {
    // 桥接：将 SourceValidity 映射到引擎的 ConcurrentValidateCallback 签名。
    engine_.validateSourcesConcurrent(
        testQuery, timeoutMs, concurrency,
        [perSource](size_t idx, const std::string& name,
                    SourceValidity validity, int latencyMs,
                    const std::string& /*detail*/) {
            perSource(idx, name, validity, latencyMs);
        },
        [done](int validCount, int invalidCount, int /*removedCount*/) {
            done(validCount, invalidCount);
        },
        cancelToken);
}

int EngineSourceBackend::clear_all_sources() {
    return engine_.clearAllSources();
}

std::string EngineSourceBackend::export_good_sources() {
    return engine_.exportGoodSources();
}

}  // namespace ariaread::vm

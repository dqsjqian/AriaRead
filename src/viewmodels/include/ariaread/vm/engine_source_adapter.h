#pragma once
/// @file engine_source_adapter.h
/// @brief 把 AriaRead 引擎的书源管理 API 适配成 SourceBackend。

#include "ariaread/vm/source_view_model.h"

namespace ariaread {
class BookSourceEngine;
}

namespace ariaread::vm {

/// 用真实引擎实现的书源管理后端。
class EngineSourceBackend : public SourceBackend {
public:
    explicit EngineSourceBackend(ariaread::BookSourceEngine& engine);

    ariaread::SourceListResult get_source_list() override;
    int load_sources_from_json(const std::string& jsonArray) override;
    int load_sources_from_file(const std::string& filePath) override;
    void validate_sources(
        const std::string& testQuery, int timeoutMs, int concurrency,
        std::function<void(size_t, const std::string&, SourceValidity, int)> perSource,
        std::function<void(int, int)> done,
        std::shared_ptr<ariaread::CancelToken> cancelToken) override;
    int clear_all_sources() override;
    std::string export_good_sources() override;

private:
    ariaread::BookSourceEngine& engine_;
};

}  // namespace ariaread::vm

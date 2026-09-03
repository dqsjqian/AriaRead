#pragma once
/// @file engine_source_adapter.h
/// @brief 把 OpenRead 引擎的书源管理 API 适配成 SourceBackend。

#include "openread/vm/source_view_model.h"

namespace openread {
class BookSourceEngine;
}

namespace openread::vm {

/// 用真实引擎实现的书源管理后端。
class EngineSourceBackend : public SourceBackend {
public:
    explicit EngineSourceBackend(openread::BookSourceEngine& engine);

    openread::SourceListResult get_source_list() override;
    int load_sources_from_json(const std::string& jsonArray) override;
    int load_sources_from_file(const std::string& filePath) override;
    void validate_sources(
        const std::string& testQuery, int timeoutMs, int concurrency,
        std::function<void(size_t, const std::string&, SourceValidity, int)> perSource,
        std::function<void(int, int)> done,
        std::shared_ptr<openread::CancelToken> cancelToken) override;
    int clear_all_sources() override;
    std::string export_good_sources() override;

private:
    openread::BookSourceEngine& engine_;
};

}  // namespace openread::vm

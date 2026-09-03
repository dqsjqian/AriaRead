#pragma once
/// @file engine_reader_adapter.h
/// @brief 把 OpenRead 引擎的目录/正文/进度 API 适配成 ReaderBackend。

#include "openread/vm/reader_view_model.h"

namespace openread {
class BookSourceEngine;
}

namespace openread::vm {

/// 用真实引擎实现的阅读器后端。
class EngineReaderBackend : public ReaderBackend {
public:
    explicit EngineReaderBackend(openread::BookSourceEngine& engine);

    std::vector<openread::Chapter> load_catalog(
        const std::string& bookUrl, const std::string& sourceUrl) override;
    std::string load_content(
        const std::string& bookUrl, const std::string& chapterUrl,
        int chapterIndex, const std::string& sourceUrl) override;
    void save_progress(const openread::ReadProgress& progress) override;
    openread::ReadProgress load_progress(const std::string& bookUrl) override;

private:
    openread::BookSourceEngine& engine_;
};

}  // namespace openread::vm

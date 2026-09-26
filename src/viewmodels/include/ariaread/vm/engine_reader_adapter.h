#pragma once
/// @file engine_reader_adapter.h
/// @brief 把 AriaRead 引擎的目录/正文/进度 API 适配成 ReaderBackend。

#include "ariaread/vm/reader_view_model.h"

namespace ariaread {
class BookSourceEngine;
}

namespace ariaread::vm {

/// 用真实引擎实现的阅读器后端。
class EngineReaderBackend : public ReaderBackend {
public:
    explicit EngineReaderBackend(ariaread::BookSourceEngine& engine);

    std::vector<ariaread::Chapter> load_catalog(
        const std::string& bookUrl, const std::string& sourceUrl) override;
    std::string load_content(
        const std::string& bookUrl, const std::string& chapterUrl,
        int chapterIndex, const std::string& sourceUrl) override;
    void save_progress(const ariaread::ReadProgress& progress) override;
    ariaread::ReadProgress load_progress(const std::string& bookUrl) override;

private:
    ariaread::BookSourceEngine& engine_;
};

}  // namespace ariaread::vm

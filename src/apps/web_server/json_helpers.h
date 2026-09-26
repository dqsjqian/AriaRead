/// @file json_helpers.h
/// @brief JSON 序列化辅助函数（Book / Chapter / Source / Bookshelf 等）。

#pragma once

#include "ariaread/engine.h"
#include <nlohmann/json.hpp>
#include <string>

namespace ariaread::web {

using json = nlohmann::json;

/// Book → JSON（基础字段）
json book_to_json(const ariaread::Book& b);

/// Book → JSON（带回源信息：sourceName / sourceIndex / sourceUrl）
json book_to_json_with_source(const ariaread::Book& b,
                              size_t sourceIndex,
                              const std::string& sourceName,
                              const std::string& sourceUrl);

/// Chapter → JSON
json chapter_to_json(const ariaread::Chapter& c);

/// SourceSummary → JSON
json source_summary_to_json(const ariaread::SourceSummary& s);

/// BookshelfDetail → JSON（含阅读进度）
json bookshelf_detail_to_json(const ariaread::BookshelfDetail& d);

/// HTTP GET 下载（libcurl，用于 /api/sources/url）
std::string httpDownload(const std::string& url, int timeoutSec = 30);

/// 安全 JSON 序列化：非法 UTF-8 字节替换为 U+FFFD，不会抛异常。
/// 必须用于所有 HTTP 响应的 dump 路径，防止书源返回的 GBK/乱码导致 type_error.316 崩溃。
inline std::string safeDump(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace ariaread::web

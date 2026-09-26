#pragma once
/// @file source_parser.h
/// @brief 书源规则解析器 —— 解析主流格式的书源 JSON

#include "ariaread/types.h"
#include <string>
#include <vector>

namespace ariaread {

// ──────────────────────────────────────────────
// 书源解析器
// ──────────────────────────────────────────────
class SourceParser {
public:
    /// 从 JSON 字符串解析单个书源
    static BookSource parse(const std::string& jsonStr);

    /// 从 JSON 数组解析多个书源
    static std::vector<BookSource> parseArray(const std::string& jsonStr);

    /// 从文件加载书源
    static std::vector<BookSource> loadFromFile(const std::string& filePath);

    /// 验证书源是否有效
    static bool validate(const BookSource& source);

    /// 验证书源并返回错误信息
    static std::string validateDetail(const BookSource& source);

    /// 序列化为 JSON
    static std::string serialize(const BookSource& source);

    /// 序列化多个书源
    static std::string serializeArray(const std::vector<BookSource>& sources);
};

} // namespace ariaread

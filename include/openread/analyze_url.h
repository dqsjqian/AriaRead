#pragma once
/// @file analyze_url.h
/// @brief searchUrl 解析器
/// 三步解析法：
///   1. analyzeJs()   — 执行 @js: / <js></js> 规则
///   2. replaceKeyPageJs() — 替换 {{key}}, {{page}}, {{内嵌JS}}
///   3. analyzeUrl()  — 分离 URL 和 options, 拼接 baseUrl

#include <string>
#include <map>

namespace openread {

/// 解析后的 URL 请求信息
struct AnalyzedUrl {
    std::string url;            ///< 最终请求 URL（绝对路径）
    std::string method = "GET"; ///< 请求方法
    std::string body;           ///< POST body
    std::string charset;        ///< 编码（gbk/utf-8）
    std::map<std::string, std::string> headers; ///< 请求头
    std::string ruleUrl;        ///< 原始规则 URL（调试用）
    std::string baseUrl;        ///< 基础 URL（调试用）
};

class JsRuntime;  // 前向声明

/// searchUrl 解析器
class AnalyzeUrl {
public:
    /// 构造解析器
    /// @param ruleUrl  原始 searchUrl 规则字符串
    /// @param baseUrl  书源的 bookSourceUrl，用于拼接相对路径
    /// @param key      搜索关键词
    /// @param page     页码（默认 1）
    /// @param js       JS 运行时（可为 nullptr，无 JS 规则时不需要）
    AnalyzeUrl(const std::string& ruleUrl,
               const std::string& baseUrl = "",
               const std::string& key = "",
               int page = 1,
               JsRuntime* js = nullptr);

    /// 获取解析结果
    const AnalyzedUrl& result() const { return result_; }

    /// 辅助：URL 拼接（相对路径 → 绝对路径），对齐 legado URL(base, rel)。
    /// 公开以便单测与复用（纯函数，无副作用）。
    static std::string getAbsoluteURL(const std::string& baseUrl,
                                       const std::string& relativePath);

private:
    /// 阶段1：执行 @js: / <js></js> 规则
    void analyzeJs();

    /// 阶段2：替换 {{key}}, {{page}}, {{内嵌JS}}
    void replaceKeyPageJs();

    /// 阶段3：分离 URL 和 options, 拼接 baseUrl
    void analyzeUrl();

    /// 辅助：URL 编码
    static std::string urlEncode(const std::string& value, const std::string& charset = "");

    /// 辅助：提取 baseUrl（去掉路径部分，只保留 scheme://host）
    static std::string extractBaseUrl(const std::string& url);

private:
    std::string ruleUrl_;     ///< 当前处理中的规则字符串
    std::string baseUrl_;     ///< 书源 baseUrl
    std::string key_;         ///< 搜索关键词
    int page_;                ///< 页码
    JsRuntime* js_;           ///< JS 运行时
    AnalyzedUrl result_;      ///< 解析结果
};

} // namespace openread

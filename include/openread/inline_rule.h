#pragma once
/// @file inline_rule.h
/// @brief 内嵌规则替换 —— {{js}}、@get:{key}、$1/$2 正则捕获组

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace openread {

/// 内嵌规则替换器
class InlineRuleReplacer {
public:
    /// 替换内嵌规则
    /// @param rule 规则字符串
    /// @param jsEvaluator JS 执行器（用于 {{js}} 替换）
    /// @param variableProvider 变量提供器（用于 @get:{key} 替换）
    /// @param regexGroups 正则捕获组（用于 $1/$2 替换）
    /// @return 替换后的字符串
    static std::string replace(
        const std::string& rule,
        const std::function<std::string(const std::string&)>& jsEvaluator,
        const std::function<std::string(const std::string&)>& variableProvider,
        const std::vector<std::string>& regexGroups = {}
    );

    /// 替换 {{js}} 内嵌规则
    /// @param rule 规则字符串
    /// @param jsEvaluator JS 执行器
    /// @return 替换后的字符串
    static std::string replaceJs(
        const std::string& rule,
        const std::function<std::string(const std::string&)>& jsEvaluator
    );

    /// 替换 @get:{key} 变量
    /// @param rule 规则字符串
    /// @param variableProvider 变量提供器
    /// @return 替换后的字符串
    static std::string replaceVariables(
        const std::string& rule,
        const std::function<std::string(const std::string&)>& variableProvider
    );

    /// 替换 $1/$2 正则捕获组
    /// @param rule 规则字符串
    /// @param regexGroups 正则捕获组
    /// @return 替换后的字符串
    static std::string replaceRegexGroups(
        const std::string& rule,
        const std::vector<std::string>& regexGroups
    );

private:
    /// 查找 {{...}} 占位符
    static std::vector<std::pair<size_t, size_t>> findJsPlaceholders(const std::string& rule);

    /// 查找 @get:{...} 占位符
    static std::vector<std::pair<size_t, size_t>> findVariablePlaceholders(const std::string& rule);

    /// 查找 $1/$2 占位符
    static std::vector<std::pair<size_t, size_t>> findRegexGroupPlaceholders(const std::string& rule);
};

} // namespace openread

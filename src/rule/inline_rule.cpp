/// @file inline_rule.cpp
/// @brief 内嵌规则替换实现 —— {{js}}、@get:{key}、$1/$2 正则捕获组

#include "ariaread/inline_rule.h"
#include <regex>
#include <sstream>

namespace ariaread {

std::string InlineRuleReplacer::replace(
    const std::string& rule,
    const std::function<std::string(const std::string&)>& jsEvaluator,
    const std::function<std::string(const std::string&)>& variableProvider,
    const std::vector<std::string>& regexGroups
) {
    std::string result = rule;

    // 替换 {{js}} 内嵌规则
    result = replaceJs(result, jsEvaluator);

    // 替换 @get:{key} 变量
    result = replaceVariables(result, variableProvider);

    // 替换 $1/$2 正则捕获组
    result = replaceRegexGroups(result, regexGroups);

    return result;
}

std::string InlineRuleReplacer::replaceJs(
    const std::string& rule,
    const std::function<std::string(const std::string&)>& jsEvaluator
) {
    auto placeholders = findJsPlaceholders(rule);
    if (placeholders.empty()) {
        return rule;
    }

    std::string result;
    size_t lastPos = 0;

    for (const auto& [start, end] : placeholders) {
        // 添加占位符之前的内容
        result += rule.substr(lastPos, start - lastPos);

        // 提取 JS 代码
        std::string jsCode = rule.substr(start + 2, end - start - 4);

        // 执行 JS 代码
        std::string jsResult = jsEvaluator(jsCode);
        result += jsResult;

        lastPos = end;
    }

    // 添加剩余内容
    result += rule.substr(lastPos);

    return result;
}

std::string InlineRuleReplacer::replaceVariables(
    const std::string& rule,
    const std::function<std::string(const std::string&)>& variableProvider
) {
    auto placeholders = findVariablePlaceholders(rule);
    if (placeholders.empty()) {
        return rule;
    }

    std::string result;
    size_t lastPos = 0;

    for (const auto& [start, end] : placeholders) {
        // 添加占位符之前的内容
        result += rule.substr(lastPos, start - lastPos);

        // 提取变量名
        std::string varName = rule.substr(start + 6, end - start - 7);

        // 获取变量值
        std::string varValue = variableProvider(varName);
        result += varValue;

        lastPos = end;
    }

    // 添加剩余内容
    result += rule.substr(lastPos);

    return result;
}

std::string InlineRuleReplacer::replaceRegexGroups(
    const std::string& rule,
    const std::vector<std::string>& regexGroups
) {
    auto placeholders = findRegexGroupPlaceholders(rule);
    if (placeholders.empty()) {
        return rule;
    }

    std::string result;
    size_t lastPos = 0;

    for (const auto& [start, end] : placeholders) {
        // 添加占位符之前的内容
        result += rule.substr(lastPos, start - lastPos);

        // 提取组号
        std::string groupStr = rule.substr(start + 1, end - start - 1);
        int groupIndex = std::stoi(groupStr);

        // 获取组值
        if (groupIndex >= 0 && groupIndex < static_cast<int>(regexGroups.size())) {
            result += regexGroups[groupIndex];
        } else {
            // 组号超出范围，保留原始占位符
            result += rule.substr(start, end - start);
        }

        lastPos = end;
    }

    // 添加剩余内容
    result += rule.substr(lastPos);

    return result;
}

std::vector<std::pair<size_t, size_t>> InlineRuleReplacer::findJsPlaceholders(const std::string& rule) {
    std::vector<std::pair<size_t, size_t>> placeholders;

    size_t pos = 0;
    while (pos < rule.size()) {
        size_t start = rule.find("{{", pos);
        if (start == std::string::npos) {
            break;
        }

        size_t end = rule.find("}}", start + 2);
        if (end == std::string::npos) {
            break;
        }

        placeholders.emplace_back(start, end + 2);
        pos = end + 2;
    }

    return placeholders;
}

std::vector<std::pair<size_t, size_t>> InlineRuleReplacer::findVariablePlaceholders(const std::string& rule) {
    std::vector<std::pair<size_t, size_t>> placeholders;

    size_t pos = 0;
    while (pos < rule.size()) {
        size_t start = rule.find("@get:{", pos);
        if (start == std::string::npos) {
            break;
        }

        size_t end = rule.find('}', start + 6);
        if (end == std::string::npos) {
            break;
        }

        placeholders.emplace_back(start, end + 1);
        pos = end + 1;
    }

    return placeholders;
}

std::vector<std::pair<size_t, size_t>> InlineRuleReplacer::findRegexGroupPlaceholders(const std::string& rule) {
    std::vector<std::pair<size_t, size_t>> placeholders;

    size_t pos = 0;
    while (pos < rule.size()) {
        if (rule[pos] == '$' && pos + 1 < rule.size() && std::isdigit(rule[pos + 1])) {
            size_t start = pos;
            pos += 2;
            while (pos < rule.size() && std::isdigit(rule[pos])) {
                ++pos;
            }
            placeholders.emplace_back(start, pos);
        } else {
            ++pos;
        }
    }

    return placeholders;
}

} // namespace ariaread

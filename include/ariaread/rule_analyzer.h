#pragma once
/// @file rule_analyzer.h
/// @brief 规则解析器状态机 —— 平衡组、三路分隔符、引号保护
/// 移植自 legado 的 RuleAnalyzer.kt

#include <string>
#include <vector>
#include <functional>

namespace ariaread {

/// 规则解析器 —— 状态机
class RuleAnalyzer {
public:
    /// @param data 被处理的规则字符串
    /// @param code 是否为代码模式（JSON/JS 用 chompCodeBalanced，否则用 chompRuleBalanced）
    explicit RuleAnalyzer(const std::string& data, bool code = false);

    /// 重置 pos 到 0，方便复用
    void reSetPos();

    /// 修剪当前规则之前的 '@' 或空白符
    void trim();

    /// 分割规则（支持三路分隔符 &&、||、%%）
    /// @param split 分隔符列表（如 "&&", "||", "%%"）
    /// @return 分割后的规则列表
    std::vector<std::string> splitRule(const std::vector<std::string>& split);

    /// 替换内嵌规则（如 {$.rule}、{{js}}、@get:{key}）
    /// @param inner 起始标志（如 "{$."）
    /// @param startStep 不属于规则部分的前置字符长度（如 {$. 中 { 不属于规则，startStep=1）
    /// @param endStep 不属于规则部分的后置字符长度
    /// @param fr 查找到内嵌规则时，用于解析的函数
    /// @return 替换后的字符串，如果没有替换则返回空字符串
    std::string innerRule(const std::string& inner,
                          int startStep,
                          int endStep,
                          const std::function<std::string(const std::string&)>& fr);

    /// 替换内嵌规则（使用起始和结束字符串）
    /// @param startStr 起始字符串（如 "{{"）
    /// @param endStr 结束字符串（如 "}}"）
    /// @param fr 查找到内嵌规则时，用于解析的函数
    /// @return 替换后的字符串
    std::string innerRule(const std::string& startStr,
                          const std::string& endStr,
                          const std::function<std::string(const std::string&)>& fr);

    /// 获取当前分割字符串（&&、||、%%）
    const std::string& elementsType() const { return elementsType_; }

    /// 获取规则列表
    const std::vector<std::string>& ruleList() const { return rule_; }

private:
    std::string queue_;           // 被处理字符串
    size_t pos_ = 0;              // 当前处理到的位置
    size_t start_ = 0;            // 当前处理字段的开始
    size_t startX_ = 0;           // 当前规则的开始
    std::vector<std::string> rule_;  // 分割出的规则列表
    int step_ = 0;                // 分割字符的长度
    std::string elementsType_;    // 当前分割字符串
    bool code_;                   // 是否为代码模式

    /// 从剩余字串中拉出一个字符串，直到但不包括匹配序列
    /// @param seq 查找的字符串（区分大小写）
    /// @return 是否找到相应字段
    bool consumeTo(const std::string& seq);

    /// 从剩余字串中拉出一个字符串，直到但不包括匹配序列（匹配参数列表中一项即为匹配）
    /// @param seq 匹配字符串序列
    /// @return 成功返回 true 并设置间隔，失败则直接返回 false
    bool consumeToAny(const std::vector<std::string>& seq);

    /// 从剩余字串中拉出一个字符串，直到但不包括匹配序列（匹配参数列表中一项即为匹配）
    /// @param seq 匹配字符序列
    /// @return 返回匹配位置
    int findToAny(const std::vector<char>& seq);

    /// 拉出一个非内嵌代码平衡组（存在转义文本）
    bool chompCodeBalanced(char open, char close);

    /// 拉出一个规则平衡组（xpath 和 jsoup 中引号内转义字符无效）
    bool chompRuleBalanced(char open, char close);

    /// 平衡组函数（根据模式选择 chompCodeBalanced 或 chompRuleBalanced）
    bool chompBalanced(char open, char close) {
        return code_ ? chompCodeBalanced(open, close) : chompRuleBalanced(open, close);
    }

    /// 二段匹配（elementsType 非空时调用，比首段更快）
    std::vector<std::string> splitRuleNext();

    /// 转义字符
    static constexpr char ESC = '\\';
};

} // namespace ariaread

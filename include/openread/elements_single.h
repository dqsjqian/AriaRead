#pragma once
/// @file elements_single.h
/// @brief ElementsSingle 索引切片系统 —— 移植自 legado
/// 支持负数索引、区间切片、排除模式、反向列表

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <tuple>

namespace openread {

/// ElementsSingle 索引切片系统
class ElementsSingle {
public:
    ElementsSingle() = default;

    /// 解析规则并获取索引集合
    /// @param rule 规则字符串（如 "tag.div.-1:10:2" 或 "tag.div[-1, 3:-2:-10, 2]"）
    /// @param elements 元素列表
    /// @return 筛选后的元素列表
    std::vector<int> parseAndFilter(const std::string& rule, int elementsSize);

    /// 获取前置规则（如 "tag.div"）
    const std::string& getBeforeRule() const { return beforeRule_; }

    /// 获取分割字符（'.' 或 '!'）
    char getSplit() const { return split_; }

private:
    char split_ = '.';
    std::string beforeRule_;
    std::vector<int> indexDefault_;
    std::vector<std::variant<int, std::tuple<int, int, int>>> indexes_;

    /// 解析索引规则
    void findIndexSet(const std::string& rule);

    /// 解析单个索引
    std::optional<int> parseIndex(const std::string& s);

    /// 解析区间
    std::tuple<int, int, int> parseRange(const std::string& s);
};

} // namespace openread

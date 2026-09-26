#pragma once
/// @file css_engine.h
/// @brief 基于 Gumbo DOM 的完整 CSS3 选择器引擎
///
/// 支持：
///   - 类型选择器  div / *  / 自定义标签
///   - class       .foo  / 多 class .a.b
///   - id          #bar
///   - 属性        [attr] [a=v] [a^=v] [a$=v] [a*=v] [a~=v] [a|=v]（可带引号、i 标志）
///   - 组合器      后代(空格) 子代(>) 相邻兄弟(+) 通用兄弟(~)
///   - 分组        a, b, c
///   - 伪类        :first-child :last-child :only-child :nth-child(n) :nth-of-type(n)
///                 :first-of-type :last-of-type :not(...) :empty :contains(text)（jsoup 扩展）
///
/// 设计：把选择器编译成「复合选择器序列 + 组合器」结构，再在 DOM 上求值。

#include <gumbo.h>
#include <string>
#include <vector>
#include <memory>

namespace ariaread {

/// CSS 选择器引擎：编译一次，可对多个根节点反复求值。
class CssEngine {
public:
    /// 编译 CSS 选择器（允许逗号分组）。编译失败时 valid()==false。
    explicit CssEngine(const std::string& selector);

    /// 在以 root 为根（含 root 自身及其后代）的子树中查找所有匹配元素，
    /// 保持文档顺序、去重。
    std::vector<const GumboNode*> select(const GumboNode* root) const;

    /// 在多个根上下文中查找（结果合并、去重、保持顺序）。
    std::vector<const GumboNode*> select(const std::vector<const GumboNode*>& roots) const;

    bool valid() const { return valid_; }

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    bool valid_ = false;
};

} // namespace ariaread

#pragma once
/// @file xpath_engine.h
/// @brief 基于 Gumbo DOM 的 XPath 引擎（覆盖书源常用子集）
///
/// 支持：
///   - 绝对/相对路径   /html/body/div   //div   ./div   .//a
///   - 通配             //*  /div/*
///   - 轴(常用)         child:: descendant:: descendant-or-self:: parent:: self::
///                      following-sibling::  （以及简写 // = descendant-or-self::）
///   - 谓词             [n]（1-based） [last()] [position()<n]
///                      [@attr] [@attr='v'] [@attr="v"]
///                      [contains(@class,'v')] [contains(text(),'v')]
///                      [text()='v'] [last()-1] [@a and @b]（简单 and/or）
///   - 末端取值         /text()  /@href  （以及取节点本身的 outerHtml）
///
/// 返回值语义与 legado 对齐：
///   - 命中 /@attr  → 属性字符串列表
///   - 命中 /text() → 文本列表
///   - 命中元素     → outerHtml 列表（供链式规则继续处理）

#include <gumbo.h>
#include <string>
#include <vector>
#include <memory>

namespace ariaread {

/// XPath 求值结果项：要么是节点，要么是字符串值（属性/文本）
struct XPathResult {
    const GumboNode* node = nullptr; // 非空表示节点
    std::string value;               // node==nullptr 时为属性/文本值
    bool isString = false;
};

class XPathEngine {
public:
    explicit XPathEngine(const std::string& expr);

    /// 在 root 子树上求值，返回结果项列表（保持文档顺序）
    std::vector<XPathResult> evaluate(const GumboNode* root) const;

    /// 便捷：直接拿字符串结果（属性/文本/元素 outerHtml），需传原始 HTML 以还原 outerHtml
    std::vector<std::string> evaluateToStrings(const GumboNode* root,
                                               const std::string& originalHtml) const;

    bool valid() const { return valid_; }

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    bool valid_ = false;
};

} // namespace ariaread

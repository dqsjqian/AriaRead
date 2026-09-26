#pragma once
/// @file selector.h
/// @brief 选择器引擎 —— 支持 JSONPath / XPath / CSS / Regex

#include <string>
#include <vector>
#include <memory>

namespace ariaread {

// ──────────────────────────────────────────────
// 选择器类型枚举
// ──────────────────────────────────────────────
enum class SelectorType {
    JsonPath,       ///< $.data.list
    XPath,          ///< //div[@class='item']
    Css,            ///< .item > h2
    Regex,          ///< @regex:pattern
    JsEval,         ///< @js:code
    DefaultJSoup,   ///< JSoup 语法: class.xxx, tag.xxx, id.xxx, text.xxx
    Unknown
};

// ──────────────────────────────────────────────
// 选择器接口
// ──────────────────────────────────────────────
class Selector {
public:
    virtual ~Selector() = default;

    /// 从内容中提取数据
    virtual std::vector<std::string> select(const std::string& content) = 0;

    /// 选择器类型
    virtual SelectorType type() const = 0;

    /// 原始规则字符串
    virtual std::string rule() const = 0;
};

// ──────────────────────────────────────────────
// JSONPath 选择器
// ──────────────────────────────────────────────
class JsonPathSelector : public Selector {
public:
    explicit JsonPathSelector(const std::string& path);
    std::vector<std::string> select(const std::string& content) override;
    SelectorType type() const override { return SelectorType::JsonPath; }
    std::string rule() const override { return path_; }

private:
    std::string path_;
    std::vector<std::string> tokenize(const std::string& path);
    std::vector<std::string> selectSingle(const std::string& content, const std::string& rule);
};

// ──────────────────────────────────────────────
// 正则选择器
// ──────────────────────────────────────────────
class RegexSelector : public Selector {
public:
    explicit RegexSelector(const std::string& pattern);
    ~RegexSelector() override;
    std::vector<std::string> select(const std::string& content) override;
    SelectorType type() const override { return SelectorType::Regex; }
    std::string rule() const override { return pattern_; }

    RegexSelector(const RegexSelector&) = delete;
    RegexSelector& operator=(const RegexSelector&) = delete;
    RegexSelector(RegexSelector&&) noexcept = default;
    RegexSelector& operator=(RegexSelector&&) noexcept = default;

private:
    std::string pattern_;
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

// ──────────────────────────────────────────────
// CSS 选择器（依赖 Gumbo，Phase 2 实现）
// ──────────────────────────────────────────────
class CssSelector : public Selector {
public:
    explicit CssSelector(const std::string& selector);
    std::vector<std::string> select(const std::string& content) override;
    SelectorType type() const override { return SelectorType::Css; }
    std::string rule() const override { return selector_; }

private:
    std::string selector_;
    std::vector<std::string> selectSingle(const std::string& content, const std::string& rule);
};

// ──────────────────────────────────────────────
// XPath 选择器（依赖 pugixml，Phase 2 实现）
// ──────────────────────────────────────────────
class XPathSelector : public Selector {
public:
    explicit XPathSelector(const std::string& path);
    std::vector<std::string> select(const std::string& content) override;
    SelectorType type() const override { return SelectorType::XPath; }
    std::string rule() const override { return path_; }

private:
    std::string path_;
    std::vector<std::string> selectSingle(const std::string& content, const std::string& rule);
};

// ──────────────────────────────────────────────
// JS 执行选择器（依赖 QuickJS）
// ──────────────────────────────────────────────
class JsEvalSelector : public Selector {
public:
    explicit JsEvalSelector(const std::string& code);
    std::vector<std::string> select(const std::string& content) override;
    SelectorType type() const override { return SelectorType::JsEval; }
    std::string rule() const override { return code_; }

    /// 设置 JS 执行上下文（由引擎注入）
    void setJsContext(void* ctx) { jsCtx_ = ctx; }

private:
    std::string code_;
    void* jsCtx_ = nullptr;
    std::vector<std::string> selectSingle(const std::string& content, const std::string& rule);
};

// ──────────────────────────────────────────────
// Default JSoup 选择器
// 支持书源中的 JSoup 语法：
//   class.xxx     → 按 CSS class 查找
//   tag.xxx       → 按标签名查找
//   id.xxx        → 按 id 查找
//   text.xxx      → 按文本内容查找
//   @text         → 获取文本内容
//   @href         → 获取 href 属性
//   @src          → 获取 src 属性
//   @alt          → 获取 alt 属性
//   @attr:xxx     → 获取指定属性
//   .N            → 取第 N 个元素（0-based）
//   @children     → 获取子元素
//   @li / @div 等 → 获取指定标签的子元素
// ──────────────────────────────────────────────
class DefaultJSoupSelector : public Selector {
public:
    explicit DefaultJSoupSelector(const std::string& rule);
    ~DefaultJSoupSelector() override;
    std::vector<std::string> select(const std::string& content) override;
    SelectorType type() const override { return SelectorType::DefaultJSoup; }
    std::string rule() const override { return rule_; }

private:
    std::string rule_;
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

// ──────────────────────────────────────────────
// 选择器工厂
// ──────────────────────────────────────────────
class SelectorFactory {
public:
    /// 根据规则前缀自动创建对应选择器
    /// - $ 开头 → JSONPath
    /// - @css: → CSS
    /// - @xpath: → XPath
    /// - @regex: → Regex
    /// - @js: → JS 执行
    /// - || 表示 or 运算（第一个非空结果）
    static std::unique_ptr<Selector> create(const std::string& rule);

    /// 解析 || 组合规则（依次尝试，返回第一个非空结果）
    static std::vector<std::unique_ptr<Selector>> createOrChain(const std::string& rule);

    /// 检测选择器类型
    static SelectorType detectType(const std::string& rule);
};

} // namespace ariaread

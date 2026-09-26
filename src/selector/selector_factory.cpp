/// @file selector_factory.cpp
/// @brief 选择器工厂：类型检测、创建、|| 链式组合

#include "ariaread/selector.h"
#include <string>
#include <algorithm>

namespace ariaread {

// ──────────────────────────────────────────────
// 选择器工厂
// ──────────────────────────────────────────────
SelectorType SelectorFactory::detectType(const std::string& rule) {
    if (rule.empty()) return SelectorType::Unknown;

    if (rule[0] == '$') return SelectorType::JsonPath;

    if (rule.rfind("@CSS:", 0) == 0 || rule.rfind("@css:", 0) == 0) return SelectorType::Css;
    if (rule.rfind("@XPath:", 0) == 0 || rule.rfind("@xpath:", 0) == 0) return SelectorType::XPath;
    if (rule.rfind("@regex:", 0) == 0) return SelectorType::Regex;
    if (rule.rfind("@js:", 0) == 0) return SelectorType::JsEval;

    if (rule.rfind("//", 0) == 0) return SelectorType::XPath;

    if (rule.rfind("class.", 0) == 0) return SelectorType::DefaultJSoup;
    if (rule.rfind("tag.", 0) == 0) return SelectorType::DefaultJSoup;
    if (rule.rfind("id.", 0) == 0) return SelectorType::DefaultJSoup;
    if (rule.rfind("text.", 0) == 0) return SelectorType::DefaultJSoup;

    // 裸的 JSoup 提取关键词（legado 支持单独作为最后一段规则使用）
    // 例如 ruleToc.chapterName = "text"、chapterUrl = "href"
    if (rule == "text" || rule == "textNodes" || rule == "ownText" ||
        rule == "html" || rule == "all" || rule == "href" || rule == "src" ||
        rule == "alt" || rule == "content" || rule == "value" ||
        rule == "children") {
        return SelectorType::DefaultJSoup;
    }
    // attr:xxx / attr.xxx 取任意属性
    if (rule.rfind("attr:", 0) == 0 || rule.rfind("attr.", 0) == 0) {
        return SelectorType::DefaultJSoup;
    }

    // Legado 裸 JSoup 语法：以 '.'(class) 或 '#'(id) 开头
    // 例如 ruleArticles = ".list@li"、ruleImage = ".cover@src"
    if (rule[0] == '.' || rule[0] == '#') {
        return SelectorType::DefaultJSoup;
    }

    // 含 '@' 的规则一律视为 JSoup 选择器@取值 形式
    // （覆盖裸属性名 img@data-original、子标签 .list@li、a@href 等所有情形）。
    // 走到这里已排除 @css/@xpath/@regex/@js/@CSS 等特殊前缀，故剩余 '@' 必为 JSoup 取值符。
    if (rule.find('@') != std::string::npos) {
        return SelectorType::DefaultJSoup;
    }

    // 不含 '@' 的裸 HTML 标签名（如 ruleArticles = "li"、ruleContent = "div"）。
    {
        std::string lower = rule;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        // 去掉可能的索引尾巴（li.0 / div[0]）再判断
        size_t cut = lower.find_first_of(".[!");
        std::string head = (cut == std::string::npos) ? lower : lower.substr(0, cut);
        static const char* tags[] = {
            "a","article","aside","b","blockquote","body","caption","cite","code",
            "dd","details","div","dl","dt","em","figure","footer","form","h1","h2",
            "h3","h4","h5","h6","header","i","iframe","img","input","label","li",
            "main","mark","nav","ol","option","p","pre","q","section","select","small",
            "span","strong","sub","summary","sup","table","tbody","td","th","thead",
            "time","title","tr","u","ul","video"
        };
        for (const char* t : tags) {
            if (head == t) return SelectorType::DefaultJSoup;
        }
    }

    return SelectorType::Unknown;
}

std::unique_ptr<Selector> SelectorFactory::create(const std::string& rule) {
    if (rule.empty()) return nullptr;

    auto type = detectType(rule);

    switch (type) {
    case SelectorType::JsonPath:
        return std::make_unique<JsonPathSelector>(rule);
    case SelectorType::Regex:
        return std::make_unique<RegexSelector>(rule.substr(7));
    case SelectorType::Css:
        return std::make_unique<CssSelector>(rule.substr(5));
    case SelectorType::XPath:
        if (rule.rfind("@", 0) == 0) {
            return std::make_unique<XPathSelector>(rule.substr(7));
        }
        return std::make_unique<XPathSelector>(rule);
    case SelectorType::JsEval:
        return std::make_unique<JsEvalSelector>(rule.substr(4));
    case SelectorType::DefaultJSoup:
        return std::make_unique<DefaultJSoupSelector>(rule);
    default:
        if (rule.find('.') != std::string::npos && rule[0] != '$') {
            return std::make_unique<DefaultJSoupSelector>(rule);
        }
        return std::make_unique<JsonPathSelector>(rule);
    }
}

std::vector<std::unique_ptr<Selector>> SelectorFactory::createOrChain(const std::string& rule) {
    std::vector<std::unique_ptr<Selector>> chain;

    std::string remaining = rule;
    size_t pos;
    while ((pos = remaining.find("||")) != std::string::npos) {
        std::string segment = remaining.substr(0, pos);
        remaining = remaining.substr(pos + 2);

        size_t b = segment.find_first_not_of(" \t\r\n");
        size_t e = segment.find_last_not_of(" \t\r\n");
        if (b != std::string::npos) {
            segment = segment.substr(b, e - b + 1);
            if (!segment.empty()) {
                auto sel = create(segment);
                if (sel) chain.push_back(std::move(sel));
            }
        }
    }

    {
        size_t b = remaining.find_first_not_of(" \t\r\n");
        size_t e = remaining.find_last_not_of(" \t\r\n");
        if (b != std::string::npos) {
            remaining = remaining.substr(b, e - b + 1);
            if (!remaining.empty()) {
                auto sel = create(remaining);
                if (sel) chain.push_back(std::move(sel));
            }
        }
    }

    return chain;
}

} // namespace ariaread

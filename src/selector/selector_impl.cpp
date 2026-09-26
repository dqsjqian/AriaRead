/// @file selector_impl.cpp
/// @brief 各选择器实现：JsonPath、Regex、CSS、XPath、JsEval、DefaultJSoup

#include "ariaread/selector.h"
#include "ariaread/gumbo_helper.h"
#include "ariaread/elements_single.h"
#include "ariaread/css_engine.h"
#include "ariaread/xpath_engine.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <regex>
#include <sstream>
#include <algorithm>
#include <gumbo.h>

namespace ariaread {

using json = nlohmann::json;

// ──────────────────────────────────────────────
// JSONPath 选择器
// ──────────────────────────────────────────────
JsonPathSelector::JsonPathSelector(const std::string& path) : path_(path) {}

std::vector<std::string> JsonPathSelector::select(const std::string& content) {
    // 三路分隔符（&&、||、%%）的拆分与组合统一在 applyRule 层处理，
    // 选择器本身只负责执行单条规则。
    return selectSingle(content, path_);
}

std::vector<std::string> JsonPathSelector::selectSingle(const std::string& content, const std::string& rule) {
    std::vector<std::string> results;

    try {
        auto j = json::parse(content);
        auto tokens = tokenize(rule);

        json current = j;
        bool valid = true;

        for (const auto& token : tokens) {
            if (token == "$") continue;

            if (token.size() >= 2 && token.front() == '[' && token.back() == ']') {
                std::string idx = token.substr(1, token.length() - 2);
                if (idx == "*") {
                    if (current.is_array()) {
                        for (const auto& item : current) {
                            results.push_back(item.is_string()
                                ? item.get<std::string>()
                                : item.dump());
                        }
                        return results;
                    }
                } else if (idx.find(':') != std::string::npos) {
                    // 切片 [start:end:step]
                    std::vector<std::string> parts;
                    std::istringstream iss(idx);
                    std::string part;
                    while (std::getline(iss, part, ':')) {
                        parts.push_back(part);
                    }

                    int start = 0, end = -1, step = 1;
                    if (parts.size() >= 1 && !parts[0].empty()) {
                        start = std::stoi(parts[0]);
                    }
                    if (parts.size() >= 2 && !parts[1].empty()) {
                        end = std::stoi(parts[1]);
                    }
                    if (parts.size() >= 3 && !parts[2].empty()) {
                        step = std::stoi(parts[2]);
                    }

                    if (current.is_array()) {
                        int size = current.size();
                        if (start < 0) start += size;
                        if (end < 0) end += size;
                        if (start < 0) start = 0;
                        if (end > size) end = size;

                        for (int i = start; i < end; i += step) {
                            results.push_back(current[i].is_string()
                                ? current[i].get<std::string>()
                                : current[i].dump());
                        }
                        return results;
                    }
                } else if (idx.find('?') != std::string::npos) {
                    // 筛选器 [?(@.foo > 10)]
                    // 这里只是基本实现，完整的筛选器需要更复杂的解析
                    // 目前先返回空，后续可以扩展
                    valid = false;
                    break;
                } else {
                    int index = std::stoi(idx);
                    if (current.is_array() && index >= 0 &&
                        index < static_cast<int>(current.size())) {
                        current = current[index];
                    } else {
                        valid = false;
                        break;
                    }
                }
            } else if (token == "..") {
                // 递归下降
                // 这里只是基本实现，完整的递归下降需要更复杂的实现
                // 目前先返回空，后续可以扩展
                valid = false;
                break;
            } else {
                if (current.is_object() && current.contains(token)) {
                    current = current[token];
                } else {
                    valid = false;
                    break;
                }
            }
        }

        if (valid) {
            if (current.is_string()) {
                results.push_back(current.get<std::string>());
            } else if (current.is_array()) {
                for (const auto& item : current) {
                    results.push_back(item.is_string()
                        ? item.get<std::string>()
                        : item.dump());
                }
            } else if (!current.is_null()) {
                results.push_back(current.dump());
            }
        }
    } catch (const std::exception&) {}

    return results;
}

std::vector<std::string> JsonPathSelector::tokenize(const std::string& path) {
    std::vector<std::string> tokens;
    std::string current;

    for (size_t i = 0; i < path.length(); ++i) {
        char c = path[i];
        if (c == '.' && !current.empty()) {
            tokens.push_back(current);
            current.clear();
        } else if (c == '[') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            size_t end = path.find(']', i);
            if (end != std::string::npos) {
                tokens.push_back(path.substr(i, end - i + 1));
                i = end;
            }
        } else if (c == '.') {
            // '.' 且 current 为空时（如 [0] 后面的 .），直接跳过，不追加
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }

    return tokens;
}

// ──────────────────────────────────────────────
// 正则选择器（PIMPL 隐藏 std::regex）
// ──────────────────────────────────────────────
struct RegexSelector::Impl {
    std::regex re;
    Impl(const std::string& pattern) : re(pattern, std::regex::ECMAScript) {}
};

RegexSelector::RegexSelector(const std::string& pattern)
    : pattern_(pattern), pImpl(std::make_unique<Impl>(pattern)) {}

RegexSelector::~RegexSelector() = default;

std::vector<std::string> RegexSelector::select(const std::string& content) {
    std::vector<std::string> results;

    try {
        std::sregex_iterator iter(content.begin(), content.end(), pImpl->re);
        std::sregex_iterator end;

        for (; iter != end; ++iter) {
            if (iter->size() > 1) {
                results.push_back(iter->str(1));
            } else {
                results.push_back(iter->str());
            }
        }
    } catch (const std::exception&) {}

    return results;
}

// ──────────────────────────────────────────────
// CSS 选择器（基于 Gumbo）
// ──────────────────────────────────────────────
CssSelector::CssSelector(const std::string& selector) : selector_(selector) {}

std::vector<std::string> CssSelector::select(const std::string& content) {
    // 三路分隔符的拆分与组合统一在 applyRule 层处理。
    return selectSingle(content, selector_);
}

std::vector<std::string> CssSelector::selectSingle(const std::string& content, const std::string& rule) {
    std::vector<std::string> results;

    std::string sel = rule;
    size_t b = sel.find_first_not_of(" \t");
    size_t e = sel.find_last_not_of(" \t");
    if (b != std::string::npos) sel = sel.substr(b, e - b + 1);
    if (sel.empty()) return results;

    // legado 的 @CSS: 规则形如  选择器@取值   （取值: text/html/href/src/属性名）。
    // 末尾若带 @xxx 且 xxx 不是组合选择器的一部分，则视为取值指令。
    std::string cssPart = sel;
    std::string extract = "text"; // 默认取文本（与 jsoup .text() 习惯一致）
    bool hasExtract = false;
    size_t at = sel.rfind('@');
    if (at != std::string::npos && at > 0) {
        std::string tail = sel.substr(at + 1);
        // 取值关键字白名单 + 任意属性名（不含空格/组合符）
        bool looksLikeExtract = !tail.empty() &&
            tail.find(' ') == std::string::npos &&
            tail.find('>') == std::string::npos &&
            tail.find('.') == std::string::npos &&
            tail.find('#') == std::string::npos &&
            tail.find('[') == std::string::npos;
        if (looksLikeExtract) {
            cssPart = sel.substr(0, at);
            extract = tail;
            hasExtract = true;
        }
    }

    GumboOutput* output = gumbo_parse(content.c_str());
    if (!output) return results;

    CssEngine engine(cssPart);
    if (engine.valid()) {
        auto nodes = engine.select(output->root);
        for (auto* node : nodes) {
            if (!hasExtract || extract == "html" || extract == "innerHtml") {
                if (extract == "html" || extract == "innerHtml") {
                    results.push_back(gumbo_helper::getInnerHtml(node, content));
                } else {
                    results.push_back(gumbo_helper::getOuterHtml(node, content));
                }
            } else if (extract == "text" || extract == "allText") {
                std::string t = gumbo_helper::getText(node);
                size_t tb = t.find_first_not_of(" \t\r\n");
                size_t te = t.find_last_not_of(" \t\r\n");
                if (tb != std::string::npos) results.push_back(t.substr(tb, te - tb + 1));
            } else if (extract == "ownText") {
                std::string t = gumbo_helper::getOwnText(node);
                size_t tb = t.find_first_not_of(" \t\r\n");
                size_t te = t.find_last_not_of(" \t\r\n");
                if (tb != std::string::npos) results.push_back(t.substr(tb, te - tb + 1));
            } else if (extract == "outerHtml" || extract == "all") {
                results.push_back(gumbo_helper::getOuterHtml(node, content));
            } else {
                // 当作属性名
                std::string v = gumbo_helper::getAttribute(node, extract);
                if (!v.empty()) results.push_back(v);
            }
        }
    }

    gumbo_destroy_output(&kGumboDefaultOptions, output);
    return results;
}

// ──────────────────────────────────────────────
// XPath 选择器（基于 Gumbo，支持基本 XPath）
// ──────────────────────────────────────────────
XPathSelector::XPathSelector(const std::string& path) : path_(path) {}

std::vector<std::string> XPathSelector::select(const std::string& content) {
    // 三路分隔符的拆分与组合统一在 applyRule 层处理。
    return selectSingle(content, path_);
}

std::vector<std::string> XPathSelector::selectSingle(const std::string& content, const std::string& rule) {
    std::vector<std::string> results;

    std::string expr = rule;
    size_t b = expr.find_first_not_of(" \t");
    size_t e = expr.find_last_not_of(" \t");
    if (b != std::string::npos) expr = expr.substr(b, e - b + 1);
    if (expr.empty()) return results;

    GumboOutput* output = gumbo_parse(content.c_str());
    if (!output) return results;

    XPathEngine engine(expr);
    if (engine.valid()) {
        results = engine.evaluateToStrings(output->root, content);
    }

    gumbo_destroy_output(&kGumboDefaultOptions, output);
    return results;
}

// ──────────────────────────────────────────────
// JS 执行选择器
// ──────────────────────────────────────────────
JsEvalSelector::JsEvalSelector(const std::string& code) : code_(code) {}

std::vector<std::string> JsEvalSelector::select(const std::string& content) {
    // 三路分隔符的拆分与组合统一在 applyRule 层处理。
    return selectSingle(content, code_);
}

std::vector<std::string> JsEvalSelector::selectSingle(const std::string& content, const std::string& rule) {
    std::vector<std::string> results;

    // 这里应该使用 QuickJS 执行 JS 代码
    // 目前只是基本实现，后续可以扩展
    // 支持格式：
    // - @js:code  （执行 JS 代码）
    // - {{js}}  （内嵌 JS 执行）

    // 如果有 JS 上下文，使用它来执行
    if (jsCtx_) {
        // TODO: 使用 QuickJS 执行 JS 代码
        // 这里需要实现 QuickJS 的绑定和执行逻辑
    }

    return results;
}

// ──────────────────────────────────────────────
// Default JSoup 选择器
// ──────────────────────────────────────────────
struct DefaultJSoupSelector::Impl {
    struct Step {
        enum class Type {
            FindByClass,
            FindByTag,
            FindById,
            FindByText,
            GetText,
            GetAttr,
            GetIndex,
            GetChildren,
            GetChildByTag,
            FilterByIndex,  // 新增：索引过滤
        };
        Type type;
        std::string value;
        int index = -1;
        std::string indexRule;  // 新增：索引规则（支持负数、区间等）
    };

    std::vector<Step> steps;

    /// 判断一个 token 是否"看起来像 HTML 属性名"：
    /// 由字母/数字/'-'/'_'/':' 组成，且首字符为字母。
    /// 用于区分裸属性取值（img@data-original）与裸 class（@some-class 罕见）。
    static bool looksLikeAttrName(const std::string& s) {
        if (s.empty()) return false;
        if (!std::isalpha(static_cast<unsigned char>(s[0]))) return false;
        for (char c : s) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) ||
                  c == '-' || c == '_' || c == ':')) {
                return false;
            }
        }
        return true;
    }

    void parse(const std::string& rule) {
        std::vector<std::string> parts;
        std::string current;
        for (size_t i = 0; i < rule.size(); ++i) {
            if (rule[i] == '@') {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
            } else {
                current += rule[i];
            }
        }
        if (!current.empty()) {
            parts.push_back(current);
        }

        for (size_t partIdx = 0; partIdx < parts.size(); ++partIdx) {
            auto& part = parts[partIdx];
            const bool isFirstPart = (partIdx == 0);
            size_t b = part.find_first_not_of(" \t");
            size_t e = part.find_last_not_of(" \t");
            if (b == std::string::npos) continue;
            part = part.substr(b, e - b + 1);

            if (part.empty()) continue;
            // 检查是否包含索引规则（[...] 或 .-1:10:2 等）
            std::string mainPart = part;
            std::string indexRule;

            // 检查 [index] 格式
            size_t bracketPos = part.find('[');
            if (bracketPos != std::string::npos) {
                mainPart = part.substr(0, bracketPos);
                indexRule = part.substr(bracketPos);
            } else {
                // 检查阅读原本的索引写法：tag.div.-1 / tag.div.0 / tag.div.-1:10:2 / tag.div!0:3
                // 分隔符可以是 '.'（选择）或 '!'（排除）。
                // 关键：分隔符后面必须是「合法索引 token」——只能由数字、'-'、':'、','、空格
                // 组成，且至少含一个数字。否则像 class.book-item、class.book-list 这类带连字符
                // 的 class 名会被误判成索引（这是大量书源解析失败的根因）。
                size_t dotPos = part.rfind('.');
                size_t bangPos = part.rfind('!');
                // 取更靠后的那个分隔符作为索引起点
                size_t sepPos = std::string::npos;
                char sepChar = '.';
                if (dotPos != std::string::npos && dotPos > 0) {
                    sepPos = dotPos; sepChar = '.';
                }
                if (bangPos != std::string::npos && bangPos > 0 &&
                    (sepPos == std::string::npos || bangPos > sepPos)) {
                    sepPos = bangPos; sepChar = '!';
                }
                if (sepPos != std::string::npos) {
                    std::string maybeIndex = part.substr(sepPos + 1);
                    bool looksLikeIndex = !maybeIndex.empty();
                    bool hasDigit = false;
                    for (char c : maybeIndex) {
                        if (c >= '0' && c <= '9') {
                            hasDigit = true;
                        } else if (c == '-' || c == ':' ||
                                   c == ',' || c == ' ') {
                            // 允许的索引语法字符
                        } else {
                            // 出现字母等非索引字符 → 不是索引，是普通选择器（如 book-item）
                            looksLikeIndex = false;
                            break;
                        }
                    }
                    if (looksLikeIndex && hasDigit) {
                        mainPart = part.substr(0, sepPos);
                        // 保留前导分隔符（'.' 或 '!'）一起传给 ElementsSingle::findIndexSet。
                        // legado 的 findIndexSet 依赖"完整规则尾部扫描"——逆向扫描遇到
                        // '.'/'!' 分隔符才提取索引并 return；若只传剥离后的纯数字（如 "0"），
                        // 逆向循环会走到 rus[-1] 越界，导致单数字索引（tag.a.0 等）全部失效。
                        // 这正是 href / 目录项 / 正文取不到的根因。
                        indexRule = std::string(1, sepChar) + maybeIndex;
                    }
                }
            }

            if (mainPart.rfind("class.", 0) == 0) {
                std::string rest = mainPart.substr(6);
                steps.push_back({Step::Type::FindByClass, rest, -1, ""});
                if (!indexRule.empty()) {
                    steps.push_back({Step::Type::FilterByIndex, "", -1, indexRule});
                }
            } else if (mainPart.size() > 1 && mainPart[0] == '.') {
                // Legado 裸 class 语法： .list / .book-item（去掉前导点）
                std::string rest = mainPart.substr(1);
                steps.push_back({Step::Type::FindByClass, rest, -1, ""});
                if (!indexRule.empty()) {
                    steps.push_back({Step::Type::FilterByIndex, "", -1, indexRule});
                }
            } else if (mainPart.size() > 1 && mainPart[0] == '#') {
                // Legado 裸 id 语法： #main
                steps.push_back({Step::Type::FindById, mainPart.substr(1), -1, ""});
                if (!indexRule.empty()) {
                    steps.push_back({Step::Type::FilterByIndex, "", -1, indexRule});
                }
            } else if (mainPart.rfind("tag.", 0) == 0) {
                std::string rest = mainPart.substr(4);
                steps.push_back({Step::Type::FindByTag, rest, -1, ""});
                if (!indexRule.empty()) {
                    steps.push_back({Step::Type::FilterByIndex, "", -1, indexRule});
                }
            } else if (mainPart.rfind("id.", 0) == 0) {
                std::string rest = mainPart.substr(3);
                steps.push_back({Step::Type::FindById, rest, -1, ""});
                if (!indexRule.empty()) {
                    steps.push_back({Step::Type::FilterByIndex, "", -1, indexRule});
                }
            } else if (mainPart.rfind("text.", 0) == 0) {
                steps.push_back({Step::Type::FindByText, mainPart.substr(5), -1, ""});
                if (!indexRule.empty()) {
                    steps.push_back({Step::Type::FilterByIndex, "", -1, indexRule});
                }
            } else if (mainPart == "text") {
                steps.push_back({Step::Type::GetText, "", -1, ""});
            } else if (mainPart == "textNodes") {
                steps.push_back({Step::Type::GetText, "", -1, ""});
            } else if (mainPart == "ownText") {
                steps.push_back({Step::Type::GetText, "", -1, ""});
            } else if (mainPart == "html") {
                steps.push_back({Step::Type::GetText, "html", -1, ""});
            } else if (mainPart == "href") {
                steps.push_back({Step::Type::GetAttr, "href", -1, ""});
            } else if (mainPart == "src") {
                steps.push_back({Step::Type::GetAttr, "src", -1, ""});
            } else if (mainPart == "alt") {
                steps.push_back({Step::Type::GetAttr, "alt", -1, ""});
            } else if (mainPart == "content") {
                steps.push_back({Step::Type::GetAttr, "content", -1, ""});
            } else if (mainPart == "value") {
                steps.push_back({Step::Type::GetAttr, "value", -1, ""});
            } else if (mainPart.rfind("attr:", 0) == 0 || mainPart.rfind("attr.", 0) == 0) {
                steps.push_back({Step::Type::GetAttr, mainPart.substr(5), -1, ""});
            } else if (mainPart == "children") {
                steps.push_back({Step::Type::GetChildren, "", -1, ""});
            } else {
                std::string lower = mainPart;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

                static const std::vector<std::string> commonTags = {
                    "a", "abbr", "address", "article", "aside", "b", "blockquote",
                    "body", "br", "button", "caption", "cite", "code", "col",
                    "dd", "del", "details", "dfn", "div", "dl", "dt", "em",
                    "fieldset", "figcaption", "figure", "footer", "form",
                    "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hr",
                    "html", "i", "iframe", "img", "input", "ins", "kbd", "label",
                    "legend", "li", "link", "main", "mark", "meta", "nav", "noscript",
                    "ol", "optgroup", "option", "output", "p", "pre", "progress",
                    "q", "rp", "rt", "ruby", "s", "samp", "script", "section",
                    "select", "small", "source", "span", "strong", "style", "sub",
                    "summary", "sup", "table", "tbody", "td", "template", "textarea",
                    "tfoot", "th", "thead", "time", "title", "tr", "track", "u",
                    "ul", "var", "video", "wbr"
                };

                bool isTag = std::find(commonTags.begin(), commonTags.end(), lower) != commonTags.end();
                if (isTag) {
                    steps.push_back({Step::Type::GetChildByTag, lower, -1, ""});
                    if (!indexRule.empty()) {
                        steps.push_back({Step::Type::FilterByIndex, "", -1, indexRule});
                    }
                } else if (!isFirstPart && indexRule.empty() &&
                           looksLikeAttrName(mainPart)) {
                    // Legado 裸属性取值语法：选择器@属性名（如 img@data-original、a@data-href）。
                    // 非首段、且不是已知标签/class、且形如合法属性名（含连字符/冒号也允许）时，
                    // 作为属性提取，而不是误判成 class 查找（这是 ruleImage 取不到图的根因之一）。
                    steps.push_back({Step::Type::GetAttr, mainPart, -1, ""});
                } else {
                    steps.push_back({Step::Type::FindByClass, mainPart, -1, ""});
                    if (!indexRule.empty()) {
                        steps.push_back({Step::Type::FilterByIndex, "", -1, indexRule});
                    }
                }
            }
        }
    }
};

DefaultJSoupSelector::DefaultJSoupSelector(const std::string& rule)
    : rule_(rule), pImpl(std::make_unique<Impl>()) {
    pImpl->parse(rule);
}

DefaultJSoupSelector::~DefaultJSoupSelector() = default;

std::vector<std::string> DefaultJSoupSelector::select(const std::string& content) {
    std::vector<std::string> results;

    if (pImpl->steps.empty()) return results;

    GumboOutput* output = gumbo_parse(content.c_str());
    if (!output) return results;

    std::vector<const GumboNode*> currentNodes = {output->root};
    bool isExtracted = false;

    for (const auto& step : pImpl->steps) {
        if (currentNodes.empty()) break;

        std::vector<const GumboNode*> nextNodes;

        switch (step.type) {
            case Impl::Step::Type::FindByClass: {
                for (auto* node : currentNodes) {
                    gumbo_helper::findByClass(node, step.value, nextNodes);
                }
                currentNodes = std::move(nextNodes);
                break;
            }
            case Impl::Step::Type::FindByTag: {
                for (auto* node : currentNodes) {
                    gumbo_helper::findByTagName(node, step.value, nextNodes);
                }
                currentNodes = std::move(nextNodes);
                break;
            }
            case Impl::Step::Type::FindById: {
                for (auto* node : currentNodes) {
                    auto* found = gumbo_helper::findById(node, step.value);
                    if (found) nextNodes.push_back(found);
                }
                currentNodes = std::move(nextNodes);
                break;
            }
            case Impl::Step::Type::FindByText: {
                for (auto* node : currentNodes) {
                    gumbo_helper::findByText(node, step.value, nextNodes);
                }
                currentNodes = std::move(nextNodes);
                break;
            }
            case Impl::Step::Type::GetIndex: {
                if (step.index >= 0 && step.index < static_cast<int>(currentNodes.size())) {
                    nextNodes.push_back(currentNodes[step.index]);
                }
                currentNodes = std::move(nextNodes);
                break;
            }
            case Impl::Step::Type::FilterByIndex: {
                // 使用 ElementsSingle 进行索引过滤
                ElementsSingle es;
                auto indexSet = es.parseAndFilter(step.indexRule, currentNodes.size());
                if (es.getSplit() == '!') {
                    // 排除模式（tag.p!0 去广告、class.item!-1 去尾项等）：
                    // 保留所有"不在 indexSet 中"的节点，顺序不变。
                    std::vector<bool> excluded(currentNodes.size(), false);
                    for (int idx : indexSet) {
                        if (idx >= 0 && idx < static_cast<int>(currentNodes.size())) {
                            excluded[idx] = true;
                        }
                    }
                    for (size_t i = 0; i < currentNodes.size(); ++i) {
                        if (!excluded[i]) nextNodes.push_back(currentNodes[i]);
                    }
                } else {
                    // 选择模式：仅保留 indexSet 中的节点
                    for (int idx : indexSet) {
                        if (idx >= 0 && idx < static_cast<int>(currentNodes.size())) {
                            nextNodes.push_back(currentNodes[idx]);
                        }
                    }
                }
                currentNodes = std::move(nextNodes);
                break;
            }
            case Impl::Step::Type::GetChildren: {
                for (auto* node : currentNodes) {
                    auto children = gumbo_helper::getChildren(node);
                    nextNodes.insert(nextNodes.end(), children.begin(), children.end());
                }
                currentNodes = std::move(nextNodes);
                break;
            }
            case Impl::Step::Type::GetChildByTag: {
                for (auto* node : currentNodes) {
                    auto children = gumbo_helper::getChildrenByTag(node, step.value);
                    if (!children.empty()) {
                        nextNodes.insert(nextNodes.end(), children.begin(), children.end());
                    } else {
                        gumbo_helper::findByTagName(node, step.value, nextNodes);
                    }
                }
                currentNodes = std::move(nextNodes);
                break;
            }
            case Impl::Step::Type::GetText: {
                isExtracted = true;
                for (auto* node : currentNodes) {
                    if (step.value == "html") {
                        results.push_back(gumbo_helper::getInnerHtml(node, content));
                    } else {
                        std::string text = gumbo_helper::getText(node);
                        size_t b = text.find_first_not_of(" \t\r\n");
                        size_t e = text.find_last_not_of(" \t\r\n");
                        if (b != std::string::npos) {
                            results.push_back(text.substr(b, e - b + 1));
                        }
                    }
                }
                break;
            }
            case Impl::Step::Type::GetAttr: {
                isExtracted = true;
                // 关键 fix（2026-06-09）：当 chapterUrl="href"、coverUrl="src" 等"裸属性规则"
                // 直接作用于一个 <a>/<img> 片段时，其 outerHtml 经 gumbo 重新解析后，根节点
                // 是 document/html/body 包装层，自身没有 href 属性。必须递归到第一个真正
                // 持有目标属性的后代元素，行为对齐 jsoup `selectFirst("[href]").attr("href")`。
                // 这是 "在线搜索结果无法解析出目录" 的根因之一（生产 db 里所有 chapter_url 都
                // 退化为 bookUrl，就是因为 href 取不到）。
                std::function<std::string(const GumboNode*)> findAttrDescendant =
                    [&](const GumboNode* n) -> std::string {
                        if (!n) return "";
                        if (n->type == GUMBO_NODE_ELEMENT) {
                            std::string v = gumbo_helper::getAttribute(n, step.value);
                            if (!v.empty()) return v;
                            const auto& children = n->v.element.children;
                            for (unsigned i = 0; i < children.length; ++i) {
                                auto* child = static_cast<const GumboNode*>(children.data[i]);
                                std::string vv = findAttrDescendant(child);
                                if (!vv.empty()) return vv;
                            }
                        } else if (n->type == GUMBO_NODE_DOCUMENT) {
                            const auto& children = n->v.document.children;
                            for (unsigned i = 0; i < children.length; ++i) {
                                auto* child = static_cast<const GumboNode*>(children.data[i]);
                                std::string vv = findAttrDescendant(child);
                                if (!vv.empty()) return vv;
                            }
                        }
                        return "";
                    };
                for (auto* node : currentNodes) {
                    std::string val = gumbo_helper::getAttribute(node, step.value);
                    if (val.empty()) {
                        // 兜底：递归找第一个持有该属性的后代
                        val = findAttrDescendant(node);
                    }
                    if (!val.empty()) {
                        results.push_back(val);
                    }
                }
                break;
            }
        }
    }

    if (!isExtracted) {
        for (auto* node : currentNodes) {
            results.push_back(gumbo_helper::getOuterHtml(node, content));
        }
    }

    gumbo_destroy_output(&kGumboDefaultOptions, output);

    // ── CSS 引擎兜底 ──
    // 手写 JSoup mini-parser 覆盖不了的复杂选择器（属性选择器、组合器、
    // 伪类、带连字符 class 的边界情况）会得到空结果。此时把规则按
    // "选择器@取值" 切开，交给完整 CSS3 引擎重试一次，最大化对齐 jsoup。
    if (results.empty() && !rule_.empty()) {
        // 规则末段若是取值指令（text/html/href/src/属性名），分离出来
        std::string cssPart = rule_;
        std::string extract;
        size_t at = rule_.rfind('@');
        if (at != std::string::npos && at > 0) {
            std::string tail = rule_.substr(at + 1);
            bool looksLikeExtract = !tail.empty() &&
                tail.find(' ') == std::string::npos &&
                tail.find('@') == std::string::npos &&
                tail.find('>') == std::string::npos;
            if (looksLikeExtract) { cssPart = rule_.substr(0, at); extract = tail; }
        }
        // 把 legado 的 class./tag./id. 前缀转成标准 CSS（class.x→.x, tag.x→x, id.x→#x）
        auto toCss = [](std::string s) -> std::string {
            // 仅处理最常见前缀，避免误伤
            auto repl = [&](const std::string& from, const std::string& to){
                size_t p = 0;
                while ((p = s.find(from, p)) != std::string::npos) {
                    // 仅在词首或空白/组合符后替换
                    if (p == 0 || s[p-1]==' '||s[p-1]=='>'||s[p-1]=='+'||s[p-1]=='~') {
                        s.replace(p, from.size(), to);
                        p += to.size();
                    } else { p += from.size(); }
                }
            };
            repl("class.", ".");
            repl("tag.", "");
            repl("id.", "#");
            return s;
        };
        std::string cssRule = toCss(cssPart);

        GumboOutput* out2 = gumbo_parse(content.c_str());
        if (out2) {
            CssEngine eng(cssRule);
            if (eng.valid()) {
                auto nodes = eng.select(out2->root);
                for (auto* node : nodes) {
                    if (extract.empty() || extract=="html") {
                        results.push_back(extract=="html"
                            ? gumbo_helper::getInnerHtml(node, content)
                            : gumbo_helper::getOuterHtml(node, content));
                    } else if (extract=="text") {
                        std::string t = gumbo_helper::getText(node);
                        size_t b=t.find_first_not_of(" \t\r\n"), e=t.find_last_not_of(" \t\r\n");
                        if (b!=std::string::npos) results.push_back(t.substr(b,e-b+1));
                    } else if (extract=="ownText") {
                        std::string t = gumbo_helper::getOwnText(node);
                        size_t b=t.find_first_not_of(" \t\r\n"), e=t.find_last_not_of(" \t\r\n");
                        if (b!=std::string::npos) results.push_back(t.substr(b,e-b+1));
                    } else {
                        std::string v = gumbo_helper::getAttribute(node, extract);
                        if (!v.empty()) results.push_back(v);
                    }
                }
            }
            gumbo_destroy_output(&kGumboDefaultOptions, out2);
        }
    }

    return results;
}

} // namespace ariaread

#pragma once
/// @file gumbo_helper.h
/// @brief Gumbo HTML 解析辅助函数

#include <gumbo.h>
#include <string>
#include <vector>

namespace ariaread {
namespace gumbo_helper {

/// 获取元素的指定属性值
std::string getAttribute(const GumboNode* node, const std::string& attrName);

/// 获取元素的 class 属性
std::string getClass(const GumboNode* node);

/// 检查元素是否包含指定 class
bool hasClass(const GumboNode* node, const std::string& className);

/// 获取元素的纯文本内容（递归）
std::string getText(const GumboNode* node);

/// 获取元素的 outerHTML（简化版）
std::string getOuterHtml(const GumboNode* node, const std::string& originalHtml);

/// 获取元素的 innerHTML（简化版）
std::string getInnerHtml(const GumboNode* node, const std::string& originalHtml);

/// 按 class 查找所有匹配元素
void findByClass(const GumboNode* node, const std::string& className,
                 std::vector<const GumboNode*>& results);

/// 按标签名查找所有匹配元素
void findByTag(const GumboNode* node, GumboTag tag,
               std::vector<const GumboNode*>& results);

/// 按标签名字符串查找（支持自定义标签）
void findByTagName(const GumboNode* node, const std::string& tagName,
                   std::vector<const GumboNode*>& results);

/// 按 id 查找元素
const GumboNode* findById(const GumboNode* node, const std::string& id);

/// 按文本内容查找
void findByText(const GumboNode* node, const std::string& text,
                std::vector<const GumboNode*>& results);

/// 获取直接子元素
std::vector<const GumboNode*> getChildren(const GumboNode* node);

/// 获取指定标签的直接子元素
std::vector<const GumboNode*> getChildrenByTag(const GumboNode* node,
                                                const std::string& tagName);

// ──────────────────────────────────────────────
// CSS / XPath 引擎所需的 DOM 导航与匹配原语
// ──────────────────────────────────────────────

/// 获取节点的标签名（小写，自定义标签也支持）
std::string getTagName(const GumboNode* node);

/// 判断节点是否为元素节点
inline bool isElement(const GumboNode* node) {
    return node && node->type == GUMBO_NODE_ELEMENT;
}

/// 获取直接子元素（仅元素节点），与 getChildren 等价，语义更清晰
std::vector<const GumboNode*> elementChildren(const GumboNode* node);

/// 获取所有后代元素（深度优先，不含自身）
void descendants(const GumboNode* node, std::vector<const GumboNode*>& out);

/// 获取节点在其父元素的「元素兄弟」列表中的位置（0-based）；无父或非元素返回 -1
int elementIndexInParent(const GumboNode* node, const GumboNode* parent);

/// 获取父元素（向上找到最近的 ELEMENT 父节点）
const GumboNode* parentElement(const GumboNode* node);

/// 获取 ownText：仅本节点直接文本子节点拼接（不含后代元素的文本）
std::string getOwnText(const GumboNode* node);

/// 获取每个直接文本子节点的 trim 文本（对应 jsoup textNodes），空的跳过
std::vector<std::string> getTextNodes(const GumboNode* node);

/// 判断元素是否拥有某属性
bool hasAttribute(const GumboNode* node, const std::string& attrName);

} // namespace gumbo_helper
} // namespace ariaread

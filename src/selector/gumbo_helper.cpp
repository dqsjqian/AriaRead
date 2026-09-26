/// @file gumbo_helper.cpp
/// @brief Gumbo HTML 解析辅助函数实现

#include "ariaread/gumbo_helper.h"

#include <sstream>
#include <algorithm>

namespace ariaread {
namespace gumbo_helper {

std::string getAttribute(const GumboNode* node, const std::string& attrName) {
    if (node->type != GUMBO_NODE_ELEMENT) return "";
    GumboAttribute* attr = gumbo_get_attribute(&node->v.element.attributes, attrName.c_str());
    return attr ? std::string(attr->value) : "";
}

std::string getClass(const GumboNode* node) {
    return getAttribute(node, "class");
}

bool hasClass(const GumboNode* node, const std::string& className) {
    std::string cls = getClass(node);
    if (cls.empty()) return false;
    std::istringstream iss(cls);
    std::string token;
    while (iss >> token) {
        if (token == className) return true;
    }
    return false;
}

std::string getText(const GumboNode* node) {
    if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_WHITESPACE) {
        return std::string(node->v.text.text);
    }
    if (node->type != GUMBO_NODE_ELEMENT) return "";

    std::string result;
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        auto* child = static_cast<GumboNode*>(children->data[i]);
        result += getText(child);
    }
    return result;
}

std::string getOuterHtml(const GumboNode* node, const std::string& originalHtml) {
    if (node->type != GUMBO_NODE_ELEMENT) {
        if (node->type == GUMBO_NODE_TEXT) return std::string(node->v.text.text);
        return "";
    }
    GumboStringPiece piece = node->v.element.original_tag;
    if (piece.data && piece.length > 0) {
        size_t start = piece.data - originalHtml.c_str();
        GumboStringPiece endPiece = node->v.element.original_end_tag;
        if (endPiece.data && endPiece.length > 0) {
            size_t end = (endPiece.data - originalHtml.c_str()) + endPiece.length;
            if (start < originalHtml.size() && end <= originalHtml.size() && end > start) {
                return originalHtml.substr(start, end - start);
            }
        }
        if (start < originalHtml.size()) {
            size_t end = start + piece.length;
            if (end <= originalHtml.size()) {
                return originalHtml.substr(start, end - start);
            }
        }
    }
    return getText(node);
}

std::string getInnerHtml(const GumboNode* node, const std::string& originalHtml) {
    if (node->type != GUMBO_NODE_ELEMENT) return getText(node);

    GumboStringPiece startTag = node->v.element.original_tag;
    GumboStringPiece endTag = node->v.element.original_end_tag;

    if (startTag.data && startTag.length > 0 && endTag.data && endTag.length > 0) {
        size_t innerStart = (startTag.data - originalHtml.c_str()) + startTag.length;
        size_t innerEnd = endTag.data - originalHtml.c_str();
        if (innerStart < originalHtml.size() && innerEnd <= originalHtml.size() && innerEnd > innerStart) {
            return originalHtml.substr(innerStart, innerEnd - innerStart);
        }
    }
    return getText(node);
}

void findByClass(const GumboNode* node, const std::string& className,
                 std::vector<const GumboNode*>& results) {
    if (node->type != GUMBO_NODE_ELEMENT) return;
    if (hasClass(node, className)) {
        results.push_back(node);
    }
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        findByClass(static_cast<GumboNode*>(children->data[i]), className, results);
    }
}

void findByTag(const GumboNode* node, GumboTag tag,
               std::vector<const GumboNode*>& results) {
    if (node->type != GUMBO_NODE_ELEMENT) return;
    if (node->v.element.tag == tag) {
        results.push_back(node);
    }
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        findByTag(static_cast<GumboNode*>(children->data[i]), tag, results);
    }
}

void findByTagName(const GumboNode* node, const std::string& tagName,
                   std::vector<const GumboNode*>& results) {
    if (node->type != GUMBO_NODE_ELEMENT) return;

    GumboStringPiece piece = node->v.element.original_tag;
    if (piece.data && piece.length > 0) {
        std::string tag(piece.data + 1, piece.length - 1);
        size_t end = tag.find_first_of(" \t\r\n>/");
        if (end != std::string::npos) tag = tag.substr(0, end);
        std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
        if (tag == tagName) {
            results.push_back(node);
        }
    }

    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        findByTagName(static_cast<GumboNode*>(children->data[i]), tagName, results);
    }
}

const GumboNode* findById(const GumboNode* node, const std::string& id) {
    if (node->type != GUMBO_NODE_ELEMENT) return nullptr;
    if (getAttribute(node, "id") == id) return node;
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        auto* found = findById(static_cast<GumboNode*>(children->data[i]), id);
        if (found) return found;
    }
    return nullptr;
}

void findByText(const GumboNode* node, const std::string& text,
                std::vector<const GumboNode*>& results) {
    if (node->type != GUMBO_NODE_ELEMENT) return;
    std::string nodeText = getText(node);
    if (nodeText.find(text) != std::string::npos) {
        results.push_back(node);
    }
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        findByText(static_cast<GumboNode*>(children->data[i]), text, results);
    }
}

std::vector<const GumboNode*> getChildren(const GumboNode* node) {
    std::vector<const GumboNode*> result;
    if (node->type != GUMBO_NODE_ELEMENT) return result;
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        auto* child = static_cast<GumboNode*>(children->data[i]);
        if (child->type == GUMBO_NODE_ELEMENT) {
            result.push_back(child);
        }
    }
    return result;
}

std::vector<const GumboNode*> getChildrenByTag(const GumboNode* node,
                                                const std::string& tagName) {
    std::vector<const GumboNode*> result;
    if (node->type != GUMBO_NODE_ELEMENT) return result;
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        auto* child = static_cast<GumboNode*>(children->data[i]);
        if (child->type != GUMBO_NODE_ELEMENT) continue;

        GumboStringPiece piece = child->v.element.original_tag;
        if (piece.data && piece.length > 0) {
            std::string tag(piece.data + 1, piece.length - 1);
            size_t end = tag.find_first_of(" \t\r\n>/");
            if (end != std::string::npos) tag = tag.substr(0, end);
            std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
            if (tag == tagName) {
                result.push_back(child);
            }
        }
    }
    return result;
}

// ──────────────────────────────────────────────
// CSS / XPath 引擎所需原语
// ──────────────────────────────────────────────

std::string getTagName(const GumboNode* node) {
    if (!node || node->type != GUMBO_NODE_ELEMENT) return "";
    // 优先用 gumbo 的 tag 枚举（标准标签更快更准）
    GumboTag tag = node->v.element.tag;
    if (tag != GUMBO_TAG_UNKNOWN) {
        const char* n = gumbo_normalized_tagname(tag);
        if (n && *n) return std::string(n);
    }
    // 自定义/未知标签：从 original_tag 文本里抠
    GumboStringPiece piece = node->v.element.original_tag;
    if (piece.data && piece.length > 0) {
        std::string t(piece.data + 1, piece.length - 1);
        size_t end = t.find_first_of(" \t\r\n>/");
        if (end != std::string::npos) t = t.substr(0, end);
        std::transform(t.begin(), t.end(), t.begin(), ::tolower);
        return t;
    }
    return "";
}

std::vector<const GumboNode*> elementChildren(const GumboNode* node) {
    return getChildren(node);
}

void descendants(const GumboNode* node, std::vector<const GumboNode*>& out) {
    if (!node || node->type != GUMBO_NODE_ELEMENT) return;
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        auto* child = static_cast<GumboNode*>(children->data[i]);
        if (child->type == GUMBO_NODE_ELEMENT) {
            out.push_back(child);
            descendants(child, out);
        }
    }
}

int elementIndexInParent(const GumboNode* node, const GumboNode* parent) {
    if (!node || !parent || parent->type != GUMBO_NODE_ELEMENT) return -1;
    const GumboVector* children = &parent->v.element.children;
    int idx = 0;
    for (unsigned int i = 0; i < children->length; ++i) {
        auto* child = static_cast<GumboNode*>(children->data[i]);
        if (child->type != GUMBO_NODE_ELEMENT) continue;
        if (child == node) return idx;
        ++idx;
    }
    return -1;
}

const GumboNode* parentElement(const GumboNode* node) {
    if (!node) return nullptr;
    const GumboNode* p = node->parent;
    while (p && p->type != GUMBO_NODE_ELEMENT) {
        p = p->parent;
    }
    return p;
}

std::string getOwnText(const GumboNode* node) {
    if (!node || node->type != GUMBO_NODE_ELEMENT) return "";
    std::string result;
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        auto* child = static_cast<GumboNode*>(children->data[i]);
        if (child->type == GUMBO_NODE_TEXT || child->type == GUMBO_NODE_WHITESPACE) {
            result += child->v.text.text;
        }
    }
    return result;
}

std::vector<std::string> getTextNodes(const GumboNode* node) {
    std::vector<std::string> out;
    if (!node || node->type != GUMBO_NODE_ELEMENT) return out;
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        auto* child = static_cast<GumboNode*>(children->data[i]);
        if (child->type == GUMBO_NODE_TEXT || child->type == GUMBO_NODE_WHITESPACE) {
            std::string t = child->v.text.text;
            size_t b = t.find_first_not_of(" \t\r\n");
            size_t e = t.find_last_not_of(" \t\r\n");
            if (b != std::string::npos) {
                out.push_back(t.substr(b, e - b + 1));
            }
        }
    }
    return out;
}

bool hasAttribute(const GumboNode* node, const std::string& attrName) {
    if (!node || node->type != GUMBO_NODE_ELEMENT) return false;
    return gumbo_get_attribute(&node->v.element.attributes, attrName.c_str()) != nullptr;
}

} // namespace gumbo_helper
} // namespace ariaread
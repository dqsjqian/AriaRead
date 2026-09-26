/// @file xpath_engine.cpp
/// @brief XPath 引擎实现（书源常用子集），基于 Gumbo DOM。
///
/// 设计：把表达式拆成「步(step)」序列，每步 = 轴 + 节点测试 + 谓词列表。
/// 逐步在当前节点集上推进，最后一步若是 @attr / text() 则产出字符串值。

#include "ariaread/xpath_engine.h"
#include "ariaread/gumbo_helper.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <functional>

namespace ariaread {

namespace {

using gumbo_helper::getTagName;
using gumbo_helper::getAttribute;
using gumbo_helper::hasAttribute;
using gumbo_helper::getText;
using gumbo_helper::elementChildren;
using gumbo_helper::parentElement;
using gumbo_helper::descendants;

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

enum class Axis {
    Child, Descendant, DescendantOrSelf, Self, Parent,
    FollowingSibling, PrecedingSibling, Attribute
};

// 末端取值类型
enum class Terminal { None, Text, Attr };

struct Step {
    Axis axis = Axis::Child;
    std::string nodeTest;            // 标签名 / "*" / "" (用于 text()、@attr 时)
    std::vector<std::string> preds;  // 谓词原文（不含[]）
    Terminal terminal = Terminal::None;
    std::string attrName;            // terminal==Attr 时
};

// ──────────────────────────────────────────────
// 表达式 → 步序列 解析
// ──────────────────────────────────────────────
class XPathParser {
public:
    explicit XPathParser(const std::string& src) : s_(trim(src)) {}

    bool parse(std::vector<Step>& steps, bool& absolute) {
        absolute = false;
        size_t i = 0;
        // 处理前导  /  //  ./  .//
        if (s_.rfind(".//", 0) == 0) { /* 相对 descendant */ steps.push_back(makeDOS()); i = 3; }
        else if (s_.rfind("./", 0) == 0) { i = 2; }
        else if (s_.rfind("//", 0) == 0) { absolute = true; steps.push_back(makeDOS()); i = 2; }
        else if (!s_.empty() && s_[0] == '/') { absolute = true; i = 1; }

        while (i < s_.size()) {
            // 读取一个 step（到下一个未被括号包裹的 '/'）
            std::string raw;
            int depth = 0;
            while (i < s_.size()) {
                char c = s_[i];
                if (c == '[') depth++;
                else if (c == ']') depth--;
                if (c == '/' && depth == 0) break;
                raw += c; ++i;
            }
            // 处理分隔符： '//' → 下一步前插入 descendant-or-self
            bool nextIsDoubleSlash = false;
            if (i < s_.size() && s_[i] == '/') {
                ++i;
                if (i < s_.size() && s_[i] == '/') { nextIsDoubleSlash = true; ++i; }
            }

            raw = trim(raw);
            if (raw == "." ) {
                // 当前节点，无操作
            } else if (!raw.empty()) {
                Step st;
                if (!parseStep(raw, st)) return false;
                steps.push_back(std::move(st));
            }
            if (nextIsDoubleSlash) steps.push_back(makeDOS());
        }
        return !steps.empty() || absolute;
    }

private:
    std::string s_;

    static Step makeDOS() { Step st; st.axis = Axis::DescendantOrSelf; st.nodeTest = "*"; return st; }

    bool parseStep(const std::string& raw, Step& st) {
        std::string body = raw;
        // 轴前缀  axis::
        size_t axPos = body.find("::");
        if (axPos != std::string::npos) {
            std::string ax = body.substr(0, axPos);
            body = body.substr(axPos + 2);
            if (ax=="child") st.axis=Axis::Child;
            else if (ax=="descendant") st.axis=Axis::Descendant;
            else if (ax=="descendant-or-self") st.axis=Axis::DescendantOrSelf;
            else if (ax=="self") st.axis=Axis::Self;
            else if (ax=="parent") st.axis=Axis::Parent;
            else if (ax=="following-sibling") st.axis=Axis::FollowingSibling;
            else if (ax=="preceding-sibling") st.axis=Axis::PrecedingSibling;
            else if (ax=="attribute") st.axis=Axis::Attribute;
            else st.axis=Axis::Child;
        }

        // 谓词： 抠出所有 [...]
        std::string nodePart;
        size_t j = 0;
        while (j < body.size()) {
            if (body[j] == '[') {
                int depth = 1; ++j;
                std::string pred;
                while (j < body.size() && depth > 0) {
                    if (body[j]=='[') depth++;
                    else if (body[j]==']') { depth--; if (depth==0) break; }
                    pred += body[j]; ++j;
                }
                if (j < body.size() && body[j]==']') ++j;
                st.preds.push_back(trim(pred));
            } else {
                nodePart += body[j]; ++j;
            }
        }
        nodePart = trim(nodePart);

        // 末端取值
        if (nodePart == "text()") { st.terminal = Terminal::Text; st.nodeTest = "*"; }
        else if (!nodePart.empty() && nodePart[0]=='@') {
            st.terminal = Terminal::Attr; st.attrName = nodePart.substr(1); st.axis = Axis::Attribute;
        }
        else if (nodePart.rfind("attribute::",0)==0) {
            st.terminal = Terminal::Attr; st.attrName = nodePart.substr(11); st.axis = Axis::Attribute;
        }
        else {
            st.nodeTest = nodePart.empty() ? "*" : nodePart;
        }
        return true;
    }
};

// ──────────────────────────────────────────────
// 谓词求值
// ──────────────────────────────────────────────
// 把单个元素在「同测试节点集合」中的 1-based 位置 + 集合大小传入，支持 position()/last()
struct PredCtx {
    int position = 1;
    int size = 1;
};

std::string stripQuotes(const std::string& s) {
    std::string t = trim(s);
    if (t.size()>=2 && (t.front()=='"'||t.front()=='\'') && t.back()==t.front())
        return t.substr(1, t.size()-2);
    return t;
}

// 求一个「原子条件」的真值（不含 and/or）
bool evalAtom(const GumboNode* node, const std::string& atomRaw, const PredCtx& ctx) {
    std::string atom = trim(atomRaw);
    if (atom.empty()) return true;

    // 纯数字 → 位置谓词
    bool allDigit = !atom.empty() && std::all_of(atom.begin(), atom.end(), [](char c){return std::isdigit((unsigned char)c);});
    if (allDigit) { try { return ctx.position == std::stoi(atom); } catch(...) { return false; } }

    // last()
    if (atom == "last()") return ctx.position == ctx.size;
    // last()-N
    if (atom.rfind("last()",0)==0) {
        std::string rest = trim(atom.substr(6));
        if (!rest.empty() && rest[0]=='-') {
            try { int n = std::stoi(rest.substr(1)); return ctx.position == ctx.size - n; } catch(...) {}
        }
        if (rest.empty()) return ctx.position == ctx.size;
    }
    // position() 比较  position()<n  position()<=n  position()=n  position()>n
    if (atom.rfind("position()",0)==0) {
        std::string rest = trim(atom.substr(10));
        auto cmp = [&](const std::string& op, int rhs)->bool{
            if (op=="<")  return ctx.position <  rhs;
            if (op=="<=") return ctx.position <= rhs;
            if (op==">")  return ctx.position >  rhs;
            if (op==">=") return ctx.position >= rhs;
            if (op=="="||op=="==") return ctx.position == rhs;
            return false;
        };
        for (const std::string op : {"<=",">=","<",">","=="," ="}) {
            if (rest.rfind(op,0)==0) {
                std::string nstr = trim(rest.substr(op.size()));
                try { return cmp(trim(op), std::stoi(nstr)); } catch(...) { return false; }
            }
        }
    }

    // @attr  存在性
    if (atom[0]=='@' && atom.find('=')==std::string::npos) {
        return hasAttribute(node, trim(atom.substr(1)));
    }
    // @attr = 'v'
    if (atom[0]=='@') {
        size_t eq = atom.find('=');
        std::string name = trim(atom.substr(1, eq-1));
        std::string val  = stripQuotes(atom.substr(eq+1));
        return hasAttribute(node, name) && getAttribute(node, name) == val;
    }
    // contains(@attr,'v') / contains(text(),'v')
    if (atom.rfind("contains(",0)==0) {
        std::string inner = atom.substr(9);
        if (!inner.empty() && inner.back()==')') inner.pop_back();
        size_t comma = inner.find(',');
        if (comma==std::string::npos) return false;
        std::string lhs = trim(inner.substr(0, comma));
        std::string rhs = stripQuotes(inner.substr(comma+1));
        std::string hay;
        if (lhs=="text()" || lhs==".") hay = getText(node);
        else if (!lhs.empty() && lhs[0]=='@') hay = getAttribute(node, trim(lhs.substr(1)));
        else hay = getText(node);
        return hay.find(rhs) != std::string::npos;
    }
    // starts-with(@attr,'v')
    if (atom.rfind("starts-with(",0)==0) {
        std::string inner = atom.substr(12);
        if (!inner.empty() && inner.back()==')') inner.pop_back();
        size_t comma = inner.find(',');
        if (comma==std::string::npos) return false;
        std::string lhs = trim(inner.substr(0, comma));
        std::string rhs = stripQuotes(inner.substr(comma+1));
        std::string hay = (!lhs.empty() && lhs[0]=='@') ? getAttribute(node, trim(lhs.substr(1))) : getText(node);
        return hay.rfind(rhs,0)==0;
    }
    // text()='v'
    if (atom.rfind("text()",0)==0) {
        size_t eq = atom.find('=');
        if (eq!=std::string::npos) {
            std::string val = stripQuotes(atom.substr(eq+1));
            return trim(getText(node)) == val;
        }
        return !getText(node).empty();
    }

    return false;
}

// 求一个谓词（支持简单 and / or，不含括号嵌套优先级）
bool evalPredicate(const GumboNode* node, const std::string& pred, const PredCtx& ctx) {
    std::string p = trim(pred);
    // 拆 or（优先级最低）
    std::vector<std::string> orParts;
    {
        size_t start=0; int depth=0;
        for (size_t i=0;i+3<=p.size();){
            if (p[i]=='(') {depth++; ++i; continue;}
            if (p[i]==')') {depth--; ++i; continue;}
            if (depth==0 && p.compare(i,4," or ")==0){ orParts.push_back(p.substr(start,i-start)); start=i+4; i+=4; continue;}
            ++i;
        }
        orParts.push_back(p.substr(start));
    }
    auto evalAnd = [&](const std::string& seg)->bool{
        std::vector<std::string> andParts; size_t start=0; int depth=0;
        for (size_t i=0;i+5<=seg.size();){
            if (seg[i]=='(') {depth++; ++i; continue;}
            if (seg[i]==')') {depth--; ++i; continue;}
            if (depth==0 && seg.compare(i,5," and ")==0){ andParts.push_back(seg.substr(start,i-start)); start=i+5; i+=5; continue;}
            ++i;
        }
        andParts.push_back(seg.substr(start));
        for (auto& a : andParts) if (!evalAtom(node, a, ctx)) return false;
        return true;
    };
    for (auto& seg : orParts) if (evalAnd(seg)) return true;
    return false;
}

// ──────────────────────────────────────────────
// 求值引擎
// ──────────────────────────────────────────────
// 对一个节点应用某 step 的轴 + 节点测试，得到候选节点（未过滤谓词）
std::vector<const GumboNode*> applyAxis(const GumboNode* ctx, const Step& st) {
    std::vector<const GumboNode*> out;
    auto tagOk = [&](const GumboNode* n)->bool{
        if (st.nodeTest=="*" || st.nodeTest.empty()) return true;
        return getTagName(n)==st.nodeTest;
    };
    switch (st.axis) {
        case Axis::Self:
            if (gumbo_helper::isElement(ctx) && tagOk(ctx)) out.push_back(ctx);
            break;
        case Axis::Child:
            for (auto* c : elementChildren(ctx)) if (tagOk(c)) out.push_back(c);
            break;
        case Axis::Descendant: {
            std::vector<const GumboNode*> d; descendants(ctx, d);
            for (auto* n : d) if (tagOk(n)) out.push_back(n);
            break;
        }
        case Axis::DescendantOrSelf: {
            if (gumbo_helper::isElement(ctx) && tagOk(ctx)) out.push_back(ctx);
            std::vector<const GumboNode*> d; descendants(ctx, d);
            for (auto* n : d) if (tagOk(n)) out.push_back(n);
            break;
        }
        case Axis::Parent: {
            auto* p = parentElement(ctx);
            if (p && tagOk(p)) out.push_back(p);
            break;
        }
        case Axis::FollowingSibling: {
            auto* p = parentElement(ctx);
            if (p) {
                bool after=false;
                for (auto* s : elementChildren(p)) {
                    if (s==ctx) { after=true; continue; }
                    if (after && tagOk(s)) out.push_back(s);
                }
            }
            break;
        }
        case Axis::PrecedingSibling: {
            auto* p = parentElement(ctx);
            if (p) {
                for (auto* s : elementChildren(p)) {
                    if (s==ctx) break;
                    if (tagOk(s)) out.push_back(s);
                }
            }
            break;
        }
        case Axis::Attribute:
            // 属性轴在末端取值时单独处理，这里返回 ctx 自身
            if (gumbo_helper::isElement(ctx)) out.push_back(ctx);
            break;
    }
    return out;
}

} // anonymous namespace

// ──────────────────────────────────────────────
// XPathEngine 外壳
// ──────────────────────────────────────────────
struct XPathEngine::Impl {
    std::vector<Step> steps;
    bool absolute = false;
};

XPathEngine::XPathEngine(const std::string& expr) : impl_(std::make_shared<Impl>()) {
    XPathParser p(expr);
    valid_ = p.parse(impl_->steps, impl_->absolute);
}

std::vector<XPathResult> XPathEngine::evaluate(const GumboNode* root) const {
    std::vector<XPathResult> results;
    if (!valid_ || !root) return results;

    // 起点：绝对路径从文档根开始，相对从 root 开始
    std::vector<const GumboNode*> ctxSet;
    ctxSet.push_back(root);

    for (size_t si = 0; si < impl_->steps.size(); ++si) {
        const Step& st = impl_->steps[si];

        // 末端属性/文本取值
        if (st.terminal == Terminal::Attr) {
            for (auto* n : ctxSet) {
                if (hasAttribute(n, st.attrName)) {
                    XPathResult r; r.isString=true; r.value=getAttribute(n, st.attrName);
                    results.push_back(std::move(r));
                }
            }
            return results;
        }
        if (st.terminal == Terminal::Text) {
            // text()：本步收集每个上下文节点的文本
            // 谓词（如有）作用于节点
            for (auto* n : ctxSet) {
                XPathResult r; r.isString=true; r.value=getText(n);
                results.push_back(std::move(r));
            }
            return results;
        }

        // 普通步：对每个上下文节点应用轴，合并候选，再按谓词过滤
        std::vector<const GumboNode*> next;
        std::unordered_set<const GumboNode*> seen;
        for (auto* ctx : ctxSet) {
            auto cand = applyAxis(ctx, st);
            // 谓词依赖「同一上下文下的候选集」做 position()/last()
            std::vector<const GumboNode*> filtered;
            if (st.preds.empty()) {
                filtered = cand;
            } else {
                for (size_t k = 0; k < cand.size(); ++k) {
                    PredCtx pc; pc.position=(int)k+1; pc.size=(int)cand.size();
                    bool ok=true;
                    for (auto& pred : st.preds) if (!evalPredicate(cand[k], pred, pc)) { ok=false; break; }
                    if (ok) filtered.push_back(cand[k]);
                }
            }
            for (auto* n : filtered) if (seen.insert(n).second) next.push_back(n);
        }
        ctxSet = std::move(next);
        if (ctxSet.empty()) return results;
    }

    // 末端是元素集合
    for (auto* n : ctxSet) {
        XPathResult r; r.node=n; r.isString=false;
        results.push_back(std::move(r));
    }
    return results;
}

std::vector<std::string> XPathEngine::evaluateToStrings(const GumboNode* root,
                                                        const std::string& originalHtml) const {
    std::vector<std::string> out;
    for (auto& r : evaluate(root)) {
        if (r.isString) {
            out.push_back(r.value);
        } else if (r.node) {
            out.push_back(gumbo_helper::getOuterHtml(r.node, originalHtml));
        }
    }
    return out;
}

} // namespace ariaread

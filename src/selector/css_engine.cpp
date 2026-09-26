/// @file css_engine.cpp
/// @brief 完整 CSS3 选择器引擎实现（基于 Gumbo DOM）
///
/// 解析模型：
///   Selector(逗号分组)  := ComplexSelector ("," ComplexSelector)*
///   ComplexSelector      := CompoundSelector (Combinator CompoundSelector)*
///   Combinator           := ' ' (后代) | '>' (子) | '+' (相邻兄弟) | '~' (通用兄弟)
///   CompoundSelector     := SimpleSelector+   (无空格连写，全部需匹配同一元素)
///   SimpleSelector       := TypeSel | '.'class | '#'id | '['attr']' | ':'pseudo
///
/// 求值采用「从右往左」匹配：先用最右复合选择器收集候选元素，
/// 再对每个候选回溯验证左侧组合链是否成立。这是 CSS 引擎的标准高效做法。

#include "ariaread/css_engine.h"
#include "ariaread/gumbo_helper.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace ariaread {

namespace {

using gumbo_helper::getTagName;
using gumbo_helper::getAttribute;
using gumbo_helper::hasAttribute;
using gumbo_helper::getClass;
using gumbo_helper::getText;
using gumbo_helper::parentElement;
using gumbo_helper::descendants;

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 属性匹配运算符
enum class AttrOp { Exists, Equals, Prefix, Suffix, Contains, Includes, DashMatch };

struct AttrSel {
    std::string name;
    AttrOp op = AttrOp::Exists;
    std::string value;
    bool caseInsensitive = false;
};

enum class PseudoType {
    FirstChild, LastChild, OnlyChild, Empty,
    NthChild, NthLastChild, NthOfType, NthLastOfType,
    FirstOfType, LastOfType, OnlyOfType,
    Not, Contains, Has, Unknown
};

struct CompiledSelector; // fwd

struct Pseudo {
    PseudoType type = PseudoType::Unknown;
    // nth: an+b
    int a = 0, b = 0;
    std::string text;                         // :contains(text)
    std::shared_ptr<CompiledSelector> sub;    // :not(...) / :has(...)
};

// 复合选择器：连写的若干 simple selector，全部要匹配同一元素
struct Compound {
    std::string tag;        // "" 或 "*" 表示任意
    bool universal = true;  // 是否无类型约束
    std::vector<std::string> classes;
    std::string id;
    std::vector<AttrSel> attrs;
    std::vector<Pseudo> pseudos;
};

enum class Comb { Descendant, Child, AdjacentSibling, GeneralSibling };

// 一条 complex selector：compound 序列，combinators_[i] 描述 compounds_[i] 与 compounds_[i+1] 的关系
struct Complex {
    std::vector<Compound> compounds;
    std::vector<Comb> combinators; // size == compounds.size()-1
};

// 整个选择器（逗号分组）
struct CompiledSelector {
    std::vector<Complex> groups;
    bool valid = false;
};

// ──────────────────────────────────────────────
// 解析器
// ──────────────────────────────────────────────
class Parser {
public:
    explicit Parser(const std::string& src) : s_(src) {}

    bool parse(CompiledSelector& out) {
        skipWs();
        while (pos_ < s_.size()) {
            Complex cx;
            if (!parseComplex(cx)) return false;
            out.groups.push_back(std::move(cx));
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                skipWs();
            } else {
                break;
            }
        }
        out.valid = !out.groups.empty();
        return out.valid;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    void skipWs() { while (pos_ < s_.size() && std::isspace((unsigned char)s_[pos_])) ++pos_; }
    bool eof() const { return pos_ >= s_.size(); }
    char cur() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }

    bool parseComplex(Complex& cx) {
        Compound c;
        if (!parseCompound(c)) return false;
        cx.compounds.push_back(std::move(c));

        while (pos_ < s_.size()) {
            // 探测组合器（可能前后有空白）
            size_t save = pos_;
            bool sawWs = false;
            while (pos_ < s_.size() && std::isspace((unsigned char)s_[pos_])) { ++pos_; sawWs = true; }
            if (pos_ >= s_.size() || s_[pos_] == ',') { pos_ = save; break; }

            Comb comb = Comb::Descendant;
            char ch = s_[pos_];
            if (ch == '>' || ch == '+' || ch == '~') {
                comb = (ch == '>') ? Comb::Child
                     : (ch == '+') ? Comb::AdjacentSibling
                                   : Comb::GeneralSibling;
                ++pos_;
                skipWs();
            } else if (sawWs) {
                comb = Comb::Descendant;
            } else {
                // 不是组合器也不是空白 → compound 结束（理论上不会到这）
                pos_ = save;
                break;
            }

            Compound nc;
            if (!parseCompound(nc)) return false;
            cx.combinators.push_back(comb);
            cx.compounds.push_back(std::move(nc));
        }
        return !cx.compounds.empty();
    }

    bool parseCompound(Compound& c) {
        bool any = false;
        while (pos_ < s_.size()) {
            char ch = s_[pos_];
            if (ch == '*') {
                c.universal = true; c.tag = "*"; ++pos_; any = true;
            } else if (ch == '.') {
                ++pos_;
                std::string name = readIdent();
                if (name.empty()) return false;
                c.classes.push_back(name); any = true;
            } else if (ch == '#') {
                ++pos_;
                std::string name = readIdent();
                if (name.empty()) return false;
                c.id = name; any = true;
            } else if (ch == '[') {
                AttrSel a;
                if (!parseAttr(a)) return false;
                c.attrs.push_back(std::move(a)); any = true;
            } else if (ch == ':') {
                Pseudo p;
                if (!parsePseudo(p)) return false;
                c.pseudos.push_back(std::move(p)); any = true;
            } else if (std::isalpha((unsigned char)ch) || ch == '_' || (unsigned char)ch >= 0x80) {
                std::string tag = readIdent();
                if (tag.empty()) return false;
                c.tag = toLower(tag); c.universal = false; any = true;
            } else {
                break;
            }
        }
        return any;
    }

    std::string readIdent() {
        // CSS 标识符：字母数字 - _ 以及转义；这里宽松处理（含 UTF-8 字节）
        std::string out;
        while (pos_ < s_.size()) {
            char ch = s_[pos_];
            if (std::isalnum((unsigned char)ch) || ch == '-' || ch == '_' || (unsigned char)ch >= 0x80) {
                out += ch; ++pos_;
            } else if (ch == '\\' && pos_ + 1 < s_.size()) {
                out += s_[pos_ + 1]; pos_ += 2;
            } else break;
        }
        return out;
    }

    bool parseAttr(AttrSel& a) {
        ++pos_; // '['
        skipWs();
        a.name = readIdent();
        if (a.name.empty()) return false;
        skipWs();
        if (cur() == ']') { ++pos_; a.op = AttrOp::Exists; return true; }

        // 运算符
        char ch = cur();
        if (ch == '=') { a.op = AttrOp::Equals; ++pos_; }
        else if ((ch=='^'||ch=='$'||ch=='*'||ch=='~'||ch=='|') && pos_+1<s_.size() && s_[pos_+1]=='=') {
            a.op = (ch=='^')?AttrOp::Prefix:(ch=='$')?AttrOp::Suffix:(ch=='*')?AttrOp::Contains
                  :(ch=='~')?AttrOp::Includes:AttrOp::DashMatch;
            pos_ += 2;
        } else return false;

        skipWs();
        // 值（可带引号）
        if (cur() == '"' || cur() == '\'') {
            char q = cur(); ++pos_;
            std::string v;
            while (pos_ < s_.size() && s_[pos_] != q) {
                if (s_[pos_]=='\\' && pos_+1<s_.size()) { v += s_[pos_+1]; pos_+=2; }
                else { v += s_[pos_]; ++pos_; }
            }
            if (cur()==q) ++pos_;
            a.value = v;
        } else {
            std::string v;
            while (pos_ < s_.size() && s_[pos_] != ']' && !std::isspace((unsigned char)s_[pos_])) {
                v += s_[pos_]; ++pos_;
            }
            a.value = v;
        }
        skipWs();
        // case-insensitive 标志  [a=v i]
        if ((cur()=='i'||cur()=='I')) { a.caseInsensitive = true; ++pos_; skipWs(); }
        if (cur() != ']') return false;
        ++pos_;
        return true;
    }

    bool parsePseudo(Pseudo& p) {
        ++pos_; // ':'
        if (cur()==':') ++pos_; // 兼容 ::pseudo-element 当作伪类宽松处理
        std::string name = toLower(readIdent());
        if (name.empty()) return false;

        bool hasArg = (cur()=='(');
        std::string arg;
        if (hasArg) {
            ++pos_;
            int depth = 1;
            while (pos_ < s_.size() && depth > 0) {
                char ch = s_[pos_];
                if (ch=='(') depth++;
                else if (ch==')') { depth--; if (depth==0) break; }
                arg += ch; ++pos_;
            }
            if (cur()==')') ++pos_;
        }

        if (name=="first-child") p.type=PseudoType::FirstChild;
        else if (name=="last-child") p.type=PseudoType::LastChild;
        else if (name=="only-child") p.type=PseudoType::OnlyChild;
        else if (name=="only-of-type") p.type=PseudoType::OnlyOfType;
        else if (name=="empty") p.type=PseudoType::Empty;
        else if (name=="first-of-type") p.type=PseudoType::FirstOfType;
        else if (name=="last-of-type") p.type=PseudoType::LastOfType;
        else if (name=="nth-child") { p.type=PseudoType::NthChild; parseNth(arg, p.a, p.b); }
        else if (name=="nth-last-child") { p.type=PseudoType::NthLastChild; parseNth(arg, p.a, p.b); }
        else if (name=="nth-of-type") { p.type=PseudoType::NthOfType; parseNth(arg, p.a, p.b); }
        else if (name=="nth-last-of-type") { p.type=PseudoType::NthLastOfType; parseNth(arg, p.a, p.b); }
        else if (name=="not" || name=="has") {
            p.type = (name=="not") ? PseudoType::Not : PseudoType::Has;
            p.sub = std::make_shared<CompiledSelector>();
            Parser sub(arg);
            sub.parse(*p.sub); // 解析失败则 sub->valid==false，求值时按不匹配处理
        }
        else if (name=="contains") {
            p.type=PseudoType::Contains;
            std::string t = trim(arg);
            if (t.size()>=2 && (t.front()=='"'||t.front()=='\'')) t = t.substr(1, t.size()-2);
            p.text = t;
        }
        else p.type = PseudoType::Unknown; // 未知伪类：宽松忽略（匹配恒真），避免误杀
        return true;
    }

    // 解析 an+b：支持 odd / even / 整数 / an+b / -n+b 等
    static void parseNth(const std::string& raw, int& a, int& b) {
        std::string s = toLower(trim(raw));
        a = 0; b = 0;
        if (s=="odd") { a=2; b=1; return; }
        if (s=="even") { a=2; b=0; return; }
        size_t nPos = s.find('n');
        if (nPos == std::string::npos) {
            // 纯数字 b
            try { b = std::stoi(s); } catch(...) { b = 0; }
            a = 0; return;
        }
        std::string aPart = s.substr(0, nPos);
        std::string bPart = s.substr(nPos+1);
        if (aPart.empty() || aPart=="+") a = 1;
        else if (aPart=="-") a = -1;
        else { try { a = std::stoi(aPart); } catch(...) { a = 0; } }
        bPart = trim(bPart);
        if (bPart.empty()) b = 0;
        else { try { b = std::stoi(bPart); } catch(...) { b = 0; } }
    }
};

// ──────────────────────────────────────────────
// 求值
// ──────────────────────────────────────────────
bool attrMatch(const GumboNode* node, const AttrSel& a) {
    if (a.op == AttrOp::Exists) return hasAttribute(node, a.name);
    if (!hasAttribute(node, a.name)) return false;
    std::string val = getAttribute(node, a.name);
    std::string want = a.value;
    std::string have = val;
    if (a.caseInsensitive) { have = toLower(have); want = toLower(want); }

    switch (a.op) {
        case AttrOp::Equals:   return have == want;
        case AttrOp::Prefix:   return !want.empty() && have.rfind(want, 0) == 0;
        case AttrOp::Suffix:   return !want.empty() && have.size()>=want.size() &&
                                      have.compare(have.size()-want.size(), want.size(), want)==0;
        case AttrOp::Contains: return !want.empty() && have.find(want) != std::string::npos;
        case AttrOp::Includes: {
            // 空白分隔的 token 列表里整词匹配
            std::string token;
            std::istringstream iss(have);
            while (iss >> token) if (token == want) return true;
            return false;
        }
        case AttrOp::DashMatch: // val == want 或以 want- 开头
            return have == want || (have.size()>want.size() && have.rfind(want+"-",0)==0);
        default: return false;
    }
}

bool hasClass(const GumboNode* node, const std::string& cls) {
    std::string c = getClass(node);
    if (c.empty()) return false;
    std::string token;
    std::istringstream iss(c);
    while (iss >> token) if (token == cls) return true;
    return false;
}

// 前向声明（complex 求值用于 :not/:has）
bool matchComplexAgainst(const Complex& cx, const GumboNode* node);
bool complexMatchesAsContext(const CompiledSelector& sel, const GumboNode* contextNode, bool relative);

bool pseudoMatch(const GumboNode* node, const Pseudo& p) {
    const GumboNode* parent = parentElement(node);
    auto siblingsOf = [&](bool sameTypeOnly) -> std::vector<const GumboNode*> {
        std::vector<const GumboNode*> sibs;
        if (!parent) { sibs.push_back(node); return sibs; }
        std::string tag = getTagName(node);
        for (auto* c : gumbo_helper::elementChildren(parent)) {
            if (!sameTypeOnly || getTagName(c) == tag) sibs.push_back(c);
        }
        return sibs;
    };
    auto indexIn = [&](const std::vector<const GumboNode*>& sibs) -> int {
        for (size_t i = 0; i < sibs.size(); ++i) if (sibs[i]==node) return (int)i;
        return -1;
    };
    auto nthOk = [&](int idx1based, int a, int b) -> bool {
        // an+b：存在 n>=0 使 idx == a*n + b
        if (a == 0) return idx1based == b;
        int diff = idx1based - b;
        if (diff % a != 0) return false;
        return diff / a >= 0;
    };

    switch (p.type) {
        case PseudoType::FirstChild: { auto s=siblingsOf(false); return !s.empty() && s.front()==node; }
        case PseudoType::LastChild:  { auto s=siblingsOf(false); return !s.empty() && s.back()==node; }
        case PseudoType::OnlyChild:  { auto s=siblingsOf(false); return s.size()==1 && s[0]==node; }
        case PseudoType::FirstOfType:{ auto s=siblingsOf(true); return !s.empty() && s.front()==node; }
        case PseudoType::LastOfType: { auto s=siblingsOf(true); return !s.empty() && s.back()==node; }
        case PseudoType::OnlyOfType: { auto s=siblingsOf(true); return s.size()==1 && s[0]==node; }
        case PseudoType::Empty: {
            // 无元素子节点且无非空白文本
            const GumboVector* ch = &node->v.element.children;
            for (unsigned i=0;i<ch->length;++i){
                auto* c = static_cast<GumboNode*>(ch->data[i]);
                if (c->type==GUMBO_NODE_ELEMENT) return false;
                if (c->type==GUMBO_NODE_TEXT){
                    std::string t=c->v.text.text;
                    if (t.find_first_not_of(" \t\r\n")!=std::string::npos) return false;
                }
            }
            return true;
        }
        case PseudoType::NthChild:   { auto s=siblingsOf(false); int i=indexIn(s); return i>=0 && nthOk(i+1,p.a,p.b); }
        case PseudoType::NthLastChild:{auto s=siblingsOf(false); int i=indexIn(s); return i>=0 && nthOk((int)s.size()-i,p.a,p.b); }
        case PseudoType::NthOfType:  { auto s=siblingsOf(true);  int i=indexIn(s); return i>=0 && nthOk(i+1,p.a,p.b); }
        case PseudoType::NthLastOfType:{auto s=siblingsOf(true); int i=indexIn(s); return i>=0 && nthOk((int)s.size()-i,p.a,p.b); }
        case PseudoType::Contains: {
            std::string txt = getText(node);
            return txt.find(p.text) != std::string::npos;
        }
        case PseudoType::Not: {
            if (!p.sub || !p.sub->valid) return true; // 解析失败 → 不排除
            // :not 内部是简单选择器列表，任一匹配则整体为假
            for (const auto& g : p.sub->groups) {
                if (matchComplexAgainst(g, node)) return false;
            }
            return true;
        }
        case PseudoType::Has: {
            if (!p.sub || !p.sub->valid) return false;
            // :has(rel)：node 的后代（相对选择）中存在匹配
            return complexMatchesAsContext(*p.sub, node, true);
        }
        case PseudoType::Unknown:
        default:
            return true; // 宽松：未知伪类不阻断匹配
    }
}

// 单个元素是否匹配一个 compound
bool compoundMatch(const GumboNode* node, const Compound& c) {
    if (!gumbo_helper::isElement(node)) return false;
    if (!c.universal && c.tag != "*" && !c.tag.empty()) {
        if (getTagName(node) != c.tag) return false;
    }
    if (!c.id.empty() && getAttribute(node, "id") != c.id) return false;
    for (auto& cls : c.classes) if (!hasClass(node, cls)) return false;
    for (auto& a : c.attrs) if (!attrMatch(node, a)) return false;
    for (auto& ps : c.pseudos) if (!pseudoMatch(node, ps)) return false;
    return true;
}

// 从右往左验证：node 匹配 compounds[last]，再回溯组合链
bool matchComplexAgainst(const Complex& cx, const GumboNode* node) {
    int last = (int)cx.compounds.size() - 1;
    if (last < 0) return false;
    if (!compoundMatch(node, cx.compounds[last])) return false;

    // 回溯：对 i 从 last-1 到 0，按 combinators[i] 找到能匹配的祖先/兄弟
    const GumboNode* cur = node;
    for (int i = last - 1; i >= 0; --i) {
        Comb comb = cx.combinators[i];
        const Compound& target = cx.compounds[i];
        bool matched = false;
        if (comb == Comb::Child) {
            const GumboNode* p = parentElement(cur);
            if (p && compoundMatch(p, target)) { cur = p; matched = true; }
        } else if (comb == Comb::Descendant) {
            const GumboNode* p = parentElement(cur);
            while (p) {
                if (compoundMatch(p, target)) { cur = p; matched = true; break; }
                p = parentElement(p);
            }
        } else if (comb == Comb::AdjacentSibling) {
            const GumboNode* p = parentElement(cur);
            if (p) {
                const GumboNode* prev = nullptr;
                for (auto* sib : gumbo_helper::elementChildren(p)) {
                    if (sib == cur) break;
                    prev = sib;
                }
                if (prev && compoundMatch(prev, target)) { cur = prev; matched = true; }
            }
        } else { // GeneralSibling
            const GumboNode* p = parentElement(cur);
            if (p) {
                for (auto* sib : gumbo_helper::elementChildren(p)) {
                    if (sib == cur) break;
                    if (compoundMatch(sib, target)) { cur = sib; matched = true; /* 取最近一个即可继续 */ }
                }
            }
        }
        if (!matched) return false;
    }
    return true;
}

// 用于 :has —— 相对上下文求值（在 contextNode 的后代里找匹配）
bool complexMatchesAsContext(const CompiledSelector& sel, const GumboNode* contextNode, bool /*relative*/) {
    std::vector<const GumboNode*> desc;
    descendants(contextNode, desc);
    for (const auto& g : sel.groups) {
        for (auto* d : desc) {
            if (matchComplexAgainst(g, d)) {
                // 还需保证匹配链顶端在 contextNode 范围内——matchComplexAgainst 已从右回溯，
                // 这里 contextNode 作为隐式祖先，简化处理为只要后代匹配整链即可。
                return true;
            }
        }
    }
    return false;
}

} // anonymous namespace

// ──────────────────────────────────────────────
// CssEngine 外壳
// ──────────────────────────────────────────────
struct CssEngine::Impl {
    CompiledSelector sel;
};

CssEngine::CssEngine(const std::string& selector) : impl_(std::make_shared<Impl>()) {
    Parser p(selector);
    valid_ = p.parse(impl_->sel);
}

std::vector<const GumboNode*> CssEngine::select(const GumboNode* root) const {
    std::vector<const GumboNode*> out;
    if (!valid_ || !root) return out;

    // 候选集合：root 自身 + 全部后代
    std::vector<const GumboNode*> pool;
    if (gumbo_helper::isElement(root)) pool.push_back(root);
    descendants(root, pool);

    std::unordered_set<const GumboNode*> seen;
    // 保持文档顺序：pool 本身就是 DFS 顺序
    for (auto* node : pool) {
        for (const auto& g : impl_->sel.groups) {
            if (matchComplexAgainst(g, node)) {
                if (seen.insert(node).second) out.push_back(node);
                break;
            }
        }
    }
    return out;
}

std::vector<const GumboNode*> CssEngine::select(const std::vector<const GumboNode*>& roots) const {
    std::vector<const GumboNode*> out;
    std::unordered_set<const GumboNode*> seen;
    for (auto* r : roots) {
        for (auto* n : select(r)) {
            if (seen.insert(n).second) out.push_back(n);
        }
    }
    return out;
}

} // namespace ariaread

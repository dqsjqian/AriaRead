#include "ariaread/engine_impl.h"

#include <cctype>
#include <algorithm>
#include <sstream>
#include <regex>
#include <cstring>

// 编码转换：Unix 用 iconv；Windows 用 Win32 代码页 API ——
// MSVC 没有 iconv.h，MinGW 虽有 MSYS2 libiconv 但多一个外部依赖；
// GBK/BIG5/SHIFT_JIS/EUC-KR 等 CJK 代码页 MultiByteToWideChar 原生支持。
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace ariaread {
namespace detail {

std::string normalizeText(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);

        // 跳过简单 HTML 标签
        if (c == '<') {
            size_t j = text.find('>', i + 1);
            if (j != std::string::npos) {
                i = j + 1;
                continue;
            }
        }

        // 跳过 ASCII 空白和常见分隔符
        if (c <= 0x7F) {
            if (std::isspace(c) || c == '-' || c == '_' || c == '.' || c == '|'
                || c == '[' || c == ']' || c == '(' || c == ')') {
                ++i;
                continue;
            }
            out.push_back(static_cast<char>(std::tolower(c)));
            ++i;
            continue;
        }

        // 跳过部分常见 UTF-8 分隔符（中文场景）
        if (i + 2 < text.size()) {
            unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
            bool isSep =
                (c == 0xE3 && c1 == 0x80 && (c2 == 0x90 || c2 == 0x91 || c2 == 0x88 || c2 == 0x89 || c2 == 0x90 || c2 == 0x91)) ||
                (c == 0xC2 && c1 == 0xB7) ||
                (c == 0xE2 && c1 == 0x80 && (c2 == 0xA2 || c2 == 0xA7));
            if (isSep) {
                i += 3;
                continue;
            }
        }

        // 原样保留 UTF-8 字节
        out.push_back(static_cast<char>(c));
        ++i;
    }

    return out;
}

int bookMatchScore(const Book& book,
                   const std::string& keyword,
                   bool matchName,
                   bool matchAuthor,
                   bool matchIntro) {
    const std::string kw = normalizeText(keyword);
    if (kw.empty()) return 0;

    const std::string name = normalizeText(book.name);
    const std::string author = normalizeText(book.author);
    const std::string intro = normalizeText(book.intro);

    int score = 0;

    if (matchName && !name.empty()) {
        if (name == kw) score = std::max(score, 1000);
        else if (name.rfind(kw, 0) == 0) score = std::max(score, 850);
        else if (name.find(kw) != std::string::npos) score = std::max(score, 700);
    }

    if (matchAuthor && !author.empty()) {
        if (author == kw) score = std::max(score, 520);
        else if (author.find(kw) != std::string::npos) score = std::max(score, 420);
    }

    if (matchIntro && !intro.empty()) {
        if (intro.find(kw) != std::string::npos) score = std::max(score, 220);
    }

    return score;
}

bool isBlank(const std::string& s) {
    for (unsigned char c : s) {
        if (!std::isspace(c)) return false;
    }
    return true;
}

std::string resolveUrlWithBase(const std::string& rawUrl,
                               const std::string& baseUrl,
                               JsRuntime* js) {
    if (rawUrl.empty() || isBlank(rawUrl)) return "";
    AnalyzeUrl analyzer(rawUrl, baseUrl, "", 1, js);
    return analyzer.result().url;
}

std::string extractHrefFallback(const std::string& htmlItem,
                                const std::string& baseUrl,
                                JsRuntime* js) {
    if (htmlItem.empty()) return "";

    static const std::regex hrefRegex(R"(href\s*=\s*[\"']([^\"'#][^\"']*)[\"'])",
                                      std::regex::icase);
    std::smatch m;
    if (!std::regex_search(htmlItem, m, hrefRegex) || m.size() < 2) {
        return "";
    }

    return resolveUrlWithBase(m[1].str(), baseUrl, js);
}

std::string trimCopy(const std::string& s) {
    size_t l = 0;
    size_t r = s.size();
    while (l < r && std::isspace(static_cast<unsigned char>(s[l]))) ++l;
    while (r > l && std::isspace(static_cast<unsigned char>(s[r - 1]))) --r;
    return s.substr(l, r - l);
}

bool isValidUtf8(const std::string& s) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(s.data());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = bytes[i];
        if (c <= 0x7F) {
            ++i;
            continue;
        }

        size_t need = 0;
        if ((c >> 5) == 0x6) need = 1;        // 0xC0-0xDF
        else if ((c >> 4) == 0xE) need = 2;    // 0xE0-0xEF
        else if ((c >> 3) == 0x1E) need = 3;   // 0xF0-0xF7
        else return false;

        if (i + need >= s.size()) return false;
        for (size_t j = 1; j <= need; ++j) {
            if ((bytes[i + j] >> 6) != 0x2) return false;
        }

        // 拒绝 overlong 编码、代理对、> U+10FFFF
        unsigned char b0 = bytes[i];
        unsigned char b1 = bytes[i + 1];
        if (need == 1) {
            // 2 字节：拒绝 overlong (0xC0 0x80 = U+0000)
            if (b0 < 0xC2) return false;
        } else if (need == 2) {
            // 3 字节：拒绝代理对 (0xED 0xA0-0xBF)
            if (b0 == 0xED && b1 >= 0xA0) return false;
        } else if (need == 3) {
            // 4 字节：拒绝 > U+10FFFF (0xF4 0x90-0xBF 无效)
            if (b0 > 0xF4) return false;
            if (b0 == 0xF4 && b1 >= 0x90) return false;
            // 拒绝 overlong (0xF0 0x80-0x8F)
            if (b0 == 0xF0 && b1 < 0x90) return false;
        }

        i += need + 1;
    }
    return true;
}

std::string sanitizeUtf8(const std::string& s) {
    if (isValidUtf8(s)) return s;

    const auto* bytes = reinterpret_cast<const unsigned char*>(s.data());
    std::string out;
    out.reserve(s.size());

    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = bytes[i];
        if (c <= 0x7F) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        size_t need = 0;
        if ((c >> 5) == 0x6) need = 1;        // 0xC0-0xDF
        else if ((c >> 4) == 0xE) need = 2;    // 0xE0-0xEF
        else if ((c >> 3) == 0x1E) need = 3;   // 0xF0-0xF7
        else {
            ++i;
            continue;
        }

        if (i + need >= s.size()) break;

        bool ok = true;
        for (size_t j = 1; j <= need; ++j) {
            if ((bytes[i + j] >> 6) != 0x2) {
                ok = false;
                break;
            }
        }

        if (!ok) {
            ++i;
            continue;
        }

        // 拒绝 overlong 编码、代理对、> U+10FFFF
        unsigned char b0 = bytes[i];
        unsigned char b1 = bytes[i + 1];
        if (need == 1) {
            if (b0 < 0xC2) { ++i; continue; }
        } else if (need == 2) {
            if (b0 == 0xED && b1 >= 0xA0) { ++i; continue; }
        } else if (need == 3) {
            if (b0 > 0xF4) { ++i; continue; }
            if (b0 == 0xF4 && b1 >= 0x90) { ++i; continue; }
            if (b0 == 0xF0 && b1 < 0x90) { ++i; continue; }
        }

        for (size_t j = 0; j <= need; ++j) {
            out.push_back(static_cast<char>(bytes[i + j]));
        }
        i += need + 1;
    }

    return out;
}

std::string renderTemplateRule(const std::string& ruleTemplate,
                               const std::function<std::string(const std::string&)>& resolver) {
    if (ruleTemplate.find("{{") == std::string::npos ||
        ruleTemplate.find("}}") == std::string::npos) {
        return ruleTemplate;
    }

    static const std::regex placeholderRegex(R"(\{\{(.*?)\}\})");
    std::string out;
    out.reserve(ruleTemplate.size() + 32);

    size_t lastPos = 0;
    bool replaced = false;
    for (std::sregex_iterator it(ruleTemplate.begin(), ruleTemplate.end(), placeholderRegex),
         end; it != end; ++it) {
        const auto& m = *it;
        size_t pos = static_cast<size_t>(m.position());
        out.append(ruleTemplate, lastPos, pos - lastPos);

        std::string expr = trimCopy(m[1].str());
        std::string value = resolver(expr);
        out += value;
        replaced = true;

        lastPos = pos + static_cast<size_t>(m.length());
    }

    out.append(ruleTemplate, lastPos, std::string::npos);
    return replaced ? out : ruleTemplate;
}

std::string extractFieldWithTemplateFallback(
    const std::string& item,
    const std::string& fieldRule,
    const std::function<std::vector<std::string>(const std::string&, const std::string&)>& applyRule
) {
    if (fieldRule.empty()) return "";

    auto direct = applyRule(item, fieldRule);
    if (!direct.empty() && !direct[0].empty()) {
        return direct[0];
    }

    if (fieldRule.find("{{") == std::string::npos ||
        fieldRule.find("}}") == std::string::npos) {
        return "";
    }

    std::string rendered = renderTemplateRule(fieldRule, [&](const std::string& expr) -> std::string {
        auto vals = applyRule(item, expr);
        return vals.empty() ? std::string() : vals[0];
    });

    if (rendered == fieldRule) return "";
    if (rendered.find("{{") != std::string::npos || rendered.find("}}") != std::string::npos) {
        return "";
    }

    return rendered;
}

// ──────────────────────────────────────────────
// CDATA 剥离
// ──────────────────────────────────────────────
std::string stripCdata(const std::string& text) {
    // 快速路径：不含 CDATA 直接返回，避免无谓拷贝
    if (text.find("<![CDATA[") == std::string::npos) return text;
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    const std::string open = "<![CDATA[";
    const std::string close = "]]>";
    while (pos < text.size()) {
        size_t start = text.find(open, pos);
        if (start == std::string::npos) {
            out.append(text, pos, std::string::npos);
            break;
        }
        // 追加 CDATA 段之前的普通文本
        out.append(text, pos, start - pos);
        size_t contentStart = start + open.size();
        size_t end = text.find(close, contentStart);
        if (end == std::string::npos) {
            // 没有闭合：把剩余原文（去掉 open 标记）追加，避免丢内容
            out.append(text, contentStart, std::string::npos);
            break;
        }
        // 追加 CDATA 内部原文
        out.append(text, contentStart, end - contentStart);
        pos = end + close.size();
    }
    return out;
}

// ──────────────────────────────────────────────
// HTML 实体解码
// ──────────────────────────────────────────────
std::string decodeHtmlEntities(const std::string& rawText) {
    // 先剥离 CDATA 包裹，再解码实体（RSS/Atom 各字段统一在此收口）
    std::string text = stripCdata(rawText);
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size();) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            ++i;
            continue;
        }

        // 查找 ';' 结尾
        size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 10) {
            // 没有找到合法实体，原样保留
            out.push_back('&');
            ++i;
            continue;
        }

        std::string entity = text.substr(i + 1, semi - i - 1);
        bool decoded = false;

        if (!entity.empty() && entity[0] == '#') {
            // 数字实体：&#123; 或 &#x1A;
            unsigned long codepoint = 0;
            try {
                if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')) {
                    codepoint = std::stoul(entity.substr(2), nullptr, 16);
                } else {
                    codepoint = std::stoul(entity.substr(1), nullptr, 10);
                }
            } catch (...) {
                codepoint = 0;
            }

            if (codepoint > 0 && codepoint <= 0x10FFFF) {
                // 编码为 UTF-8
                if (codepoint <= 0x7F) {
                    out.push_back(static_cast<char>(codepoint));
                } else if (codepoint <= 0x7FF) {
                    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                } else if (codepoint <= 0xFFFF) {
                    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                decoded = true;
            }
        } else {
            // 命名实体
            if (entity == "nbsp")       { out.push_back(' ');  decoded = true; }
            else if (entity == "amp")   { out.push_back('&');  decoded = true; }
            else if (entity == "lt")    { out.push_back('<');  decoded = true; }
            else if (entity == "gt")    { out.push_back('>');  decoded = true; }
            else if (entity == "quot")  { out.push_back('"');  decoded = true; }
            else if (entity == "apos")  { out.push_back('\''); decoded = true; }
            else if (entity == "mdash") { out += "\xE2\x80\x94"; decoded = true; } // —
            else if (entity == "ndash") { out += "\xE2\x80\x93"; decoded = true; } // –
            else if (entity == "hellip"){ out += "\xE2\x80\xA6"; decoded = true; } // …
            else if (entity == "lsquo") { out += "\xE2\x80\x98"; decoded = true; } // '
            else if (entity == "rsquo") { out += "\xE2\x80\x99"; decoded = true; } // '
            else if (entity == "ldquo") { out += "\xE2\x80\x9C"; decoded = true; } // "
            else if (entity == "rdquo") { out += "\xE2\x80\x9D"; decoded = true; } // "
            else if (entity == "copy")  { out += "\xC2\xA9";     decoded = true; } // ©
            else if (entity == "reg")   { out += "\xC2\xAE";     decoded = true; } // ®
            else if (entity == "trade") { out += "\xE2\x84\xA2"; decoded = true; } // ™
            else if (entity == "times") { out += "\xC3\x97";     decoded = true; } // ×
            else if (entity == "ensp")  { out.push_back(' ');  decoded = true; }
            else if (entity == "emsp")  { out.push_back(' ');  decoded = true; }
            else if (entity == "thinsp"){ out.push_back(' ');  decoded = true; }
        }

        if (decoded) {
            i = semi + 1;
        } else {
            // 无法识别的实体，原样保留
            out.push_back('&');
            ++i;
        }
    }

    return out;
}

// ──────────────────────────────────────────────
// 正文内容清理
// ──────────────────────────────────────────────
std::string cleanContent(const std::string& rawText) {
    if (rawText.empty()) return rawText;

    // 第1步：将 <br> / <br/> / <p> / </p> / <div> / </div> 转为换行
    std::string text;
    text.reserve(rawText.size());
    for (size_t i = 0; i < rawText.size();) {
        if (rawText[i] == '<') {
            size_t j = rawText.find('>', i + 1);
            if (j != std::string::npos) {
                std::string tag = rawText.substr(i + 1, j - i - 1);
                // 去除属性，只取标签名
                size_t sp = tag.find(' ');
                std::string tagName = (sp != std::string::npos) ? tag.substr(0, sp) : tag;
                // 转小写
                for (auto& c : tagName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                // 去除 /
                if (!tagName.empty() && tagName[0] == '/') tagName = tagName.substr(1);
                if (tagName.back() == '/') tagName.pop_back();

                if (tagName == "br" || tagName == "p" || tagName == "div") {
                    text.push_back('\n');
                }
                // 跳过整个标签
                i = j + 1;
                continue;
            }
        }
        text.push_back(rawText[i]);
        ++i;
    }

    // 第2步：去除剩余 HTML 标签
    std::string noTags;
    noTags.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '<') {
            size_t j = text.find('>', i + 1);
            if (j != std::string::npos) {
                i = j + 1;
                continue;
            }
        }
        noTags.push_back(text[i]);
        ++i;
    }

    // 第3步：解码 HTML 实体
    std::string decoded = decodeHtmlEntities(noTags);

    // 第4步：规范化空白（合并连续空格，保留换行）
    std::string result;
    result.reserve(decoded.size());
    bool lastWasSpace = false;
    for (size_t i = 0; i < decoded.size(); ++i) {
        char c = decoded[i];
        if (c == '\n' || c == '\r') {
            lastWasSpace = false;
            if (c == '\r' && i + 1 < decoded.size() && decoded[i + 1] == '\n') {
                ++i; // 跳过 \r\n 中的 \n
            }
            result.push_back('\n');
        } else if (c == ' ' || c == '\t') {
            if (!lastWasSpace) {
                result.push_back(' ');
                lastWasSpace = true;
            }
        } else {
            lastWasSpace = false;
            result.push_back(c);
        }
    }

    // 第5步：去除首尾空白行
    size_t start = 0;
    while (start < result.size() && (result[start] == '\n' || result[start] == ' ')) ++start;
    size_t end = result.size();
    while (end > start && (result[end - 1] == '\n' || result[end - 1] == ' ')) --end;

    return result.substr(start, end - start);
}

// ──────────────────────────────────────────────
// 保留 <img> 的正文格式化（对齐 legado HtmlFormatter.formatKeepImg）
// ──────────────────────────────────────────────
std::string formatKeepImg(const std::string& rawText, const std::string& baseUrl) {
    if (rawText.empty()) return rawText;

    std::string out;
    out.reserve(rawText.size());

    for (size_t i = 0; i < rawText.size();) {
        if (rawText[i] == '<') {
            size_t j = rawText.find('>', i + 1);
            if (j == std::string::npos) { out.push_back(rawText[i]); ++i; continue; }
            std::string tagFull = rawText.substr(i + 1, j - i - 1);
            // 取标签名
            std::string lower = tagFull;
            for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
            std::string tagName;
            for (char c : lower) {
                if (c==' '||c=='\t'||c=='\r'||c=='\n'||c=='/'||c=='>') break;
                if (c=='/') continue;
                tagName += c;
            }
            if (!tagName.empty() && tagName[0]=='/') tagName = tagName.substr(1);

            if (tagName == "img") {
                // 提取 src，转绝对，保留为独立行的 <img src="..."> 占位
                static const std::regex srcRe(R"((?:data-src|data-original|src)\s*=\s*["']([^"']+)["'])",
                                              std::regex::icase);
                std::smatch m;
                if (std::regex_search(tagFull, m, srcRe) && m.size() >= 2) {
                    std::string src = m[1].str();
                    if (!baseUrl.empty() && src.rfind("http", 0) != 0 && src.rfind("//", 0) != 0) {
                        // 简单相对路径绝对化（与 resolveUrlWithBase 同思路，避免依赖 js）
                        if (!src.empty() && src[0] == '/') {
                            // 取 scheme://host
                            size_t schemeEnd = baseUrl.find("://");
                            if (schemeEnd != std::string::npos) {
                                size_t hostEnd = baseUrl.find('/', schemeEnd + 3);
                                std::string origin = (hostEnd==std::string::npos) ? baseUrl : baseUrl.substr(0, hostEnd);
                                src = origin + src;
                            }
                        } else {
                            size_t lastSlash = baseUrl.find_last_of('/');
                            if (lastSlash != std::string::npos && lastSlash > 8) {
                                src = baseUrl.substr(0, lastSlash + 1) + src;
                            }
                        }
                    }
                    out += "\n<img src=\"" + src + "\">\n";
                }
                i = j + 1;
                continue;
            }
            if (tagName == "br" || tagName == "p" || tagName == "div") {
                out.push_back('\n');
            }
            i = j + 1;
            continue;
        }
        out.push_back(rawText[i]);
        ++i;
    }

    // 解码实体 + 规范化空白（保留换行、保留 <img> 行）
    std::string decoded = decodeHtmlEntities(out);
    std::string result;
    result.reserve(decoded.size());
    bool lastWasSpace = false;
    for (size_t i = 0; i < decoded.size(); ++i) {
        char c = decoded[i];
        if (c == '\n' || c == '\r') {
            lastWasSpace = false;
            if (c=='\r' && i+1<decoded.size() && decoded[i+1]=='\n') ++i;
            result.push_back('\n');
        } else if (c==' '||c=='\t') {
            if (!lastWasSpace) { result.push_back(' '); lastWasSpace = true; }
        } else { lastWasSpace = false; result.push_back(c); }
    }
    // 去首尾空白行
    size_t b = 0; while (b<result.size() && (result[b]=='\n'||result[b]==' ')) ++b;
    size_t e = result.size(); while (e>b && (result[e-1]=='\n'||result[e-1]==' ')) --e;
    return result.substr(b, e - b);
}

// ──────────────────────────────────────────────
// 内容级 ##正则替换 规则（多组换行分隔）
// ──────────────────────────────────────────────
std::string applyContentReplaceRule(const std::string& text, const std::string& replaceRule) {
    if (replaceRule.empty()) return text;
    std::string result = text;

    std::istringstream iss(replaceRule);
    std::string line;
    while (std::getline(iss, line)) {
        // 去尾部 \r
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (line.empty()) continue;

        std::string pattern, replacement;
        bool firstOnly = false;
        size_t p = line.find("##");
        if (p == std::string::npos) {
            pattern = line; replacement = "";
        } else {
            pattern = line.substr(0, p);
            std::string rest = line.substr(p + 2);
            size_t p2 = rest.find("##");
            if (p2 == std::string::npos) {
                replacement = rest;
            } else {
                replacement = rest.substr(0, p2);
                std::string rest2 = rest.substr(p2 + 2);
                if (!rest2.empty()) firstOnly = true; // ###... 仅首个
            }
        }
        if (pattern.empty()) continue;
        try {
            std::regex re(pattern);
            if (firstOnly) {
                result = std::regex_replace(result, re, replacement,
                                            std::regex_constants::format_first_only);
            } else {
                result = std::regex_replace(result, re, replacement);
            }
        } catch (...) { /* 跳过非法正则 */ }
    }
    return result;
}

// ──────────────────────────────────────────────
// 统一规则解析（供 Impl::applyRule 与各 worker 线程复用）
// 语义与 legado AnalyzeByJSoup.getStringList 对齐。
// ──────────────────────────────────────────────

/// 判断 {{...}} 内嵌表达式是否为「选择器规则」（而非 JS 代码）。
/// 选择器：$/.// # 开头、@css:/@xpath:/@regex:/@get:/class./tag./id./text. 前缀、
/// 或含 @text/@href/@src/@attr/@content/@value 取值符。
static bool inlineExprIsSelector(const std::string& s) {
    if (s.empty()) return false;
    char c0 = s[0];
    if (c0 == '\x24' /*$*/ || c0 == '.' || c0 == '#') return true;
    if (s.rfind("@css:",0)==0 || s.rfind("@xpath:",0)==0 ||
        s.rfind("@regex:",0)==0 || s.rfind("@get:",0)==0 ||
        s.rfind("class.",0)==0 || s.rfind("tag.",0)==0 ||
        s.rfind("id.",0)==0 || s.rfind("text.",0)==0) return true;
    if (s.find("@text")!=std::string::npos || s.find("@href")!=std::string::npos ||
        s.find("@src")!=std::string::npos  || s.find("@attr")!=std::string::npos ||
        s.find("@content")!=std::string::npos || s.find("@value")!=std::string::npos)
        return true;
    return false;
}

std::vector<std::string> applyRuleStatic(const std::string& content,
                                         const std::string& rule,
                                         JsRuntime* js,
                                         const std::string& baseUrl) {
    if (rule.empty()) return {};

    // 0. 整条规则即为 JS（@js:... 或 <js>...</js>）：直接对 content 执行，
    //    与 legado 的 Mode.Js 规则一致。返回脚本结果（按行拆分为多结果）。
    {
        std::string trimmedRule = rule;
        size_t b = trimmedRule.find_first_not_of(" \t\r\n");
        if (b != std::string::npos) trimmedRule = trimmedRule.substr(b);
        bool isAtJs = trimmedRule.rfind("@js:", 0) == 0;
        bool isTagJs = trimmedRule.rfind("<js>", 0) == 0;
        if (js && (isAtJs || isTagJs)) {
            std::string jsCode;
            if (isAtJs) {
                jsCode = trimmedRule.substr(4);
            } else {
                size_t end = trimmedRule.rfind("</js>");
                jsCode = (end != std::string::npos)
                    ? trimmedRule.substr(4, end - 4)
                    : trimmedRule.substr(4);
            }
            std::string out = js->evalRuleJs(jsCode, content, baseUrl);
            if (out.empty()) return {};
            return { out };
        }
    }

    // 0.5. 处理 @put:{"key":"rule"} —— 先求值 rule 并写入变量表，再从规则中剔除。
    //     与 legado AnalyzeRule.splitPutRule 对齐，供后续 @get:{key} / {{@get}} 引用。
    std::string ruleAfterPut = rule;
    if (js && rule.find("@put:{") != std::string::npos) {
        static const std::regex putRe(R"(@put:\{([^}]*)\})");
        std::smatch m;
        std::string work = ruleAfterPut;
        std::string rebuilt;
        auto begin = work.cbegin();
        while (std::regex_search(begin, work.cend(), m, putRe)) {
            rebuilt.append(begin, begin + m.position(0));
            std::string jsonBody = "{" + m[1].str() + "}";
            try {
                auto putJson = nlohmann::json::parse(jsonBody);
                for (auto it = putJson.begin(); it != putJson.end(); ++it) {
                    if (it.value().is_string()) {
                        std::string subRule = it.value().get<std::string>();
                        auto vals = applyRuleStatic(content, subRule, js, baseUrl);
                        js->putVariable(it.key(), vals.empty() ? "" : vals[0]);
                    }
                }
            } catch (...) {}
            begin = begin + m.position(0) + m.length(0);
        }
        rebuilt.append(begin, work.cend());
        ruleAfterPut = rebuilt;
    }

    // 1. 替换内嵌规则（{{js}}、@get:{key}、$1/$2）
    //    仅当规则中确实包含内嵌标记时才执行，避免对普通规则做无谓处理。
    std::string processedRule = ruleAfterPut;
    bool hasInline = (ruleAfterPut.find("{{") != std::string::npos) ||
                     (ruleAfterPut.find("@get:{") != std::string::npos) ||
                     (ruleAfterPut.find("$") != std::string::npos);
    if (hasInline) {
        processedRule = InlineRuleReplacer::replace(
            ruleAfterPut,
            [js, &content, &baseUrl](const std::string& innerRaw) -> std::string {
                // {{...}} 内嵌：智能分派——选择器规则走选择器引擎（对齐 legado
                // {{@@.css@text}} / {{$.field}} / {{@get:key}}），否则当 JS 执行。
                std::string inner = trimCopy(innerRaw);
                if (inner.empty()) return "";
                bool forceSelector = false;
                std::string selRule = inner;
                if (inner.rfind("@@", 0) == 0) { forceSelector = true; selRule = inner.substr(2); }
                if (forceSelector || inlineExprIsSelector(selRule)) {
                    if (selRule.rfind("@get:",0)==0 && js) {
                        std::string key = selRule.substr(5);
                        if (key.size()>1 && key.front()=='{' && key.back()=='}')
                            key = key.substr(1, key.size()-2);
                        return js->getVariable(key);
                    }
                    auto vals = applyRuleStatic(content, selRule, js, baseUrl);
                    if (vals.empty()) return std::string();
                    // 对齐 legado AnalyzeByJSoup.getString：多结果（如 &&/%% 合并）
                    // 用 "\n" 连接为单串，而非只取首个——否则 {{选择器A&&选择器B}}
                    // 模板里会丢掉除第一个外的所有结果（如标题取到、正文被丢）。
                    if (vals.size() == 1) return vals[0];
                    std::string joined;
                    for (size_t k = 0; k < vals.size(); ++k) {
                        if (k) joined += "\n";
                        joined += vals[k];
                    }
                    return joined;
                }
                if (!js) return "";
                return js->evalRuleJs(inner, content, baseUrl);
            },
            [js](const std::string& key) -> std::string {
                // @get:{key} 变量提供器：从 JsRuntime 的变量表读取
                // （由书源 JS 中 java.put / @put:{} 写入）。
                if (!js) return "";
                return js->getVariable(key);
            }
        );

        // 纯变量规则：整条规则仅由 @get:{...} 占位符（可含空白）组成时，
        // 替换后的文本即为字面量结果，不应再当作选择器执行。
        // 对齐 legado：getString(@get:{key}) 直接返回变量值。
        {
            static const std::regex onlyGetRe(R"(^\s*(@get:\{[^}]*\}\s*)+$)");
            if (std::regex_match(ruleAfterPut, onlyGetRe)) {
                if (processedRule.empty()) return {};
                return { processedRule };
            }
        }

        // 模板规则：原规则含 {{...}} 占位符，替换后的文本即为最终字面量结果，
        // 不应再当作选择器对 content 二次求值（对齐 legado AnalyzeRule 模板语义）。
        // 仅当替换确实发生了变化、且结果里不再残留 {{}} 占位符时生效。
        if (ruleAfterPut.find("{{") != std::string::npos &&
            processedRule != ruleAfterPut &&
            processedRule.find("{{") == std::string::npos) {
            // 仍允许末尾 ## 正则替换：交给后续步骤处理（selectorRule 会被当字面量返回）
            std::string literal = processedRule;
            std::string regexP, regexR; bool all = true;
            size_t rp = literal.find("##");
            if (rp != std::string::npos) {
                std::string sel = literal.substr(0, rp);
                std::string after = literal.substr(rp + 2);
                std::vector<std::string> ps; size_t st = 0;
                while (st < after.size()) {
                    size_t p = after.find("##", st);
                    if (p == std::string::npos) { ps.push_back(after.substr(st)); break; }
                    ps.push_back(after.substr(st, p - st)); st = p + 2;
                }
                if (ps.size()==1){ regexP=ps[0]; regexR=""; }
                else if (ps.size()==2){ regexP=ps[0]; regexR=ps[1]; }
                else if (ps.size()>=3){ regexP=ps[0]; regexR=ps[1]; all=false; }
                literal = sel;
                if (!regexP.empty()) {
                    try {
                        std::regex re(regexP);
                        literal = all ? std::regex_replace(literal, re, regexR)
                                      : std::regex_replace(literal, re, regexR,
                                            std::regex_constants::format_first_only);
                    } catch (...) {}
                }
            }
            if (literal.empty()) return {};
            return { literal };
        }
    }

    // 2. 拆分 ## 正则替换（与 legado BookListRule/ContentRule 对齐）
    //   ##regex                    → 删除匹配
    //   ##regex##replacement       → 替换所有匹配
    //   ##regex##replacement###    → 仅替换第一个匹配
    std::string selectorRule = processedRule;
    std::string regexPattern;
    std::string regexReplacement;
    bool regexReplaceAll = true;

    size_t replacePos = processedRule.find("##");
    if (replacePos != std::string::npos) {
        selectorRule = processedRule.substr(0, replacePos);
        std::string afterHash = processedRule.substr(replacePos + 2);
        // 按 ## 拆分
        std::vector<std::string> regexParts;
        size_t start = 0;
        while (start < afterHash.size()) {
            size_t pos = afterHash.find("##", start);
            if (pos == std::string::npos) {
                regexParts.push_back(afterHash.substr(start));
                break;
            }
            regexParts.push_back(afterHash.substr(start, pos - start));
            start = pos + 2;
        }
        if (regexParts.size() == 1) {
            // ##regex → 删除匹配
            regexPattern = regexParts[0];
            regexReplacement = "";
        } else if (regexParts.size() == 2) {
            // ##regex##replacement → 替换所有匹配
            regexPattern = regexParts[0];
            regexReplacement = regexParts[1];
        } else if (regexParts.size() >= 3) {
            // ##regex##replacement### → 仅替换第一个匹配
            regexPattern = regexParts[0];
            regexReplacement = regexParts[1];
            regexReplaceAll = false;
        }
    }

    // 3. 规则缓存（键必须包含 content 指纹，否则同一规则应用到不同 item 会串结果）
    auto& cacheManager = RuleCacheManager::getInstance();
    auto& ruleCache = cacheManager.getRuleCache();
    const std::string cacheKey =
        selectorRule + "\x01" +
        std::to_string(std::hash<std::string>{}(content));

    auto applyRegexReplace = [&](std::vector<std::string>& items) {
        if (regexPattern.empty() || items.empty()) return;
        try {
            auto& regexCache = cacheManager.getRegexCache();
            std::shared_ptr<std::regex> re;
            if (!regexCache.get(regexPattern, re)) {
                re = std::make_shared<std::regex>(regexPattern);
                regexCache.put(regexPattern, re);
            }
            for (auto& item : items) {
                std::string replaced;
                if (regexReplaceAll) {
                    replaced = std::regex_replace(item, *re, regexReplacement);
                } else {
                    // 仅替换第一个匹配
                    replaced = std::regex_replace(item, *re, regexReplacement,
                        std::regex_constants::format_first_only);
                }
                if (isValidUtf8(replaced)) {
                    item = std::move(replaced);
                }
            }
        } catch (const std::exception&) {
            // 忽略非法正则
        }
    };

    std::vector<std::string> cachedResults;
    if (ruleCache.get(cacheKey, cachedResults)) {
        applyRegexReplace(cachedResults);
        return cachedResults;
    }

    // 4. 缓存未命中：用 RuleAnalyzer 做三路分隔符（&&、||、%%）智能切分
    RuleAnalyzer analyzer(selectorRule);
    auto rules = analyzer.splitRule({"&&", "||", "%%"});

    std::vector<std::string> finalResults;

    if (rules.size() == 1) {
        // 单规则。先判断是否为 JS 规则（@js:/<js>），是则直接执行；
        // 否则走选择器。
        std::string r0 = rules[0];
        size_t rb = r0.find_first_not_of(" \t\r\n");
        std::string r0t = (rb == std::string::npos) ? r0 : r0.substr(rb);

        // ── 整条规则即为 JS（@js:... 或 <js>...</js> 开头）──
        if (js && (r0t.rfind("@js:", 0) == 0 || r0t.rfind("<js>", 0) == 0)) {
            std::string jsCode;
            if (r0t.rfind("@js:", 0) == 0) {
                jsCode = r0t.substr(4);
            } else {
                size_t end = r0t.rfind("</js>");
                jsCode = (end != std::string::npos) ? r0t.substr(4, end - 4) : r0t.substr(4);
            }
            std::string out = js->evalRuleJs(jsCode, content, baseUrl);
            if (!out.empty()) finalResults = { out };
        } else {
            // ── 链式 JS：选择器结果再喂给 JS ──
            // 例如 "a@href@js:result.replace(...)" 或 "class.xx@text<js>...</js>"
            // 仅对 JSoup 风格规则（不以 $/@CSS:/@XPath:/@regex://// 开头）检测。
            std::string selectorPrefix;
            std::string chainJsCode;
            bool hasChainJs = false;

            bool isJsoupStyle = !(r0t.rfind("$", 0) == 0 ||
                                  r0t.rfind("@CSS:", 0) == 0 || r0t.rfind("@css:", 0) == 0 ||
                                  r0t.rfind("@XPath:", 0) == 0 || r0t.rfind("@xpath:", 0) == 0 ||
                                  r0t.rfind("@regex:", 0) == 0 ||
                                  r0t.rfind("//", 0) == 0);

            if (isJsoupStyle && js) {
                // 检测 @js: 链式（选择器@js:jsCode）
                size_t atJsPos = r0t.find("@js:");
                if (atJsPos != std::string::npos && atJsPos > 0) {
                    selectorPrefix = r0t.substr(0, atJsPos);
                    chainJsCode = r0t.substr(atJsPos + 4);
                    hasChainJs = true;
                }
                // 检测 <js>...</js> 链式（选择器<js>jsCode</js>）
                if (!hasChainJs) {
                    size_t tagJsPos = r0t.find("<js>");
                    if (tagJsPos != std::string::npos && tagJsPos > 0) {
                        selectorPrefix = r0t.substr(0, tagJsPos);
                        size_t endJs = r0t.rfind("</js>");
                        chainJsCode = (endJs != std::string::npos)
                            ? r0t.substr(tagJsPos + 4, endJs - tagJsPos - 4)
                            : r0t.substr(tagJsPos + 4);
                        hasChainJs = true;
                    }
                }
            }

            if (hasChainJs) {
                // 先执行选择器前缀，再对每个结果执行 JS 转换
                auto chain = SelectorFactory::createOrChain(selectorPrefix);
                for (auto& selector : chain) {
                    auto results = selector->select(content);
                    if (!results.empty()) {
                        for (auto& r : results) {
                            std::string out = js->evalRuleJs(chainJsCode, r, baseUrl);
                            if (!out.empty()) finalResults.push_back(std::move(out));
                        }
                        break;
                    }
                }
            } else {
                // 普通选择器
                auto chain = SelectorFactory::createOrChain(rules[0]);
                for (auto& selector : chain) {
                    auto results = selector->select(content);
                    if (!results.empty()) {
                        finalResults = results;
                        break;
                    }
                }
            }
        }
    } else {
        // 多规则，语义与 legado AnalyzeByJSoup.getStringList 对齐：
        //   ||  互备：依次尝试，遇到第一个非空结果即停止
        //   %%  并行：各子规则结果按下标交叉合并
        //   &&  链式（及无类型）：前一个输出喂给下一个输入（legado 核心行为）
        const std::string& elementsType = analyzer.elementsType();

        if (elementsType == "%%") {
            // 并行：各自独立执行，结果交叉合并
            std::vector<std::vector<std::string>> collected;
            for (const auto& r : rules) {
                auto results = applyRuleStatic(content, r, js, baseUrl);
                if (!results.empty()) collected.push_back(std::move(results));
            }
            if (!collected.empty()) {
                size_t maxSize = 0;
                for (const auto& v : collected) maxSize = std::max(maxSize, v.size());
                for (size_t i = 0; i < maxSize; ++i) {
                    for (const auto& v : collected) {
                        if (i < v.size()) finalResults.push_back(v[i]);
                    }
                }
            }
        } else if (elementsType == "||") {
            // || 互备：依次尝试，遇到第一个非空结果即停止（对齐 legado）
            for (const auto& r : rules) {
                auto results = applyRuleStatic(content, r, js, baseUrl);
                if (!results.empty()) { finalResults = std::move(results); break; }
            }
        } else {
            // && 或默认：合并语义（对齐 legado AnalyzeByJSoup.getStringList /
            // getElements 对 "&&" 的处理）——每个子规则独立作用于【同一份 content】，
            // 结果【依次拼接】，而非把前一个输出当作后一个输入（管道）。
            //
            // 此前的管道实现是「绝大部分正文取到也点不开/空白」的核心根因：
            // 例如 ruleContent = "class.text-detail@html && class.picture-detail@html"，
            // 管道会先取 text-detail，再在该片段里找 picture-detail（必然找不到）→ 整体空。
            // 正确行为：分别取 text-detail 与 picture-detail，再拼接。
            for (const auto& r : rules) {
                auto results = applyRuleStatic(content, r, js, baseUrl);
                finalResults.insert(finalResults.end(), results.begin(), results.end());
            }
        }
    }

    // 5. 缓存 + 正则替换
    if (!finalResults.empty()) {
        ruleCache.put(cacheKey, finalResults);
    }
    applyRegexReplace(finalResults);

    return finalResults;
}

// ──────────────────────────────────────────────
// 清洗作者字段
// ──────────────────────────────────────────────
std::string cleanAuthorField(const std::string& author) {
    // 与 legado AppPattern.authorRegex 对齐：
    //   ^\s*作\s*者[:：\s]+   — 去除开头的 "作者："/"作者:"/"作 者：" 等
    //   |\s+著                — 去除末尾的 " 著"
    static const std::regex authorRegex(R"(^\s*作\s*者[:：\s]+|\s+著)");
    std::string result = std::regex_replace(author, authorRegex, "");

    // 去除首尾空白
    size_t b = result.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = result.find_last_not_of(" \t\r\n");
    return result.substr(b, e - b + 1);
}

// ──────────────────────────────────────────────
// 编码转换
// ──────────────────────────────────────────────

#ifdef _WIN32

/// ensureUtf8 标准化后的编码名 → Windows 代码页；未知编码返回 0（调用方原样返回）
static UINT encodingToCodePage(const std::string& enc) {
    if (enc == "GBK") return 936;
    if (enc == "BIG5") return 950;
    if (enc == "EUC-KR") return 949;
    if (enc == "SHIFT_JIS") return 932;
    if (enc == "ISO-8859-1") return 28591;
    if (enc == "WINDOWS-1252") return 1252;
    return 0;
}

/// Windows 版编码转换：fromEnc 代码页 → UTF-16 → UTF-8。
/// toEnc 仅支持 UTF-8 —— 与唯一调用点 ensureUtf8 的用法一致。
static std::string iconvConvert(const std::string& data, const char* fromEnc, const char* toEnc = "UTF-8") {
    if (data.empty()) return data;
    if (toEnc == nullptr || std::strcmp(toEnc, "UTF-8") != 0) return data;

    const UINT cp = encodingToCodePage(fromEnc);
    if (cp == 0) return data;  // 不支持的编码，原样返回

    // fromEnc → UTF-16
    const int wideLen = MultiByteToWideChar(cp, 0, data.data(), static_cast<int>(data.size()), nullptr, 0);
    if (wideLen <= 0) return data;
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (MultiByteToWideChar(cp, 0, data.data(), static_cast<int>(data.size()), wide.data(), wideLen) != wideLen) {
        return data;
    }

    // UTF-16 → UTF-8
    const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return data;
    std::string out(static_cast<size_t>(utf8Len), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, out.data(), utf8Len, nullptr, nullptr) != utf8Len) {
        return data;
    }
    return out;
}

#else

/// 使用 iconv 将 fromEnc 编码的 data 转换为 toEnc（通常为 UTF-8）
static std::string iconvConvert(const std::string& data, const char* fromEnc, const char* toEnc = "UTF-8") {
    if (data.empty()) return data;
    iconv_t cd = iconv_open(toEnc, fromEnc);
    if (cd == (iconv_t)-1) return data;  // 不支持的编码，原样返回

    size_t inLeft = data.size();
    size_t outBufSize = inLeft * 2 + 16;  // 输出缓冲区
    std::string out;
    out.resize(outBufSize);

    char* inPtr = const_cast<char*>(data.data());
    char* outPtr = &out[0];
    size_t outLeft = outBufSize;

    size_t ret = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
    iconv_close(cd);

    if (ret == (size_t)-1 && inLeft == data.size()) {
        return data;  // 转换失败，原样返回
    }
    out.resize(outBufSize - outLeft);
    return out;
}

#endif // _WIN32

/// 从字符串中提取 charset 声明（HTML meta 或 XML encoding）
static std::string extractCharset(const std::string& data) {
    // HTML: <meta charset="gbk"> 或 <meta http-equiv="Content-Type" content="text/html; charset=gbk">
    std::regex charsetRe(R"(charset=["']?\s*([a-zA-Z0-9_-]+))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(data.begin(), data.begin() + std::min<size_t>(data.size(), 2048), m, charsetRe)) {
        return m[1].str();
    }
    // XML: <?xml version="1.0" encoding="gbk"?>
    std::regex xmlEncRe(R"(encoding=["']([a-zA-Z0-9_-]+))", std::regex::icase);
    if (std::regex_search(data.begin(), data.begin() + std::min<size_t>(data.size(), 512), m, xmlEncRe)) {
        return m[1].str();
    }
    return "";
}

std::string ensureUtf8(const std::string& data, const std::string& declaredCharset) {
    if (data.empty()) return data;

    // 去除 UTF-8 BOM（\xEF\xBB\xBF），Gumbo 解析器不识别 BOM 前缀，
    // 会导致选择器匹配失败（如经典佛经 .post@all 提取为空）。
    std::string cleaned = data;
    if (cleaned.size() >= 3 &&
        static_cast<unsigned char>(cleaned[0]) == 0xEF &&
        static_cast<unsigned char>(cleaned[1]) == 0xBB &&
        static_cast<unsigned char>(cleaned[2]) == 0xBF) {
        cleaned = cleaned.substr(3);
    }

    if (isValidUtf8(cleaned)) return cleaned;  // 已经是合法 UTF-8

    // 从声明的 charset 或内容中提取编码
    std::string charset = declaredCharset;
    if (charset.empty()) charset = extractCharset(data);

    // 标准化编码名
    std::string enc;
    for (auto& c : charset) enc += std::tolower(static_cast<unsigned char>(c));

    // 常见编码别名映射
    if (enc == "gb2312" || enc == "gb18030" || enc == "gbk" || enc == "gb_2312" || enc == "gbk2312") {
        enc = "GBK";
    } else if (enc == "big5" || enc == "big-5") {
        enc = "BIG5";
    } else if (enc == "euc-kr" || enc == "euc_kr" || enc == "ks_c_5601") {
        enc = "EUC-KR";
    } else if (enc == "shift_jis" || enc == "shift-jis" || enc == "sjis") {
        enc = "SHIFT_JIS";
    } else if (enc == "iso-8859-1" || enc == "latin1" || enc == "latin-1") {
        enc = "ISO-8859-1";
    } else if (enc == "windows-1252" || enc == "cp1252") {
        enc = "WINDOWS-1252";
    } else if (enc.empty() || enc == "utf-8" || enc == "utf8") {
        // 已经是 UTF-8 或无法检测
        if (isValidUtf8(data)) return data;
        // 尝试用 GBK 兜底
        enc = "GBK";
    }

    return iconvConvert(data, enc.c_str());
}

} // namespace detail
} // namespace ariaread

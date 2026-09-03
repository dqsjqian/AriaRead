#include "openread/analyze_url.h"
#include "openread/js_runtime.h"

#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <vector>

namespace openread {

using json = nlohmann::json;

// ──────────────────────────────────────────────
// 构造
// ──────────────────────────────────────────────
AnalyzeUrl::AnalyzeUrl(const std::string& ruleUrl,
                       const std::string& baseUrl,
                       const std::string& key,
                       int page,
                       JsRuntime* js)
    : ruleUrl_(ruleUrl), baseUrl_(baseUrl), key_(key), page_(page), js_(js)
{
    // 清理 baseUrl 中可能含的参数部分
    // paramPattern = \s*,\s*(?=\{)
    // 即匹配逗号后跟 { 的位置，截取逗号之前
    static const std::regex paramPattern(R"(\s*,\s*(?=\{))");
    std::smatch m;
    if (std::regex_search(baseUrl_, m, paramPattern)) {
        baseUrl_ = baseUrl_.substr(0, m.position());
    }

    result_.ruleUrl = ruleUrl;
    result_.baseUrl = baseUrl_;

    // 三步解析
    analyzeJs();
    replaceKeyPageJs();
    analyzeUrl();
}

// ──────────────────────────────────────────────
// 阶段1：执行 @js: / <js></js>
// ──────────────────────────────────────────────
void AnalyzeUrl::analyzeJs() {
    // JS_PATTERN: <js>([\w\W]*?)</js>|@js:([\w\W]*)
    static const std::regex jsPattern(R"(<js>([\w\W]*?)</js>|@js:([\w\W]*))",
                                       std::regex::icase);

    // 先检查是否包含 JS 规则
    if (!std::regex_search(ruleUrl_, jsPattern)) {
        // 没有 JS 规则，直接返回
        return;
    }

    std::string result;
    bool hasJsMatch = false;
    size_t start = 0;
    std::sregex_iterator it(ruleUrl_.begin(), ruleUrl_.end(), jsPattern);
    std::sregex_iterator end;

    for (; it != end; ++it) {
        hasJsMatch = true;
        auto& match = *it;

        // 匹配之前的文本
        if (match.position() > (long)start) {
            std::string prefix = ruleUrl_.substr(start, match.position() - start);
            // trim
            size_t b = prefix.find_first_not_of(" \t\r\n");
            size_t e = prefix.find_last_not_of(" \t\r\n");
            if (b != std::string::npos && e != std::string::npos) {
                std::string trimmed = prefix.substr(b, e - b + 1);
                if (!trimmed.empty()) {
                    if (!result.empty()) {
                        size_t pos;
                        while ((pos = trimmed.find("@result")) != std::string::npos) {
                            trimmed.replace(pos, 7, result);
                        }
                    }
                    // 如果 result 还是空的，prefix 就是初始 result
                    if (result.empty()) {
                        result = trimmed;
                    }
                }
            }
        }

        // 执行 JS
        std::string jsCode = match[2].matched ? match[2].str() : match[1].str();
        if (js_ && !jsCode.empty()) {
            // 注入兼容的变量
            // 注意：不用 IIFE 包裹，直接执行，最后一个表达式的值作为返回值
            // 提供 java 对象的简化桩实现
            std::string stubJava = R"(
var java = {
  put: function(k, v) { if (k === 'url') result = v; },
  get: function(k) { return ''; },
  ajax: function(url) { return ''; },
  ajaxAll: function(urlList) { return []; },
  log: function(msg) { },
  cache: { get: function(k) { return ''; }, put: function(k, v) { } },
  cookie: { getCookie: function(k) { return ''; }, setCookie: function(k, v) { } },
  getString: function(urlStr) { return ''; },
  getByteArray: function(urlStr) { return []; }
};
var source = {
  get: function(k) { return ''; },
  put: function(k, v) { },
  key: '',
  bookSourceUrl: '',
  bookSourceName: ''
};
var cookie = java.cookie;
var cache = java.cache;
)";

            std::string wrappedJs =
                "var baseUrl = " + json(baseUrl_).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
                "var key = " + json(key_).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
                "var page = " + std::to_string(page_) + ";\n"
                "var result = " + json(result).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
                "var url = '';\n"
                + stubJava + "\n"
                "source.key = " + json(key_).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
                "source.bookSourceName = '';\n"
                "source.bookSourceUrl = " + json(baseUrl_).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
                // 执行用户 JS 代码，最后一个表达式的值赋给 __return
                "var __return = (function() {\n"
                + jsCode + "\n"
                "})();\n"
                // 优先级：__return（JS代码返回值）> java.put设置的result > url变量
                "if (__return && typeof __return === 'string') result = __return;\n"
                "else if (result && typeof result === 'string' && result.length > 0) { /* java.put 已设置 */ }\n"
                "else if (url && typeof url === 'string' && url.length > 0) result = url;\n"
                "else result = __return || '';\n"
                "result;\n";

            auto evalResult = js_->eval(wrappedJs);
            if (!evalResult.empty()) {
                result = evalResult;
            }
        }

        start = match.position() + match.length();
    }

    // 处理最后的文本
    if (start < ruleUrl_.size()) {
        std::string tail = ruleUrl_.substr(start);
        size_t b = tail.find_first_not_of(" \t\r\n");
        size_t e = tail.find_last_not_of(" \t\r\n");
        if (b != std::string::npos && e != std::string::npos) {
            std::string trimmed = tail.substr(b, e - b + 1);
            if (!trimmed.empty()) {
                if (!result.empty()) {
                    size_t pos;
                    while ((pos = trimmed.find("@result")) != std::string::npos) {
                        trimmed.replace(pos, 7, result);
                    }
                    result = trimmed;
                } else {
                    result = trimmed;
                }
            }
        }
    }

    // 只有 JS 匹配成功才更新 ruleUrl_
    if (hasJsMatch && !result.empty()) {
        ruleUrl_ = result;
    }
}

// ──────────────────────────────────────────────
// 阶段2：替换 {{key}}, {{page}}, {{内嵌JS}}
// ──────────────────────────────────────────────
void AnalyzeUrl::replaceKeyPageJs() {
    // 1. 替换内嵌 JS {{...}}（非 key/page 的内嵌规则）
    static const std::regex innerJsPattern(R"(\{\{([^\}]+)\}\})");

    std::string url = ruleUrl_;
    std::string newUrl;
    size_t lastPos = 0;
    std::sregex_iterator it(url.begin(), url.end(), innerJsPattern);
    std::sregex_iterator end;

    for (; it != end; ++it) {
        auto& match = *it;
        newUrl += url.substr(lastPos, match.position() - lastPos);

        std::string inner = match[1].str();

        // 先检查是否是 key/page 等简单变量
        if (inner == "key") {
            // 在 replaceKeyPageJs 阶段不做 URL 编码
            // URL 编码在 analyzeUrl 阶段根据上下文处理
            // （URL 路径中需要编码，body 中可能需要也可能不需要）
            newUrl += key_;
        } else if (inner == "page") {
            newUrl += std::to_string(page_);
        } else if (js_ && !inner.empty()) {
            // 内嵌 JS 规则，执行
            std::string wrappedJs =
                "var baseUrl = " + json(baseUrl_).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
                "var key = " + json(key_).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
                "var page = " + std::to_string(page_) + ";\n"
                "var result = '';\n"
                "var java = { put: function(k, v) { }, get: function(k) { return ''; } };\n"
                + inner + "\n";

            auto evalResult = js_->eval(wrappedJs);
            if (!evalResult.empty()) {
                // Double 整数格式化为无小数点
                newUrl += evalResult;
            }
        } else {
            // 无法执行 JS，保留原文本
            newUrl += match[0].str();
        }

        lastPos = match.position() + match.length();
    }
    newUrl += url.substr(lastPos);

    // 2. 替换 <page1,page2,...> 分页格式
    // pagePattern = <(.*?)>
    static const std::regex pagePattern("<(.*?)>");
    std::string finalUrl;
    size_t pos2 = 0;
    std::sregex_iterator it2(newUrl.begin(), newUrl.end(), pagePattern);
    std::sregex_iterator end2;

    for (; it2 != end2; ++it2) {
        auto& match = *it2;
        finalUrl += newUrl.substr(pos2, match.position() - pos2);

        std::string pagesStr = match[1].str();
        // 按 , 分割
        std::vector<std::string> pages;
        std::istringstream iss(pagesStr);
        std::string token;
        while (std::getline(iss, token, ',')) {
            // trim
            size_t b = token.find_first_not_of(" \t");
            size_t e = token.find_last_not_of(" \t");
            if (b != std::string::npos) {
                pages.push_back(token.substr(b, e - b + 1));
            }
        }

        if (!pages.empty()) {
            if (page_ <= (int)pages.size()) {
                finalUrl += pages[page_ - 1];
            } else {
                finalUrl += pages.back();
            }
        }

        pos2 = match.position() + match.length();
    }
    finalUrl += newUrl.substr(pos2);

    ruleUrl_ = finalUrl;
}

// ──────────────────────────────────────────────
// 阶段3：分离 URL 和 options, 拼接 baseUrl
// ──────────────────────────────────────────────
void AnalyzeUrl::analyzeUrl() {
    // 分离 URL 和 options JSON
    // 格式：url,{options} 或 url,{'options'}
    // 查找第一个 ,{ 模式（逗号后紧跟 { 或空白+{）
    std::string urlNoOption = ruleUrl_;
    std::string optionStr;

    // 从后往前找最外层的 { 对应的逗号
    // 策略：找到第一个 ,{ 或 , { 或 ,\n{ 的位置
    for (size_t i = 0; i < ruleUrl_.size(); ++i) {
        if (ruleUrl_[i] == ',') {
            // 检查逗号后面（跳过空白）是否是 {
            size_t j = i + 1;
            while (j < ruleUrl_.size() && (ruleUrl_[j] == ' ' || ruleUrl_[j] == '\t' ||
                   ruleUrl_[j] == '\r' || ruleUrl_[j] == '\n')) {
                ++j;
            }
            if (j < ruleUrl_.size() && ruleUrl_[j] == '{') {
                urlNoOption = ruleUrl_.substr(0, i);
                optionStr = ruleUrl_.substr(j);
                break;
            }
        }
    }

    // URL 拼接：相对路径 → 绝对路径
    result_.url = getAbsoluteURL(baseUrl_, urlNoOption);

    // 对 URL 中的非 ASCII 字符做 URL 编码（中文等）
    {
        std::string encoded;
        for (unsigned char c : result_.url) {
            if (c > 0x7F) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", c);
                encoded += buf;
            } else {
                encoded += static_cast<char>(c);
            }
        }
        result_.url = encoded;
    }

    // 更新 baseUrl
    std::string newBase = extractBaseUrl(result_.url);
    if (!newBase.empty()) {
        baseUrl_ = newBase;
        result_.baseUrl = baseUrl_;
    }

    // 解析 options JSON
    if (!optionStr.empty()) {
        try {
            // 书源中 options 可能使用单引号，需要转换为双引号
            std::string jsonStr = optionStr;
            // 简单的单引号转双引号（注意不要替换字符串值内部的单引号）
            // options 格式通常是：{'method':'POST','body':'...'}
            // 需要转换为标准 JSON
            bool inString = false;
            char stringChar = 0;
            for (size_t i = 0; i < jsonStr.size(); ++i) {
                char c = jsonStr[i];
                if (inString) {
                    if (c == '\\' && i + 1 < jsonStr.size()) {
                        ++i; // 跳过转义字符
                        continue;
                    }
                    if (c == stringChar) {
                        inString = false;
                        if (c == '\'') jsonStr[i] = '"';
                    }
                } else {
                    if (c == '\'' || c == '"') {
                        inString = true;
                        stringChar = c;
                        if (c == '\'') jsonStr[i] = '"';
                    }
                }
            }

            auto opts = json::parse(jsonStr);

            // method
            if (opts.contains("method")) {
                std::string method = opts["method"].get<std::string>();
                std::transform(method.begin(), method.end(), method.begin(), ::toupper);
                result_.method = method;
            }

            // body
            if (opts.contains("body")) {
                auto& bodyVal = opts["body"];
                if (bodyVal.is_string()) {
                    result_.body = bodyVal.get<std::string>();
                } else {
                    result_.body = bodyVal.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                }
            }

            // charset
            if (opts.contains("charset")) {
                result_.charset = opts["charset"].get<std::string>();
            }

            // headers
            if (opts.contains("headers")) {
                auto& hdrs = opts["headers"];
                if (hdrs.is_object()) {
                    for (auto it = hdrs.begin(); it != hdrs.end(); ++it) {
                        result_.headers[it.key()] = it.value().get<std::string>();
                    }
                } else if (hdrs.is_string()) {
                    // headers 可能是 JSON 字符串
                    try {
                        auto hdrMap = json::parse(hdrs.get<std::string>());
                        for (auto it = hdrMap.begin(); it != hdrMap.end(); ++it) {
                            result_.headers[it.key()] = it.value().get<std::string>();
                        }
                    } catch (...) {}
                }
            }

            // webView（暂不实现，记录日志）
            if (opts.contains("webView")) {
                // TODO: WebView 支持
            }

            // webJs（暂不实现）
            if (opts.contains("webJs")) {
                // TODO: webJs 支持
            }

            // js（URL 解析后执行的 JS，结果赋值给 url）
            if (opts.contains("js") && js_) {
                std::string jsCode = opts["js"].get<std::string>();
                if (!jsCode.empty()) {
                    std::string wrappedJs =
                        "var baseUrl = " + json(baseUrl_).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
                        "var key = " + json(key_).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
                        "var page = " + std::to_string(page_) + ";\n"
                        "var url = " + json(result_.url).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
                        + jsCode + "\n";

                    auto evalResult = js_->eval(wrappedJs);
                    if (!evalResult.empty()) {
                        result_.url = evalResult;
                    }
                }
            }

        } catch (const std::exception& e) {
            // options JSON 解析失败，忽略
        }
    }

    // 处理 body 中的 {{key}} / {{page}} 替换
    if (!result_.body.empty()) {
        size_t pos;
        while ((pos = result_.body.find("{{key}}")) != std::string::npos) {
            result_.body.replace(pos, 7, urlEncode(key_, result_.charset));
        }
        while ((pos = result_.body.find("{{page}}")) != std::string::npos) {
            result_.body.replace(pos, 8, std::to_string(page_));
        }
    }
}

// ──────────────────────────────────────────────
// 辅助方法
// ──────────────────────────────────────────────
std::string AnalyzeUrl::getAbsoluteURL(const std::string& baseUrl,
                                        const std::string& relativePath) {
    if (baseUrl.empty()) return relativePath;

    std::string trimmed = relativePath;
    // trim
    size_t b = trimmed.find_first_not_of(" \t\r\n");
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    if (b != std::string::npos) trimmed = trimmed.substr(b, e - b + 1);
    else trimmed.clear();

    // 已经是绝对 URL
    if (trimmed.find("http://") == 0 || trimmed.find("https://") == 0) {
        return trimmed;
    }

    // data: URL 直接返回
    if (trimmed.find("data:") == 0) {
        return trimmed;
    }

    // javascript: 返回空
    if (trimmed.find("javascript:") == 0) {
        return "";
    }

    // 去掉 baseUrl 中的 options 部分（逗号后跟 {）
    std::string cleanBase = baseUrl;
    static const std::regex paramPattern(R"(\s*,\s*(?=\{))");
    std::smatch m;
    if (std::regex_search(cleanBase, m, paramPattern)) {
        cleanBase = cleanBase.substr(0, m.position());
    }

    // 解析 base 的 scheme://authority 与 path
    // 对齐 legado 的 URL(base, rel) 语义：正确处理协议相对 // 、绝对路径 / 、
    // 相对路径与 ../、避免 host 重复（如 //m.zol.com.cn/m.zol.com.cn/...）。
    std::string scheme, authority, basePath;
    {
        size_t schemeEnd = cleanBase.find("://");
        if (schemeEnd != std::string::npos) {
            scheme = cleanBase.substr(0, schemeEnd);  // http / https
            size_t hostStart = schemeEnd + 3;
            size_t pathStart = cleanBase.find('/', hostStart);
            if (pathStart != std::string::npos) {
                authority = cleanBase.substr(hostStart, pathStart - hostStart);
                basePath = cleanBase.substr(pathStart);  // 含前导 /
            } else {
                authority = cleanBase.substr(hostStart);
                basePath = "/";
            }
        }
    }

    // 规范化路径中的 ./ 与 ../（RFC 3986 remove_dot_segments 的简化版）
    auto normalizePath = [](const std::string& path) -> std::string {
        std::vector<std::string> segs;
        size_t i = 0;
        bool absolute = !path.empty() && path[0] == '/';
        while (i < path.size()) {
            size_t slash = path.find('/', i);
            std::string seg = (slash == std::string::npos)
                ? path.substr(i) : path.substr(i, slash - i);
            if (seg == "..") {
                if (!segs.empty()) segs.pop_back();
            } else if (seg != "." && !seg.empty()) {
                segs.push_back(seg);
            } else if (seg.empty() && slash == std::string::npos) {
                // 末尾的 / —— 保留尾随斜杠语义
                segs.push_back("");
            }
            if (slash == std::string::npos) break;
            i = slash + 1;
            if (i == path.size()) { segs.push_back(""); break; }  // 尾随 /
        }
        std::string out = absolute ? "/" : "";
        for (size_t k = 0; k < segs.size(); ++k) {
            out += segs[k];
            if (k + 1 < segs.size()) out += "/";
        }
        return out;
    };

    if (trimmed.empty()) return cleanBase;

    // 协议相对 URL：//host/path → 继承 base 的 scheme
    if (trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '/') {
        if (!scheme.empty()) return scheme + ":" + trimmed;
        return "https:" + trimmed;
    }

    // 无法解析出 scheme/authority 时，退回旧的简单拼接（容错）
    if (scheme.empty() || authority.empty()) {
        if (trimmed[0] == '/') return cleanBase + trimmed;  // 不理想但不崩
        std::string bp = cleanBase;
        size_t ls = bp.rfind('/');
        if (ls != std::string::npos && ls > bp.find("://") + 2) bp = bp.substr(0, ls + 1);
        else bp += '/';
        return bp + trimmed;
    }

    std::string origin = scheme + "://" + authority;

    // 绝对路径：/path → origin + 规范化(path)
    if (trimmed[0] == '/') {
        return origin + normalizePath(trimmed);
    }

    // 相对路径：基于 base 的目录拼接，再规范化 ../
    std::string dir = basePath;
    size_t lastSlash = dir.rfind('/');
    if (lastSlash != std::string::npos) dir = dir.substr(0, lastSlash + 1);
    else dir = "/";
    return origin + normalizePath(dir + trimmed);
}

std::string AnalyzeUrl::urlEncode(const std::string& value, const std::string& charset) {
    // 简单的 URL 编码实现
    std::string result;
    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            result += buf;
        }
    }
    return result;
}

std::string AnalyzeUrl::extractBaseUrl(const std::string& url) {
    // 从完整 URL 提取 scheme://host
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return "";

    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    if (pathStart != std::string::npos) {
        return url.substr(0, pathStart);
    }
    return url;
}

} // namespace openread

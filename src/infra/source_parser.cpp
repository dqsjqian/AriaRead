#include "ariaread/source_parser.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

namespace ariaread {

using json = nlohmann::json;

// ──────────────────────────────────────────────
// 辅助函数
// ──────────────────────────────────────────────
static std::string getString(const json& j, const std::string& key,
                              const std::string& defaultVal = "") {
    if (j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return defaultVal;
}

static int getInt(const json& j, const std::string& key, int defaultVal = 0) {
    if (j.contains(key) && j[key].is_number_integer()) {
        return j[key].get<int>();
    }
    return defaultVal;
}

// ──────────────────────────────────────────────
// 解析搜索规则
// ──────────────────────────────────────────────
static SearchRule parseSearchRule(const json& j) {
    SearchRule rule;
    if (!j.is_object()) return rule;

    rule.bookList = getString(j, "bookList");
    rule.name = getString(j, "name");
    rule.author = getString(j, "author");
    rule.coverUrl = getString(j, "coverUrl");
    rule.bookUrl = getString(j, "bookUrl");
    rule.lastChapter = getString(j, "lastChapter");
    rule.intro = getString(j, "intro");
    rule.kind = getString(j, "kind");
    rule.wordCount = getString(j, "wordCount");

    return rule;
}

// ──────────────────────────────────────────────
// 解析书籍详情规则（ruleBookInfo）
// ──────────────────────────────────────────────
static BookInfoRule parseBookInfoRule(const json& j) {
    BookInfoRule rule;
    if (!j.is_object()) return rule;

    rule.init = getString(j, "init");
    rule.name = getString(j, "name");
    rule.author = getString(j, "author");
    rule.coverUrl = getString(j, "coverUrl");
    rule.intro = getString(j, "intro");
    rule.kind = getString(j, "kind");
    rule.lastChapter = getString(j, "lastChapter");
    rule.wordCount = getString(j, "wordCount");
    rule.tocUrl = getString(j, "tocUrl");

    return rule;
}

// ──────────────────────────────────────────────
// 解析目录规则
// ──────────────────────────────────────────────
static CatalogRule parseCatalogRule(const json& j) {
    CatalogRule rule;
    if (!j.is_object()) return rule;

    rule.chapterList = getString(j, "chapterList");
    rule.chapterName = getString(j, "chapterName");
    rule.chapterUrl = getString(j, "chapterUrl");
    rule.nextPage = getString(j, "nextTocUrl", getString(j, "nextPage"));
    rule.isVip = getString(j, "isVip");
    rule.isVolume = getString(j, "isVolume");
    rule.updateTime = getString(j, "updateTime");
    rule.formatJs = getString(j, "formatJs");

    return rule;
}

// ──────────────────────────────────────────────
// 解析正文规则
// ──────────────────────────────────────────────
static ContentRule parseContentRule(const json& j) {
    ContentRule rule;
    if (!j.is_object()) return rule;

    rule.content = getString(j, "content");
    rule.nextPage = getString(j, "nextPage");
    rule.replace = getString(j, "replace");
    rule.regex = getString(j, "regex");

    return rule;
}

// ──────────────────────────────────────────────
// 解析 HTTP 头
// ──────────────────────────────────────────────
static std::map<std::string, std::string> parseHeaders(const json& j,
                                                         const std::string& key) {
    std::map<std::string, std::string> headers;

    // 书源的 header 字段可能是 JSON 字符串或对象
    if (!j.contains(key)) return headers;

    const auto& h = j[key];
    if (h.is_object()) {
        for (auto it = h.begin(); it != h.end(); ++it) {
            if (it.value().is_string()) {
                headers[it.key()] = it.value().get<std::string>();
            }
        }
    } else if (h.is_string()) {
        try {
            auto parsed = json::parse(h.get<std::string>());
            for (auto it = parsed.begin(); it != parsed.end(); ++it) {
                if (it.value().is_string()) {
                    headers[it.key()] = it.value().get<std::string>();
                }
            }
        } catch (...) {}
    }

    return headers;
}

// ──────────────────────────────────────────────
// 解析单个书源
// ──────────────────────────────────────────────
BookSource SourceParser::parse(const std::string& jsonStr) {
    BookSource source;

    try {
        auto j = json::parse(jsonStr);

        // 基本信息
        source.name = getString(j, "bookSourceName", getString(j, "name"));
        source.url = getString(j, "bookSourceUrl", getString(j, "url"));
        source.group = getString(j, "bookSourceGroup", getString(j, "group"));
        source.icon = getString(j, "bookSourceIcon", getString(j, "icon"));
        source.comment = getString(j, "bookSourceComment", getString(j, "comment"));
        source.enabled = getInt(j, "enabled", 1);
        source.enabledExplore = getInt(j, "enabledExplore", 1);
        source.weight = getInt(j, "weight", 0);

        // 搜索配置
        source.searchUrl = getString(j, "searchUrl");
        if (j.contains("ruleSearch")) {
            source.searchRule = parseSearchRule(j["ruleSearch"]);
        }

        // 发现配置
        source.exploreUrl = getString(j, "exploreUrl");
        if (j.contains("ruleExplore")) {
            source.exploreRule = parseSearchRule(j["ruleExplore"]);
        }

        // 书籍详情配置
        if (j.contains("ruleBookInfo")) {
            source.bookInfoRule = parseBookInfoRule(j["ruleBookInfo"]);
        }

        // 目录配置
        if (j.contains("ruleToc") || j.contains("ruleCatalog")) {
            auto& toc = j.contains("ruleToc") ? j["ruleToc"] : j["ruleCatalog"];
            source.catalogRule = parseCatalogRule(toc);
        }

        // 正文配置
        if (j.contains("ruleContent")) {
            source.contentRule = parseContentRule(j["ruleContent"]);
        }

        // HTTP 配置
        source.headers = parseHeaders(j, "header");
        source.userAgent = getString(j, "userAgent");
        source.loginUrl = getString(j, "loginUrl");
        source.loginUi = getString(j, "loginUi");
        source.loginCheckUrl = getString(j, "loginCheckUrl");
        source.loginCheckJs = getString(j, "loginCheckJs");
        source.timeout = getInt(j, "timeout", 30000);

    } catch (const std::exception&) {
        // 解析失败返回空书源
    }

    return source;
}

// ──────────────────────────────────────────────
// 解析书源数组
// ──────────────────────────────────────────────
std::vector<BookSource> SourceParser::parseArray(const std::string& jsonStr) {
    std::vector<BookSource> sources;

    try {
        auto j = json::parse(jsonStr);

        if (j.is_array()) {
            for (const auto& item : j) {
                auto source = parse(item.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
                if (validate(source)) {
                    sources.push_back(std::move(source));
                }
            }
        } else if (j.is_object()) {
            // 单个书源也接受
            auto source = parse(jsonStr);
            if (validate(source)) {
                sources.push_back(std::move(source));
            }
        }
    } catch (const std::exception&) {
        // Bug3 修复：支持 txt 格式（NDJSON，每行一个 JSON 对象）
        // legado 导出的 txt 书源通常是 NDJSON 格式
        std::istringstream stream(jsonStr);
        std::string line;
        while (std::getline(stream, line)) {
            // 跳过空行和纯空白行
            size_t first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) continue;
            std::string trimmed = line.substr(first);
            // 跳过非 JSON 行（不以 { 开头的行）
            if (trimmed.empty() || trimmed[0] != '{') continue;
            try {
                auto source = parse(trimmed);
                if (validate(source)) {
                    sources.push_back(std::move(source));
                }
            } catch (const std::exception&) {
                // 单行解析失败，跳过继续
            }
        }
    }

    return sources;
}

// ──────────────────────────────────────────────
// 从文件加载
// ──────────────────────────────────────────────
std::vector<BookSource> SourceParser::loadFromFile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) return {};

    std::stringstream buf;
    buf << file.rdbuf();
    return parseArray(buf.str());
}

// ──────────────────────────────────────────────
// 验证
// ──────────────────────────────────────────────

/// 判断 searchUrl 是否"看上去像合法搜索规则"。
/// 规则：legado/阅读 3.0 风格的 searchUrl 必定是 URL（绝对或相对）+ 至少一个动态部分。
/// 动态部分常见形式：
///   - 占位符: {{key}} / {key} / searchKey / {{page}} / {page}
///   - JS 表达式: {{java.put("key",key)}} / {{...js...}}  → 含 `{{` + `}}`
///   - JS 变量替换: ${...}
/// 否则就是静态 URL 或占位符，无法发起搜索。
static bool looksLikeValidSearchUrl(const std::string& s) {
    if (s.empty()) return false;
    if (s.size() < 4) return false;
    // 明显占位符
    if (s == "-" || s == "?" || s == "/" ||
        s == "undefined" || s == "null" || s == "none" || s == "todo") {
        return false;
    }
    // legado JS 表达式块（必有成对 {{ }}）
    if (s.find("{{") != std::string::npos && s.find("}}") != std::string::npos) {
        return true;
    }
    // 经典占位符
    if (s.find("{key}") != std::string::npos) return true;
    if (s.find("searchKey") != std::string::npos) return true;
    if (s.find("{{page}}") != std::string::npos) return true;
    if (s.find("{page}") != std::string::npos) return true;
    // ES6 模板字符串
    if (s.find("${") != std::string::npos) return true;
    return false;
}

bool SourceParser::validate(const BookSource& source) {
    if (source.name.empty() || source.url.empty()) return false;
    // 至少要有一个可用入口（搜索 OR 发现），否则书源无法做任何事
    bool hasUsableSearch = looksLikeValidSearchUrl(source.searchUrl);
    bool hasExplore = !source.exploreUrl.empty() &&
                      source.exploreUrl != "-" &&
                      source.exploreUrl != "undefined";
    return hasUsableSearch || hasExplore;
}

std::string SourceParser::validateDetail(const BookSource& source) {
    std::string errors;
    if (source.name.empty()) errors += "name is empty; ";
    if (source.url.empty()) errors += "url is empty; ";
    bool hasUsableSearch = looksLikeValidSearchUrl(source.searchUrl);
    bool hasExplore = !source.exploreUrl.empty() &&
                      source.exploreUrl != "-" &&
                      source.exploreUrl != "undefined";
    if (!hasUsableSearch && !hasExplore) {
        errors += "no usable searchUrl (must contain {{key}}/searchKey/{key}) and no exploreUrl; ";
    }
    return errors.empty() ? "OK" : errors;
}

// ──────────────────────────────────────────────
// 序列化
// ──────────────────────────────────────────────
std::string SourceParser::serialize(const BookSource& source) {
    json j;
    j["bookSourceName"] = source.name;
    j["bookSourceUrl"] = source.url;
    j["bookSourceGroup"] = source.group;
    j["bookSourceIcon"] = source.icon;
    j["bookSourceComment"] = source.comment;
    j["enabled"] = source.enabled;
    j["enabledExplore"] = source.enabledExplore;
    j["weight"] = source.weight;
    j["searchUrl"] = source.searchUrl;
    j["exploreUrl"] = source.exploreUrl;
    j["timeout"] = source.timeout;

    // 搜索规则
    json searchRule;
    searchRule["bookList"] = source.searchRule.bookList;
    searchRule["name"] = source.searchRule.name;
    searchRule["author"] = source.searchRule.author;
    searchRule["coverUrl"] = source.searchRule.coverUrl;
    searchRule["bookUrl"] = source.searchRule.bookUrl;
    searchRule["lastChapter"] = source.searchRule.lastChapter;
    searchRule["intro"] = source.searchRule.intro;
    j["ruleSearch"] = searchRule;

    // 书籍详情规则
    json bookInfoRule;
    bookInfoRule["init"] = source.bookInfoRule.init;
    bookInfoRule["name"] = source.bookInfoRule.name;
    bookInfoRule["author"] = source.bookInfoRule.author;
    bookInfoRule["coverUrl"] = source.bookInfoRule.coverUrl;
    bookInfoRule["intro"] = source.bookInfoRule.intro;
    bookInfoRule["kind"] = source.bookInfoRule.kind;
    bookInfoRule["lastChapter"] = source.bookInfoRule.lastChapter;
    bookInfoRule["wordCount"] = source.bookInfoRule.wordCount;
    bookInfoRule["tocUrl"] = source.bookInfoRule.tocUrl;
    j["ruleBookInfo"] = bookInfoRule;

    // 目录规则
    json catalogRule;
    catalogRule["chapterList"] = source.catalogRule.chapterList;
    catalogRule["chapterName"] = source.catalogRule.chapterName;
    catalogRule["chapterUrl"] = source.catalogRule.chapterUrl;
    catalogRule["nextTocUrl"] = source.catalogRule.nextPage;
    catalogRule["isVip"] = source.catalogRule.isVip;
    catalogRule["isVolume"] = source.catalogRule.isVolume;
    catalogRule["updateTime"] = source.catalogRule.updateTime;
    catalogRule["formatJs"] = source.catalogRule.formatJs;
    j["ruleToc"] = catalogRule;

    // 正文规则
    json contentRule;
    contentRule["content"] = source.contentRule.content;
    contentRule["nextPage"] = source.contentRule.nextPage;
    contentRule["replace"] = source.contentRule.replace;
    contentRule["regex"] = source.contentRule.regex;
    j["ruleContent"] = contentRule;

    // HTTP 头
    if (!source.headers.empty()) {
        j["header"] = source.headers;
    }

    // HTTP 配置
    if (!source.userAgent.empty()) j["userAgent"] = source.userAgent;
    if (!source.loginUrl.empty()) j["loginUrl"] = source.loginUrl;
    if (!source.loginUi.empty()) j["loginUi"] = source.loginUi;
    if (!source.loginCheckUrl.empty()) j["loginCheckUrl"] = source.loginCheckUrl;
    if (!source.loginCheckJs.empty()) j["loginCheckJs"] = source.loginCheckJs;

    return j.dump(2);
}

std::string SourceParser::serializeArray(const std::vector<BookSource>& sources) {
    json arr = json::array();
    for (const auto& source : sources) {
        arr.push_back(json::parse(serialize(source)));
    }
    return arr.dump(2);
}

} // namespace ariaread

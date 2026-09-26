/// @file engine_rss.cpp
/// @brief D8: RSS 订阅源 —— 抓取、解析、存储

#include "ariaread/engine_impl.h"
#include "ariaread/http_client.h"
#include "ariaread/parallel.h"

#include <cstring>
#include <algorithm>
#include <regex>
#include <chrono>
#include <fstream>
#include <sstream>

namespace ariaread {

using json = nlohmann::json;

// ──────────────────────────────────────────────
// 内部：RSS/Atom 解析
// ──────────────────────────────────────────────

namespace {

/// 从 HTML 内容中提取首图 URL
std::string extractFirstImage(const std::string& html) {
    std::regex imgRe(R"(<img[^>]+src=["']([^"']+)["'])");
    std::smatch m;
    if (std::regex_search(html, m, imgRe)) {
        return m[1].str();
    }
    return "";
}

/// 拼接相对 URL
std::string joinUrl(const std::string& base, const std::string& relative) {
    if (detail::isBlank(relative)) return "";
    return AnalyzeUrl::getAbsoluteURL(base, detail::stripCdata(detail::decodeHtmlEntities(relative)));
}

/// 解析 RSS 2.0 / Atom feed XML
std::vector<RssArticle> parseRssFeed(const std::string& xmlText, const std::string& sourceUrl) {
    std::vector<RssArticle> articles;

    // 简单的基于正则的解析（不引入 XML 库，保持轻量）
    // 先判断是 Atom 还是 RSS
    bool isAtom = xmlText.find("<feed") != std::string::npos ||
                  xmlText.find("xmlns=\"http://www.w3.org/2005/Atom\"") != std::string::npos;

    if (isAtom) {
        // Atom: 提取 entry
        std::regex entryRe(R"(<entry[^>]*>([\s\S]*?)</entry>)");
        std::regex titleRe(R"(<title[^>]*>([\s\S]*?)</title>)");
        std::regex linkRe(R"(<link[^>]*href=["']([^"']+)["'][^>]*>)");
        std::regex publishedRe(R"(<published[^>]*>([\s\S]*?)</published>)");
        std::regex updatedRe(R"(<updated[^>]*>([\s\S]*?)</updated>)");
        std::regex summaryRe(R"(<summary[^>]*>([\s\S]*?)</summary>)");
        std::regex contentRe(R"(<content[^>]*>([\s\S]*?)</content>)");

        std::sregex_iterator it(xmlText.begin(), xmlText.end(), entryRe);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            std::string entry = it->str(1);
            RssArticle art;
            art.sourceUrl = sourceUrl;

            std::smatch m;
            if (std::regex_search(entry, m, titleRe)) {
                art.title = detail::decodeHtmlEntities(detail::stripCdata(m[1].str()));
            }
            const std::regex relRe(R"(\brel\s*=\s*["']([^"']+)["'])");
            for (auto link = std::sregex_iterator(entry.begin(), entry.end(), linkRe);
                 link != std::sregex_iterator(); ++link) {
                std::smatch rel;
                const auto tag = link->str();
                if (!std::regex_search(tag, rel, relRe) || rel[1] == "alternate") {
                    art.link = (*link)[1].str();
                    break;
                }
            }
            // 尝试 published，fallback 到 updated
            std::string dateStr;
            if (std::regex_search(entry, m, publishedRe)) {
                dateStr = m[1].str();
            } else if (std::regex_search(entry, m, updatedRe)) {
                dateStr = m[1].str();
            }
            if (!dateStr.empty()) {
                try {
                    // ISO 8601: 2024-01-15T08:30:00+00:00
                    auto pos = dateStr.find('T');
                    if (pos != std::string::npos) {
                        std::string d = dateStr.substr(0, pos);
                        std::string t = dateStr.substr(pos + 1);
                        int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
                        sscanf(d.c_str(), "%d-%d-%d", &year, &month, &day);
                        sscanf(t.c_str(), "%d:%d:%d", &hour, &min, &sec);
                        std::tm tm = {};
                        tm.tm_year = year - 1900;
                        tm.tm_mon = month - 1;
                        tm.tm_mday = day;
                        tm.tm_hour = hour;
                        tm.tm_min = min;
                        tm.tm_sec = sec;
                        tm.tm_isdst = 0;
                        art.pubDate = static_cast<int64_t>(std::mktime(&tm));
                    }
                } catch (...) {}
            }
            if (std::regex_search(entry, m, contentRe)) {
                art.content = detail::decodeHtmlEntities(detail::stripCdata(m[1].str()));
            } else if (std::regex_search(entry, m, summaryRe)) {
                art.description = detail::decodeHtmlEntities(detail::stripCdata(m[1].str()));
            }
            if (art.content.empty() && art.description.empty()) {
                // 尝试从 entry 中找 description
                std::regex descRe2(R"(<description[^>]*>([\s\S]*?)</description>)");
                if (std::regex_search(entry, m, descRe2)) {
                    art.description = detail::decodeHtmlEntities(detail::stripCdata(m[1].str()));
                }
            }

            // 提取图片
            std::string imgHtml = art.content.empty() ? art.description : art.content;
            art.image = extractFirstImage(imgHtml);
            if (!art.image.empty()) {
                art.image = joinUrl(sourceUrl, art.image);
            }

            if (!art.title.empty() || !art.link.empty()) {
                articles.push_back(std::move(art));
            }
        }
    } else {
        // RSS 2.0
        std::regex itemRe(R"(<item[^>]*>([\s\S]*?)</item>)");
        std::regex titleRe(R"(<title[^>]*>([\s\S]*?)</title>)");
        std::regex linkRe(R"(<link[^>]*>([\s\S]*?)</link>)");
        std::regex pubDateRe(R"(<pubDate[^>]*>([\s\S]*?)</pubDate>)");
        std::regex descRe(R"(<description[^>]*>([\s\S]*?)</description>)");
        std::regex contentEncodedRe(R"(<content:encoded[^>]*>([\s\S]*?)</content:encoded>)");
        std::regex mediaRe(R"(<media:content[^>]*url=["']([^"']+)["'][^>]*>)");
        std::regex enclosureRe(R"(<enclosure[^>]*url=["']([^"']+)["'][^>]*>)");

        std::sregex_iterator it(xmlText.begin(), xmlText.end(), itemRe);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            std::string item = it->str(1);
            RssArticle art;
            art.sourceUrl = sourceUrl;

            std::smatch m;
            if (std::regex_search(item, m, titleRe)) {
                art.title = detail::decodeHtmlEntities(detail::stripCdata(m[1].str()));
            }
            if (std::regex_search(item, m, linkRe)) {
                art.link = detail::decodeHtmlEntities(m[1].str());
            }
            if (std::regex_search(item, m, pubDateRe)) {
                std::string dateStr = m[1].str();
                // RFC 822: Mon, 26 Apr 2026 12:00:00 GMT
                if (!dateStr.empty()) {
                    try {
                        std::tm tm = {};
                        char monthStr[4] = {};
                        int day = 0, year = 0, hour = 0, min = 0, sec = 0;
                        char tz[8] = {};
                        // 格式: Mon, 26 Apr 2026 12:00:00 GMT
                        if (sscanf(dateStr.c_str(), "%*3s, %d %3s %d %d:%d:%d %7s",
                                   &day, monthStr, &year, &hour, &min, &sec, tz) >= 6) {
                            static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                                           "Jul","Aug","Sep","Oct","Nov","Dec"};
                            for (int i = 0; i < 12; ++i) {
                                if (strncmp(monthStr, months[i], 3) == 0) {
                                    tm.tm_mon = i;
                                    break;
                                }
                            }
                            tm.tm_mday = day;
                            tm.tm_year = year - 1900;
                            tm.tm_hour = hour;
                            tm.tm_min = min;
                            tm.tm_sec = sec;
                            tm.tm_isdst = 0;
                            art.pubDate = static_cast<int64_t>(std::mktime(&tm));
                        }
                    } catch (...) {}
                }
            }
            if (std::regex_search(item, m, contentEncodedRe)) {
                art.content = detail::decodeHtmlEntities(detail::stripCdata(m[1].str()));
            }
            if (std::regex_search(item, m, descRe)) {
                art.description = detail::decodeHtmlEntities(detail::stripCdata(m[1].str()));
            }

            // media:content / enclosure
            if (std::regex_search(item, m, mediaRe)) {
                art.image = detail::stripCdata(m[1].str());
            } else if (std::regex_search(item, m, enclosureRe)) {
                art.image = detail::stripCdata(m[1].str());
            }

            // 从 content/description 中提取图片
            if (art.image.empty()) {
                std::string imgHtml = art.content.empty() ? art.description : art.content;
                art.image = extractFirstImage(imgHtml);
            }
            if (!art.image.empty()) {
                art.image = joinUrl(sourceUrl, art.image);
            }

            if (!art.title.empty() || !art.link.empty()) {
                articles.push_back(std::move(art));
            }
        }
    }

    for (auto& article : articles) {
        article.link = joinUrl(sourceUrl, article.link);
        article.image = joinUrl(sourceUrl, article.image);
    }
    return articles;
}

// ──────────────────────────────────────────────
// Legado 规则订阅源解析（对齐 RssParserByRule.kt）
// ──────────────────────────────────────────────

/// 解析 sortUrl，返回 [(名称, url)]，对齐 legado RssSource.sortUrls()。
/// 格式：每行 "名称::url"；无 "::" 则整行作为 url，名称留空；
/// sortUrl 为空时回退到 sourceUrl 单通道。
std::vector<std::pair<std::string, std::string>> parseSortUrls(
        const std::string& sortUrl, const std::string& sourceUrl) {
    std::vector<std::pair<std::string, std::string>> channels;
    std::string s = sortUrl;
    // 去掉首尾空白
    auto isWs = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t b = 0, e = s.size();
    while (b < e && isWs(s[b])) ++b;
    while (e > b && isWs(s[e - 1])) --e;
    s = s.substr(b, e - b);

    if (s.empty()) {
        channels.emplace_back("", sourceUrl);
        return channels;
    }
    // 按行切分
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        // trim
        size_t lb = 0, le = line.size();
        while (lb < le && isWs(line[lb])) ++lb;
        while (le > lb && isWs(line[le - 1])) --le;
        std::string ln = line.substr(lb, le - lb);
        if (ln.empty()) continue;
        auto pos = ln.find("::");
        if (pos != std::string::npos) {
            channels.emplace_back(ln.substr(0, pos), ln.substr(pos + 2));
        } else {
            channels.emplace_back("", ln);
        }
    }
    if (channels.empty()) channels.emplace_back("", sourceUrl);
    return channels;
}

/// 取规则解析结果的第一个非空值
std::string firstOf(const std::vector<std::string>& v) {
    for (const auto& s : v) {
        if (!detail::isBlank(s)) return s;
    }
    return v.empty() ? std::string() : v.front();
}

/// 基于规则解析单篇文章（对齐 legado RssParserByRule.getRssArticle）。
/// @param itemHtml  ruleArticles 切出的单条 item HTML/JSON 片段
/// @param src       订阅源（含各 rule 字段）
/// @param baseUrl   当前页面 URL（用于相对链接绝对化）
/// @param js        JS 运行时
RssArticle parseRssArticleByRule(const std::string& itemHtml,
                                 const RssSource& src,
                                 const std::string& baseUrl,
                                 JsRuntime* js) {
    RssArticle art;
    art.sourceUrl = src.sourceUrl;

    auto apply = [&](const std::string& rule) -> std::vector<std::string> {
        if (detail::isBlank(rule)) return {};
        return detail::applyRuleStatic(itemHtml, rule, js, baseUrl);
    };

    if (!detail::isBlank(src.ruleTitle)) {
        art.title = detail::decodeHtmlEntities(firstOf(apply(src.ruleTitle)));
    }
    if (!detail::isBlank(src.rulePubDate)) {
        // legado RssArticle.pubDate 是字符串原文；AriaRead 的 pubDate 是 int64 时间戳。
        // 这里尝试从规则文本中提取 yyyy-mm-dd 解析为时间戳；失败则保持 0
        // （前端仍可按入库时间 createdAt 排序，不影响展示）。
        std::string dateText = firstOf(apply(src.rulePubDate));
        if (!dateText.empty()) {
            std::smatch dm;
            std::regex ymd(R"((\d{4})-(\d{1,2})-(\d{1,2}))");
            if (std::regex_search(dateText, dm, ymd)) {
                std::tm tm = {};
                tm.tm_year = std::atoi(dm[1].str().c_str()) - 1900;
                tm.tm_mon  = std::atoi(dm[2].str().c_str()) - 1;
                tm.tm_mday = std::atoi(dm[3].str().c_str());
                tm.tm_isdst = 0;
                art.pubDate = static_cast<int64_t>(std::mktime(&tm));
            }
        }
    }
    // 链接
    std::string rawLink;
    if (!detail::isBlank(src.ruleLink)) {
        rawLink = detail::extractFieldWithTemplateFallback(
            itemHtml, src.ruleLink,
            [&](const std::string& content, const std::string& r) {
                return detail::applyRuleStatic(content, r, js, baseUrl);
            });
    }
    art.link = joinUrl(baseUrl, rawLink);
    if (art.link.empty()) {
        art.link = detail::extractHrefFallback(itemHtml, baseUrl, js);
    }
    // 描述
    if (!detail::isBlank(src.ruleDescription)) {
        art.description = detail::decodeHtmlEntities(firstOf(apply(src.ruleDescription)));
    }
    // 图片
    if (!detail::isBlank(src.ruleImage)) {
        std::string rawImg = firstOf(apply(src.ruleImage));
        art.image = detail::resolveUrlWithBase(rawImg, baseUrl, js);
        if (art.image.empty()) art.image = rawImg;
    }
    if (art.image.empty()) {
        std::string imgHtml = art.description.empty() ? itemHtml : art.description;
        std::string img = extractFirstImage(imgHtml);
        if (!img.empty()) img = joinUrl(baseUrl, img);
        art.image = img;
    }
    return art;
}

/// 判断订阅源是否为「规则源」（含 ruleArticles，需要按规则解析），
/// 对应 legado 中 ruleArticles 非空的 RssSource。
inline bool isRuleSource(const RssSource& src) {
    return !detail::isBlank(src.ruleArticles);
}

// Each operation owns its JS context: globals, headers and AJAX callbacks cannot leak
// between sources or outlive captured stack variables.
class RssPipeline {
public:
    JsRuntime js;
    RssSource source;
    HttpClientFunc client;
    HttpRequestFunc legacy;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int requests = 0;
    std::string failure;
    std::map<std::string, std::string> headers;

    RssPipeline(const RssSource& src, HttpClientFunc http, HttpRequestFunc old)
        : source(src), client(std::move(http)), legacy(std::move(old)) {
        js.setMemoryLimit(64 * 1024 * 1024);
        js.setStackSize(2 * 1024 * 1024);
        js.setExecutionTimeout(3000);
        js.setInterruptCallback([this] { return !failure.empty() || std::chrono::steady_clock::now() >= deadline; });
        js.setSelectorFunc([](const std::string& body, const std::string& rule) {
            return detail::applyRuleStatic(body, rule, nullptr, "");
        });
        js.putVariable("source_sourceComment", src.sourceComment);
        js.putVariable("source_sourceUrl", src.sourceUrl);
        js.setHttpFunc([this](const std::string& url, const std::string& method,
                              const std::string& h, const std::string& body) {
            HttpRequest req;
            req.url = AnalyzeUrl::getAbsoluteURL(source.sourceUrl, url);
            req.method = method.empty() ? "GET" : method;
            req.body = body;
            mergeHeaders(req.headers, h);
            return send(req).body;
        });
        std::string header = src.header;
        if (header.rfind("@js:", 0) == 0) header = js.eval(header.substr(4));
        mergeHeaders(headers, header);
        if (!detail::isBlank(src.jsLib)) {
            js.eval(src.jsLib);
            if (!js.getLastError().empty()) throw std::runtime_error("RSS jsLib: " + js.getLastError());
        }
    }

    static void mergeHeaders(std::map<std::string, std::string>& target, const std::string& text) {
        if (detail::isBlank(text)) return;
        auto j = json::parse(text);
        if (!j.is_object()) throw std::runtime_error("RSS 请求头必须为 JSON 对象");
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) target[it.key()] = it.value().get<std::string>();
        }
    }

    HttpResponse send(HttpRequest req) {
        try {
            check();
            if (++requests > 20) throw std::runtime_error("RSS 请求超过 20 次限制");
            if (req.url.rfind("http://", 0) != 0 && req.url.rfind("https://", 0) != 0)
                throw std::runtime_error("RSS 链接不是可请求的 HTTP/HTTPS 地址");
            auto overrides = req.headers;
            req.headers = {{"User-Agent", "Mozilla/5.0"}, {"Accept", "application/rss+xml,application/atom+xml,text/html,*/*"},
                           {"Accept-Encoding", "identity"}};
            for (const auto& [k, v] : headers) req.headers[k] = v;
            for (const auto& [k, v] : overrides) req.headers[k] = v;
            req.timeoutMs = std::max(1, std::min(10000, static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count())));
            req.maxResponseBytes = 4 * 1024 * 1024;
            HttpResponse response;
            if (client) response = client(req);
            else if (legacy) { response.statusCode = 200; response.body = legacy(req.url, req.method, json(req.headers).dump(), req.body); }
            else {
                auto http = createDefaultHttpClient();
                if (!http) throw std::runtime_error("HTTP 客户端不可用");
                response = http(req);
            }
            if (!response.error.empty()) throw std::runtime_error(response.error);
            if (response.statusCode < 200 || response.statusCode >= 300)
                throw std::runtime_error("HTTP " + std::to_string(response.statusCode));
            if (response.body.size() > req.maxResponseBytes) throw std::runtime_error("RSS 响应超过 4 MiB 限制");
            response.body = detail::ensureUtf8(response.body);
            if (detail::isBlank(response.body)) throw std::runtime_error("服务器返回空内容");
            if (response.effectiveUrl.empty()) response.effectiveUrl = req.url;
            check();
            return response;
        } catch (const std::exception& error) {
            failure = error.what();
            throw;
        }
    }

    HttpResponse fetch(const std::string& url, const std::string& base) {
        auto ruleUrl = url;
        if (ruleUrl.rfind("@js:", 0) == 0 || ruleUrl.rfind("<js>", 0) == 0) {
            auto script = ruleUrl.substr(4);
            if (ruleUrl.rfind("<js>", 0) == 0) {
                const auto end = script.rfind("</js>");
                if (end != std::string::npos) script.resize(end);
            }
            ruleUrl = js.evalRuleJs(script, "", base);
            check();
            if (!js.getLastError().empty()) throw std::runtime_error("RSS URL 规则: " + js.getLastError());
            if (detail::isBlank(ruleUrl)) throw std::runtime_error("RSS URL 规则返回空地址");
        }
        AnalyzeUrl analyzer(ruleUrl, base, "", 1, &js);
        check();
        if (!js.getLastError().empty()) throw std::runtime_error("RSS URL 规则: " + js.getLastError());
        const auto& parsed = analyzer.result();
        HttpRequest req;
        req.url = parsed.url; req.method = parsed.method; req.body = parsed.body;
        req.headers = parsed.headers; req.charset = parsed.charset;
        return send(req);
    }

    std::vector<std::string> apply(const std::string& body, const std::string& rule, const std::string& base) {
        auto result = detail::applyRuleStatic(body, rule, &js, base);
        check();
        if (!js.getLastError().empty()) throw std::runtime_error("RSS 规则: " + js.getLastError());
        return result;
    }

    void check() const {
        if (!failure.empty()) throw std::runtime_error(failure);
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("RSS 操作超过 30 秒限制");
    }
};

std::string downloadRss(const std::string& url, HttpClientFunc client, HttpRequestFunc legacy) {
    RssSource source; source.sourceUrl = url;
    RssPipeline pipeline(source, std::move(client), std::move(legacy));
    return pipeline.fetch(url, url).body;
}

/// 从完整网页 HTML 中提取「正文主体」HTML（轻量可读性提取）。
/// 优先级：<article> > <main> > id/class 含 content|article|post|entry 的容器 > <body>。
/// 同时剔除 script/style/nav/header/footer/aside 等非正文噪声，并把相对资源 URL 绝对化，
/// 保留 img/video/iframe 等富媒体标签，供前端 iframe 沙箱渲染完整原文（图文+视频）。
/// 对齐 legado「ruleContent 为空时点进去看原文网页」的体验。
std::string normalizeRssHtml(std::string body, const std::string& baseUrl) {
    // 懒加载图片回填：许多图片站把真实地址放在 data-src/data-original/realsrc/
    // data-lazy-src 等属性，src 只是占位（loading.gif）。把首个懒加载属性回填到 src，
    // 否则前端渲染只会看到占位图或空白（tuiimg.com / mmonly.cc 等）。
    {
        std::regex imgTag(R"(<img\b[^>]*>)", std::regex::icase);
        std::regex lazyAttr(
            R"((?:data-src|data-original|data-lazy-src|data-lazyload|realsrc|data-echo)\s*=\s*(["'])([^"']+)\1)",
            std::regex::icase);
        std::string rebuilt;
        auto begin = std::sregex_iterator(body.begin(), body.end(), imgTag);
        auto end = std::sregex_iterator();
        size_t last = 0;
        for (auto it = begin; it != end; ++it) {
            rebuilt.append(body, last, it->position() - last);
            std::string tag = it->str();
            std::smatch lm;
            if (std::regex_search(tag, lm, lazyAttr)) {
                std::string realUrl = lm[2].str();
                // 用真实地址覆盖/插入 src
                std::regex srcAttr(R"(\ssrc\s*=\s*(["'])[^"']*\1)", std::regex::icase);
                if (std::regex_search(tag, srcAttr)) {
                    tag = std::regex_replace(tag, srcAttr, " src=\"" + realUrl + "\"");
                } else {
                    tag.insert(4, " src=\"" + realUrl + "\"");  // 紧跟 "<img"
                }
            }
            rebuilt.append(tag);
            last = it->position() + it->length();
        }
        rebuilt.append(body, last, std::string::npos);
        body.swap(rebuilt);
    }

    // Resolve both root-relative and directory-relative media/link URLs.
    const std::regex attr(R"((\s)(src|href|poster)\s*=\s*(?:(["'])([^"']*)\3|([^\s"'=<>`]+)))", std::regex::icase);
    std::string resolved;
    size_t last = 0;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), attr); it != std::sregex_iterator(); ++it) {
        resolved.append(body, last, it->position() - last);
        auto url = joinUrl(baseUrl, (*it)[4].matched ? (*it)[4].str() : (*it)[5].str());
        resolved += " " + (*it)[2].str() + "=\"";
        for (char c : url) {
            if (c == '&') resolved += "&amp;";
            else if (c == '\"') resolved += "&quot;";
            else if (c == '<') resolved += "&lt;";
            else resolved += c;
        }
        resolved += "\"";
        last = it->position() + it->length();
    }
    resolved.append(body, last, std::string::npos);
    return resolved;
}

std::string extractReadableHtml(const std::string& html, const std::string& baseUrl) {
    if (html.empty()) return "";

    auto pickBlock = [&](const std::string& tag) -> std::string {
        std::regex re("<" + tag + "[^>]*>([\\s\\S]*?)</" + tag + ">",
                      std::regex::icase);
        std::smatch m;
        // 取最长的一个匹配（正文容器通常最大）
        std::string best;
        auto it = std::sregex_iterator(html.begin(), html.end(), re);
        auto end = std::sregex_iterator();
        for (; it != end; ++it) {
            std::string cand = it->str(1);
            if (cand.size() > best.size()) best = cand;
        }
        return best;
    };

    std::string body = pickBlock("article");
    if (body.empty()) {
        std::string mainBlk = pickBlock("main");
        if (mainBlk.size() > body.size()) body = mainBlk;
    }
    if (body.empty()) {
        // 退到 <body>
        std::string b = pickBlock("body");
        if (b.size() > body.size()) body = b;
    }
    if (body.empty()) body = html;

    // 剔除噪声标签（含内容）
    static const char* dropTags[] = {"script", "style", "noscript", "nav",
                                     "header", "footer", "aside", "form"};
    for (const char* t : dropTags) {
        std::regex re(std::string("<") + t + "[^>]*>[\\s\\S]*?</" + t + ">",
                      std::regex::icase);
        body = std::regex_replace(body, re, "");
    }
    // 剔除注释
    body = std::regex_replace(body, std::regex("<!--[\\s\\S]*?-->"), "");

    return normalizeRssHtml(body, baseUrl);
}

bool hasReadableContent(const std::string& html) {
    auto clean = std::regex_replace(html, std::regex(R"(<(script|style|head|noscript)\b[^>]*>[\s\S]*?</\1>)", std::regex::icase), "");
    auto text = std::regex_replace(clean, std::regex("<[^>]*>"), "");
    text = detail::decodeHtmlEntities(text);
    return !detail::isBlank(text) || std::regex_search(clean, std::regex(R"(<(img|video|audio|source)\b[^>]*\s(src|poster)\s*=\s*["']\s*[^\s"'][^"']*["'])", std::regex::icase));
}

bool hasPasswordField(const std::string& html) {
    static const std::regex input(R"(<input\b[^>]*type\s*=\s*["']?password\b)", std::regex::icase);
    return std::regex_search(html, input);
}

bool hasPublicArticleBlock(const std::string& html) {
    static const std::regex block(R"(<(article|main)\b[^>]*>([\s\S]*?)</\1>)", std::regex::icase);
    for (auto it = std::sregex_iterator(html.begin(), html.end(), block); it != std::sregex_iterator(); ++it) {
        const auto content = it->str(2);
        if (!hasPasswordField(content) && hasReadableContent(content)) return true;
    }
    return false;
}

std::vector<RssArticle> collectRssArticles(RssPipeline& pipeline, std::string& diagnostic) {
    const auto& src = pipeline.source;
    std::vector<RssArticle> articles;
    int64_t order = 0;
    auto sort = src.sortUrl;
    if (sort.rfind("@js:", 0) == 0 || sort.rfind("<js>", 0) == 0) {
        auto script = sort.substr(4);
        if (sort.rfind("<js>", 0) == 0) {
            const auto end = script.rfind("</js>");
            if (end != std::string::npos) script.resize(end);
        }
        sort = pipeline.js.evalRuleJs(script, "", src.sourceUrl);
        if (!pipeline.js.getLastError().empty()) throw std::runtime_error("RSS 分类规则: " + pipeline.js.getLastError());
    }
    const auto channels = parseSortUrls(sort, src.sourceUrl);
    for (const auto& [name, url] : channels) {
        try {
            auto response = pipeline.fetch(url, src.sourceUrl);
            std::vector<RssArticle> parsed;
            if (isRuleSource(src)) {
                auto rule = src.ruleArticles;
                const bool reverse = rule[0] == '-';
                if (reverse) rule.erase(0, 1);
                auto items = pipeline.apply(response.body, rule, response.effectiveUrl);
                if (items.size() == 1) {
                    auto array = json::parse(items[0], nullptr, false);
                    if (array.is_array()) {
                        items.clear();
                        for (const auto& item : array) items.push_back(item.is_string() ? item.get<std::string>() : item.dump());
                    }
                }
                for (const auto& item : items) {
                    auto article = parseRssArticleByRule(item, src, response.effectiveUrl, &pipeline.js);
                    pipeline.check();
                    if (!pipeline.js.getLastError().empty()) throw std::runtime_error(pipeline.js.getLastError());
                    if (detail::isBlank(article.title)) continue;
                    parsed.push_back(std::move(article));
                }
                if (reverse) std::reverse(parsed.begin(), parsed.end());
            } else {
                parsed = parseRssFeed(response.body, response.effectiveUrl);
            }
            if (parsed.empty()) throw std::runtime_error(isRuleSource(src) ? "列表规则未解析到有效文章，请检查列表与标题规则" : "未解析到 RSS/Atom 文章（空订阅或响应格式不匹配）");
            for (auto& article : parsed) {
                article.sourceUrl = src.sourceUrl;
                article.order = ++order;
                if (detail::isBlank(article.link)) article.link = src.sourceUrl + "#item-" + std::to_string(order);
                articles.push_back(std::move(article));
            }
        } catch (const std::exception& error) {
            if (!diagnostic.empty()) diagnostic += "; ";
            diagnostic += (name.empty() ? "订阅列表" : name) + ": " + error.what();
            // A failed channel must not poison independent later channels.
            pipeline.failure.clear();
        }
    }
    return articles;
}

RssContentResult loadRssContent(RssPipeline& pipeline, const RssArticle& article) {
    RssContentResult result;
    const auto& src = pipeline.source;
    const bool placeholder = article.link.empty() || article.link.find("#item-") != std::string::npos;
    if (!placeholder) {
        AnalyzeUrl url(article.link, src.sourceUrl, "", 1, &pipeline.js);
        if (url.result().url.rfind("http://", 0) == 0 || url.result().url.rfind("https://", 0) == 0)
            result.originalUrl = url.result().url;
    }
    const auto inlineBase = result.originalUrl.empty() ? src.sourceUrl : result.originalUrl;
    const auto inlineContent = normalizeRssHtml(article.content, inlineBase);
    const auto inlineDescription = normalizeRssHtml(article.description, inlineBase);
    if (hasReadableContent(inlineContent)) { result.content = inlineContent; return result; }
    // Legado descriptions can be complete inline content. Explicit content rules take
    // precedence over a summary, while standard feeds do not need a second download.
    if (detail::isBlank(src.ruleContent) && hasReadableContent(inlineDescription)) {
        result.content = inlineDescription; return result;
    }
    if (placeholder && detail::isBlank(src.ruleContent) && !article.image.empty()) {
        result.content = normalizeRssHtml("<img src=\"" + article.image + "\">", inlineBase);
        if (hasReadableContent(result.content)) return result;
        result.content.clear();
    }
    std::string downloaded;
    std::string downloadedUrl;
    bool requiresLogin = false;
    try {
        if (placeholder && detail::isBlank(src.ruleContent)) throw std::runtime_error("文章没有正文或有效原文链接");
        auto response = pipeline.fetch(placeholder ? src.sourceUrl : article.link, src.sourceUrl);
        if (!placeholder) result.originalUrl = response.effectiveUrl;
        downloaded = response.body;
        downloadedUrl = response.effectiveUrl;
        // A site-wide login widget does not make a public article login-only.
        requiresLogin = hasPasswordField(downloaded) && !hasPublicArticleBlock(downloaded);
        if (!detail::isBlank(src.ruleContent)) {
            auto parts = pipeline.apply(response.body, src.ruleContent, response.effectiveUrl);
            for (const auto& part : parts) { if (!result.content.empty()) result.content += "\n"; result.content += part; }
        } else {
            result.content = extractReadableHtml(response.body, response.effectiveUrl);
        }
        result.content = normalizeRssHtml(result.content, response.effectiveUrl);
        // An explicit article selector can identify public content in non-semantic
        // containers, but a broad body rule containing the login form cannot.
        if (!detail::isBlank(src.ruleContent) && hasReadableContent(result.content) && !hasPasswordField(result.content))
            requiresLogin = false;
        if (requiresLogin) throw std::runtime_error("页面要求登录，请在原网页中查看");
        if (!hasReadableContent(result.content)) {
            result.content.clear();
            throw std::runtime_error("未提取到正文，可能需要登录、浏览器脚本或更新正文规则");
        }
    } catch (const std::exception& error) {
        result.error = std::string("正文加载失败: ") + error.what();
        result.content = hasReadableContent(inlineDescription) ? inlineDescription : "";
        if (result.content.empty() && !downloaded.empty() && !requiresLogin) {
            auto fallback = extractReadableHtml(downloaded, downloadedUrl);
            if (hasReadableContent(fallback)) result.content = std::move(fallback);
        }
    }
    return result;
}

} // anonymous namespace

// ──────────────────────────────────────────────
// 引擎 RSS 接口实现
// ──────────────────────────────────────────────

void BookSourceEngine::upsertRssSource(const RssSource& source) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return;
    }
    pImpl->db->upsertRssSource(source);
}

std::vector<RssSource> BookSourceEngine::getRssSources() {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return {};
    }
    return pImpl->db->getAllRssSources();
}

bool BookSourceEngine::removeRssSource(const std::string& sourceUrl) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return false;
    }
    return pImpl->db->removeRssSource(sourceUrl);
}

std::vector<std::string> BookSourceEngine::checkRssSources(
    std::function<void(int done, int total, const std::string& current, bool ok)> progressCallback) {
    std::vector<std::string> removed;
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return removed;
    }
    auto sources = pImpl->db->getAllRssSources();
    int total = static_cast<int>(sources.size());
    if (total == 0) return removed;

    std::mutex removedMutex;
    std::atomic<int> doneCount{0};
    const int concurrency = 20;

    detail::parallelForEach(sources.size(), concurrency, [&](std::size_t i) {
        const auto& s = sources[i];
        bool ok = false;

        if (pImpl->httpClientFunc) {
            HttpRequest req;
            req.url = s.sourceUrl;
            req.method = "HEAD";
            req.timeoutMs = 3000;
            auto resp = pImpl->httpClientFunc(req);
            ok = (resp.statusCode >= 200 && resp.statusCode < 400);
            if (!ok && resp.statusCode == 405) {
                req.method = "GET";
                req.timeoutMs = 3000;
                resp = pImpl->httpClientFunc(req);
                ok = (resp.statusCode >= 200 && resp.statusCode < 400);
            }
        }

        if (!ok) {
            std::lock_guard<std::mutex> lock(removedMutex);
            if (pImpl->db->removeRssSource(s.sourceUrl)) {
                removed.push_back(s.sourceUrl);
            }
        }

        int done = ++doneCount;
        if (progressCallback) {
            progressCallback(done, total, s.sourceUrl, ok);
        }
    });
    return removed;
}

void BookSourceEngine::checkRssSourcesRated(
    std::function<void(int, int, const std::string&, const std::string&, int)> progressCallback) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return;
    }
    auto sources = pImpl->db->getAllRssSources();
    int total = static_cast<int>(sources.size());
    if (total == 0) return;

    std::atomic<int> doneCount{0};
    std::mutex dbMutex;
    detail::parallelForEach(sources.size(), 4, [&](std::size_t i) {
        auto source = sources[i];
        const auto start = std::chrono::steady_clock::now();
        bool hasArticles = false;
        bool readable = false;
        try {
            RssPipeline pipeline(source, pImpl->httpClientFunc, pImpl->httpFunc);
            std::string diagnostic;
            auto articles = collectRssArticles(pipeline, diagnostic);
            hasArticles = !articles.empty();
            readable = hasArticles && diagnostic.empty();
            // Sample the first two entries through the same path the reader uses.
            for (size_t n = 0; n < std::min<size_t>(2, articles.size()); ++n) {
                RssPipeline reader(source, pImpl->httpClientFunc, pImpl->httpFunc);
                reader.deadline = pipeline.deadline;
                auto content = loadRssContent(reader, articles[n]);
                if (!content.error.empty() || !hasReadableContent(content.content)) readable = false;
            }
        } catch (...) { readable = false; }
        source.latencyMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
        source.validity = !hasArticles ? "invalid" : !readable ? "poor" :
            source.latencyMs < 1000 ? "excellent" : source.latencyMs < 3000 ? "good" : "poor";
        {
            std::lock_guard<std::mutex> lock(dbMutex);
            pImpl->db->upsertRssSource(source);
        }
        const int done = ++doneCount;
        if (progressCallback) progressCallback(done, total, source.sourceName.empty() ? source.sourceUrl : source.sourceName,
                                               source.validity, source.latencyMs);
    });
}

int BookSourceEngine::clearInvalidRssSources() {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return 0;
    }
    auto sources = pImpl->db->getAllRssSources();
    int removed = 0;
    for (const auto& s : sources) {
        if (s.validity == "invalid") {
            if (pImpl->db->removeRssSource(s.sourceUrl)) ++removed;
        }
    }
    return removed;
}

RssFetchResult BookSourceEngine::fetchRssSource(const std::string& sourceUrl) {
    RssFetchResult result;
    if (!pImpl->db) { result.error = "Database not initialized"; return result; }
    try {
        auto source = pImpl->db->getRssSourceByUrl(sourceUrl);
        if (!source) { result.error = "订阅源不存在"; return result; }
        RssPipeline pipeline(*source, pImpl->httpClientFunc, pImpl->httpFunc);
        auto articles = collectRssArticles(pipeline, result.error);
        for (auto& article : articles) {
            pImpl->db->upsertRssArticle(article);
            ++result.inserted;
        }
        result.count = static_cast<int>(articles.size());
        if (!articles.empty()) {
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            pImpl->db->updateRssSourceLastUpdateTime(sourceUrl, now);
        }
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}

RssArticleListResult BookSourceEngine::getRssArticles(const std::string& sourceUrl, int page, int pageSize) {
    RssArticleListResult result;
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        result.error = pImpl->lastError;
        return result;
    }
    page = std::max(1, page);
    pageSize = std::max(1, std::min(pageSize, 200));
    result.articles = pImpl->db->getRssArticles(sourceUrl, page, pageSize, result.total);
    result.page = page;
    result.pageSize = pageSize;
    return result;
}

RssArticleListResult BookSourceEngine::loadRssArticles(const std::string& sourceUrl, int page, int pageSize) {
    auto result = getRssArticles(sourceUrl, page, pageSize);
    // Imported update timestamps do not prove that this device has cached articles.
    // An empty later page must not trigger another network refresh.
    if (!result.error.empty() || result.total > 0 || sourceUrl.empty()) return result;
    const auto fetched = fetchRssSource(sourceUrl);
    result = getRssArticles(sourceUrl, page, pageSize);
    result.error = fetched.error;
    return result;
}

RssArticle BookSourceEngine::getRssArticle(int64_t id) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return {};
    }
    return pImpl->db->getRssArticle(id);
}

/// 获取单篇文章的正文内容（懒加载：点击时才抓取）
std::string BookSourceEngine::getRssArticleContent(int64_t articleId) {
    return getRssArticleContentResult(articleId).content;
}

RssContentResult BookSourceEngine::getRssArticleContentResult(int64_t articleId) {
    if (!pImpl->db) return {"", "Database not initialized", ""};
    auto article = pImpl->db->getRssArticle(articleId);
    if (!article.id) return {"", "文章不存在", ""};
    auto source = pImpl->db->getRssSourceByUrl(article.sourceUrl);
    if (!source) return {article.content, "订阅源不存在", ""};
    try {
        RssPipeline pipeline(*source, pImpl->httpClientFunc, pImpl->httpFunc);
        return loadRssContent(pipeline, article);
    } catch (const std::exception& error) { return {"", error.what(), ""}; }
}

void BookSourceEngine::clearAllRss() {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return;
    }
    pImpl->db->clearAllRss();
}

std::pair<int, std::string> BookSourceEngine::importRssSourcesFromFile(const std::string& filePath) {
    if (!pImpl->db) {
        return {0, "Database not initialized"};
    }
    try {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            return {0, "Failed to open file: " + filePath};
        }
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        return importRssSourcesFromJson(content);
    } catch (const std::exception& e) {
        return {0, std::string("File read error: ") + e.what()};
    }
}

std::pair<int, std::string> BookSourceEngine::importRssSourcesFromUrl(const std::string& url) {
    if (!pImpl->db) {
        return {0, "Database not initialized"};
    }

    // 下载 JSON
    std::string jsonText = downloadRss(url, pImpl->httpClientFunc, pImpl->httpFunc);
    if (jsonText.empty()) {
        return {0, "下载失败，请检查 URL 和网络"};
    }

    // 检查是否是 sourceUrls 包装对象
    try {
        auto j = json::parse(jsonText);
        if (j.is_object() && j.contains("sourceUrls") && j["sourceUrls"].is_array()) {
            int totalCount = 0;
            std::string lastError;
            for (const auto& u : j["sourceUrls"]) {
                if (!u.is_string()) continue;
                auto subUrl = u.get<std::string>();
                auto subJson = downloadRss(subUrl, pImpl->httpClientFunc, pImpl->httpFunc);
                if (!subJson.empty()) {
                    auto [c, err] = importRssSourcesFromJson(subJson);
                    if (c >= 0) totalCount += c;
                    if (!err.empty()) lastError = err;
                }
            }
            return {totalCount, lastError};
        }
    } catch (...) {}

    return importRssSourcesFromJson(jsonText);
}

std::pair<int, std::string> BookSourceEngine::importRssSourcesFromJson(const std::string& jsonText) {
    if (!pImpl->db) {
        return {0, "Database not initialized"};
    }
    return pImpl->db->importRssSourcesFromJson(jsonText);
}

} // namespace ariaread

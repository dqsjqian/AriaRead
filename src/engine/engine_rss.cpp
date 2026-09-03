/// @file engine_rss.cpp
/// @brief D8: RSS 订阅源 —— 抓取、解析、存储

#include "openread/engine_impl.h"
#include "openread/http_client.h"
#include "openread/parallel.h"

#include <cstring>
#include <algorithm>
#include <regex>
#include <chrono>
#include <fstream>
#include <sstream>

namespace openread {

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
    if (relative.empty()) return "";
    if (relative.find("http://") == 0 || relative.find("https://") == 0) {
        return relative;
    }
    if (relative.empty()) return base;
    if (!base.empty() && base.back() == '/' && !relative.empty() && relative.front() == '/') {
        return base + relative.substr(1);
    }
    if (!base.empty() && base.back() != '/' && !relative.empty() && relative.front() != '/') {
        return base + "/" + relative;
    }
    return base + relative;
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
                art.title = detail::decodeHtmlEntities(m[1].str());
            }
            if (std::regex_search(entry, m, linkRe)) {
                art.link = detail::stripCdata(m[1].str());
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
                art.content = detail::decodeHtmlEntities(m[1].str());
            } else if (std::regex_search(entry, m, summaryRe)) {
                art.description = detail::decodeHtmlEntities(m[1].str());
            }
            if (art.content.empty() && art.description.empty()) {
                // 尝试从 entry 中找 description
                std::regex descRe2(R"(<description[^>]*>([\s\S]*?)</description>)");
                if (std::regex_search(entry, m, descRe2)) {
                    art.description = detail::decodeHtmlEntities(m[1].str());
                }
            }

            // 提取图片
            std::string imgHtml = art.content.empty() ? art.description : art.content;
            art.image = extractFirstImage(imgHtml);
            if (!art.image.empty() && art.image.front() == '/') {
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
                art.title = detail::decodeHtmlEntities(m[1].str());
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
                art.content = detail::decodeHtmlEntities(m[1].str());
            }
            if (std::regex_search(item, m, descRe)) {
                art.description = detail::decodeHtmlEntities(m[1].str());
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
            if (!art.image.empty() && art.image.front() == '/') {
                art.image = joinUrl(sourceUrl, art.image);
            }

            if (!art.title.empty() || !art.link.empty()) {
                articles.push_back(std::move(art));
            }
        }
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
        // legado RssArticle.pubDate 是字符串原文；OpenRead 的 pubDate 是 int64 时间戳。
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
    art.link = detail::resolveUrlWithBase(detail::stripCdata(rawLink), baseUrl, js);
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
        if (!img.empty() && img.front() == '/') img = joinUrl(baseUrl, img);
        art.image = img;
    }
    return art;
}

/// 判断订阅源是否为「规则源」（含 ruleArticles，需要按规则解析），
/// 对应 legado 中 ruleArticles 非空的 RssSource。
inline bool isRuleSource(const RssSource& src) {
    return !detail::isBlank(src.ruleArticles);
}

/// 下载 RSS feed。
/// @param requireFeedMarkers true=要求响应含 <rss>/<feed> 等 XML 标志（标准 feed 场景）；
///        false=任意非空响应都接受（规则源/JSON 源/网页源场景，对齐 legado 不做 feed 校验）。
std::string downloadRss(const std::string& url, HttpClientFunc httpClientFunc,
                        HttpRequestFunc httpFunc, bool requireFeedMarkers = true,
                        const std::string& extraHeadersJson = "") {
    auto doRequest = [&](HttpClientFunc client) -> std::string {
        HttpRequest req;
        req.url = url;
        req.method = "GET";
        req.timeoutMs = 15000;
        // 完整浏览器 UA，降低被反爬拦截的概率
        req.headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
        req.headers["Accept"] = "application/rss+xml,application/atom+xml,application/xml,text/xml,text/html,*/*";
        req.headers["Accept-Language"] = "zh-CN,zh;q=0.9,en;q=0.8";
        req.headers["Accept-Encoding"] = "identity";  // 禁用压缩，避免 libcurl 无 zlib 时拿到二进制数据
        // 订阅源自定义 header 覆盖默认值
        if (!extraHeadersJson.empty()) {
            try {
                auto h = json::parse(extraHeadersJson);
                if (h.is_object()) {
                    for (auto it = h.begin(); it != h.end(); ++it) {
                        if (it.value().is_string())
                            req.headers[it.key()] = it.value().get<std::string>();
                    }
                }
            } catch (...) {}
        }
        auto resp = client(req);
        if (resp.statusCode >= 200 && resp.statusCode < 400) {
            // 编码转换：确保返回 UTF-8
            std::string body = detail::ensureUtf8(resp.body);
            if (!requireFeedMarkers) {
                return body;  // 规则源：任意非空响应都交给规则引擎解析
            }
            // 标准 feed：简单校验，有效 RSS/Atom 至少包含一个标志性标签
            if (body.find("<rss") != std::string::npos ||
                body.find("<feed") != std::string::npos ||
                body.find("<channel") != std::string::npos ||
                body.find("<item>") != std::string::npos ||
                body.find("<entry>") != std::string::npos) {
                return body;
            }
            // 返回空但保留诊断信息（由调用方通过 lastError 或其他方式输出）
            return "";
        }
        return "";
    };

    if (httpClientFunc) {
        return doRequest(httpClientFunc);
    }
    if (httpFunc) {
        // 修复：httpFunc 分支此前直接返回原始 body，未做编码转换，
        // 导致 GBK/GB2312 网页（如 mmonly.cc）标题/正文乱码。统一经 ensureUtf8。
        std::string body = httpFunc(
            url, "GET",
            "{\"User-Agent\":\"Mozilla/5.0\",\"Accept\":\"application/rss+xml,*/*\"}", "");
        return detail::ensureUtf8(body);
    }
    auto client = createDefaultHttpClient();
    if (client) {
        return doRequest(client);
    }
    return "";
}

/// 从完整网页 HTML 中提取「正文主体」HTML（轻量可读性提取）。
/// 优先级：<article> > <main> > id/class 含 content|article|post|entry 的容器 > <body>。
/// 同时剔除 script/style/nav/header/footer/aside 等非正文噪声，并把相对资源 URL 绝对化，
/// 保留 img/video/iframe 等富媒体标签，供前端 iframe 沙箱渲染完整原文（图文+视频）。
/// 对齐 legado「ruleContent 为空时点进去看原文网页」的体验。
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
    if (body.size() < 200) {
        std::string mainBlk = pickBlock("main");
        if (mainBlk.size() > body.size()) body = mainBlk;
    }
    if (body.size() < 200) {
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
                std::regex srcAttr(R"(\bsrc\s*=\s*(["'])[^"']*\1)", std::regex::icase);
                if (std::regex_search(tag, srcAttr)) {
                    tag = std::regex_replace(tag, srcAttr, "src=\"" + realUrl + "\"");
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

    // 相对 URL 绝对化：src=/.. 与 href=/.. 补全为 baseUrl 的 origin
    std::string origin;
    {
        size_t schemeEnd = baseUrl.find("://");
        if (schemeEnd != std::string::npos) {
            size_t hostEnd = baseUrl.find('/', schemeEnd + 3);
            origin = (hostEnd == std::string::npos) ? baseUrl : baseUrl.substr(0, hostEnd);
        }
    }
    if (!origin.empty()) {
        // src="/x" / src='/x'  →  origin + /x （避免匹配 // 协议相对与 http）
        std::regex relAttr(R"((src|href|data-src|poster)\s*=\s*(["'])(/[^/"'][^"']*)\2)",
                           std::regex::icase);
        body = std::regex_replace(body, relAttr, "$1=$2" + origin + "$3$2");
    }
    return body;
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

    // 预热 JS 对象池（每个工作线程需要一个独立的 JsRuntime）
    pImpl->warmUpJsPool(16);

    std::atomic<int> doneCount{0};
    std::mutex dbMutex;
    const int concurrency = 16;

    // 评级阈值（毫秒）：优<1000 良<3000 差<5000 否则无效。
    // 5 秒内抓不到内容（超时或解析不出）一律评 invalid，对齐用户要求。
    auto rate = [](bool ok, int latencyMs) -> std::string {
        if (!ok) return "invalid";
        if (latencyMs < 1000) return "excellent";
        if (latencyMs < 3000) return "good";
        if (latencyMs < 5000) return "poor";
        return "invalid";
    };

    detail::parallelForEach(sources.size(), concurrency, [&](std::size_t i) {
            auto s = sources[i];

            bool ok = false;
            bool contentUnreachable = false;
            int latencyMs = -1;
            auto t0 = std::chrono::steady_clock::now();

            if (pImpl->httpClientFunc) {
                // 每个工作线程从对象池获取独立的 JsRuntime
                auto threadJs = pImpl->acquireJsRuntime();
                // 透传 sourceComment（覆盖写入，避免对象池复用残留污染）
                threadJs->putVariable("source_sourceComment", s.sourceComment);

                // 规则源：抓取首个通道并尝试解析（更贴近真实可用性）；
                // 标准源：GET + 解析文章。
                HttpRequest req;
                // 规则源用 sortUrl 首通道，否则用 sourceUrl
                std::string probeUrl = s.sourceUrl;
                if (!s.sortUrl.empty()) {
                    auto chans = parseSortUrls(s.sortUrl, s.sourceUrl);
                    if (!chans.empty()) {
                        AnalyzeUrl au(chans[0].second, s.sourceUrl, "", 1, threadJs.get());
                        auto an = au.result();
                        probeUrl = an.url.empty() ? chans[0].second : an.url;
                    }
                }
                req.url = probeUrl;
                req.method = "GET";
                req.timeoutMs = 5000;
                req.headers["User-Agent"] =
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
                // 关键：与 downloadRss 保持一致的请求头，否则评级口径与抓取口径不一。
                // Accept-Encoding: identity 禁用压缩——libcurl 无 zlib 时 gzip 响应会变成
                // 二进制乱码，解析必然失败，导致「抓取正常但评级 invalid」（如 36氪）。
                req.headers["Accept"] =
                    "application/rss+xml,application/atom+xml,application/xml,"
                    "text/xml,text/html,*/*";
                req.headers["Accept-Language"] = "zh-CN,zh;q=0.9,en;q=0.8";
                req.headers["Accept-Encoding"] = "identity";
                auto resp = pImpl->httpClientFunc(req);
                bool http2xx = (resp.statusCode >= 200 && resp.statusCode < 400);
                if (http2xx && !resp.body.empty()) {
                    if (isRuleSource(s)) {
                        // 规则源：能切出至少一个条目才算有效
                        auto items = detail::applyRuleStatic(resp.body, s.ruleArticles,
                                                             threadJs.get(), probeUrl);
                        ok = !items.empty();
                    } else {
                        // 标准 feed：尝试解析，能解析出文章才算有效
                        auto articles = parseRssFeed(resp.body, probeUrl);
                        ok = !articles.empty();
                        // 抽样点开校验：解析成功 ≠ 文章页可读。取首篇真实 http(s) link
                        // 探一次，确认正文页可达，避免「评优质却点不开」。
                        // 仅对存在真实外链的标准 feed 生效（含 #item- 占位的跳过）。
                        if (ok) {
                            std::string firstLink;
                            for (const auto& a : articles) {
                                if (!a.link.empty() &&
                                    (a.link.rfind("http://", 0) == 0 ||
                                     a.link.rfind("https://", 0) == 0) &&
                                    a.link.find("#item-") == std::string::npos) {
                                    firstLink = a.link;
                                    break;
                                }
                            }
                            if (!firstLink.empty()) {
                                HttpRequest creq;
                                creq.url = firstLink;
                                creq.method = "GET";
                                creq.timeoutMs = 5000;
                                creq.headers["User-Agent"] = req.headers["User-Agent"];
                                creq.headers["Accept-Encoding"] = "identity";
                                auto cresp = pImpl->httpClientFunc(creq);
                                bool cok = (cresp.statusCode >= 200 && cresp.statusCode < 400)
                                           && !cresp.body.empty();
                                // 文章页不可达：feed 本身可用，但点开会失败 → 降级为 poor，
                                // 不再误评 excellent/good。
                                contentUnreachable = !cok;
                            }
                        }
                    }
                }

                // 归还 JsRuntime 到对象池
                pImpl->releaseJsRuntime(std::move(threadJs));
            }

            auto t1 = std::chrono::steady_clock::now();
            latencyMs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

            std::string validity = rate(ok, latencyMs);
            // 文章页不可达：feed 解析成功也只评「差」，让「优质」名副其实
            if (ok && contentUnreachable &&
                (validity == "excellent" || validity == "good")) {
                validity = "poor";
            }

            // 持久化评级
            s.validity = validity;
            s.latencyMs = ok ? latencyMs : -1;
            {
                std::lock_guard<std::mutex> lk(dbMutex);
                try { pImpl->db->upsertRssSource(s); } catch (...) {}
            }

            int done = ++doneCount;
            if (progressCallback) {
                std::string displayName = s.sourceName.empty() ? s.sourceUrl : s.sourceName;
                progressCallback(done, total, detail::sanitizeUtf8(displayName),
                                 validity, s.latencyMs);
            }
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
    if (!pImpl->db) {
        result.error = "Database not initialized";
        return result;
    }

    // 0. 取出完整订阅源（含 Legado 规则字段）
    RssSource src;
    bool found = false;
    for (const auto& s : pImpl->db->getAllRssSources()) {
        if (s.sourceUrl == sourceUrl) { src = s; found = true; break; }
    }
    if (!found) {
        // 容错：DB 里没有就构造一个仅含 URL 的源，按标准 feed 处理
        src.sourceUrl = sourceUrl;
    }

    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // ── 分支 A：Legado 规则订阅源（ruleArticles 非空） ──
    if (isRuleSource(src)) {
        auto articles = fetchRssByRule(src, result);
        if (articles.empty() && !result.error.empty()) {
            return result;  // fetchRssByRule 已填好 error
        }
        for (auto& art : articles) {
            try {
                pImpl->db->upsertRssArticle(art);
                ++result.inserted;
            } catch (...) {
                ++result.skipped;
            }
        }
        result.count = static_cast<int>(articles.size());
        pImpl->db->updateRssSourceLastUpdateTime(sourceUrl, now);
        return result;
    }

    // ── 分支 B：标准 RSS/Atom feed（singleUrl 或无规则源） ──
    // 1. 下载 RSS
    std::string xmlText = downloadRss(sourceUrl, pImpl->httpClientFunc, pImpl->httpFunc,
                                      /*requireFeedMarkers=*/true, src.header);
    if (xmlText.empty()) {
        result.error = "下载 RSS 失败，请检查 URL 和网络";
        return result;
    }

    // 2. 解析
    auto articles = parseRssFeed(xmlText, sourceUrl);
    if (articles.empty()) {
        std::string preview = detail::sanitizeUtf8(xmlText.substr(0, 500));
        // 去掉换行方便一行显示
        for (auto& c : preview) { if (c == '\n' || c == '\r') c = ' '; }
        result.error = "未能解析到文章。内容长度=" + std::to_string(xmlText.size()) +
                      "，前500字节=[" + preview + "]";
        return result;
    }

    // 3. 写入数据库
    int64_t seq = 0;
    for (auto& art : articles) {
        // 序号递增，保留 feed 原始顺序
        art.order = ++seq;
        // 空 link 兜底：用 (源URL#序号) 占位，避免空 link 互相覆盖
        if (art.link.empty()) {
            art.link = sourceUrl + "#item-" + std::to_string(art.order);
        }
        try {
            pImpl->db->upsertRssArticle(art);
            ++result.inserted;
        } catch (...) {
            ++result.skipped;
        }
    }
    result.count = static_cast<int>(articles.size());

    // 4. 更新源的最后更新时间
    pImpl->db->updateRssSourceLastUpdateTime(sourceUrl, now);

    return result;
}

// ──────────────────────────────────────────────
// 规则订阅源抓取（对齐 legado RssParserByRule + Rss.getArticles）
// ──────────────────────────────────────────────
std::vector<RssArticle> BookSourceEngine::fetchRssByRule(const RssSource& src,
                                                         RssFetchResult& result) {
    std::vector<RssArticle> all;

    if (!pImpl->httpFunc && !pImpl->httpClientFunc) {
        result.error = "HTTP callback not set";
        return all;
    }

    // 从对象池获取独立的 JsRuntime（RSS 规则源需要独立的 httpFunc）
    auto threadJs = pImpl->acquireJsRuntime();

    // 透传 sourceComment：legado 复杂源（如壁纸喵）依赖 source.sourceComment
    // 中预定义的核心变量，规则 JS 首句 eval(String(source.sourceComment))。
    threadJs->putVariable("source_sourceComment", src.sourceComment);

    // 设置 RSS 源的 httpFunc（使用 src.header 而非 currentSource().headers）
    auto httpFunc = pImpl->httpFunc;
    auto httpClientFunc = pImpl->httpClientFunc;
    std::string srcHeader = src.header;
    threadJs->setHttpFunc([&srcHeader, httpFunc, httpClientFunc](const std::string& url,
                                                                   const std::string& method,
                                                                   const std::string& headersJson,
                                                                   const std::string& body) -> std::string {
        // 合并 RSS 源的自定义 headers
        HttpRequest req;
        req.url = url;
        req.method = method.empty() ? "GET" : method;
        req.body = body;
        req.timeoutMs = 15000;

        try {
            auto h = json::parse(headersJson.empty() ? "{}" : headersJson);
            for (auto it = h.begin(); it != h.end(); ++it) {
                if (it.value().is_string())
                    req.headers[it.key()] = it.value().get<std::string>();
            }
        } catch (...) {}

        // 合并 RSS 源的自定义 headers
        if (!srcHeader.empty()) {
            try {
                auto h = json::parse(srcHeader);
                for (auto it = h.begin(); it != h.end(); ++it) {
                    if (it.value().is_string())
                        req.headers[it.key()] = it.value().get<std::string>();
                }
            } catch (...) {}
        }

        if (httpClientFunc) {
            auto resp = httpClientFunc(req);
            if (resp.statusCode >= 200 && resp.statusCode < 400) {
                return detail::ensureUtf8(resp.body);
            }
            return "";
        }
        if (httpFunc) {
            return httpFunc(url, method, headersJson, body);
        }
        return "";
    });

    // 解析 sortUrl 通道（无则单通道 = sourceUrl）
    auto channels = parseSortUrls(src.sortUrl, src.sourceUrl);

    std::string diag;
    // 全局序号：对齐 legado RssArticle.order（保留抓取顺序，且作为空 link 文章的去重保险）
    int64_t seq = 0;
    for (const auto& ch : channels) {
        const std::string& channelUrl = ch.second;

        // 用 AnalyzeUrl 处理 {{page}}/{{key}}/@js: 等模板（page=1，key 空）
        AnalyzeUrl analyzer(channelUrl, src.sourceUrl, "", 1, threadJs.get());
        auto analyzed = analyzer.result();
        std::string reqUrl = analyzed.url.empty() ? channelUrl : analyzed.url;

        // 下载（规则源不做 feed 校验）
        std::string body = downloadRss(reqUrl, httpClientFunc, httpFunc,
                                       /*requireFeedMarkers=*/false, src.header);
        if (body.empty()) {
            diag += "[通道 " + detail::sanitizeUtf8(ch.first.empty() ? reqUrl : ch.first) + " 下载为空] ";
            continue;
        }

        // 用 ruleArticles 切出列表
        auto items = detail::applyRuleStatic(body, src.ruleArticles,
                                             threadJs.get(), reqUrl);
        if (items.empty()) {
            std::string preview = detail::sanitizeUtf8(body.substr(0, 200));
            for (auto& c : preview) { if (c == '\n' || c == '\r') c = ' '; }
            diag += "[通道 " + detail::sanitizeUtf8(ch.first.empty() ? reqUrl : ch.first) +
                    " ruleArticles 无结果, len=" + std::to_string(body.size()) +
                    ", 前200=" + preview + "] ";
            continue;
        }

        for (auto& item : items) {
            RssArticle art = parseRssArticleByRule(item, src, reqUrl, threadJs.get());
            // 给标题/链接都空的条目兜底剔除
            if (art.title.empty() && art.link.empty()) continue;
            // 序号递增，保留抓取顺序（对齐 legado order）
            art.order = ++seq;
            // 空 link 兜底：生成稳定唯一占位键，避免同源多篇空 link 文章
            // 因 UNIQUE(source_url, link) 互相 ON CONFLICT 覆盖塌缩成 1 条。
            if (art.link.empty()) {
                art.link = src.sourceUrl + "#item-" + std::to_string(art.order);
            }
            all.push_back(std::move(art));
        }
    }

    // 归还 JsRuntime 到对象池
    pImpl->releaseJsRuntime(std::move(threadJs));

    if (all.empty()) {
        result.error = "规则订阅源未解析到文章。" +
                       (diag.empty() ? std::string("（请检查 ruleArticles / sortUrl）") : detail::sanitizeUtf8(diag));
    }
    return all;
}

RssArticleListResult BookSourceEngine::getRssArticles(const std::string& sourceUrl, int page, int pageSize) {
    RssArticleListResult result;
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return result;
    }
    page = std::max(1, page);
    pageSize = std::max(1, std::min(pageSize, 200));
    result.articles = pImpl->db->getRssArticles(sourceUrl, page, pageSize, result.total);
    result.page = page;
    result.pageSize = pageSize;
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
    if (!pImpl->db) return "";
    auto article = pImpl->db->getRssArticle(articleId);
    if (article.content.empty() && article.link.empty()) return "";

    // 如果已有内容，直接返回
    if (!article.content.empty()) return article.content;

    // 查找对应的源，获取 ruleContent
    RssSource src;
    for (const auto& s : pImpl->db->getAllRssSources()) {
        if (s.sourceUrl == article.sourceUrl) { src = s; break; }
    }

    bool canHttp = pImpl->httpClientFunc || pImpl->httpFunc;
    bool isPlaceholder = article.link.empty() ||
                         article.link.find("#item-") != std::string::npos;

    // 规范化文章链接：legado 风格 link 可能带 ",{options}" 选项后缀（如 webView），
    // 或为相对/模板 URL。用 AnalyzeUrl 解析出真实可下载 URL，否则下载必然失败
    // （©痴汉等源 link 形如 https://x/a.html,{"webView":true}）。
    std::string fetchUrl;
    if (!isPlaceholder && canHttp) {
        auto njs = pImpl->acquireJsRuntime();
        AnalyzeUrl au(article.link, article.sourceUrl, "", 1, njs.get());
        auto an = au.result();
        pImpl->releaseJsRuntime(std::move(njs));
        fetchUrl = an.url.empty() ? article.link : an.url;
        // 去掉残留的 ",{...}" / ",\{...}" 选项后缀兜底（legado URL 选项语法，
        // 入库时引号可能被转义为 ,\{\"...）
        auto comma = fetchUrl.find(",{");
        if (comma == std::string::npos) comma = fetchUrl.find(",\\{");
        if (comma != std::string::npos) fetchUrl = fetchUrl.substr(0, comma);
    }
    bool hasFetchUrl = !fetchUrl.empty() &&
                       (fetchUrl.rfind("http://", 0) == 0 || fetchUrl.rfind("https://", 0) == 0);

    // 分支 1：无 ruleContent（标准 feed / 普通网页源）
    //   有真实链接 → 下载原文页，提取正文主体 HTML（图文/视频齐全，前端 iframe 渲染）；
    //   提取失败时回退到原始 HTML（让前端 iframe 加载，对齐 legado WebView 行为）；
    //   无链接则回退 description。
    if (detail::isBlank(src.ruleContent)) {
        if (hasFetchUrl) {
            std::string body = detail::ensureUtf8(
                downloadRss(fetchUrl, pImpl->httpClientFunc, pImpl->httpFunc,
                            false, src.header));
            if (!body.empty()) {
                // 优先尝试提取可读正文
                std::string readable = extractReadableHtml(body, fetchUrl);
                if (readable.size() > 200) return readable;
                // 提取失败（SPA / 懒加载 / 正文在 JS 中）：不要把整页离散 HTML 塞进
                // iframe srcdoc —— 其相对资源/脚本会因缺失原始 origin + CSP 而白屏，
                // 表现为「点不开但原址能打开」。改为返回一段带「打开原文」按钮的提示页，
                // 锚点在浏览器中以完整 origin 打开，行为与 legado WebView 一致。
                fprintf(stderr, "[RSS] extractReadableHtml too short (%zu bytes), returning open-original fallback\n",
                        readable.size());
                std::string safeUrl = fetchUrl;
                // 转义引号，避免破坏 href 属性
                std::string escUrl;
                for (char c : safeUrl) {
                    if (c == '"') escUrl += "&quot;";
                    else if (c == '<') escUrl += "&lt;";
                    else if (c == '>') escUrl += "&gt;";
                    else escUrl += c;
                }
                return "<div style=\"padding:32px;text-align:center;font-family:sans-serif;color:#444\">"
                       "<p style=\"font-size:15px;margin-bottom:8px\">该文章正文需在原网页中查看"
                       "（页面依赖脚本渲染，无法在阅读器内直接呈现）。</p>"
                       "<p style=\"margin:18px 0\"><a href=\"" + escUrl + "\" target=\"_blank\" rel=\"noopener\" "
                       "style=\"display:inline-block;padding:10px 22px;background:#2563eb;color:#fff;"
                       "border-radius:8px;text-decoration:none;font-size:14px\">在浏览器中打开原文 ↗</a></p>"
                       "</div>";
            } else {
                fprintf(stderr, "[RSS] downloadRss failed for fetchUrl='%s'\n", fetchUrl.c_str());
            }
        }
        return article.description;
    }

    // 分支 2：规则源（有 ruleContent）
    if (!canHttp) return article.description;

    // 决定用哪个页面 URL 喂给 ruleContent：
    //   - 有真实文章 link → 用文章页 URL（下载文章页）
    //   - 占位 link（图片源频道，无单篇真实链接）→ 用源/频道 URL，
    //     让 ruleContent 中的 JS 基于 baseUrl 自行渲染瀑布流（对齐 legado）。
    std::string pageUrl = hasFetchUrl ? fetchUrl : src.sourceUrl;
    std::string body = detail::ensureUtf8(
        downloadRss(pageUrl, pImpl->httpClientFunc, pImpl->httpFunc,
                    false, src.header));
    // 占位 link 场景下，body 可能是频道 JSON/HTML；即使为空也继续，
    // 因为 ruleContent 的 JS 经常自己再发请求（java.ajax）。
    if (body.empty() && hasFetchUrl) return article.description;

    // DEBUG: 临时日志
    fprintf(stderr, "[DEBUG] articleId=%lld ruleContent='%s' pageUrl='%s' bodyLen=%zu bodyStart='%.20s'\n",
            articleId, src.ruleContent.c_str(), pageUrl.c_str(), body.size(), body.c_str());

    auto threadJs = pImpl->acquireJsRuntime();
    // 透传 sourceComment，供 ruleContent 中的引导 JS 使用
    threadJs->putVariable("source_sourceComment", src.sourceComment);
    auto parts = detail::applyRuleStatic(body, src.ruleContent, threadJs.get(), pageUrl);
    pImpl->releaseJsRuntime(std::move(threadJs));

    fprintf(stderr, "[DEBUG] parts.size()=%zu\n", parts.size());
    if (parts.empty()) return article.description;

    // 合并所有提取的片段
    std::string content;
    for (const auto& p : parts) {
        if (!content.empty()) content += "\n\n";
        content += detail::decodeHtmlEntities(p);
    }
    return content;
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

} // namespace openread

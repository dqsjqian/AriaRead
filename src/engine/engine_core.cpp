/// @file engine_core.cpp
/// @brief 引擎核心：构造/析构、配置、书源管理、单源搜索、目录、正文、发现、状态查询

#include "ariaread/engine_impl.h"
#include "ariaread/version.h"

#include <cstring>
#include <algorithm>

namespace ariaread {

using json = nlohmann::json;

// ──────────────────────────────────────────────
// 构造/析构
// ──────────────────────────────────────────────
BookSourceEngine::BookSourceEngine() : pImpl(std::make_unique<Impl>()) {
    pImpl->owner = this;
    // 绑定选择器桥接到主 JsRuntime（使 java.getString/getElements 可用，
    // 即使调用方从不显式 setHttpClient/setHttpRequest）。
    Impl::bindJsSelector(pImpl->js.get());
    pImpl->startRefreshWorker();
}

BookSourceEngine::~BookSourceEngine() = default;

BookSourceEngine::BookSourceEngine(BookSourceEngine&&) noexcept = default;
BookSourceEngine& BookSourceEngine::operator=(BookSourceEngine&&) noexcept = default;

// ──────────────────────────────────────────────
// 配置
// ──────────────────────────────────────────────
void BookSourceEngine::setHttpRequest(HttpRequestFunc func) {
    pImpl->httpFunc = std::move(func);

    // 显式注入简易 HTTP 回调时，优先使用注入实现
    if (pImpl->httpFunc) {
        pImpl->httpClientFunc = HttpClientFunc{};
        pImpl->bindJsHttp();
        return;
    }

    // 若两种回调都为空，则恢复默认内置 HTTP 客户端（若可用）
    if (!pImpl->httpClientFunc) {
        pImpl->httpClientFunc = createDefaultHttpClient();
    }
    pImpl->bindJsHttp();
}

void BookSourceEngine::setHttpClient(HttpClientFunc func) {
    pImpl->httpClientFunc = std::move(func);

    // 显式注入完整版 HTTP 客户端时，优先使用注入实现
    if (pImpl->httpClientFunc) {
        pImpl->httpFunc = HttpRequestFunc{};
        pImpl->bindJsHttp();
        return;
    }

    // 清空后若无简易回调，恢复默认内置 HTTP 客户端（若可用）
    if (!pImpl->httpFunc) {
        pImpl->httpClientFunc = createDefaultHttpClient();
    }
    pImpl->bindJsHttp();
}

void BookSourceEngine::setJsLogCallback(JsLogFunc func) {
    pImpl->logFunc = func;
    pImpl->js->setLogCallback(func);
}

void BookSourceEngine::setOperationCheck(std::function<void()> check) {
    pImpl->operationCheck = std::move(check);
    pImpl->operationFailure = nullptr;
    if (!pImpl->operationCheck) {
        pImpl->js->setInterruptCallback({});
        return;
    }
    pImpl->js->setInterruptCallback([impl = pImpl.get()] {
        try {
            impl->checkOperation();
            return false;
        } catch (...) {
            // QuickJS 的 C 回调只能返回中断标志；HTTP/规则后置检查重新报告原因。
            impl->operationFailure = std::current_exception();
            return true;
        }
    });
}

void BookSourceEngine::setLogCallback(JsLogFunc func) {
    pImpl->logCallback = func;
}

void BookSourceEngine::log(const std::string& msg) const {
    if (pImpl->logCallback) {
        pImpl->logCallback(msg);
    }
}

void BookSourceEngine::setDatabasePath(const std::string& dbPath) {
    pImpl->dbPath = dbPath;
    if (!dbPath.empty()) {
        pImpl->db = std::make_unique<SourceDatabase>(dbPath);
    } else {
        pImpl->db.reset();
    }
}

int BookSourceEngine::loadSourcesFromDatabase() {
    if (!pImpl->db) return 0;
    auto dbSources = pImpl->db->getAllSources();
    int count = static_cast<int>(dbSources.size());
    pImpl->sources = std::move(dbSources);
    pImpl->currentSourceIndex = 0;
    return count;
}

int BookSourceEngine::syncSourcesToDatabase() {
    if (!pImpl->db || pImpl->sources.empty()) return 0;
    return pImpl->db->insertSources(pImpl->sources);
}

SourceDatabase* BookSourceEngine::database() {
    return pImpl->db.get();
}

// ──────────────────────────────────────────────
// 书源管理
// ──────────────────────────────────────────────
bool BookSourceEngine::loadSource(const std::string& jsonStr) {
    auto source = SourceParser::parse(jsonStr);
    if (!SourceParser::validate(source)) {
        pImpl->lastError = "Invalid source: " + SourceParser::validateDetail(source);
        return false;
    }

    if (pImpl->db) {
        if (pImpl->db->sourceExists(source.url)) {
            pImpl->lastError = "Source already exists: " + source.url;
            return false;
        }
        pImpl->db->insertSources({source});
    }

    pImpl->sources.push_back(std::move(source));
    return true;
}

int BookSourceEngine::loadSources(const std::string& jsonArray) {
    auto parsed = SourceParser::parseArray(jsonArray);
    int count = 0;
    for (auto& source : parsed) {
        if (!SourceParser::validate(source)) continue;

        if (pImpl->db) {
            if (pImpl->db->sourceExists(source.url)) continue;
            pImpl->db->insertSources({source});
        }

        pImpl->sources.push_back(std::move(source));
        ++count;
    }
    return count;
}

int BookSourceEngine::loadSourcesFromFile(const std::string& filePath) {
    auto parsed = SourceParser::loadFromFile(filePath);
    int count = 0;
    for (auto& source : parsed) {
        if (!SourceParser::validate(source)) continue;

        if (pImpl->db) {
            if (pImpl->db->sourceExists(source.url)) continue;
            pImpl->db->insertSources({source});
        }

        pImpl->sources.push_back(std::move(source));
        ++count;
    }
    return count;
}

const BookSource& BookSourceEngine::currentSource() const {
    return pImpl->currentSource();
}

const std::vector<BookSource>& BookSourceEngine::sources() const {
    return pImpl->sources;
}

bool BookSourceEngine::selectSource(size_t index) {
    if (index < pImpl->sources.size()) {
        pImpl->currentSourceIndex = index;
        return true;
    }
    pImpl->lastError = "Source index out of range";
    return false;
}

bool BookSourceEngine::selectSourceByName(const std::string& name) {
    for (size_t i = 0; i < pImpl->sources.size(); ++i) {
        if (pImpl->sources[i].name == name) {
            pImpl->currentSourceIndex = i;
            return true;
        }
    }
    pImpl->lastError = "Source not found: " + name;
    return false;
}

bool BookSourceEngine::removeSource(size_t index) {
    if (index >= pImpl->sources.size()) {
        pImpl->lastError = "Source index out of range";
        return false;
    }
    // 同时从数据库中删除
    if (pImpl->db) {
        pImpl->db->removeSource(pImpl->sources[index].url);
    }
    pImpl->sources.erase(pImpl->sources.begin() + static_cast<ptrdiff_t>(index));
    if (pImpl->currentSourceIndex >= pImpl->sources.size() && !pImpl->sources.empty()) {
        pImpl->currentSourceIndex = pImpl->sources.size() - 1;
    }
    return true;
}

bool BookSourceEngine::removeSourceByUrl(const std::string& url) {
    for (size_t i = 0; i < pImpl->sources.size(); ++i) {
        if (pImpl->sources[i].url == url) {
            return removeSource(i);
        }
    }
    pImpl->lastError = "Source not found: " + url;
    return false;
}

int BookSourceEngine::removeInvalidSources() {
    int removed = 0;
    for (int i = static_cast<int>(pImpl->sources.size()) - 1; i >= 0; --i) {
        auto v = pImpl->sources[i].validity;
        // Poor（差）也视为无效，一并删除
        if (v == SourceValidity::Invalid || v == SourceValidity::Poor) {
            if (pImpl->db) {
                pImpl->db->removeSource(pImpl->sources[i].url);
            }
            pImpl->sources.erase(pImpl->sources.begin() + i);
            ++removed;
        }
    }
    if (pImpl->currentSourceIndex >= pImpl->sources.size() && !pImpl->sources.empty()) {
        pImpl->currentSourceIndex = pImpl->sources.size() - 1;
    }
    return removed;
}

int BookSourceEngine::clearAllSources() {
    int removed = static_cast<int>(pImpl->sources.size());

    if (pImpl->db) {
        for (const auto& s : pImpl->sources) {
            pImpl->db->removeSource(s.url);
        }
    }

    pImpl->sources.clear();
    pImpl->currentSourceIndex = 0;
    pImpl->lastError.clear();

    return removed;
}

int BookSourceEngine::validSourceCount() const {
    int count = 0;
    for (const auto& s : pImpl->sources) {
        if ((s.validity == SourceValidity::Excellent ||
             s.validity == SourceValidity::Good) && !s.searchUrl.empty()) {
            ++count;
        }
    }
    return count;
}

void BookSourceEngine::setSourceValidity(size_t index, SourceValidity validity, int latencyMs) {
    if (index < pImpl->sources.size()) {
        pImpl->sources[index].validity = validity;
        if (latencyMs >= 0) {
            pImpl->sources[index].latencyMs = latencyMs;
        }
        if (pImpl->db) {
            pImpl->db->updateValidity(pImpl->sources[index].url, validity, pImpl->sources[index].latencyMs);
        }
    }
}

void BookSourceEngine::setConcurrency(int n) {
    pImpl->defaultConcurrency = std::max(1, std::min(n, 32));
}

void BookSourceEngine::setValidateMaxTaskSec(int sec) {
    // E2: 合理范围 10~120 秒
    pImpl->validateMaxTaskSec = std::max(10, std::min(sec, 120));
}

void BookSourceEngine::warmUpJsPool(int count) {
    pImpl->warmUpJsPool(count);
}

// ──────────────────────────────────────────────
// 搜索（单源）
// ──────────────────────────────────────────────
std::vector<Book> BookSourceEngine::search(const std::string& keyword,
                                            bool matchName,
                                            bool matchAuthor,
                                            bool matchIntro) {
    pImpl->checkOperation();
    std::vector<Book> results;

    if (pImpl->sources.empty()) {
        pImpl->lastError = "No source loaded";
        return results;
    }

    if (!pImpl->httpFunc && !pImpl->httpClientFunc) {
        pImpl->lastError = "HTTP callback not set";
        return results;
    }

    auto& source = pImpl->currentSource();
    if (source.searchUrl.empty()) {
        pImpl->lastError = "Search URL not configured";
        return results;
    }

    AnalyzeUrl analyzer(source.searchUrl, source.url, keyword, 1, pImpl->js.get());
    auto analyzed = analyzer.result();

    pImpl->log("Search ruleUrl: " + analyzed.ruleUrl);
    pImpl->log("Search final URL: " + analyzed.url);
    pImpl->log("Search method: " + analyzed.method);
    pImpl->log("Search body: " + (analyzed.body.empty() ? "(empty)" : analyzed.body.substr(0, 200)));
    pImpl->log("Search charset: " + analyzed.charset);

    std::string headersJson = "{}";
    if (!analyzed.headers.empty()) {
        json h = json::object();
        for (auto& [k, v] : analyzed.headers) {
            h[k] = v;
        }
        headersJson = h.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    std::string response = pImpl->httpRequest(
        analyzed.url, analyzed.method, headersJson, analyzed.body);
    if (response.empty()) {
        return results;
    }

    pImpl->log("Response size: " + std::to_string(response.size()));

    auto& rule = source.searchRule;

    auto bookItems = pImpl->applyRule(response, rule.bookList);
    pImpl->log("Book items found: " + std::to_string(bookItems.size()));

    for (auto& item : bookItems) {
        Book book;
        auto names = pImpl->applyRule(item, rule.name);
        if (!names.empty()) book.name = names[0];

        auto authors = pImpl->applyRule(item, rule.author);
        if (!authors.empty()) book.author = detail::cleanAuthorField(authors[0]);

        auto covers = pImpl->applyRule(item, rule.coverUrl);
        if (!covers.empty()) book.coverUrl = covers[0];

        std::string rawBookUrl = detail::extractFieldWithTemplateFallback(
            item,
            rule.bookUrl,
            [&](const std::string& content, const std::string& r) {
                return pImpl->applyRule(content, r);
            }
        );
        book.bookUrl = detail::resolveUrlWithBase(rawBookUrl, analyzed.url, pImpl->js.get());
        if (book.bookUrl.empty()) {
            book.bookUrl = detail::extractHrefFallback(item, analyzed.url, pImpl->js.get());
        }

        auto chapters = pImpl->applyRule(item, rule.lastChapter);
        if (!chapters.empty()) book.lastChapter = chapters[0];

        auto intros = pImpl->applyRule(item, rule.intro);
        if (!intros.empty()) book.intro = intros[0];

        auto kinds = pImpl->applyRule(item, rule.kind);
        if (!kinds.empty()) book.kind = kinds[0];

        if (!book.name.empty()) {
            // 如果调用方指定了匹配维度，则用 bookMatchScore 过滤
            const bool hasFilter = matchName || matchAuthor || matchIntro;
            if (hasFilter) {
                int score = detail::bookMatchScore(book, keyword, matchName, matchAuthor, matchIntro);
                if (score <= 0) continue;
                book.matchScore = score;
            }
            results.push_back(std::move(book));
        }
    }

    return results;
}

// ──────────────────────────────────────────────
// 获取目录
// ──────────────────────────────────────────────
std::vector<Chapter> BookSourceEngine::getCatalog(const std::string& bookUrl) {
    pImpl->checkOperation();
    std::vector<Chapter> results;

    if (!pImpl->httpFunc && !pImpl->httpClientFunc) {
        pImpl->lastError = "HTTP callback not set";
        return results;
    }

    auto& source = pImpl->currentSource();
    auto& rule = source.catalogRule;

    // listRule 前缀：'-' 表示倒序（解析后整体反转），'+' 仅去前缀（对齐 legado）
    std::string listRule = rule.chapterList;
    bool reverse = false;
    if (!listRule.empty() && listRule[0] == '-') { reverse = true; listRule = listRule.substr(1); }
    else if (!listRule.empty() && listRule[0] == '+') { listRule = listRule.substr(1); }

    auto parseCatalogFromPage = [&](const std::string& pageHtml, const std::string& pageBaseUrl) {
        auto chapterItems = pImpl->applyRule(pageHtml, listRule);
        int baseIndex = static_cast<int>(results.size());  // 全局偏移，确保跨页 index 不重复

        for (size_t i = 0; i < chapterItems.size(); ++i) {
            Chapter ch;
            ch.index = baseIndex + static_cast<int>(i);

            auto titles = pImpl->applyRule(chapterItems[i], rule.chapterName);
            if (!titles.empty()) ch.title = detail::sanitizeUtf8(titles[0]);

            std::string rawChapterUrl = detail::extractFieldWithTemplateFallback(
                chapterItems[i],
                rule.chapterUrl,
                [&](const std::string& content, const std::string& r) {
                    return pImpl->applyRule(content, r);
                }
            );
            if (!rawChapterUrl.empty()) {
                ch.url = detail::sanitizeUtf8(detail::resolveUrlWithBase(rawChapterUrl, pageBaseUrl, pImpl->js.get()));
            }

            // isVip / isVolume 判断（任何非空且非 "false"/"0" 即为真，对齐 legado isTrue）
            auto isTrue = [&](const std::string& fieldRule) -> bool {
                if (fieldRule.empty()) return false;
                auto vals = pImpl->applyRule(chapterItems[i], fieldRule);
                if (vals.empty()) return false;
                std::string v = detail::trimCopy(vals[0]);
                if (v.empty()) return false;
                std::string lv = v;
                for (auto& c : lv) c = (char)std::tolower((unsigned char)c);
                return !(lv == "false" || lv == "0" || lv == "null" || lv == "no");
            };
            ch.isVip = isTrue(rule.isVip);
            ch.isVolume = isTrue(rule.isVolume);

            // 卷标题无 url 时用 title 占位，普通章节无 url 用 baseUrl（对齐 legado）
            if (ch.url.empty()) {
                if (ch.isVolume) ch.url = ch.title + std::to_string(ch.index);
                else ch.url = pageBaseUrl;
            }

            if (!ch.title.empty() || !ch.url.empty()) {
                results.push_back(std::move(ch));
            }
        }
    };

    AnalyzeUrl analyzer(bookUrl, source.url, "", 1, pImpl->js.get());
    auto analyzed = analyzer.result();

    std::string headersJson = "{}";
    if (!analyzed.headers.empty()) {
        json h = json::object();
        for (auto& [k, v] : analyzed.headers) {
            h[k] = v;
        }
        headersJson = h.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    std::string response = pImpl->httpRequest(
        analyzed.url, analyzed.method, headersJson, analyzed.body);
    if (!response.empty()) {
        parseCatalogFromPage(response, analyzed.url);

        // 通用目录翻页（nextTocUrl/nextPage），对齐 legado BookChapterList。
        // 最多 100 页，已访问 URL 去重，防御死循环。
        if (!rule.nextPage.empty()) {
            std::unordered_set<std::string> visitedToc;
            visitedToc.insert(analyzed.url);
            std::string prevHtml = response;
            std::string prevBase = analyzed.url;
            int pageGuard = 0;
            while (pageGuard++ < 100) {
                auto nexts = pImpl->applyRule(prevHtml, rule.nextPage);
                if (nexts.empty() || nexts[0].empty()) break;
                std::string nextTocUrl = detail::resolveUrlWithBase(nexts[0], prevBase, pImpl->js.get());
                if (nextTocUrl.empty() || visitedToc.count(nextTocUrl)) break;
                visitedToc.insert(nextTocUrl);

                AnalyzeUrl na(nextTocUrl, source.url, "", 1, pImpl->js.get());
                auto naRes = na.result();
                std::string nh = "{}";
                if (!naRes.headers.empty()) {
                    json hh = json::object();
                    for (auto& [k, v] : naRes.headers) hh[k] = v;
                    nh = hh.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                }
                std::string nextHtml = pImpl->httpRequest(naRes.url, naRes.method, nh, naRes.body);
                if (nextHtml.empty()) break;
                size_t before = results.size();
                parseCatalogFromPage(nextHtml, naRes.url);
                if (results.size() == before) break; // 没有新增 → 停止
                prevHtml = nextHtml;
                prevBase = naRes.url;
            }
        }

        // listRule 以 '-' 开头表示页面本身倒序，解析后整体反转还原正序
        if (reverse && !results.empty()) {
            std::reverse(results.begin(), results.end());
            for (size_t i = 0; i < results.size(); ++i) results[i].index = static_cast<int>(i);
        }
    }

    auto looksLikePseudoCatalog = [&]() -> bool {
        if (results.size() != 1) return false;
        const auto& one = results.front();
        if (!one.title.empty()) return false;
        if (one.url.empty()) return false;
        return one.url.rfind(analyzed.url + "/chapters/", 0) == 0;
    };

    // 如果直接失败或无章节，尝试 ruleBookInfo.tocUrl 回退
    if ((results.empty() || looksLikePseudoCatalog()) && !source.bookInfoRule.tocUrl.empty()) {
        auto tocCandidates = pImpl->applyRule(response, source.bookInfoRule.tocUrl);

        if (!tocCandidates.empty()) {
            std::string tocUrl = detail::resolveUrlWithBase(tocCandidates[0], analyzed.url, pImpl->js.get());
            if (!tocUrl.empty()) {
                AnalyzeUrl tocAnalyzer(tocUrl, analyzed.url, "", 1, pImpl->js.get());
                auto tocAnalyzed = tocAnalyzer.result();

                std::string tocHeadersJson = "{}";
                if (!tocAnalyzed.headers.empty()) {
                    json h = json::object();
                    for (auto& [k, v] : tocAnalyzed.headers) {
                        h[k] = v;
                    }
                    tocHeadersJson = h.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                }

                std::string tocResponse = pImpl->httpRequest(
                    tocAnalyzed.url, tocAnalyzed.method, tocHeadersJson, tocAnalyzed.body);
                if (!tocResponse.empty()) {
                    parseCatalogFromPage(tocResponse, tocAnalyzed.url);
                }
            }
        }
    }

    // 兜底：API 书源常见目录地址为 bookUrl + /chapters
    if (results.empty() || looksLikePseudoCatalog()) {
        if (looksLikePseudoCatalog()) {
            results.clear();
        }

        std::string chaptersUrl = analyzed.url;
        if (!chaptersUrl.empty() && chaptersUrl.find("/chapters") == std::string::npos) {
            if (!chaptersUrl.empty() && chaptersUrl.back() == '/') {
                chaptersUrl += "chapters";
            } else {
                chaptersUrl += "/chapters";
            }

            int page = 1;
            const int pageSize = 200;
            const int maxPages = 100;

            while (page <= maxPages) {
                std::string pagedUrl = chaptersUrl;
                if (pagedUrl.find('?') != std::string::npos) {
                    pagedUrl += "&pi=" + std::to_string(page) + "&ps=" + std::to_string(pageSize);
                } else {
                    pagedUrl += "?pi=" + std::to_string(page) + "&ps=" + std::to_string(pageSize);
                }

                AnalyzeUrl chaptersAnalyzer(pagedUrl, analyzed.url, "", page, pImpl->js.get());
                auto chaptersAnalyzed = chaptersAnalyzer.result();

                std::string chaptersHeadersJson = "{}";
                if (!chaptersAnalyzed.headers.empty()) {
                    json h = json::object();
                    for (auto& [k, v] : chaptersAnalyzed.headers) {
                        h[k] = v;
                    }
                    chaptersHeadersJson = h.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                }

                std::string chaptersResponse = pImpl->httpRequest(
                    chaptersAnalyzed.url,
                    chaptersAnalyzed.method,
                    chaptersHeadersJson,
                    chaptersAnalyzed.body
                );
                if (chaptersResponse.empty()) break;

                size_t beforeCount = results.size();
                parseCatalogFromPage(chaptersResponse, chaptersAnalyzed.url);
                size_t added = results.size() - beforeCount;

                if (added == 0) break;

                bool hasMore = false;
                try {
                    auto respJson = json::parse(chaptersResponse);
                    if (respJson.contains("paging") && respJson["paging"].is_object()) {
                        auto& paging = respJson["paging"];
                        int totalCount = paging.value("count", 0);
                        int currentPage = paging.value("pi", page);
                        int currentPs = paging.value("ps", pageSize);
                        if (totalCount > 0 && currentPage * currentPs < totalCount) {
                            hasMore = true;
                        }
                    }
                } catch (...) {}

                if (!hasMore) break;
                ++page;
            }
        }
    }

    return results;
}

std::vector<Chapter> BookSourceEngine::getCatalogForSource(
    const std::string& bookUrl,
    int sourceIndex,
    const std::string& sourceName
) {
    std::lock_guard<std::recursive_mutex> lock(pImpl->engineMutex);

    if (pImpl->sources.empty()) {
        pImpl->lastError = "No source loaded";
        return {};
    }

    size_t prev = pImpl->currentSourceIndex;

    bool selected = false;
    if (!sourceName.empty()) {
        selected = selectSourceByName(sourceName);
    } else if (sourceIndex >= 0) {
        selected = selectSource(static_cast<size_t>(sourceIndex));
    }

    if (!selected && (!sourceName.empty() || sourceIndex >= 0)) {
        pImpl->currentSourceIndex = prev;
        return {};
    }

    try {
        auto result = getCatalog(bookUrl);
        pImpl->currentSourceIndex = prev;
        return result;
    } catch (...) {
        pImpl->currentSourceIndex = prev;
        throw;
    }
}

// ──────────────────────────────────────────────
// 获取正文
// ──────────────────────────────────────────────
std::string BookSourceEngine::getContent(const std::string& chapterUrl) {
    pImpl->checkOperation();
    if (!pImpl->httpFunc && !pImpl->httpClientFunc) {
        pImpl->lastError = "HTTP callback not set";
        return "";
    }

    auto& source = pImpl->currentSource();
    auto& rule = source.contentRule;

    // 解析单页正文 + 抓取下一页链接（对齐 legado BookContent.analyzeContent）
    auto parsePage = [&](const std::string& html, const std::string& pageBaseUrl,
                         std::string& outNextUrl) -> std::string {
        outNextUrl.clear();
        auto contents = pImpl->applyRule(html, rule.content);
        std::string pageText;
        if (!contents.empty()) {
            // 多段正文（&&/%% 合并）按换行拼接，保留图片
            for (size_t i = 0; i < contents.size(); ++i) {
                if (i) pageText += "\n";
                pageText += contents[i];
            }
        }
        // 下一页链接
        if (!rule.nextPage.empty()) {
            auto nexts = pImpl->applyRule(html, rule.nextPage);
            if (!nexts.empty() && !nexts[0].empty()) {
                outNextUrl = detail::resolveUrlWithBase(nexts[0], pageBaseUrl, pImpl->js.get());
            }
        }
        return pageText;
    };

    std::string firstResp = pImpl->httpRequest(chapterUrl);
    if (firstResp.empty()) return "";

    std::vector<std::string> pages;
    std::unordered_set<std::string> visited;
    visited.insert(chapterUrl);

    std::string nextUrl;
    pages.push_back(parsePage(firstResp, chapterUrl, nextUrl));

    // 正文翻页循环（最多 50 页，防御死循环 / 恶意书源）
    int guard = 0;
    std::string curBase = chapterUrl;
    while (!nextUrl.empty() && visited.find(nextUrl) == visited.end() && guard++ < 50) {
        visited.insert(nextUrl);
        std::string resp = pImpl->httpRequest(nextUrl);
        if (resp.empty()) break;
        std::string thisBase = nextUrl;
        std::string nu;
        pages.push_back(parsePage(resp, thisBase, nu));
        curBase = thisBase;
        nextUrl = nu;
    }

    // 合并所有分页 → 保留图片格式化 → 全文正则替换
    std::string merged;
    for (size_t i = 0; i < pages.size(); ++i) {
        if (i) merged += "\n";
        merged += pages[i];
    }

    std::string text = detail::formatKeepImg(merged, curBase);

    // ##正则替换（content rule 的 replace 字段，legado 风格多组规则）
    if (!rule.replace.empty()) {
        text = detail::applyContentReplaceRule(text, rule.replace);
    }

    text = detail::sanitizeUtf8(text);
    return text;
}

std::string BookSourceEngine::getContentForSource(
    const std::string& chapterUrl,
    int sourceIndex,
    const std::string& sourceName
) {
    std::lock_guard<std::recursive_mutex> lock(pImpl->engineMutex);

    if (pImpl->sources.empty()) {
        pImpl->lastError = "No source loaded";
        return "";
    }

    size_t prev = pImpl->currentSourceIndex;

    bool selected = false;
    if (!sourceName.empty()) {
        selected = selectSourceByName(sourceName);
    } else if (sourceIndex >= 0) {
        selected = selectSource(static_cast<size_t>(sourceIndex));
    }

    if (!selected && (!sourceName.empty() || sourceIndex >= 0)) {
        pImpl->currentSourceIndex = prev;
        return "";
    }

    try {
        auto result = getContent(chapterUrl);
        pImpl->currentSourceIndex = prev;
        return result;
    } catch (...) {
        pImpl->currentSourceIndex = prev;
        throw;
    }
}

// ──────────────────────────────────────────────
// RSS 源正文提取（使用 rss_sources 表的 ruleContent）
// ──────────────────────────────────────────────
std::string BookSourceEngine::getContentForRssSource(
    const std::string& chapterUrl,
    const RssSource& rssSrc
) {
    if (!pImpl->httpFunc && !pImpl->httpClientFunc) {
        pImpl->lastError = "HTTP callback not set";
        return "";
    }

    // 从对象池获取独立的 JsRuntime（RSS 规则源需要独立的 httpFunc）
    auto threadJs = pImpl->acquireJsRuntime();

    // 透传 sourceComment
    threadJs->putVariable("source_sourceComment", rssSrc.sourceComment);

    // 设置 RSS 源的 httpFunc（使用 src.header）
    auto httpFunc = pImpl->httpFunc;
    auto httpClientFunc = pImpl->httpClientFunc;
    std::string srcHeader = rssSrc.header;
    threadJs->setHttpFunc([&srcHeader, httpFunc, httpClientFunc](const std::string& url,
                                                                   const std::string& method,
                                                                   const std::string& headersJson,
                                                                   const std::string& body) -> std::string {
        HttpRequest req;
        req.url = url;
        req.method = method.empty() ? "GET" : method;
        req.body = body;
        req.timeoutMs = 15000;
        // 完整浏览器 UA，降低被反爬拦截的概率
        req.headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
        req.headers["Accept"] = "text/html,application/xhtml+xml,application/xml,*/*;q=0.9";
        req.headers["Accept-Language"] = "zh-CN,zh;q=0.9,en;q=0.8";
        req.headers["Accept-Encoding"] = "identity";

        try {
            auto h = json::parse(headersJson.empty() ? "{}" : headersJson);
            for (auto it = h.begin(); it != h.end(); ++it) {
                if (it.value().is_string())
                    req.headers[it.key()] = it.value().get<std::string>();
            }
        } catch (...) {}

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

    // 下载正文页面（通过临时切换 currentSource 来使用 httpRequest）
    // 构造一个临时 BookSource，仅用于 httpRequest 的 header 合并
    BookSource tempSource;
    tempSource.url = rssSrc.sourceUrl;
    tempSource.name = rssSrc.sourceName;
    tempSource.timeout = 15000;
    // 解析 RSS 源的 header 到 tempSource.headers
    if (!rssSrc.header.empty()) {
        try {
            auto h = json::parse(rssSrc.header);
            for (auto it = h.begin(); it != h.end(); ++it) {
                if (it.value().is_string())
                    tempSource.headers[it.key()] = it.value().get<std::string>();
            }
        } catch (...) {}
    }

    size_t prevIdx = pImpl->currentSourceIndex;
    pImpl->sources.push_back(std::move(tempSource));
    pImpl->currentSourceIndex = pImpl->sources.size() - 1;

    std::string html;
    try {
        html = pImpl->httpRequest(chapterUrl);
    } catch (...) {
        pImpl->sources.pop_back();
        pImpl->currentSourceIndex = prevIdx;
        throw;
    }

    // 恢复
    pImpl->sources.pop_back();
    pImpl->currentSourceIndex = prevIdx;

    if (html.empty()) return "";

    // 编码转换：确保 UTF-8（对齐 downloadRss 的 ensureUtf8 逻辑）
    html = detail::ensureUtf8(html);

    // 用 ruleContent 提取正文
    auto contents = detail::applyRuleStatic(html, rssSrc.ruleContent, threadJs.get(), chapterUrl);
    std::string text;
    for (size_t i = 0; i < contents.size(); ++i) {
        if (i) text += "\n";
        text += contents[i];
    }

    text = detail::formatKeepImg(text, chapterUrl);
    text = detail::sanitizeUtf8(text);
    return text;
}

// ──────────────────────────────────────────────
// 带缓存的目录获取（逻辑下沉）
// ──────────────────────────────────────────────
/// 校验目录数据质量（过滤解析失败产生的垃圾数据）
static bool isCatalogValid(const std::vector<Chapter>& chapters) {
    if (chapters.empty()) return false;
    // 只有 1 章且标题为空 → 大概率是解析失败的垃圾数据
    if (chapters.size() == 1 && detail::isBlank(chapters[0].title)) return false;
    // 所有章节标题都为空 → 无效
    bool hasTitle = false;
    for (const auto& ch : chapters) {
        if (!detail::isBlank(ch.title)) { hasTitle = true; break; }
    }
    return hasTitle;
}

std::vector<Chapter> BookSourceEngine::getCatalogWithCache(
    const std::string& bookUrl,
    const std::string& sourceUrl,
    int sourceIndex,
    const std::string& sourceName
) {
    // 1. 优先读缓存（不持锁，纯数据库读取，不阻塞其他请求）
    if (pImpl->db) {
        auto cached = pImpl->db->getCachedCatalog(bookUrl);
        if (!cached.empty()) {
            if (isCatalogValid(cached)) return cached;
            // 缓存数据无效，清除后走网络重新请求
            try { pImpl->db->removeCachedCatalog(bookUrl); } catch (...) {}
        }
    }

    // 2. 缓存未命中，走网络请求（持锁，保护书源选择和 JS 引擎状态）
    auto chapters = getCatalogForSource(bookUrl, sourceIndex, sourceName);

    // 3. 校验后再缓存
    if (isCatalogValid(chapters) && pImpl->db) {
        try { pImpl->db->cacheBookCatalog(bookUrl, chapters); } catch (...) {}
    }

    return chapters;
}

// ──────────────────────────────────────────────
// 刷新目录（纯网络 + 防退化保护 + 更新缓存/书架）
// ──────────────────────────────────────────────
std::vector<Chapter> BookSourceEngine::refreshCatalog(
    const std::string& bookUrl,
    const std::string& sourceUrl,
    int sourceIndex,
    const std::string& sourceName
) {
    // 1. 从网络获取最新目录
    auto chapters = getCatalogForSource(bookUrl, sourceIndex, sourceName);

    // 2. 基本校验
    bool valid = isCatalogValid(chapters);

    if (valid && pImpl->db) {
        // 3. 防退化保护：检查旧缓存章节数
        int oldCatalogCount = pImpl->db->getCachedCatalogCount(bookUrl);
        int newCount = static_cast<int>(chapters.size());

        bool canReplace = true;
        if (oldCatalogCount > 10) {
            // 新目录不能比旧目录退化太多（少于 50%）
            if (newCount < oldCatalogCount / 2) {
                canReplace = false;
            }
            // 新目录只有 1 章空标题，明显是垃圾数据
            if (newCount == 1 && detail::isBlank(chapters[0].title)) {
                canReplace = false;
            }
        }

        if (canReplace) {
            // 4. 安全写入缓存
            try { pImpl->db->cacheBookCatalog(bookUrl, chapters); } catch (...) {}
            // 5. 更新书架表
            try {
                std::string lastChapter = chapters.back().title;
                pImpl->db->updateBookshelfLastChapter(bookUrl, lastChapter, newCount, false);
            } catch (...) {}
        } else {
            // 退化被拒绝，返回旧缓存目录
            auto cached = pImpl->db->getCachedCatalog(bookUrl);
            if (!cached.empty() && isCatalogValid(cached)) {
                return cached;
            }
            // 旧缓存也无效，只能返回网络数据（虽然退化了，但总比空好）
        }
    }

    return chapters;
}

// ──────────────────────────────────────────────
// 带缓存的正文获取（逻辑下沉）
// ──────────────────────────────────────────────
std::string BookSourceEngine::getContentWithCache(
    const std::string& chapterUrl,
    const std::string& bookUrl,
    int chapterIndex,
    const std::string& sourceUrl,
    int sourceIndex,
    const std::string& sourceName
) {
    // 1. 优先读缓存（不持锁，纯数据库读取，不阻塞其他请求）
    if (pImpl->db && !bookUrl.empty() && chapterIndex >= 0) {
        auto cached = pImpl->db->getCachedContent(bookUrl, chapterIndex);
        if (!cached.empty()) {
            return detail::cleanContent(cached);
        }
    }

    // 2. RSS 源正文提取：当 sourceUrl 匹配到 rss_sources 表中的源时，
    //    使用该源的 ruleContent 提取正文（而非依赖 book_sources 表）。
    //    这是 RSS 源"点开文章空白"的核心根因——RSS 源的规则在 rss_sources 表，
    //    但 getContentForSource 只查 book_sources 表（95 条 vs 337 条）。
    if (pImpl->db && !sourceUrl.empty()) {
        auto rssSrc = pImpl->db->getRssSourceByUrl(sourceUrl);
        if (rssSrc && !rssSrc->ruleContent.empty()) {
            std::lock_guard<std::recursive_mutex> lock(pImpl->engineMutex);
            auto text = getContentForRssSource(chapterUrl, *rssSrc);
            if (!text.empty() && pImpl->db && !bookUrl.empty() && chapterIndex >= 0) {
                try { pImpl->db->cacheChapterContent(bookUrl, chapterIndex, chapterUrl, text); } catch (...) {}
            }
            return text;
        }
    }

    // 3. 书源正文提取（原有逻辑）
    auto text = getContentForSource(chapterUrl, sourceIndex, sourceName);

    // 4. 自动缓存（需要 bookUrl + chapterIndex 才能写入）
    if (!text.empty() && pImpl->db && !bookUrl.empty() && chapterIndex >= 0) {
        try { pImpl->db->cacheChapterContent(bookUrl, chapterIndex, chapterUrl, text); } catch (...) {}
    }

    return text;
}

// ──────────────────────────────────────────────
// 发现/推荐
// ──────────────────────────────────────────────
std::vector<Book> BookSourceEngine::explore(const std::string& exploreUrl) {
    std::vector<Book> results;

    if (!pImpl->httpFunc && !pImpl->httpClientFunc) {
        pImpl->lastError = "HTTP callback not set";
        return results;
    }

    auto& source = pImpl->currentSource();
    auto& rule = source.exploreRule;

    if (rule.bookList.empty()) {
        rule = source.searchRule;
    }

    std::string response = pImpl->httpRequest(exploreUrl);
    if (response.empty()) return results;

    AnalyzeUrl exploreAnalyzer(exploreUrl, source.url, "", 1, pImpl->js.get());
    auto explored = exploreAnalyzer.result();

    auto bookItems = pImpl->applyRule(response, rule.bookList);

    for (auto& item : bookItems) {
        Book book;
        auto names = pImpl->applyRule(item, rule.name);
        if (!names.empty()) book.name = names[0];

        auto authors = pImpl->applyRule(item, rule.author);
        if (!authors.empty()) book.author = detail::cleanAuthorField(authors[0]);

        auto covers = pImpl->applyRule(item, rule.coverUrl);
        if (!covers.empty()) book.coverUrl = covers[0];

        std::string rawBookUrl = detail::extractFieldWithTemplateFallback(
            item,
            rule.bookUrl,
            [&](const std::string& content, const std::string& r) {
                return pImpl->applyRule(content, r);
            }
        );
        book.bookUrl = detail::resolveUrlWithBase(rawBookUrl, explored.url, pImpl->js.get());
        if (book.bookUrl.empty()) {
            book.bookUrl = detail::extractHrefFallback(item, explored.url, pImpl->js.get());
        }

        if (!book.name.empty()) {
            results.push_back(std::move(book));
        }
    }

    return results;
}

// ──────────────────────────────────────────────
// 状态查询
// ──────────────────────────────────────────────
bool BookSourceEngine::isAvailable() const {
    return !pImpl->sources.empty() &&
           (pImpl->httpFunc || pImpl->httpClientFunc);
}

std::string BookSourceEngine::getLastError() const {
    return pImpl->lastError;
}

std::string BookSourceEngine::getSourceInfo() const {
    auto& s = pImpl->currentSource();
    json j;
    j["name"] = s.name;
    j["url"] = s.url;
    j["group"] = s.group;
    j["enabled"] = s.enabled;
    j["searchUrl"] = s.searchUrl;
    j["sourceCount"] = pImpl->sources.size();
    j["currentIndex"] = pImpl->currentSourceIndex;
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

SourceListResult BookSourceEngine::getSourceList() const {
    SourceListResult result;
    // totalCount 只统计有 searchUrl 的书源（与验证进度条口径一致）
    result.totalCount = static_cast<int>(std::count_if(
        pImpl->sources.begin(), pImpl->sources.end(),
        [](const BookSource& s) { return !s.searchUrl.empty(); }
    ));

    // 构建数据库 latency 兜底映射（内存中 latency 未同步时从数据库查）
    std::map<std::string, int> dbLatencyMap;
    if (pImpl->db) {
        try {
            auto dbSources = pImpl->db->getAllSources();
            for (const auto& ds : dbSources) {
                if (ds.latencyMs >= 0) {
                    dbLatencyMap[ds.url] = ds.latencyMs;
                }
            }
        } catch (...) {}
    }

    for (const auto& s : pImpl->sources) {
        std::string v = validityToString(s.validity);

        // 跳过无搜索地址的书源（与 totalCount 口径一致）
        if (s.searchUrl.empty()) {
            continue;
        }

        // 统计各状态数量（stats）
        switch (s.validity) {
            case SourceValidity::Excellent: ++result.stats.excellent; break;
            case SourceValidity::Good:      ++result.stats.good; break;
            case SourceValidity::Poor:      ++result.stats.poor; break;
            case SourceValidity::Invalid:   ++result.stats.invalid; break;
            default:                        ++result.stats.unknown; break;
        }

        // 统计有效源（只统计有 searchUrl 的）
        if (isValidValidity(s.validity)) {
            ++result.validCount;
        }

        // 书源列表只返回待检测(Unknown)、优(Excellent)、良(Good)三种状态
        // 差(Poor)和无效(Invalid)不进入列表，只保留在 stats 统计中
        if (s.validity == SourceValidity::Poor || s.validity == SourceValidity::Invalid) {
            continue;
        }

        SourceSummary summary;
        summary.name = s.name;
        summary.url = s.url;
        summary.group = s.group;
        summary.searchUrl = s.searchUrl;
        summary.exploreUrl = s.exploreUrl;
        summary.validity = v;

        // latency 兜底：内存中没有就从数据库查
        if (s.latencyMs >= 0) {
            summary.latencyMs = s.latencyMs;
        } else {
            auto it = dbLatencyMap.find(s.url);
            if (it != dbLatencyMap.end()) {
                summary.latencyMs = it->second;
            }
        }

        result.sources.push_back(std::move(summary));
    }

    return result;
}

std::string BookSourceEngine::exportGoodSources() const {
    // 筛选优+良的书源，使用 SourceParser::serializeArray 序列化为标准 JSON
    std::vector<BookSource> goodSources;
    for (const auto& s : pImpl->sources) {
        if (s.searchUrl.empty()) continue;
        if (s.validity == SourceValidity::Excellent || s.validity == SourceValidity::Good) {
            goodSources.push_back(s);
        }
    }
    return SourceParser::serializeArray(goodSources);
}

std::string BookSourceEngine::version() {
    return ARIAREAD_VERSION;
}

} // namespace ariaread

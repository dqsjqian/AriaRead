#include "ariaread/database.h"
#include "ariaread/engine_impl.h"
#include <unordered_set>

namespace ariaread {

using json = nlohmann::json;
using detail::sanitizeUtf8;

// ──────────────────────────────────────────────
// Impl
// ──────────────────────────────────────────────
class SourceDatabase::Impl {
public:
    std::unique_ptr<sqlite::database> db;
};

// ──────────────────────────────────────────────
// 构造/析构
// ──────────────────────────────────────────────
SourceDatabase::SourceDatabase(const std::string& dbPath)
    : pImpl(std::make_unique<Impl>()) {
    pImpl->db = std::make_unique<sqlite::database>(dbPath);

    // 启用 WAL 模式：允许读写并发（读操作不会被写操作阻塞）
    // 这对多线程场景至关重要：后台刷新/检测线程写入时，Web 请求线程仍可读取
    *pImpl->db << "PRAGMA journal_mode = WAL;";
    // 设置 busy_timeout：当数据库被锁时，等待最多 5 秒而不是立即失败
    *pImpl->db << "PRAGMA busy_timeout = 5000;";
    // A1: 外键 PRAGMA 一开连接就设（连接级配置，每次 open DB 都必须显式启用）
    // 保证 ON DELETE CASCADE 生效（未来若改为连接池方案仍会正确）
    *pImpl->db << "PRAGMA foreign_keys = ON;";

    initSchema();
    initRssSchema();
    runMigrations();
}

SourceDatabase::~SourceDatabase() = default;

SourceDatabase::SourceDatabase(SourceDatabase&&) noexcept = default;
SourceDatabase& SourceDatabase::operator=(SourceDatabase&&) noexcept = default;

// ──────────────────────────────────────────────
// 初始化表结构
// ──────────────────────────────────────────────
void SourceDatabase::initRssSchema() {
    auto& db = *pImpl->db;
    try {
        // 检测旧表列数（旧版宽表 38 列，新版精简 14 列）
        int colCount = 0;
        bool hasValidity = false;
        try {
            db << "PRAGMA table_info(rss_sources);" >> [&](int, std::string name, std::string, int, std::optional<std::string>, int) {
                ++colCount;
                if (name == "validity") hasValidity = true;
            };
        } catch (...) {}

        // 旧版宽表（>14 列）需要先迁移数据，不能粗暴 DROP
        if (colCount > 14) {
            db << "ALTER TABLE rss_sources RENAME TO rss_sources_old;";

            db << "CREATE TABLE IF NOT EXISTS rss_sources ("
                  "  source_url TEXT PRIMARY KEY NOT NULL,"
                  "  source_name TEXT NOT NULL DEFAULT '',"
                  "  source_icon TEXT NOT NULL DEFAULT '',"
                  "  source_group TEXT NOT NULL DEFAULT '',"
                  "  enabled INTEGER NOT NULL DEFAULT 1,"
                  "  sort_url TEXT NOT NULL DEFAULT '',"
                  "  article_style INTEGER NOT NULL DEFAULT 0,"
                  "  custom_order INTEGER NOT NULL DEFAULT 0,"
                  "  last_update_time INTEGER NOT NULL DEFAULT 0,"
                  "  validity TEXT NOT NULL DEFAULT 'unknown',"
                  "  latency_ms INTEGER NOT NULL DEFAULT -1,"
                  "  source_json TEXT NOT NULL DEFAULT '',"
                  "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
                  "  updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))"
                  ");";
            db << "CREATE INDEX IF NOT EXISTS idx_rss_group ON rss_sources(source_group);";
            db << "CREATE INDEX IF NOT EXISTS idx_rss_enabled ON rss_sources(enabled);";
            db << "CREATE INDEX IF NOT EXISTS idx_rss_validity ON rss_sources(validity);";

            // 迁移核心字段
            db << "INSERT INTO rss_sources "
                  "(source_url, source_name, source_icon, source_group, enabled, "
                  "sort_url, article_style, custom_order, last_update_time, source_json, "
                  "created_at, updated_at) "
                  "SELECT source_url, source_name, source_icon, source_group, enabled, "
                  "sort_url, article_style, custom_order, last_update_time, source_json, "
                  "created_at, updated_at "
                  "FROM rss_sources_old;";
            db << "DROP TABLE rss_sources_old;";
        } else if (colCount == 0) {
            // 表不存在，直接创建
            db << "CREATE TABLE IF NOT EXISTS rss_sources ("
                  "  source_url TEXT PRIMARY KEY NOT NULL,"
                  "  source_name TEXT NOT NULL DEFAULT '',"
                  "  source_icon TEXT NOT NULL DEFAULT '',"
                  "  source_group TEXT NOT NULL DEFAULT '',"
                  "  enabled INTEGER NOT NULL DEFAULT 1,"
                  "  sort_url TEXT NOT NULL DEFAULT '',"
                  "  article_style INTEGER NOT NULL DEFAULT 0,"
                  "  custom_order INTEGER NOT NULL DEFAULT 0,"
                  "  last_update_time INTEGER NOT NULL DEFAULT 0,"
                  "  validity TEXT NOT NULL DEFAULT 'unknown',"
                  "  latency_ms INTEGER NOT NULL DEFAULT -1,"
                  "  source_json TEXT NOT NULL DEFAULT '',"
                  "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
                  "  updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))"
                  ");";
            db << "CREATE INDEX IF NOT EXISTS idx_rss_group ON rss_sources(source_group);";
            db << "CREATE INDEX IF NOT EXISTS idx_rss_enabled ON rss_sources(enabled);";
            db << "CREATE INDEX IF NOT EXISTS idx_rss_validity ON rss_sources(validity);";
            hasValidity = true;
        }

        // 迁移：如果旧表缺少 validity 列，ALTER TABLE 添加
        if (!hasValidity && colCount > 0 && colCount <= 14) {
            try {
                db << "ALTER TABLE rss_sources ADD COLUMN validity TEXT NOT NULL DEFAULT 'unknown';";
                db << "ALTER TABLE rss_sources ADD COLUMN latency_ms INTEGER NOT NULL DEFAULT -1;";
                db << "CREATE INDEX IF NOT EXISTS idx_rss_validity ON rss_sources(validity);";
                // 从 source_json 同步 __validity / __latencyMs 到新列
                db << "UPDATE rss_sources SET "
                      "validity = COALESCE(json_extract(source_json, '$.__validity'), 'unknown'),"
                      "latency_ms = COALESCE(CAST(json_extract(source_json, '$.__latencyMs') AS INTEGER), -1);";
            } catch (...) {}
        }

        db << "CREATE TABLE IF NOT EXISTS rss_articles ("
              "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
              "  source_url TEXT NOT NULL,"
              "  title TEXT NOT NULL DEFAULT '',"
              "  link TEXT NOT NULL DEFAULT '',"
              "  pub_date INTEGER NOT NULL DEFAULT 0,"
              "  description TEXT NOT NULL DEFAULT '',"
              "  content TEXT NOT NULL DEFAULT '',"
              "  image TEXT NOT NULL DEFAULT '',"
              "  article_order INTEGER NOT NULL DEFAULT 0,"
              "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
              "  UNIQUE(source_url, link)"
              ");";
        // 迁移：旧库 rss_articles 缺少 article_order 列时补充
        try {
            bool hasOrder = false;
            db << "PRAGMA table_info(rss_articles);"
               >> [&](int, std::string name, std::string, int, std::optional<std::string>, int) {
                    if (name == "article_order") hasOrder = true;
               };
            if (!hasOrder) {
                db << "ALTER TABLE rss_articles ADD COLUMN article_order INTEGER NOT NULL DEFAULT 0;";
            }
        } catch (...) {}
        db << "CREATE INDEX IF NOT EXISTS idx_rss_article_source ON rss_articles(source_url);";
        db << "CREATE INDEX IF NOT EXISTS idx_rss_article_date ON rss_articles(pub_date DESC);";
        db << "CREATE INDEX IF NOT EXISTS idx_rss_article_order ON rss_articles(article_order DESC);";
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to init RSS schema: " + std::string(e.what()));
    }
}

void SourceDatabase::initSchema() {
    auto& db = *pImpl->db;
    try {
        db << "CREATE TABLE IF NOT EXISTS book_sources ("
              "  url TEXT PRIMARY KEY NOT NULL,"
              "  name TEXT NOT NULL DEFAULT '',"
              "  group_name TEXT NOT NULL DEFAULT '',"
              "  icon TEXT NOT NULL DEFAULT '',"
              "  comment TEXT NOT NULL DEFAULT '',"
              "  enabled INTEGER NOT NULL DEFAULT 1,"
              "  enabled_explore INTEGER NOT NULL DEFAULT 1,"
              "  weight INTEGER NOT NULL DEFAULT 0,"
              "  search_url TEXT NOT NULL DEFAULT '',"
              "  explore_url TEXT NOT NULL DEFAULT '',"
              "  timeout INTEGER NOT NULL DEFAULT 30000,"
              "  validity INTEGER NOT NULL DEFAULT 0,"
              "  latency_ms INTEGER NOT NULL DEFAULT -1,"
              "  source_json TEXT NOT NULL DEFAULT '',"
              "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
              "  updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))"
              ");";

        // 索引：按分组查询、按有效性查询、按延迟排序
        db << "CREATE INDEX IF NOT EXISTS idx_group ON book_sources(group_name);";
        db << "CREATE INDEX IF NOT EXISTS idx_validity ON book_sources(validity);";
        db << "CREATE INDEX IF NOT EXISTS idx_latency ON book_sources(latency_ms);";
        db << "CREATE INDEX IF NOT EXISTS idx_name ON book_sources(name);";

        // 书架表（book_url 作为主键）
        db << "CREATE TABLE IF NOT EXISTS bookshelf ("
              "  book_url TEXT PRIMARY KEY NOT NULL,"
              "  book_name TEXT NOT NULL DEFAULT '',"
              "  book_author TEXT NOT NULL DEFAULT '',"
              "  cover_url TEXT NOT NULL DEFAULT '',"
              "  source_name TEXT NOT NULL DEFAULT '',"
              "  source_url TEXT NOT NULL DEFAULT '',"
              "  intro TEXT NOT NULL DEFAULT '',"
              "  kind TEXT NOT NULL DEFAULT '',"
              "  last_chapter TEXT NOT NULL DEFAULT '',"
              "  total_chapters INTEGER NOT NULL DEFAULT 0,"
              "  has_update INTEGER NOT NULL DEFAULT 0,"
              "  sort_order INTEGER NOT NULL DEFAULT 0,"
              "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
              "  updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))"
              ");";

        db << "CREATE INDEX IF NOT EXISTS idx_bookshelf_updated ON bookshelf(updated_at);";
        // E1: 补 source_url 索引（换源、按源查询、以及未来 D3 书架分组等场景均会走此列）
        db << "CREATE INDEX IF NOT EXISTS idx_bookshelf_source_url ON bookshelf(source_url);";
        // D3 预留：如后续补 group_name 列，migration 会再加 idx_bookshelf_group
        // A1 注：PRAGMA foreign_keys 已在构造函数统一设置，不再重复。

        // URL 历史记录表
        db << "CREATE TABLE IF NOT EXISTS source_url_history ("
              "  url TEXT PRIMARY KEY NOT NULL,"
              "  used_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))"
              ");";

        // 阅读进度表（book_url 作为主键，外键关联 bookshelf）
        db << "CREATE TABLE IF NOT EXISTS read_progress ("
              "  book_url TEXT PRIMARY KEY NOT NULL,"
              "  chapter_index INTEGER NOT NULL DEFAULT 0,"
              "  chapter_url TEXT NOT NULL DEFAULT '',"
              "  chapter_title TEXT NOT NULL DEFAULT '',"
              "  page_offset INTEGER NOT NULL DEFAULT 0,"
              "  read_percent REAL NOT NULL DEFAULT 0.0,"
              "  last_read_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
              "  FOREIGN KEY(book_url) REFERENCES bookshelf(book_url) ON DELETE CASCADE"
              ");";

        // 目录缓存表（(book_url, chapter_index) 联合主键，支持重复 chapter_url 的书源）
        db << "CREATE TABLE IF NOT EXISTS book_chapters ("
              "  book_url TEXT NOT NULL,"
              "  chapter_url TEXT NOT NULL DEFAULT '',"
              "  chapter_index INTEGER NOT NULL DEFAULT 0,"
              "  chapter_title TEXT NOT NULL DEFAULT '',"
              "  is_vip INTEGER NOT NULL DEFAULT 0,"
              "  cached_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
              "  PRIMARY KEY(book_url, chapter_index),"
              "  FOREIGN KEY(book_url) REFERENCES bookshelf(book_url) ON DELETE CASCADE"
              ");";
        db << "CREATE INDEX IF NOT EXISTS idx_chapters_book ON book_chapters(book_url);";

        // 正文缓存表（(book_url, chapter_index) 联合主键，支持重复 chapter_url 的书源）
        db << "CREATE TABLE IF NOT EXISTS chapter_content ("
              "  book_url TEXT NOT NULL,"
              "  chapter_index INTEGER NOT NULL DEFAULT 0,"
              "  chapter_url TEXT NOT NULL DEFAULT '',"
              "  content TEXT NOT NULL DEFAULT '',"
              "  cached_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
              "  PRIMARY KEY(book_url, chapter_index),"
              "  FOREIGN KEY(book_url) REFERENCES bookshelf(book_url) ON DELETE CASCADE"
              ");";
        db << "CREATE INDEX IF NOT EXISTS idx_content_book ON chapter_content(book_url);";

        // ── 数据迁移：清洗历史脏数据 ──
        // 历史版本的作者清洗用硬编码前缀列表，未能处理"作者：作者：xxx"嵌套情况，
        // 也未处理"xxx 著"后缀。此处用 legado 风格在启动时一次性洗库。
        // 幂等：清洁数据不会被改动。
        // 注：SQLite SUBSTR 对 TEXT 按字符（UTF-8 codepoint）计数，"作者：" 是 3 字符
        try {
            for (int i = 0; i < 5; ++i) {  // 最多剥离 5 层嵌套前缀
                db << "UPDATE bookshelf SET book_author = "
                      "  TRIM(SUBSTR(book_author, 4)) "
                      "WHERE book_author LIKE '作者：%' OR book_author LIKE '作者:%' OR book_author LIKE '作者 %';";
            }
            // 剥离末尾的 " 著"（2 字符）和 "著"（1 字符）
            db << "UPDATE bookshelf SET book_author = "
                  "  TRIM(SUBSTR(book_author, 1, LENGTH(book_author) - 2)) "
                  "WHERE book_author LIKE '% 著';";
            db << "UPDATE bookshelf SET book_author = "
                  "  TRIM(SUBSTR(book_author, 1, LENGTH(book_author) - 1)) "
                  "WHERE LENGTH(book_author) > 1 AND book_author LIKE '%著';";
        } catch (const sqlite::sqlite_exception&) {
            // 迁移失败不阻塞启动
        }
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to init database schema: " + std::string(e.what()));
    }
}

// ──────────────────────────────────────────────
// 数据库迁移（E1：一次性清理旧 validity 脏数据）
// ──────────────────────────────────────────────
void SourceDatabase::runMigrations() {
    auto& db = *pImpl->db;
    try {
        // E1: 历史上 validity 曾用过 1=Checking / 2=Valid 两个值，现在枚举只用 0/3/4/5/6，
        //     把残留的 1/2 统一回退为 0（Unknown），下次验证重新定级。
        db << "UPDATE book_sources SET validity = 0 WHERE validity = 1 OR validity = 2;";
    } catch (const sqlite::sqlite_exception&) {
        // migration 非关键路径，失败也不要阻塞服务启动
    }
}

// ──────────────────────────────────────────────
// 插入书源（url 去重）
// ──────────────────────────────────────────────
int SourceDatabase::insertSources(const std::vector<BookSource>& sources) {
    auto& db = *pImpl->db;
    int inserted = 0;

    try {
        db << "begin;";

        auto stmt = db << "INSERT OR IGNORE INTO book_sources "
                          "(url, name, group_name, icon, comment, enabled, enabled_explore, "
                          "weight, search_url, explore_url, timeout, source_json) "
                          "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

        for (const auto& s : sources) {
            if (s.url.empty()) continue;

            // 序列化完整书源为 JSON
            std::string jsonStr = serializeSource(s);

            try {
                db << "INSERT OR IGNORE INTO book_sources "
                      "(url, name, group_name, icon, comment, enabled, enabled_explore, "
                      "weight, search_url, explore_url, timeout, source_json) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"
                   << s.url << s.name << s.group << s.icon << s.comment
                   << s.enabled << s.enabledExplore << s.weight
                   << s.searchUrl << s.exploreUrl << s.timeout
                   << jsonStr;
                ++inserted;
            } catch (const sqlite::sqlite_exception&) {
                // INSERT OR IGNORE 不会抛约束异常，但以防万一
            } catch (...) {
                // 跳过无法插入的源
            }
        }

        db << "commit;";
    } catch (const sqlite::sqlite_exception& e) {
        try { db << "rollback;"; } catch (...) {}
        throw std::runtime_error("Failed to insert sources: " + std::string(e.what()));
    }

    return inserted;
}

// ──────────────────────────────────────────────
// 查询所有书源
// ──────────────────────────────────────────────
std::vector<BookSource> SourceDatabase::getAllSources() {
    auto& db = *pImpl->db;
    std::vector<BookSource> result;

    try {
        db << "SELECT url, name, group_name, icon, comment, enabled, enabled_explore, "
              "weight, search_url, explore_url, timeout, validity, latency_ms, source_json "
              "FROM book_sources ORDER BY "
              "CASE validity WHEN 4 THEN 0 WHEN 5 THEN 1 WHEN 6 THEN 2 WHEN 0 THEN 3 WHEN 3 THEN 4 ELSE 5 END ASC, "
              "latency_ms ASC, weight DESC;"
           >> [&](const std::string& url, const std::string& name,
                 const std::string& group, const std::string& icon,
                 const std::string& comment, int enabled, int enabledExplore,
                 int weight, const std::string& searchUrl,
                 const std::string& exploreUrl, int timeout,
                 int validity, int latencyMs, const std::string& sourceJson) {
                BookSource s;
                s.url = url;
                s.name = name;
                s.group = group;
                s.icon = icon;
                s.comment = comment;
                s.enabled = enabled;
                s.enabledExplore = enabledExplore;
                s.weight = weight;
                s.searchUrl = searchUrl;
                s.exploreUrl = exploreUrl;
                s.timeout = timeout;
                s.validity = intToValidity(validity);
                s.latencyMs = latencyMs;

                // 从 source_json 反序列化规则（如果有）
                if (!sourceJson.empty()) {
                    deserializeRules(sourceJson, s);
                }

                result.push_back(std::move(s));
           };
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to query sources: " + std::string(e.what()));
    }

    return result;
}

// ──────────────────────────────────────────────
// 更新书源有效性
// ──────────────────────────────────────────────
void SourceDatabase::updateValidity(const std::string& url, SourceValidity validity, int latencyMs) {
    auto& db = *pImpl->db;
    try {
        db << "UPDATE book_sources SET validity = ?, latency_ms = ?, "
              "updated_at = strftime('%s','now') WHERE url = ?;"
           << validityToInt(validity) << latencyMs << url;
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to update validity: " + std::string(e.what()));
    }
}

// ──────────────────────────────────────────────
// 删除无效书源
// ──────────────────────────────────────────────
int SourceDatabase::removeInvalidSources() {
    auto& db = *pImpl->db;
    int removed = 0;
    try {
        // Poor（差）也视为无效，一并删除
        db << "SELECT COUNT(*) FROM book_sources WHERE validity IN (?, ?);"
           << validityToInt(SourceValidity::Invalid)
           << validityToInt(SourceValidity::Poor)
           >> removed;

        db << "DELETE FROM book_sources WHERE validity IN (?, ?);"
           << validityToInt(SourceValidity::Invalid)
           << validityToInt(SourceValidity::Poor);
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to remove invalid sources: " + std::string(e.what()));
    }
    return removed;
}

// ──────────────────────────────────────────────
// 删除指定 URL 的书源
// ──────────────────────────────────────────────
bool SourceDatabase::removeSource(const std::string& url) {
    auto& db = *pImpl->db;
    try {
        db << "DELETE FROM book_sources WHERE url = ?;" << url;
        return db.last_insert_rowid() > 0 || true; // DELETE 不返回 affected rows，简化处理
    } catch (const sqlite::sqlite_exception& e) {
        return false;
    }
}

// ──────────────────────────────────────────────
// 统计
// ──────────────────────────────────────────────
int SourceDatabase::getSourceCount() {
    auto& db = *pImpl->db;
    int count = 0;
    try {
        db << "SELECT COUNT(*) FROM book_sources;" >> count;
    } catch (...) {}
    return count;
}

int SourceDatabase::getValidCount() {
    auto& db = *pImpl->db;
    int count = 0;
    try {
        db << "SELECT COUNT(*) FROM book_sources WHERE validity IN (?, ?);"
           << validityToInt(SourceValidity::Excellent)
           << validityToInt(SourceValidity::Good) >> count;
    } catch (...) {}
    return count;
}

int SourceDatabase::getInvalidCount() {
    auto& db = *pImpl->db;
    int count = 0;
    try {
        db << "SELECT COUNT(*) FROM book_sources WHERE validity = ?;"
           << validityToInt(SourceValidity::Invalid) >> count;
    } catch (...) {}
    return count;
}

// ──────────────────────────────────────────────
// 检查 URL 是否已存在
// ──────────────────────────────────────────────
bool SourceDatabase::sourceExists(const std::string& url) {
    auto& db = *pImpl->db;
    int count = 0;
    try {
        db << "SELECT COUNT(*) FROM book_sources WHERE url = ?;" << url >> count;
    } catch (...) {}
    return count > 0;
}

// ──────────────────────────────────────────────
// 静默排序：按延迟重排（快的排前面）
// ──────────────────────────────────────────────
void SourceDatabase::sortByLatency() {
    // SQLite 查询已默认 ORDER BY latency_ms ASC, weight DESC
    // 无需额外操作，查询时自动排序
}

// ──────────────────────────────────────────────
// 辅助：枚举转换
// ──────────────────────────────────────────────
int SourceDatabase::validityToInt(SourceValidity v) {
    switch (v) {
        case SourceValidity::Unknown:   return 0;
        case SourceValidity::Invalid:   return 3;
        case SourceValidity::Excellent: return 4;
        case SourceValidity::Good:      return 5;
        case SourceValidity::Poor:      return 6;
    }
    return 0;
}

SourceValidity SourceDatabase::intToValidity(int v) {
    switch (v) {
        case 3:  return SourceValidity::Invalid;
        case 4:  return SourceValidity::Excellent;
        case 5:  return SourceValidity::Good;
        case 6:  return SourceValidity::Poor;
        default: return SourceValidity::Unknown;  // 0,1,2 等旧数据统一映射为 Unknown
    }
}

// ──────────────────────────────────────────────
// 辅助：序列化/反序列化
// ──────────────────────────────────────────────
std::string SourceDatabase::serializeSource(const BookSource& s) {
    json j;
    j["bookSourceName"] = sanitizeUtf8(s.name);
    j["bookSourceUrl"] = sanitizeUtf8(s.url);
    if (!s.group.empty())        j["bookSourceGroup"] = sanitizeUtf8(s.group);
    if (!s.icon.empty())         j["bookSourceIcon"] = sanitizeUtf8(s.icon);
    if (!s.comment.empty())      j["bookSourceComment"] = sanitizeUtf8(s.comment);
    j["enabled"] = s.enabled;
    j["enabledExplore"] = s.enabledExplore;
    if (s.weight != 0)           j["weight"] = s.weight;
    if (!s.searchUrl.empty())    j["searchUrl"] = sanitizeUtf8(s.searchUrl);
    if (!s.exploreUrl.empty())   j["exploreUrl"] = sanitizeUtf8(s.exploreUrl);
    if (s.timeout != 30000)      j["timeout"] = s.timeout;

    // 搜索规则
    if (!s.searchRule.bookList.empty()) {
        json rj;
        if (!s.searchRule.bookList.empty())   rj["bookList"] = sanitizeUtf8(s.searchRule.bookList);
        if (!s.searchRule.name.empty())       rj["name"] = sanitizeUtf8(s.searchRule.name);
        if (!s.searchRule.author.empty())     rj["author"] = sanitizeUtf8(s.searchRule.author);
        if (!s.searchRule.coverUrl.empty())   rj["coverUrl"] = sanitizeUtf8(s.searchRule.coverUrl);
        if (!s.searchRule.bookUrl.empty())    rj["bookUrl"] = sanitizeUtf8(s.searchRule.bookUrl);
        if (!s.searchRule.lastChapter.empty()) rj["lastChapter"] = sanitizeUtf8(s.searchRule.lastChapter);
        if (!s.searchRule.intro.empty())      rj["intro"] = sanitizeUtf8(s.searchRule.intro);
        if (!s.searchRule.kind.empty())       rj["kind"] = sanitizeUtf8(s.searchRule.kind);
        if (!s.searchRule.wordCount.empty())  rj["wordCount"] = sanitizeUtf8(s.searchRule.wordCount);
        j["ruleSearch"] = rj;
    }

    // 书籍详情规则
    if (!s.bookInfoRule.tocUrl.empty() || !s.bookInfoRule.init.empty() || !s.bookInfoRule.name.empty() ||
        !s.bookInfoRule.author.empty() || !s.bookInfoRule.coverUrl.empty() || !s.bookInfoRule.intro.empty() ||
        !s.bookInfoRule.kind.empty() || !s.bookInfoRule.lastChapter.empty() || !s.bookInfoRule.wordCount.empty()) {
        json rj;
        if (!s.bookInfoRule.init.empty())        rj["init"] = sanitizeUtf8(s.bookInfoRule.init);
        if (!s.bookInfoRule.name.empty())        rj["name"] = sanitizeUtf8(s.bookInfoRule.name);
        if (!s.bookInfoRule.author.empty())      rj["author"] = sanitizeUtf8(s.bookInfoRule.author);
        if (!s.bookInfoRule.coverUrl.empty())    rj["coverUrl"] = sanitizeUtf8(s.bookInfoRule.coverUrl);
        if (!s.bookInfoRule.intro.empty())       rj["intro"] = sanitizeUtf8(s.bookInfoRule.intro);
        if (!s.bookInfoRule.kind.empty())        rj["kind"] = sanitizeUtf8(s.bookInfoRule.kind);
        if (!s.bookInfoRule.lastChapter.empty()) rj["lastChapter"] = sanitizeUtf8(s.bookInfoRule.lastChapter);
        if (!s.bookInfoRule.wordCount.empty())   rj["wordCount"] = sanitizeUtf8(s.bookInfoRule.wordCount);
        if (!s.bookInfoRule.tocUrl.empty())      rj["tocUrl"] = sanitizeUtf8(s.bookInfoRule.tocUrl);
        j["ruleBookInfo"] = rj;
    }

    // 目录规则
    if (!s.catalogRule.chapterList.empty()) {
        json rj;
        if (!s.catalogRule.chapterList.empty()) rj["chapterList"] = sanitizeUtf8(s.catalogRule.chapterList);
        if (!s.catalogRule.chapterName.empty()) rj["chapterName"] = sanitizeUtf8(s.catalogRule.chapterName);
        if (!s.catalogRule.chapterUrl.empty())  rj["chapterUrl"] = sanitizeUtf8(s.catalogRule.chapterUrl);
        if (!s.catalogRule.nextPage.empty())    rj["nextPage"] = sanitizeUtf8(s.catalogRule.nextPage);
        j["ruleToc"] = rj;
    }

    // 正文规则
    if (!s.contentRule.content.empty()) {
        json rj;
        if (!s.contentRule.content.empty())   rj["content"] = sanitizeUtf8(s.contentRule.content);
        if (!s.contentRule.nextPage.empty())  rj["nextPage"] = sanitizeUtf8(s.contentRule.nextPage);
        if (!s.contentRule.replace.empty())   rj["replace"] = sanitizeUtf8(s.contentRule.replace);
        j["ruleContent"] = rj;
    }

    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void SourceDatabase::deserializeRules(const std::string& jsonStr, BookSource& s) {
    try {
        auto j = json::parse(sanitizeUtf8(jsonStr));

        // 搜索规则
        if (j.contains("ruleSearch") && j["ruleSearch"].is_object()) {
            auto& rj = j["ruleSearch"];
            if (rj.contains("bookList"))    s.searchRule.bookList = rj["bookList"].get<std::string>();
            if (rj.contains("name"))        s.searchRule.name = rj["name"].get<std::string>();
            if (rj.contains("author"))      s.searchRule.author = rj["author"].get<std::string>();
            if (rj.contains("coverUrl"))    s.searchRule.coverUrl = rj["coverUrl"].get<std::string>();
            if (rj.contains("bookUrl"))     s.searchRule.bookUrl = rj["bookUrl"].get<std::string>();
            if (rj.contains("lastChapter")) s.searchRule.lastChapter = rj["lastChapter"].get<std::string>();
            if (rj.contains("intro"))       s.searchRule.intro = rj["intro"].get<std::string>();
            if (rj.contains("kind"))        s.searchRule.kind = rj["kind"].get<std::string>();
            if (rj.contains("wordCount"))   s.searchRule.wordCount = rj["wordCount"].get<std::string>();
        }

        // 书籍详情规则
        if (j.contains("ruleBookInfo") && j["ruleBookInfo"].is_object()) {
            auto& rj = j["ruleBookInfo"];
            if (rj.contains("init"))        s.bookInfoRule.init = rj["init"].get<std::string>();
            if (rj.contains("name"))        s.bookInfoRule.name = rj["name"].get<std::string>();
            if (rj.contains("author"))      s.bookInfoRule.author = rj["author"].get<std::string>();
            if (rj.contains("coverUrl"))    s.bookInfoRule.coverUrl = rj["coverUrl"].get<std::string>();
            if (rj.contains("intro"))       s.bookInfoRule.intro = rj["intro"].get<std::string>();
            if (rj.contains("kind"))        s.bookInfoRule.kind = rj["kind"].get<std::string>();
            if (rj.contains("lastChapter")) s.bookInfoRule.lastChapter = rj["lastChapter"].get<std::string>();
            if (rj.contains("wordCount"))   s.bookInfoRule.wordCount = rj["wordCount"].get<std::string>();
            if (rj.contains("tocUrl"))      s.bookInfoRule.tocUrl = rj["tocUrl"].get<std::string>();
        }

        // 目录规则
        if (j.contains("ruleToc") && j["ruleToc"].is_object()) {
            auto& rj = j["ruleToc"];
            if (rj.contains("chapterList")) s.catalogRule.chapterList = rj["chapterList"].get<std::string>();
            if (rj.contains("chapterName")) s.catalogRule.chapterName = rj["chapterName"].get<std::string>();
            if (rj.contains("chapterUrl"))  s.catalogRule.chapterUrl = rj["chapterUrl"].get<std::string>();
            if (rj.contains("nextPage"))    s.catalogRule.nextPage = rj["nextPage"].get<std::string>();
        }

        // 正文规则
        if (j.contains("ruleContent") && j["ruleContent"].is_object()) {
            auto& rj = j["ruleContent"];
            if (rj.contains("content"))    s.contentRule.content = rj["content"].get<std::string>();
            if (rj.contains("nextPage"))   s.contentRule.nextPage = rj["nextPage"].get<std::string>();
            if (rj.contains("replace"))    s.contentRule.replace = rj["replace"].get<std::string>();
        }

        // 自定义 headers
        if (j.contains("header") && j["header"].is_string()) {
            try {
                auto hj = json::parse(sanitizeUtf8(j["header"].get<std::string>()));
                for (auto it = hj.begin(); it != hj.end(); ++it) {
                    s.headers[it.key()] = it.value().get<std::string>();
                }
            } catch (...) {}
        }

        // 其他字段
        if (j.contains("loginUrl"))      s.loginUrl = j["loginUrl"].get<std::string>();
        if (j.contains("loginUi"))       s.loginUi = j["loginUi"].get<std::string>();
        if (j.contains("loginCheckUrl")) s.loginCheckUrl = j["loginCheckUrl"].get<std::string>();
        if (j.contains("loginCheckJs"))  s.loginCheckJs = j["loginCheckJs"].get<std::string>();
        if (j.contains("userAgent"))     s.userAgent = j["userAgent"].get<std::string>();

    } catch (...) {
        // 反序列化失败不影响基本字段
    }
}

// ──────────────────────────────────────────────
// 书架：添加
// ──────────────────────────────────────────────
int64_t SourceDatabase::addToBookshelf(const BookshelfItem& item) {
    auto& db = *pImpl->db;
    try {
        // A1: foreign_keys 已在构造时设置，此处无需重复
        db << "INSERT OR IGNORE INTO bookshelf "
              "(book_url, book_name, book_author, cover_url, source_name, source_url, "
              "intro, kind, last_chapter, total_chapters, has_update, sort_order) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"
           << item.bookUrl << item.bookName << item.bookAuthor << item.coverUrl
           << item.sourceName << item.sourceUrl << item.intro << item.kind
           << item.lastChapter << item.totalChapters << (item.hasUpdate ? 1 : 0) << item.sortOrder;

        // 修复：INSERT OR IGNORE 在冲突时不修改 last_insert_rowid，
        // 用 changes() 判断本次是否真插入（0 = 被忽略 = 已存在）
        int changes = 0;
        db << "SELECT changes();" >> changes;
        if (changes == 0) return -1;
        return db.last_insert_rowid();
    } catch (const sqlite::sqlite_exception&) {
        return -1;
    }
}

// ──────────────────────────────────────────────
// 书架：移除（CASCADE 自动删除关联目录/正文/进度）
// ──────────────────────────────────────────────
bool SourceDatabase::removeFromBookshelf(const std::string& bookUrl) {
    auto& db = *pImpl->db;
    try {
        // A1: foreign_keys 已在构造时设置，此处无需重复
        db << "DELETE FROM bookshelf WHERE book_url = ?;" << bookUrl;
        return true;
    } catch (const sqlite::sqlite_exception&) {
        return false;
    }
}

// ──────────────────────────────────────────────
// 书架：获取列表
// ──────────────────────────────────────────────
std::vector<BookshelfItem> SourceDatabase::getBookshelf() {
    auto& db = *pImpl->db;
    std::vector<BookshelfItem> result;
    try {
        db << "SELECT rowid, book_name, book_author, cover_url, book_url, source_name, source_url, "
              "intro, kind, last_chapter, total_chapters, has_update, sort_order, created_at, updated_at "
              "FROM bookshelf ORDER BY updated_at DESC;"
           >> [&](int64_t id, const std::string& bookName, const std::string& bookAuthor,
                 const std::string& coverUrl, const std::string& bookUrl,
                 const std::string& sourceName, const std::string& sourceUrl,
                 const std::string& intro, const std::string& kind,
                 const std::string& lastChapter, int totalChapters,
                 int hasUpdate, int sortOrder, int64_t createdAt, int64_t updatedAt) {
                BookshelfItem item;
                item.id = id;
                item.bookName = bookName;
                item.bookAuthor = bookAuthor;
                item.coverUrl = coverUrl;
                item.bookUrl = bookUrl;
                item.sourceName = sourceName;
                item.sourceUrl = sourceUrl;
                item.intro = intro;
                item.kind = kind;
                item.lastChapter = lastChapter;
                item.totalChapters = totalChapters;
                item.hasUpdate = hasUpdate != 0;
                item.sortOrder = sortOrder;
                item.createdAt = createdAt;
                item.updatedAt = updatedAt;
                result.push_back(std::move(item));
           };
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to query bookshelf: " + std::string(e.what()));
    }
    return result;
}

// ──────────────────────────────────────────────
// 书架：检查是否存在
// ──────────────────────────────────────────────
bool SourceDatabase::isInBookshelf(const std::string& bookUrl) {
    auto& db = *pImpl->db;
    int count = 0;
    try {
        db << "SELECT COUNT(*) FROM bookshelf WHERE book_url = ?;" << bookUrl >> count;
    } catch (...) {}
    return count > 0;
}

// ──────────────────────────────────────────────
// 书架：更新最新章节
// ──────────────────────────────────────────────
void SourceDatabase::updateBookshelfLastChapter(const std::string& bookUrl,
                                                 const std::string& lastChapter, int totalChapters, bool hasUpdate) {
    auto& db = *pImpl->db;
    try {
        db << "UPDATE bookshelf SET last_chapter = ?, total_chapters = ?, has_update = ?, "
              "updated_at = strftime('%s','now') WHERE book_url = ?;"
           << lastChapter << totalChapters << (hasUpdate ? 1 : 0) << bookUrl;
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to update bookshelf: " + std::string(e.what()));
    }
}

// ──────────────────────────────────────────────
// 书架：换源
// 换源 = 旧 book_url 的所有缓存（目录/正文/进度）CASCADE 删除，
// 再把 bookshelf 记录的 book_url/source_name/source_url 更新为新值。
// 新目录/正文由上层重新拉取。
// ──────────────────────────────────────────────
void SourceDatabase::updateBookshelfSource(const std::string& oldBookUrl,
                                            const std::string& newSourceName, const std::string& newSourceUrl,
                                            const std::string& newBookUrl) {
    auto& db = *pImpl->db;
    try {
        // A1: foreign_keys 已在构造时设置，此处无需重复
        db << "begin;";
        // 1. 手动删除旧 book_url 关联的目录/正文/进度（CASCADE 需要 FK 开启，这里显式删更保险）
        db << "DELETE FROM book_chapters WHERE book_url = ?;" << oldBookUrl;
        db << "DELETE FROM chapter_content WHERE book_url = ?;" << oldBookUrl;
        db << "DELETE FROM read_progress WHERE book_url = ?;" << oldBookUrl;
        // 2. 更新 bookshelf 主键（book_url）及书源信息
        //    SQLite 不支持直接修改主键，需要先插入新行再删旧行
        if (newBookUrl == oldBookUrl) {
            // 新旧 bookUrl 相同，只需更新书源信息，不需要删旧行
            db << "UPDATE bookshelf SET source_name = ?, source_url = ?, "
                  "last_chapter = '', total_chapters = 0, has_update = 0, "
                  "updated_at = strftime('%s','now') WHERE book_url = ?;"
               << newSourceName << newSourceUrl << oldBookUrl;
        } else {
            // 新旧 bookUrl 不同，需要插入新行再删旧行
            db << "INSERT OR REPLACE INTO bookshelf "
                  "(book_url, book_name, book_author, cover_url, source_name, source_url, "
                  "intro, kind, last_chapter, total_chapters, has_update, sort_order, created_at) "
                  "SELECT ?, book_name, book_author, cover_url, ?, ?, "
                  "intro, kind, '', 0, 0, sort_order, created_at "
                  "FROM bookshelf WHERE book_url = ?;"
               << newBookUrl << newSourceName << newSourceUrl << oldBookUrl;
            db << "DELETE FROM bookshelf WHERE book_url = ?;" << oldBookUrl;
        }
        db << "commit;";
    } catch (const sqlite::sqlite_exception& e) {
        try { db << "rollback;"; } catch (...) {}
        throw std::runtime_error("Failed to update bookshelf source: " + std::string(e.what()));
    }
}

// ──────────────────────────────────────────────
// 书架：数量
// ──────────────────────────────────────────────
int SourceDatabase::getBookshelfCount() {
    auto& db = *pImpl->db;
    int count = 0;
    try {
        db << "SELECT COUNT(*) FROM bookshelf;" >> count;
    } catch (...) {}
    return count;
}

// ──────────────────────────────────────────────
// URL 历史记录：添加
// ──────────────────────────────────────────────
void SourceDatabase::addUrlHistory(const std::string& url, int maxCount) {
    auto& db = *pImpl->db;
    try {
        // INSERT OR REPLACE：已存在则更新 used_at
        db << "INSERT OR REPLACE INTO source_url_history (url, used_at) "
              "VALUES (?, strftime('%s','now'));"
           << url;

        // 超出上限时删除最旧的记录
        int count = 0;
        db << "SELECT COUNT(*) FROM source_url_history;" >> count;
        if (count > maxCount) {
            db << "DELETE FROM source_url_history WHERE url NOT IN "
                  "(SELECT url FROM source_url_history ORDER BY used_at DESC LIMIT ?);"
               << maxCount;
        }
    } catch (const sqlite::sqlite_exception& e) {
        // 非关键功能，静默失败
    }
}

// ──────────────────────────────────────────────
// URL 历史记录：获取
// ──────────────────────────────────────────────
std::vector<std::string> SourceDatabase::getUrlHistory(int limit) {
    auto& db = *pImpl->db;
    std::vector<std::string> result;
    try {
        db << "SELECT url FROM source_url_history ORDER BY used_at DESC LIMIT ?;"
           << limit
           >> [&](const std::string& url) {
                result.push_back(url);
           };
    } catch (...) {}
    return result;
}

// ──────────────────────────────────────────────
// URL 历史记录：删除
// ──────────────────────────────────────────────
bool SourceDatabase::removeUrlHistory(const std::string& url) {
    auto& db = *pImpl->db;
    try {
        db << "DELETE FROM source_url_history WHERE url = ?;" << url;
        return true;
    } catch (const sqlite::sqlite_exception&) {
        return false;
    }
}

// ──────────────────────────────────────────────
// 阅读进度：保存
// ──────────────────────────────────────────────
void SourceDatabase::saveReadProgress(const ReadProgress& progress) {
    auto& db = *pImpl->db;
    try {
        // 先检查书架里是否还有这本书（可能被用户删除了）
        int exists = 0;
        db << "SELECT COUNT(*) FROM bookshelf WHERE book_url = ?;"
           << progress.bookUrl >> exists;
        if (exists == 0) return; // 书已不在书架，静默跳过

        db << "INSERT OR REPLACE INTO read_progress "
              "(book_url, chapter_index, chapter_url, chapter_title, "
              "page_offset, read_percent, last_read_at) "
              "VALUES (?, ?, ?, ?, ?, ?, strftime('%s','now'));"
           << progress.bookUrl << progress.chapterIndex
           << progress.chapterUrl << progress.chapterTitle
           << progress.pageOffset << progress.readPercent;
        // 同步更新书架的 updated_at（最后阅读时间排序用）
        db << "UPDATE bookshelf SET updated_at = strftime('%s','now') WHERE book_url = ?;"
           << progress.bookUrl;
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to save read progress: " + std::string(e.what()));
    }
}

// ──────────────────────────────────────────────
// 阅读进度：获取
// ──────────────────────────────────────────────
ReadProgress SourceDatabase::getReadProgress(const std::string& bookUrl) {
    auto& db = *pImpl->db;
    ReadProgress progress;
    progress.bookUrl = bookUrl;
    try {
        db << "SELECT chapter_index, chapter_url, chapter_title, page_offset, read_percent, last_read_at "
              "FROM read_progress WHERE book_url = ?;"
           << bookUrl
           >> [&](int chapterIndex, const std::string& chapterUrl, const std::string& chapterTitle,
                 int pageOffset, double readPercent, int64_t lastReadAt) {
                progress.chapterIndex = chapterIndex;
                progress.chapterUrl = chapterUrl;
                progress.chapterTitle = chapterTitle;
                progress.pageOffset = pageOffset;
                progress.readPercent = readPercent;
                progress.lastReadAt = lastReadAt;
           };
    } catch (...) {}
    return progress;
}

// ──────────────────────────────────────────────
// 目录缓存
// ──────────────────────────────────────────────
void SourceDatabase::cacheBookCatalog(const std::string& bookUrl,
                                      const std::vector<Chapter>& chapters) {
    auto& db = *pImpl->db;
    try {
        db << "begin;";
        // 先删除旧缓存
        db << "DELETE FROM book_chapters WHERE book_url = ?;" << bookUrl;
        // 原样写入，不做任何去重处理；主键为 (book_url, chapter_index)，支持重复 chapter_url
        for (size_t i = 0; i < chapters.size(); ++i) {
            const auto& ch = chapters[i];
            db << "INSERT INTO book_chapters "
                  "(book_url, chapter_url, chapter_index, chapter_title, is_vip) "
                  "VALUES (?, ?, ?, ?, ?);"
               << bookUrl << ch.url << static_cast<int>(i) << ch.title << (ch.isVip ? 1 : 0);
        }
        db << "commit;";
    } catch (const sqlite::sqlite_exception& e) {
        try { db << "rollback;"; } catch (...) {}
        throw std::runtime_error("Failed to cache catalog: " + std::string(e.what()));
    }
}

std::vector<Chapter> SourceDatabase::getCachedCatalog(const std::string& bookUrl) {
    auto& db = *pImpl->db;
    std::vector<Chapter> result;
    try {
        db << "SELECT chapter_index, chapter_title, chapter_url, is_vip "
              "FROM book_chapters WHERE book_url = ? "
              "ORDER BY chapter_index ASC;"
           << bookUrl
           >> [&](int index, const std::string& title, const std::string& url, int isVip) {
                Chapter ch;
                ch.index = index;
                ch.title = title;
                ch.url = url;
                ch.isVip = isVip != 0;
                result.push_back(std::move(ch));
           };
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to get cached catalog: " + std::string(e.what()));
    }

    // 一致性检查：如果 book_chapters 数据异常（远少于 chapter_content），
    // 说明目录缓存曾被垃圾数据覆盖。此时从 chapter_content 表重建目录。
    // 重建的目录没有标题（用"第N章"占位），但至少能让用户正常阅读已缓存的正文。
    int contentCount = 0;
    try {
        db << "SELECT COUNT(*) FROM chapter_content WHERE book_url = ?;" << bookUrl >> contentCount;
    } catch (...) {}

    int catalogCount = static_cast<int>(result.size());
    if (contentCount > 10 && (catalogCount == 0 || catalogCount * 2 < contentCount)) {
        // 目录数据异常，从 chapter_content 表重建
        result.clear();
        try {
            db << "DELETE FROM book_chapters WHERE book_url = ?;" << bookUrl;
            db << "SELECT chapter_index, chapter_url "
                  "FROM chapter_content WHERE book_url = ? "
                  "ORDER BY chapter_index ASC;"
               << bookUrl
               >> [&](int index, const std::string& url) {
                    Chapter ch;
                    ch.index = index;
                    ch.url = url;
                    ch.title = "\xe7\xac\xac" + std::to_string(index + 1) + "\xe7\xab\xa0";  // "第N章"
                    ch.isVip = false;
                    result.push_back(std::move(ch));
               };
            // 将重建的目录写回 book_chapters 表
            if (!result.empty()) {
                for (const auto& ch : result) {
                    db << "INSERT INTO book_chapters "
                          "(book_url, chapter_url, chapter_index, chapter_title, is_vip) "
                          "VALUES (?, ?, ?, ?, ?);"
                       << bookUrl << ch.url << ch.index << ch.title << (ch.isVip ? 1 : 0);
                }
            }
        } catch (...) {}
    }

    return result;
}

void SourceDatabase::removeCachedCatalog(const std::string& bookUrl) {
    auto& db = *pImpl->db;
    try {
        db << "DELETE FROM book_chapters WHERE book_url = ?;" << bookUrl;
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to remove cached catalog: " + std::string(e.what()));
    }
}

int SourceDatabase::getCachedCatalogCount(const std::string& bookUrl) {
    auto& db = *pImpl->db;
    int count = 0;
    try {
        db << "SELECT COUNT(*) FROM book_chapters WHERE book_url = ?;" << bookUrl >> count;
    } catch (...) {}
    return count;
}

// ──────────────────────────────────────────────
// 正文缓存
// ──────────────────────────────────────────────
void SourceDatabase::cacheChapterContent(const std::string& bookUrl, int chapterIndex,
                                         const std::string& chapterUrl, const std::string& content) {
    auto& db = *pImpl->db;
    try {
        db << "INSERT OR REPLACE INTO chapter_content "
              "(book_url, chapter_index, chapter_url, content, cached_at) "
              "VALUES (?, ?, ?, ?, strftime('%s','now'));"
           << bookUrl << chapterIndex << chapterUrl << content;
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to cache content: " + std::string(e.what()));
    }
}

std::string SourceDatabase::getCachedContent(const std::string& bookUrl, int chapterIndex) {
    auto& db = *pImpl->db;
    std::string content;
    try {
        db << "SELECT content FROM chapter_content WHERE book_url = ? AND chapter_index = ?;"
           << bookUrl << chapterIndex
           >> [&](const std::string& c) { content = c; };
    } catch (...) {}
    return content;
}

void SourceDatabase::removeCachedContent(const std::string& bookUrl) {
    auto& db = *pImpl->db;
    try {
        db << "DELETE FROM chapter_content WHERE book_url = ?;" << bookUrl;
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to remove cached content: " + std::string(e.what()));
    }
}

int SourceDatabase::getCachedContentCount(const std::string& bookUrl) {
    auto& db = *pImpl->db;
    int count = 0;
    try {
        db << "SELECT COUNT(*) FROM chapter_content WHERE book_url = ?;" << bookUrl >> count;
    } catch (...) {}
    return count;
}

// ──────────────────────────────────────────────
// 批量查询（避免 N+1 问题，供 getBookshelfWithDetails 使用）
// ──────────────────────────────────────────────

std::vector<std::tuple<std::string, int>> SourceDatabase::batchGetCachedCatalogCounts() {
    auto& db = *pImpl->db;
    std::vector<std::tuple<std::string, int>> result;
    try {
        db << "SELECT book_url, COUNT(*) FROM book_chapters GROUP BY book_url;"
           >> [&](const std::string& bookUrl, int count) {
                result.emplace_back(bookUrl, count);
           };
    } catch (...) {}
    return result;
}

std::vector<std::tuple<std::string, int>> SourceDatabase::batchGetCachedContentCounts() {
    auto& db = *pImpl->db;
    std::vector<std::tuple<std::string, int>> result;
    try {
        db << "SELECT book_url, COUNT(*) FROM chapter_content GROUP BY book_url;"
           >> [&](const std::string& bookUrl, int count) {
                result.emplace_back(bookUrl, count);
           };
    } catch (...) {}
    return result;
}

std::vector<ReadProgress> SourceDatabase::batchGetReadProgress() {
    auto& db = *pImpl->db;
    std::vector<ReadProgress> result;
    try {
        db << "SELECT book_url, chapter_index, chapter_url, chapter_title, "
              "page_offset, read_percent, last_read_at FROM read_progress;"
           >> [&](const std::string& bookUrl,
                  int chapterIndex, const std::string& chapterUrl, const std::string& chapterTitle,
                  int pageOffset, double readPercent, int64_t lastReadAt) {
                ReadProgress p;
                p.bookUrl = bookUrl;
                p.chapterIndex = chapterIndex;
                p.chapterUrl = chapterUrl;
                p.chapterTitle = chapterTitle;
                p.pageOffset = pageOffset;
                p.readPercent = readPercent;
                p.lastReadAt = lastReadAt;
                result.push_back(std::move(p));
           };
    } catch (...) {}
    return result;
}

// ──────────────────────────────────────────────
// D8: RSS 订阅源 CRUD
// ──────────────────────────────────────────────

static std::string serializeRssSourceExtras(const RssSource& s) {
    json j;
    if (!s.sourceComment.empty()) j["sourceComment"] = sanitizeUtf8(s.sourceComment);
    if (!s.variableComment.empty()) j["variableComment"] = sanitizeUtf8(s.variableComment);
    if (!s.jsLib.empty()) j["jsLib"] = sanitizeUtf8(s.jsLib);
    if (s.enabledCookieJar != 1) j["enabledCookieJar"] = s.enabledCookieJar;
    if (!s.concurrentRate.empty()) j["concurrentRate"] = sanitizeUtf8(s.concurrentRate);
    if (!s.header.empty()) j["header"] = sanitizeUtf8(s.header);
    if (!s.loginUrl.empty()) j["loginUrl"] = sanitizeUtf8(s.loginUrl);
    if (!s.loginUi.empty()) j["loginUi"] = sanitizeUtf8(s.loginUi);
    if (!s.loginCheckJs.empty()) j["loginCheckJs"] = sanitizeUtf8(s.loginCheckJs);
    if (!s.coverDecodeJs.empty()) j["coverDecodeJs"] = sanitizeUtf8(s.coverDecodeJs);
    if (s.singleUrl != 0) j["singleUrl"] = s.singleUrl;
    if (!s.ruleArticles.empty()) j["ruleArticles"] = sanitizeUtf8(s.ruleArticles);
    if (!s.ruleNextPage.empty()) j["ruleNextPage"] = sanitizeUtf8(s.ruleNextPage);
    if (!s.ruleTitle.empty()) j["ruleTitle"] = sanitizeUtf8(s.ruleTitle);
    if (!s.rulePubDate.empty()) j["rulePubDate"] = sanitizeUtf8(s.rulePubDate);
    if (!s.ruleDescription.empty()) j["ruleDescription"] = sanitizeUtf8(s.ruleDescription);
    if (!s.ruleImage.empty()) j["ruleImage"] = sanitizeUtf8(s.ruleImage);
    if (!s.ruleLink.empty()) j["ruleLink"] = sanitizeUtf8(s.ruleLink);
    if (!s.ruleContent.empty()) j["ruleContent"] = sanitizeUtf8(s.ruleContent);
    if (!s.contentWhitelist.empty()) j["contentWhitelist"] = sanitizeUtf8(s.contentWhitelist);
    if (!s.contentBlacklist.empty()) j["contentBlacklist"] = sanitizeUtf8(s.contentBlacklist);
    if (!s.shouldOverrideUrlLoading.empty()) j["shouldOverrideUrlLoading"] = sanitizeUtf8(s.shouldOverrideUrlLoading);
    if (!s.style.empty()) j["style"] = sanitizeUtf8(s.style);
    if (s.enableJs != 1) j["enableJs"] = s.enableJs;
    if (s.loadWithBaseUrl != 1) j["loadWithBaseUrl"] = s.loadWithBaseUrl;
    if (!s.injectJs.empty()) j["injectJs"] = sanitizeUtf8(s.injectJs);
    // 检测评级（AriaRead 内部）
    if (!s.validity.empty() && s.validity != "unknown") j["__validity"] = sanitizeUtf8(s.validity);
    if (s.latencyMs >= 0) j["__latencyMs"] = s.latencyMs;
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

static void deserializeRssSourceExtras(RssSource& s, const std::string& jsonStr) {
    if (jsonStr.empty()) return;
    try {
        json j = json::parse(sanitizeUtf8(jsonStr));
        auto text = [&](const char* key) -> std::string {
            auto it = j.find(key);
            return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
        };
        auto flag = [&](const char* key, int fallback) {
            auto it = j.find(key);
            if (it == j.end()) return fallback;
            if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
            return it->is_number_integer() ? it->get<int>() : fallback;
        };
        s.sourceComment = text("sourceComment");
        s.variableComment = text("variableComment");
        s.jsLib = text("jsLib");
        s.enabledCookieJar = flag("enabledCookieJar", 1);
        s.concurrentRate = text("concurrentRate");
        s.header = text("header");
        s.loginUrl = text("loginUrl");
        s.loginUi = text("loginUi");
        s.loginCheckJs = text("loginCheckJs");
        s.coverDecodeJs = text("coverDecodeJs");
        s.singleUrl = flag("singleUrl", 0);
        s.ruleArticles = text("ruleArticles");
        s.ruleNextPage = text("ruleNextPage");
        s.ruleTitle = text("ruleTitle");
        s.rulePubDate = text("rulePubDate");
        s.ruleDescription = text("ruleDescription");
        s.ruleImage = text("ruleImage");
        s.ruleLink = text("ruleLink");
        s.ruleContent = text("ruleContent");
        s.contentWhitelist = text("contentWhitelist");
        s.contentBlacklist = text("contentBlacklist");
        s.shouldOverrideUrlLoading = text("shouldOverrideUrlLoading");
        s.style = text("style");
        s.enableJs = flag("enableJs", 1);
        s.loadWithBaseUrl = flag("loadWithBaseUrl", 1);
        s.injectJs = text("injectJs");
        // Rating columns are authoritative; old JSON may contain a stale rating.
    } catch (...) {}
}

void SourceDatabase::upsertRssSource(const RssSource& source) {
    auto& db = *pImpl->db;
    try {
        // 把扩展字段合并进 source_json（保留原有 JSON 中的其他键）
        std::string mergedJson = source.sourceJson;
        if (!mergedJson.empty()) {
            try {
                json base = json::parse(sanitizeUtf8(mergedJson));
                json extras = json::parse(serializeRssSourceExtras(source));
                for (auto& el : extras.items()) base[el.key()] = el.value();
                mergedJson = base.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            } catch (...) {
                mergedJson = serializeRssSourceExtras(source);
            }
        } else {
            mergedJson = serializeRssSourceExtras(source);
        }

        db << "INSERT OR REPLACE INTO rss_sources "
              "(source_url, source_name, source_icon, source_group, enabled, "
              "sort_url, article_style, custom_order, last_update_time, validity, latency_ms, "
              "source_json, updated_at) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%s','now'));"
           << sanitizeUtf8(source.sourceUrl) << sanitizeUtf8(source.sourceName)
           << sanitizeUtf8(source.sourceIcon) << sanitizeUtf8(source.sourceGroup)
           << source.enabled << sanitizeUtf8(source.sortUrl) << source.articleStyle
           << source.customOrder << source.lastUpdateTime
           << sanitizeUtf8(source.validity) << source.latencyMs << mergedJson;
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to upsert RSS source: " + std::string(e.what()));
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to upsert RSS source: " + std::string(e.what()));
    }
}

std::vector<RssSource> SourceDatabase::getAllRssSources() {
    auto& db = *pImpl->db;
    std::vector<RssSource> result;
    try {
        db << "SELECT source_url, source_name, source_icon, source_group, enabled, "
              "sort_url, article_style, custom_order, last_update_time, validity, latency_ms, "
              "source_json, created_at, updated_at "
              "FROM rss_sources ORDER BY custom_order DESC, source_name;"
           >> [&](const std::string& sourceUrl, const std::string& sourceName,
                  const std::string& sourceIcon, const std::string& sourceGroup,
                  int enabled, const std::string& sortUrl, int articleStyle,
                  int customOrder, int64_t lastUpdateTime,
                  const std::string& validity, int latencyMs,
                  const std::string& sourceJson,
                  int64_t createdAt, int64_t updatedAt) {
                RssSource s;
                s.sourceUrl = sourceUrl;
                s.sourceName = sourceName;
                s.sourceIcon = sourceIcon;
                s.sourceGroup = sourceGroup;
                s.enabled = enabled;
                s.sortUrl = sortUrl;
                s.articleStyle = articleStyle;
                s.customOrder = customOrder;
                s.lastUpdateTime = lastUpdateTime;
                s.validity = validity.empty() ? "unknown" : validity;
                s.latencyMs = latencyMs;
                s.sourceJson = sourceJson;
                s.createdAt = createdAt;
                s.updatedAt = updatedAt;
                // 从 source_json 也读一次，以 JSON 中的值为准（兼容旧数据）
                deserializeRssSourceExtras(s, sourceJson);
                result.push_back(std::move(s));
           };
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to query RSS sources: " + std::string(e.what()));
    }
    return result;
}

std::optional<RssSource> SourceDatabase::getRssSourceByUrl(const std::string& sourceUrl) {
    auto& db = *pImpl->db;
    try {
        RssSource s;
        bool found = false;
        db << "SELECT source_url, source_name, source_icon, source_group, enabled, "
              "sort_url, article_style, custom_order, last_update_time, validity, latency_ms, "
              "source_json, created_at, updated_at "
              "FROM rss_sources WHERE source_url = ?;"
           << sourceUrl
           >> [&](const std::string& su, const std::string& sourceName,
                  const std::string& sourceIcon, const std::string& sourceGroup,
                  int enabled, const std::string& sortUrl, int articleStyle,
                  int customOrder, int64_t lastUpdateTime,
                  const std::string& validity, int latencyMs,
                  const std::string& sourceJson,
                  int64_t createdAt, int64_t updatedAt) {
                s.sourceUrl = su;
                s.sourceName = sourceName;
                s.sourceIcon = sourceIcon;
                s.sourceGroup = sourceGroup;
                s.enabled = enabled;
                s.sortUrl = sortUrl;
                s.articleStyle = articleStyle;
                s.customOrder = customOrder;
                s.lastUpdateTime = lastUpdateTime;
                s.validity = validity.empty() ? "unknown" : validity;
                s.latencyMs = latencyMs;
                s.sourceJson = sourceJson;
                s.createdAt = createdAt;
                s.updatedAt = updatedAt;
                deserializeRssSourceExtras(s, sourceJson);
                found = true;
           };
        if (found) return s;
    } catch (const sqlite::sqlite_exception&) {}
    return std::nullopt;
}

bool SourceDatabase::removeRssSource(const std::string& sourceUrl) {
    auto& db = *pImpl->db;
    try {
        db << "DELETE FROM rss_articles WHERE source_url = ?;" << sourceUrl;
        db << "DELETE FROM rss_sources WHERE source_url = ?;" << sourceUrl;
        return true;
    } catch (const sqlite::sqlite_exception&) {
        return false;
    }
}

void SourceDatabase::updateRssSourceLastUpdateTime(const std::string& sourceUrl, int64_t timestamp) {
    auto& db = *pImpl->db;
    try {
        db << "UPDATE rss_sources SET last_update_time = ?, updated_at = strftime('%s','now') "
              "WHERE source_url = ?;"
           << timestamp << sourceUrl;
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to update RSS source: " + std::string(e.what()));
    }
}

void SourceDatabase::clearAllRss() {
    auto& db = *pImpl->db;
    try {
        db << "DELETE FROM rss_articles;";
        db << "DELETE FROM rss_sources;";
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to clear RSS: " + std::string(e.what()));
    }
}

void SourceDatabase::upsertRssArticle(const RssArticle& article) {
    auto& db = *pImpl->db;
    try {
        db << "INSERT INTO rss_articles (source_url, title, link, pub_date, description, content, image, article_order) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
              "ON CONFLICT(source_url, link) DO UPDATE SET "
              "title=excluded.title, pub_date=excluded.pub_date, "
              "description=excluded.description, content=excluded.content, image=excluded.image, "
              "article_order=excluded.article_order;"
           << article.sourceUrl << article.title << article.link << article.pubDate
           << article.description << article.content << article.image << article.order;
    } catch (const sqlite::sqlite_exception&) {
        // 忽略冲突异常
    }
}

std::vector<RssArticle> SourceDatabase::getRssArticles(const std::string& sourceUrl, int page, int pageSize, int& total) {
    auto& db = *pImpl->db;
    std::vector<RssArticle> result;
    total = 0;
    try {
        if (sourceUrl.empty()) {
            db << "SELECT COUNT(*) FROM rss_articles;" >> total;
            db << "SELECT id, source_url, title, link, pub_date, description, content, image, article_order "
                  "FROM rss_articles ORDER BY pub_date DESC, article_order DESC LIMIT ? OFFSET ?;"
               << pageSize << (page - 1) * pageSize
               >> [&](int64_t id, const std::string& sUrl, const std::string& title,
                      const std::string& link, int64_t pubDate, const std::string& description,
                      const std::string& content, const std::string& image, int64_t order) {
                    RssArticle a;
                    a.id = id;
                    a.sourceUrl = sUrl;
                    a.title = title;
                    a.link = link;
                    a.pubDate = pubDate;
                    a.description = description;
                    a.content = content;
                    a.image = image;
                    a.order = order;
                    result.push_back(std::move(a));
               };
        } else {
            db << "SELECT COUNT(*) FROM rss_articles WHERE source_url = ?;" << sourceUrl >> total;
            db << "SELECT id, source_url, title, link, pub_date, description, content, image, article_order "
                  "FROM rss_articles WHERE source_url = ? ORDER BY pub_date DESC, article_order DESC LIMIT ? OFFSET ?;"
               << sourceUrl << pageSize << (page - 1) * pageSize
               >> [&](int64_t id, const std::string& sUrl, const std::string& title,
                      const std::string& link, int64_t pubDate, const std::string& description,
                      const std::string& content, const std::string& image, int64_t order) {
                    RssArticle a;
                    a.id = id;
                    a.sourceUrl = sUrl;
                    a.title = title;
                    a.link = link;
                    a.pubDate = pubDate;
                    a.description = description;
                    a.content = content;
                    a.image = image;
                    a.order = order;
                    result.push_back(std::move(a));
               };
        }
    } catch (const sqlite::sqlite_exception& e) {
        throw std::runtime_error("Failed to query RSS articles: " + std::string(e.what()));
    }
    return result;
}

RssArticle SourceDatabase::getRssArticle(int64_t id) {
    auto& db = *pImpl->db;
    RssArticle result;
    try {
        db << "SELECT id, source_url, title, link, pub_date, description, content, image, article_order "
              "FROM rss_articles WHERE id = ?;"
           << id
           >> [&](int64_t rowid, const std::string& sUrl, const std::string& title,
                  const std::string& link, int64_t pubDate, const std::string& description,
                  const std::string& content, const std::string& image, int64_t order) {
                result.id = rowid;
                result.sourceUrl = sUrl;
                result.title = title;
                result.link = link;
                result.pubDate = pubDate;
                result.description = description;
                result.content = content;
                result.image = image;
                result.order = order;
           };
    } catch (...) {}
    return result;
}

void SourceDatabase::removeRssArticlesBySource(const std::string& sourceUrl) {
    auto& db = *pImpl->db;
    try {
        db << "DELETE FROM rss_articles WHERE source_url = ?;" << sourceUrl;
    } catch (const sqlite::sqlite_exception&) {}
}

// ──────────────────────────────────────────────
// 从 JSON 导入 RSS 源
// ──────────────────────────────────────────────

static void parseRssSourceFromJson(const nlohmann::json& j, RssSource& s) {
    if (j.contains("sourceUrl") && j["sourceUrl"].is_string())
        s.sourceUrl = j["sourceUrl"].get<std::string>();
    if (j.contains("sourceName") && j["sourceName"].is_string())
        s.sourceName = j["sourceName"].get<std::string>();
    if (j.contains("sourceIcon") && j["sourceIcon"].is_string())
        s.sourceIcon = j["sourceIcon"].get<std::string>();
    if (j.contains("sourceGroup") && j["sourceGroup"].is_string())
        s.sourceGroup = j["sourceGroup"].get<std::string>();
    if (j.contains("enabled")) {
        if (j["enabled"].is_boolean()) s.enabled = j["enabled"].get<bool>() ? 1 : 0;
        else if (j["enabled"].is_number()) s.enabled = j["enabled"].get<int>();
    }
    if (j.contains("sortUrl") && j["sortUrl"].is_string())
        s.sortUrl = j["sortUrl"].get<std::string>();
    if (j.contains("articleStyle") && j["articleStyle"].is_number())
        s.articleStyle = j["articleStyle"].get<int>();
    if (j.contains("customOrder") && j["customOrder"].is_number())
        s.customOrder = j["customOrder"].get<int>();
    if (j.contains("lastUpdateTime") && j["lastUpdateTime"].is_number())
        s.lastUpdateTime = j["lastUpdateTime"].get<int64_t>();

    // 第三方扩展字段
    if (j.contains("sourceComment") && j["sourceComment"].is_string())
        s.sourceComment = j["sourceComment"].get<std::string>();
    if (j.contains("variableComment") && j["variableComment"].is_string())
        s.variableComment = j["variableComment"].get<std::string>();
    if (j.contains("jsLib") && j["jsLib"].is_string())
        s.jsLib = j["jsLib"].get<std::string>();
    if (j.contains("enabledCookieJar")) {
        if (j["enabledCookieJar"].is_boolean()) s.enabledCookieJar = j["enabledCookieJar"].get<bool>() ? 1 : 0;
        else if (j["enabledCookieJar"].is_number()) s.enabledCookieJar = j["enabledCookieJar"].get<int>();
    }
    if (j.contains("concurrentRate") && j["concurrentRate"].is_string())
        s.concurrentRate = j["concurrentRate"].get<std::string>();
    if (j.contains("header") && j["header"].is_string())
        s.header = j["header"].get<std::string>();
    if (j.contains("loginUrl") && j["loginUrl"].is_string())
        s.loginUrl = j["loginUrl"].get<std::string>();
    if (j.contains("loginUi") && j["loginUi"].is_string())
        s.loginUi = j["loginUi"].get<std::string>();
    if (j.contains("loginCheckJs") && j["loginCheckJs"].is_string())
        s.loginCheckJs = j["loginCheckJs"].get<std::string>();
    if (j.contains("coverDecodeJs") && j["coverDecodeJs"].is_string())
        s.coverDecodeJs = j["coverDecodeJs"].get<std::string>();
    if (j.contains("singleUrl")) {
        if (j["singleUrl"].is_boolean()) s.singleUrl = j["singleUrl"].get<bool>() ? 1 : 0;
        else if (j["singleUrl"].is_number()) s.singleUrl = j["singleUrl"].get<int>();
    }
    if (j.contains("ruleArticles") && j["ruleArticles"].is_string())
        s.ruleArticles = j["ruleArticles"].get<std::string>();
    if (j.contains("ruleNextPage") && j["ruleNextPage"].is_string())
        s.ruleNextPage = j["ruleNextPage"].get<std::string>();
    if (j.contains("ruleTitle") && j["ruleTitle"].is_string())
        s.ruleTitle = j["ruleTitle"].get<std::string>();
    if (j.contains("rulePubDate") && j["rulePubDate"].is_string())
        s.rulePubDate = j["rulePubDate"].get<std::string>();
    if (j.contains("ruleDescription") && j["ruleDescription"].is_string())
        s.ruleDescription = j["ruleDescription"].get<std::string>();
    if (j.contains("ruleImage") && j["ruleImage"].is_string())
        s.ruleImage = j["ruleImage"].get<std::string>();
    if (j.contains("ruleLink") && j["ruleLink"].is_string())
        s.ruleLink = j["ruleLink"].get<std::string>();
    if (j.contains("ruleContent") && j["ruleContent"].is_string())
        s.ruleContent = j["ruleContent"].get<std::string>();
    if (j.contains("contentWhitelist") && j["contentWhitelist"].is_string())
        s.contentWhitelist = j["contentWhitelist"].get<std::string>();
    if (j.contains("contentBlacklist") && j["contentBlacklist"].is_string())
        s.contentBlacklist = j["contentBlacklist"].get<std::string>();
    if (j.contains("shouldOverrideUrlLoading") && j["shouldOverrideUrlLoading"].is_string())
        s.shouldOverrideUrlLoading = j["shouldOverrideUrlLoading"].get<std::string>();
    if (j.contains("style") && j["style"].is_string())
        s.style = j["style"].get<std::string>();
    if (j.contains("enableJs")) {
        if (j["enableJs"].is_boolean()) s.enableJs = j["enableJs"].get<bool>() ? 1 : 0;
        else if (j["enableJs"].is_number()) s.enableJs = j["enableJs"].get<int>();
    }
    if (j.contains("loadWithBaseUrl")) {
        if (j["loadWithBaseUrl"].is_boolean()) s.loadWithBaseUrl = j["loadWithBaseUrl"].get<bool>() ? 1 : 0;
        else if (j["loadWithBaseUrl"].is_number()) s.loadWithBaseUrl = j["loadWithBaseUrl"].get<int>();
    }
    if (j.contains("injectJs") && j["injectJs"].is_string())
        s.injectJs = j["injectJs"].get<std::string>();

    // 保存原始 JSON
    s.sourceJson = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::pair<int, std::string> SourceDatabase::importRssSourcesFromJson(const std::string& jsonText) {
    int count = 0;
    try {
        auto j = json::parse(sanitizeUtf8(jsonText));
        std::vector<RssSource> sources;

        if (j.is_array()) {
            // JSON 数组
            for (const auto& item : j) {
                if (!item.is_object()) continue;
                RssSource s;
                parseRssSourceFromJson(item, s);
                if (!s.sourceUrl.empty()) {
                    sources.push_back(std::move(s));
                }
            }
        } else if (j.is_object()) {
            // 检查是否是 sourceUrls 包装对象
            if (j.contains("sourceUrls") && j["sourceUrls"].is_array()) {
                // 返回特殊标记，让上层去逐个 URL 下载
                return {-1, "URL_LIST"};
            }
            // 单条 JSON 对象
            RssSource s;
            parseRssSourceFromJson(j, s);
            if (!s.sourceUrl.empty()) {
                sources.push_back(std::move(s));
            }
        }

        for (auto& s : sources) {
            upsertRssSource(s);
            ++count;
        }
        return {count, ""};
    } catch (const json::exception& e) {
        return {0, std::string("JSON parse error: ") + e.what()};
    } catch (const std::exception& e) {
        return {0, std::string("Import error: ") + e.what()};
    }
}

} // namespace ariaread

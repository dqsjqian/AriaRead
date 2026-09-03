/// @file engine_export.cpp
/// @brief D6: 将已全量缓存的书导出为 TXT

#include "openread/engine_impl.h"
#include <sstream>

namespace openread {

// ──────────────────────────────────────────────
// 导出为 TXT
// 格式：
//   《书名》
//   作者：XXX
//   来源：XXX
//   简介：XXX
//   ============================================
//
//   第一章 标题
//   
//   正文...
//
//   第二章 标题
//   ...
// ──────────────────────────────────────────────
std::string BookSourceEngine::exportBookToTxt(const std::string& bookUrl,
                                               const std::string& /*sourceUrl*/) {
    if (!pImpl->db) {
        pImpl->lastError = "Database not initialized";
        return "";
    }

    // 1. 读取书架条目（用来拿书名/作者/简介/来源）
    auto shelfItems = pImpl->db->getBookshelf();
    BookshelfItem book;
    bool found = false;
    for (const auto& it : shelfItems) {
        if (it.bookUrl == bookUrl) {
            book = it;
            found = true;
            break;
        }
    }
    if (!found) {
        pImpl->lastError = "Book not in bookshelf: " + bookUrl;
        return "";
    }

    // 2. 读取目录缓存
    auto chapters = pImpl->db->getCachedCatalog(bookUrl);
    if (chapters.empty()) {
        pImpl->lastError = "Catalog not cached yet";
        return "";
    }

    // 3. 按章节序组装，容忍部分章节缓存缺失（缺失章节标注"[未缓存]"）
    std::ostringstream os;
    os << "\xef\xbb\xbf";  // UTF-8 BOM，方便 Windows 记事本识别
    os << "《" << book.bookName << "》\n";
    if (!book.bookAuthor.empty()) os << "作者：" << book.bookAuthor << "\n";
    if (!book.sourceName.empty()) os << "来源：" << book.sourceName << "\n";
    if (!book.kind.empty())       os << "分类：" << book.kind << "\n";
    if (!book.intro.empty())      os << "简介：" << book.intro << "\n";
    os << "章节数：" << chapters.size() << "\n";
    os << "============================================================\n\n";

    int missing = 0;
    for (const auto& ch : chapters) {
        os << "\n\n";
        os << ch.title << "\n";
        os << "------------------------------------------------------------\n\n";
        std::string text = pImpl->db->getCachedContent(bookUrl, ch.index);
        if (text.empty()) {
            os << "[本章正文尚未缓存]\n";
            ++missing;
        } else {
            os << text << "\n";
        }
    }

    if (missing > 0) {
        pImpl->lastError = "Exported with " + std::to_string(missing) +
                            " chapter(s) missing content";
    }
    return os.str();
}

} // namespace openread

// HTTP 代理层
// 桥接 C++ 与外部 HTTP 实现（由上层注入）

#include "ariaread/types.h"
#include <string>
#include <map>

namespace ariaread {

// ──────────────────────────────────────────────
// URL 编码/解码工具
// ──────────────────────────────────────────────
namespace UrlUtils {

std::string encode(const std::string& value) {
    std::string encoded;
    encoded.reserve(value.size() * 3);

    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            encoded += buf;
        }
    }
    return encoded;
}

std::string decode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());

    for (size_t i = 0; i < value.length(); ++i) {
        if (value[i] == '%' && i + 2 < value.length()) {
            try {
                int hex = std::stoi(value.substr(i + 1, 2), nullptr, 16);
                decoded += static_cast<char>(hex);
                i += 2;
            } catch (...) {
                decoded += value[i];
            }
        } else if (value[i] == '+') {
            decoded += ' ';
        } else {
            decoded += value[i];
        }
    }
    return decoded;
}

} // namespace UrlUtils

} // namespace ariaread

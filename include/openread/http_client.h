#pragma once

#include "openread/types.h"

namespace openread {

/// 创建默认 HTTP 客户端实现（默认使用 bundled libcurl）
/// 返回空函数对象表示当前构建未启用内置 HTTP 客户端
HttpClientFunc createDefaultHttpClient();

} // namespace openread

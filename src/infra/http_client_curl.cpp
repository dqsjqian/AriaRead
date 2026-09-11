#include "openread/http_client.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

#ifdef OPENREAD_HAS_CURL
#include <curl/curl.h>
#endif

// 平台相关头文件，用于 CA 证书路径探测
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/stat.h>
#elif defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#else
#include <sys/stat.h>
#endif

namespace openread {

namespace {

inline std::string trimCopy(const std::string& s) {
    size_t l = 0;
    size_t r = s.size();
    while (l < r && std::isspace(static_cast<unsigned char>(s[l]))) ++l;
    while (r > l && std::isspace(static_cast<unsigned char>(s[r - 1]))) --r;
    return s.substr(l, r - l);
}

#ifdef OPENREAD_HAS_CURL
struct ResponseBuffer {
    std::string body;
    size_t limit;
    bool exceeded = false;
    bool allocationFailed = false;
};

size_t writeBodyCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    if (!userdata) return 0;
    auto& out = *static_cast<ResponseBuffer*>(userdata);
    if (size != 0 && nmemb > std::numeric_limits<size_t>::max() / size) {
        out.exceeded = true;
        return 0;
    }
    const size_t n = size * nmemb;
    if (n > out.limit - out.body.size()) {
        out.exceeded = true;
        return 0;
    }
    try {
        out.body.append(ptr, n);
    } catch (...) {
        out.allocationFailed = true;
        return 0;
    }
    return n;
}

size_t writeHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    if (!userdata) return 0;
    const size_t n = size * nitems;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    std::string line(buffer, n);

    auto pos = line.find(':');
    if (pos != std::string::npos) {
        std::string key = trimCopy(line.substr(0, pos));
        std::string value = trimCopy(line.substr(pos + 1));
        if (!key.empty()) {
            (*headers)[key] = value;
        }
    }
    return n;
}

// 探测系统 CA 证书 bundle 路径（OpenSSL 不像 SecureTransport/Schannel 那样自动使用系统证书链）
std::string findCaBundlePath() {
    // 1. 优先使用环境变量
    if (const char* env = std::getenv("CURL_CA_BUNDLE")) {
        return env;
    }
    if (const char* env = std::getenv("SSL_CERT_FILE")) {
        return env;
    }

#ifdef __APPLE__
    // macOS: Homebrew 安装的 CA 证书
    static const char* macPaths[] = {
        "/etc/ssl/cert.pem",                                    // macOS 系统自带
        "/opt/homebrew/etc/openssl@3/cert.pem",                 // Homebrew ARM
        "/usr/local/etc/openssl@3/cert.pem",                    // Homebrew x86
        "/opt/homebrew/share/ca-certificates/cacert.pem",       // Homebrew CA
        "/usr/local/share/ca-certificates/cacert.pem",          // Homebrew CA x86
    };
    struct stat st;
    for (const auto* p : macPaths) {
        if (::stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            return p;
        }
    }
#elif defined(_WIN32)
    // Windows: 不需要 CA bundle，curl + OpenSSL 可以通过 CURLOPT_SSL_OPTIONS
    // 设置 CURLSSLOPT_NATIVE_CA 来使用 Windows 证书存储
    return "";  // 特殊标记：使用 Windows 原生证书存储
#else
    // Linux / 其他 Unix
    static const char* linuxPaths[] = {
        "/etc/ssl/certs/ca-certificates.crt",     // Debian/Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",       // RHEL/CentOS/Fedora
        "/etc/ssl/ca-bundle.pem",                  // openSUSE
        "/etc/pki/tls/cacert.pem",                 // OpenELEC
        "/etc/ssl/cert.pem",                       // Alpine/FreeBSD
    };
    struct stat st;
    for (const auto* p : linuxPaths) {
        if (::stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            return p;
        }
    }
#endif

    return "";  // 未找到，curl 会尝试使用编译时默认路径
}

// 配置 SSL 证书验证
void configureSslCerts(CURL* curl) {
    static std::string cachedCaBundle;
    static std::once_flag caOnce;
    std::call_once(caOnce, []() {
        cachedCaBundle = findCaBundlePath();
    });

#ifdef _WIN32
    // Windows: 使用原生证书存储（curl 7.71.0+ 支持）
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#else
    if (!cachedCaBundle.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, cachedCaBundle.c_str());
    }
#endif
    // 始终启用证书验证
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
}

// 全局 curl 生命周期管理（A4）：
//   - 构造：curl_global_init（线程安全初始化）
//   - 析构：curl_global_cleanup（进程退出 / .so 卸载时释放 OpenSSL 线程数据、Winsock 等）
// 说明：由于是局部 static，C++11 保证线程安全初始化；析构在所有线程结束后执行。
struct CurlGlobalGuard {
    CurlGlobalGuard() noexcept {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~CurlGlobalGuard() noexcept {
        curl_global_cleanup();
    }
    CurlGlobalGuard(const CurlGlobalGuard&) = delete;
    CurlGlobalGuard& operator=(const CurlGlobalGuard&) = delete;
};

void ensureCurlGlobalInit() {
    static CurlGlobalGuard sGuard;  // 首次调用时初始化，程序退出时自动 cleanup
    (void)sGuard;
}

HttpResponse performCurlRequest(const HttpRequest& req) {
    HttpResponse resp;

    ensureCurlGlobalInit();

    CURL* raw = curl_easy_init();
    if (!raw) {
        resp.error = "curl_easy_init failed";
        return resp;
    }

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(raw, &curl_easy_cleanup);

    curl_easy_setopt(curl.get(), CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(req.timeoutMs));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(req.timeoutMs));

    // C1: 更严格的 libcurl 超时/防挂策略
    // - MAXREDIRS=5：防止无限 302 循环（某些书源会恶意重定向）
    // - LOW_SPEED_LIMIT + LOW_SPEED_TIME：连续 15 秒速率 < 16B/s 视为挂起，放弃请求
    //   （避免 TCP 半开连接 / 服务器挤牙膏式返回导致整体超时失效）
    // - TCP keepalive：及时探测死连接
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 16L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPINTVL, 15L);
    // 如果调用方没有显式设置 Accept-Encoding，才让 libcurl 自动处理压缩
    bool hasAcceptEncoding = false;
    for (const auto& [k, v] : req.headers) {
        if (k == "Accept-Encoding") { hasAcceptEncoding = true; break; }
    }
    if (!hasAcceptEncoding) {
        curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");  // 空串 = 启用所有支持的编码
    }

    // 配置 SSL 证书验证（OpenSSL 后端需要手动指定 CA bundle）
    configureSslCerts(curl.get());

    ResponseBuffer responseBody{{}, req.maxResponseBytes > 0 ? req.maxResponseBytes : size_t{32 * 1024 * 1024}};
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeBodyCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &responseBody);

    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, writeHeaderCallback);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &resp.headers);

    std::string method = req.method;
    std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    if (method == "POST") {
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    } else if (method != "GET") {
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    if (!req.body.empty() && method != "GET") {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    }

    struct curl_slist* curlHeaders = nullptr;
    for (const auto& [k, v] : req.headers) {
        std::string line = k + ": " + v;
        curlHeaders = curl_slist_append(curlHeaders, line.c_str());
    }
    if (curlHeaders) {
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, curlHeaders);
    }

    const CURLcode code = curl_easy_perform(curl.get());

    if (curlHeaders) {
        curl_slist_free_all(curlHeaders);
    }

    if (code != CURLE_OK) {
        if (responseBody.exceeded) {
            resp.error = "Response body exceeds " + std::to_string(responseBody.limit) + " bytes";
        } else if (responseBody.allocationFailed) {
            resp.error = "Failed to allocate response buffer";
        } else {
            resp.error = curl_easy_strerror(code);
        }
        resp.statusCode = 0;
        return resp;
    }

    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);

    resp.statusCode = static_cast<int>(status);
    resp.body = std::move(responseBody.body);
    return resp;
}
#endif

} // namespace

HttpClientFunc createDefaultHttpClient() {
#ifdef OPENREAD_HAS_CURL
    return [](const HttpRequest& req) -> HttpResponse {
        return performCurlRequest(req);
    };
#else
    return HttpClientFunc{};
#endif
}

} // namespace openread

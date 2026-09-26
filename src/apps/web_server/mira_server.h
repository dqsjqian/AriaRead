/// @file mira_server.h
/// @brief AriaRead 的 HTTP/1.1 服务层，建立在 Mira 之上（取代 cpp-httplib）。
///
/// 为什么自己有一层：Mira 一期只提供「连接上的请求循环」（`serve_connection`）
/// 与传输层，明确不做路由和静态文件——按它的分层，这些属于应用组合层。
/// 因此这里只做三件薄的事：路由表、静态文件、把阻塞业务挪出事件循环线程；
/// 解析、分帧、keep-alive、限额全部由 Mira 负责。
///
/// 线程模型（与之前的 httplib 一致，但只用一个事件循环线程）：
///   - 一个 EventLoop 线程负责 accept + 读写 + keep-alive；
///   - 每个请求派一根工作线程跑业务（搜索/RSS/导入可能耗几十秒，
///     占住事件循环会让其它请求全部停摆）；
///   - SSE（chunked provider）由工作线程产生数据，通过 StreamBridge
///     交回事件循环线程写出，客户端仍是边收边渲染。
///
/// 对外 API 有意保持与 cpp-httplib 的常用子集同形（Request/Response/
/// Server::Get/Post/...），这样 routes_*.cpp 只需换类型名，业务代码不动。

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ariaread::web {

/// 一次请求。查询串已做百分号解码（`+` 视为空格）。
struct Request {
    std::string method{"GET"};
    std::string target{};  ///< 原始请求目标，含查询串
    std::string path{};    ///< 去掉查询串后的路径
    std::string body{};
    std::unordered_map<std::string, std::string> params{};

    /// 取查询参数；不存在返回空串（与 httplib 一致）。
    [[nodiscard]] std::string get_param_value(const std::string& key) const;
    [[nodiscard]] bool has_param(const std::string& key) const;
};

/// 分块响应的写出端。工作线程调用，实现是把数据交回事件循环线程。
class DataSink {
public:
    using Fn = std::function<void(const char*, std::size_t)>;
    using Predicate = std::function<bool()>;

    DataSink(Fn write, Predicate writable = nullptr)
        : write_(std::move(write)), writable_(std::move(writable)) {}

    /// 写出一段数据；返回是否仍然可写（与 cpp-httplib 的 DataSink 同形）。
    bool write(const char* data, std::size_t size) {
        if (write_) write_(data, size);
        return is_writable();
    }
    bool write(const std::string& text) { return write(text.data(), text.size()); }

    /// 对端是否还在读。写失败或收到停止请求后为 false，长任务据此提前收尾。
    bool is_writable() const { return writable_ ? writable_() : true; }
    /// 结束本次写出（数据已即时交给事件循环，这里只是给调用方一个收尾点）。
    void done() {}

private:
    Fn write_;
    Predicate writable_;
};

/// content provider：offset 从 0 开始；返回 true 表示数据已写完。
using ContentProvider = std::function<bool(std::size_t offset, DataSink& sink)>;

class Response {
public:
    int status{200};
    std::vector<std::pair<std::string, std::string>> headers{};
    std::string body{};

    /// 设置完整响应体（Content-Length 由 Mira 自己算，不接受手填）。
    void set_content(const std::string& data, const std::string& mime) {
        body = data;
        content_type = mime;
    }
    void set_content(std::string&& data, const std::string& mime) {
        body = std::move(data);
        content_type = mime;
    }
    void set_header(const std::string& key, const std::string& value) {
        headers.emplace_back(key, value);
    }

    /// 声明一个长度未知的流式响应（SSE 走这里）。provider 由工作线程驱动。
    void set_chunked_content_provider(const std::string& mime, ContentProvider provider) {
        content_type = mime;
        provider_ = std::move(provider);
    }

    [[nodiscard]] bool has_provider() const noexcept { return provider_ != nullptr; }
    [[nodiscard]] const ContentProvider& provider() const noexcept { return provider_; }
    [[nodiscard]] const std::string& content_type_value() const noexcept {
        return content_type;
    }

private:
    std::string content_type{"text/plain"};
    ContentProvider provider_{};
};

/// 一个 Mira 驱动的 HTTP 服务。
class Server {
public:
    using Handler = std::function<void(const Request&, Response&)>;
    /// 静态文件响应前的钩子（可改响应头）；与 cpp-httplib 同名 API 语义一致。
    using FileHook = std::function<void(const Request&, Response&)>;

    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void Get(const std::string& path, Handler handler);
    void Post(const std::string& path, Handler handler);
    void Put(const std::string& path, Handler handler);
    void Delete(const std::string& path, Handler handler);

    void set_file_request_handler(FileHook hook);
    void set_static_root(std::string root);

    /// 绑定监听地址；port 传 0 由内核分配。成功返回 true。
    [[nodiscard]] bool listen(const std::string& host, int port);
    [[nodiscard]] int actual_port() const;

    /// 阻塞运行事件循环，直到 stop() 或绑定/循环出错。
    void run();
    /// 从任意线程请求停止；已在跑的请求会被取消。
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ariaread::web

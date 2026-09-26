/// @file mira_server.cpp
/// @brief mira_server.h 的实现：Mira 事件循环 + 工作线程业务派发。
///
/// 这一层只做组合：解析与分帧交给 modules/http，连接与监听交给
/// modules/transport，线程调度是 AriaRead 自己的选择（业务是阻塞的，
/// 不能跑在事件循环线程上）。

#include "mira_server.h"

#include <mira/core/error.hpp>
#include <mira/core/event_loop.hpp>
#include <mira/core/operation.hpp>
#include <mira/core/task.hpp>
#include <mira/core/task_scope.hpp>
#include <mira/http/connection.hpp>
#include <mira/http/limits.hpp>
#include <mira/http/message.hpp>
#include <mira/transport/endpoint.hpp>
#include <mira/transport/tcp.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ariaread::web {
namespace {

using Mira::EventLoop;
using Mira::OperationOptions;
using Mira::Result;
using Mira::Task;
using Mira::TaskScope;
namespace http = Mira::http;
namespace transport = Mira::transport;

using Clock = EventLoop::Clock;

// ── 小工具 ──────────────────────────────────────────────────────────────────

std::string url_decode(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < input.size()) {
            const auto hex = [](char digit) -> int {
                if (digit >= '0' && digit <= '9') return digit - '0';
                if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
                if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
                return -1;
            };
            const int high = hex(input[i + 1]);
            const int low = hex(input[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        out.push_back(c);
    }
    return out;
}

std::string upper(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::string mime_for(const std::string& path) {
    const auto extension = [&] {
        const auto dot = path.rfind('.');
        return dot == std::string::npos ? std::string{} : path.substr(dot + 1);
    }();
    if (extension == "html" || extension == "htm") return "text/html; charset=utf-8";
    if (extension == "js" || extension == "mjs") return "application/javascript; charset=utf-8";
    if (extension == "css") return "text/css; charset=utf-8";
    if (extension == "json") return "application/json; charset=utf-8";
    if (extension == "svg") return "image/svg+xml";
    if (extension == "png") return "image/png";
    if (extension == "jpg" || extension == "jpeg") return "image/jpeg";
    if (extension == "ico") return "image/x-icon";
    if (extension == "woff2") return "font/woff2";
    if (extension == "txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

std::span<const std::byte> as_bytes(std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
}

// ── 服务状态（事件循环线程与工作线程共享的部分都已加锁）────────────────────

struct Route {
    std::string method;
    std::string path;
    Server::Handler handler;
};

/// 工作线程 → 事件循环线程的单向通道。
///
/// 事件循环侧用 `sleep_until(无穷远, stop_token)` 停车（Mira 的
/// transport resolver 就是这么等线程池结果的），工作线程产出数据或结束时
/// `request_stop()` 把它唤醒，于是所有 I/O 仍然只发生在循环线程上。
class StreamBridge {
public:
    struct Head {
        int status{200};
        std::string content_type{"text/plain"};
        std::string body{};
        std::vector<std::pair<std::string, std::string>> headers{};
    };

    // ── 事件循环线程 ──
    /// 重新武装唤醒信号。`for_stream` 用于流式等待循环：那里 `streaming_`
    /// 恒为 true，不能作为「别睡」条件——否则每次 arm 都立即 request_stop，
    /// park 在提交前就被取消检查放行，事件循环被空转自旋打满，饿死所有
    /// 其它请求（实测 4 秒空转 235 万次：书源校验期间全站 loading 的根因）。
    /// 流式等待只看「已结束或已有待发数据」。
    std::stop_token arm(bool for_stream = false) {
        const std::lock_guard<std::mutex> lock(mutex_);
        signal_ = std::stop_source{};
        // 已有数据或已结束时就别睡了，否则这一侧会错过上一次的信号。
        if (finished_ || !outbox_.empty() || (!for_stream && streaming_)) {
            signal_.request_stop();
        }
        return signal_.get_token();
    }
    [[nodiscard]] bool streaming() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return streaming_;
    }
    [[nodiscard]] bool finished() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return finished_;
    }
    [[nodiscard]] bool has_pending() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return !outbox_.empty();
    }
    std::vector<std::string> take() {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> chunks(std::make_move_iterator(outbox_.begin()),
                                        std::make_move_iterator(outbox_.end()));
        outbox_.clear();
        return chunks;
    }
    Head take_head() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return std::move(head_);
    }

    // ── 工作线程 ──
    void begin_stream(int status, std::string content_type,
                      std::vector<std::pair<std::string, std::string>> headers) {
        const std::lock_guard<std::mutex> lock(mutex_);
        streaming_ = true;
        head_ = Head{status, std::move(content_type), {}, std::move(headers)};
        signal_.request_stop();
    }
    void push(std::string chunk) {
        const std::lock_guard<std::mutex> lock(mutex_);
        outbox_.push_back(std::move(chunk));
        signal_.request_stop();
    }
    void set_buffered(Head head) {
        const std::lock_guard<std::mutex> lock(mutex_);
        head_ = std::move(head);
    }
    void finish() {
        const std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        signal_.request_stop();
    }
    void cancel() { cancelled_.store(true); }
    [[nodiscard]] bool cancelled() const { return cancelled_.load(); }

private:
    std::mutex mutex_{};
    std::stop_source signal_{};
    std::deque<std::string> outbox_{};
    Head head_{};
    bool streaming_{false};
    bool finished_{false};
    std::atomic<bool> cancelled_{false};
};

struct ServerState {
    std::vector<Route> routes{};
    Server::FileHook file_hook{};
    std::string static_root{};
    std::string host{};
    int port{0};
    std::stop_source stop_source{};
    std::mutex port_mutex{};
    std::condition_variable port_ready{};
    int actual_port{-1};
    bool bound{false};
    bool failed{false};

    // 绑定失败也要立刻唤醒等端口的人：否则调用方要等到超时才知道起不来。
    void notify_failure() {
        {
            const std::lock_guard<std::mutex> lock(port_mutex);
            actual_port = -1;
            failed = true;
            bound = true;
        }
        port_ready.notify_all();
    }

    const Route* find(const std::string& method, const std::string& path) const {
        for (const auto& route : routes) {
            if (route.method == method && route.path == path) return &route;
        }
        return nullptr;
    }
};

// ── 响应构造 ────────────────────────────────────────────────────────────────

bool header_forbidden(const std::string& name) {
    // 分帧由 Mira 独占：手填这两个头会被序列化器拒绝或覆盖。
    const std::string lower = [&] {
        std::string out(name);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }();
    return lower == "content-length" || lower == "transfer-encoding";
}

http::Response make_response(const StreamBridge::Head& head) {
    http::Response response;
    response.status = head.status <= 0 ? 200u : static_cast<unsigned>(head.status);
    if (!head.content_type.empty()) {
        response.headers.append("Content-Type", head.content_type);
    }
    response.headers.append("Server", "AriaRead/Mira");
    response.headers.append("Cache-Control", "no-cache");
    response.headers.append("Access-Control-Allow-Origin", "*");
    response.headers.append("Access-Control-Allow-Headers", "*");
    response.headers.append("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    for (const auto& [name, value] : head.headers) {
        if (!header_forbidden(name)) response.headers.append(name, value);
    }
    return response;
}

// ── 静态文件 ────────────────────────────────────────────────────────────────

bool serve_static_file(ServerState& state, const Request& request, Response& response) {
    if (state.static_root.empty()) return false;

    std::string relative = request.path;
    if (relative.empty() || relative == "/" || relative.back() == '/') {
        relative += "index.html";
    }
    // 只接受仓库内路径：拒绝 .. 与绝对路径成分。
    std::vector<std::string> segments;
    std::stringstream stream(relative);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") return false;
        segments.push_back(segment);
    }
    std::filesystem::path target(state.static_root);
    for (const auto& part : segments) target /= part;

    std::error_code code;
    if (!std::filesystem::is_regular_file(target, code)) return false;
    std::ifstream file(target, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    response.set_content(buffer.str(), mime_for(target.string()));
    if (state.file_hook) state.file_hook(request, response);
    return true;
}

// ── 事件循环侧 ──────────────────────────────────────────────────────────────

Task<void> park(EventLoop& loop, std::stop_token token) {
    OperationOptions io{.stop = std::move(token), .deadline = std::nullopt};
    const Result<void> waited = co_await loop.sleep_until(Clock::time_point::max(), io);
    static_cast<void>(waited);
}

template <typename Writer>
Task<Result<void>> dispatch(ServerState& state, EventLoop& loop, const http::Request& raw,
                            Writer& writer, std::span<const std::byte> body) {
    Request request;
    request.method = upper(http::to_string(raw.method));
    request.target = raw.target;
    const auto query = raw.target.find('?');
    request.path = query == std::string::npos ? raw.target : raw.target.substr(0, query);
    request.body = std::string(reinterpret_cast<const char*>(body.data()), body.size());
    if (query != std::string::npos) {
        std::stringstream stream(raw.target.substr(query + 1));
        std::string pair;
        while (std::getline(stream, pair, '&')) {
            if (pair.empty()) continue;
            const auto equals = pair.find('=');
            if (equals == std::string::npos) {
                request.params.emplace(url_decode(pair), std::string{});
            } else {
                request.params.emplace(url_decode(pair.substr(0, equals)),
                                       url_decode(pair.substr(equals + 1)));
            }
        }
    }

    if (request.method == "OPTIONS") {
        http::Response response = make_response(StreamBridge::Head{204, "text/plain", {}, {}});
        co_return co_await writer.send(response, {});
    }

    const Route* route = state.find(request.method, request.path);
    Response response;
    StreamBridge bridge;
    std::exception_ptr failure;

    // 业务跑在自己的线程上：搜索/RSS/导入可能几十秒，占住循环会让所有请求停摆。
    std::thread worker([&] {
        try {
            if (route) {
                route->handler(request, response);
            } else if (!serve_static_file(state, request, response)) {
                response.status = 404;
                response.set_content(R"({"error":"not found"})", "application/json");
            }
        } catch (...) {
            failure = std::current_exception();
        }
        if (response.has_provider()) {
            bridge.begin_stream(response.status, response.content_type_value(),
                                response.headers);
            DataSink sink([&bridge](const char* data, std::size_t size) {
                bridge.push(std::string(data, size));
            },
                          [&bridge] { return !bridge.cancelled(); });
            try {
                static_cast<void>(response.provider()(0, sink));
            } catch (...) {
                failure = std::current_exception();
            }
        } else {
            bridge.set_buffered(StreamBridge::Head{response.status,
                                                    response.content_type_value(),
                                                    response.body, response.headers});
        }
        bridge.finish();
    });

    // 等第一个信号：要么流式响应已开头，要么业务已经跑完。
    co_await park(loop, bridge.arm());

    if (!bridge.streaming()) {
        while (!bridge.finished()) {
            co_await park(loop, bridge.arm());
        }
        worker.join();
        StreamBridge::Head head = bridge.take_head();
        if (failure) {
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& error) {
                head.status = 500;
                head.content_type = "application/json";
                head.body = std::string(R"({"error":")") + error.what() + R"("})";
            } catch (...) {
                head.status = 500;
                head.content_type = "application/json";
                head.body = R"({"error":"unknown error"})";
            }
        }
        co_return co_await writer.send(make_response(head), as_bytes(head.body));
    }

    const StreamBridge::Head head = bridge.take_head();
    const Result<void> sent = co_await writer.send_head_chunked(make_response(head));
    if (!sent) {
        worker.join();
        co_return sent;
    }
    for (;;) {
        for (std::string& chunk : bridge.take()) {
            const Result<void> written = co_await writer.write(as_bytes(chunk));
            if (!written) {
                bridge.cancel();  // 让还在跑的业务尽快收尾
                worker.join();
                co_return written;
            }
        }
        if (bridge.finished() && !bridge.has_pending()) break;
        co_await park(loop, bridge.arm(/*for_stream=*/true));
    }
    worker.join();
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[AriaRead] 流式路由异常：%s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "[AriaRead] 流式路由异常（未知类型）\n");
        }
    }
    co_return co_await writer.finish();
}

Task<void> handle_connection(ServerState& state, EventLoop& loop, transport::tcp::Socket socket) {
    http::ServerOptions options;
    options.stop = state.stop_source.get_token();
    options.idle_timeout = std::chrono::seconds(120);
    // 单个请求不设时限：SSE 搜索最长 60 秒，属于正常业务而不是慢客户端。
    options.request_timeout = std::chrono::seconds(0);
    options.max_requests_per_connection = 200;
    // 书源 JSON 导入可能几 MB，默认 1MB 偏小。
    options.limits.max_body_size = 64u * 1024u * 1024u;
    options.limits.max_chunk_size = 8u * 1024u * 1024u;

    const Result<void> served = co_await http::serve_connection(
        socket,
        [&state, &loop](const http::Request& request, auto& writer,
                        std::span<const std::byte> body) -> Task<Result<void>> {
            co_return co_await dispatch(state, loop, request, writer, body);
        },
        options);
    static_cast<void>(served);
    socket.close();
}

Task<void> accept_loop(ServerState& state, EventLoop& loop, transport::tcp::Listener& listener) {
    TaskScope scope;
    while (!state.stop_source.stop_requested()) {
        OperationOptions io{.stop = state.stop_source.get_token(), .deadline = std::nullopt};
        Result<transport::tcp::Socket> accepted = co_await listener.accept(io);
        if (!accepted) break;
        scope.spawn(handle_connection(state, loop, std::move(*accepted)));
    }
    co_await scope.join();
}

}  // namespace

// ── Server 公开接口 ─────────────────────────────────────────────────────────

struct Server::Impl {
    ServerState state{};
};

std::string Request::get_param_value(const std::string& key) const {
    const auto found = params.find(key);
    return found == params.end() ? std::string{} : found->second;
}

bool Request::has_param(const std::string& key) const {
    return params.find(key) != params.end();
}

Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() = default;

void Server::Get(const std::string& path, Handler handler) {
    impl_->state.routes.push_back(Route{"GET", path, std::move(handler)});
}
void Server::Post(const std::string& path, Handler handler) {
    impl_->state.routes.push_back(Route{"POST", path, std::move(handler)});
}
void Server::Put(const std::string& path, Handler handler) {
    impl_->state.routes.push_back(Route{"PUT", path, std::move(handler)});
}
void Server::Delete(const std::string& path, Handler handler) {
    impl_->state.routes.push_back(Route{"DELETE", path, std::move(handler)});
}

void Server::set_file_request_handler(FileHook hook) {
    impl_->state.file_hook = std::move(hook);
}
void Server::set_static_root(std::string root) {
    impl_->state.static_root = std::move(root);
}

bool Server::listen(const std::string& host, int port) {
    if (port < 0 || port > 65535) return false;
    impl_->state.host = host;
    impl_->state.port = port;
    return true;
}

int Server::actual_port() const {
    std::unique_lock<std::mutex> lock(impl_->state.port_mutex);
    impl_->state.port_ready.wait_for(lock, std::chrono::seconds(5),
                                     [&] { return impl_->state.bound; });
    return impl_->state.actual_port;
}

void Server::run() {
    ServerState& state = impl_->state;
    Result<EventLoop> loop = EventLoop::create();
    if (!loop) {
        std::fprintf(stderr, "[AriaRead] 无法创建事件循环\n");
        state.notify_failure();
        return;
    }
    EventLoop& event_loop = *loop;

    const std::uint16_t port = static_cast<std::uint16_t>(state.port);
    Result<transport::Endpoint> endpoint =
        state.host.empty() ? transport::Endpoint::any(port)
                           : transport::Endpoint::parse(state.host, port);
    if (!endpoint) {
        std::fprintf(stderr, "[AriaRead] 监听地址无效：%s:%d\n", state.host.c_str(), state.port);
        state.notify_failure();
        return;
    }
    Result<transport::tcp::Listener> listener = transport::tcp::Listener::bind(event_loop, *endpoint);
    if (!listener) {
        std::fprintf(stderr, "[AriaRead] 绑定 %s:%d 失败（地址不可用或已被占用）\n",
                     state.host.c_str(), state.port);
        state.notify_failure();
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(state.port_mutex);
        state.actual_port = static_cast<int>(listener->local_endpoint().port());
        state.bound = true;
    }
    state.port_ready.notify_all();

    const Result<void> result =
        event_loop.run_until_complete(accept_loop(state, event_loop, *listener));
    static_cast<void>(result);
}

void Server::stop() { impl_->state.stop_source.request_stop(); }

}  // namespace ariaread::web

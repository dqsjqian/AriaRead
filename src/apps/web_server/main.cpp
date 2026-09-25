/// @file main.cpp
/// @brief OpenRead Web Server 入口。
///
/// 架构：
///   主线程   = Aria 响应式图线程（MainThreadExecutor pump）
///   后台     = ThreadPoolExecutor（worker，搜索/目录/正文在其上执行）
///   HTTP 线程 = Continuo 事件循环（Server，/api/* REST + 静态资源）
///
/// HTTP 层原先是 Aria HttpAdapter（内部 vendored cpp-httplib），现改为
/// Continuo：解析/分帧/keep-alive 交给 Continuo，路由与静态文件在
/// continuo_server.{h,cpp}，业务仍跑在独立线程上，避免占住事件循环。

#include "openread/engine.h"
#include "openread/vm/search_view_model.h"
#include "openread/vm/engine_search_adapter.h"
#include "openread/vm/bookshelf_view_model.h"
#include "openread/vm/engine_bookshelf_adapter.h"
#include "openread/vm/reader_view_model.h"
#include "openread/vm/engine_reader_adapter.h"
#include "openread/vm/source_view_model.h"
#include "openread/vm/engine_source_adapter.h"

#include "aria/async/executor.hpp"

#include "continuo_server.h"
#include "json_helpers.h"
#include "startup_options.h"
#include "routes.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#if defined(_MSC_VER) && !defined(S_ISDIR)
// MSVC's <sys/stat.h> has no POSIX S_IS* wrappers.
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

// 全局运行标志（信号处理 + routes.cpp 共享）
namespace openread::web {
std::atomic<bool> g_running{true};
}

namespace {
void on_signal(int) { openread::web::g_running.store(false); }
}  // namespace

int main(int argc, char** argv) {
    using namespace aria::async;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // ── 命令行参数解析 ──────────────────────────────────────────────
    openread::web::StartupOptions options;
    try {
        options = openread::web::parseStartupOptions(argc, argv);
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << "\nUse --help for usage.\n";
        return 1;
    }
    if (options.help) {
        std::cout << "OpenRead Web Server\n\n"
                  << "Usage: openread [options] [port] [static_root] [db_path]\n\n"
                  << "Options:\n"
                  << "  -p, --port PORT   Listen port (1-65535, default: 9091)\n"
                  << "  --host HOST       Listen address (default: 127.0.0.1)\n"
                  << "  --db PATH         Database path (default: $HOME/.openread/openread.db)\n"
                  << "  --web-root PATH   Frontend static files directory\n"
                  << "  -h, --help        Show this help\n";
        return 0;
    }
    const auto port = options.port;
    const auto& host = options.host;

    // 静态资源目录解析
    std::string static_root = options.web_root;
    std::string exe_dir;
#if defined(_WIN32)
    {
        char exe_buf[MAX_PATH];
        DWORD len = GetModuleFileNameA(nullptr, exe_buf, MAX_PATH);
        if (len > 0) exe_dir = std::string(exe_buf, len);
    }
#else
    {
        char exe_buf[4096];
        ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf)-1);
        if (len <= 0) len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf)-1);
#  ifdef __APPLE__
        if (len <= 0) {
            uint32_t sz = sizeof(exe_buf);
            if (_NSGetExecutablePath(exe_buf, &sz) == 0) len = std::char_traits<char>::length(exe_buf);
        }
#  endif
        if (len > 0) exe_dir = std::string(exe_buf, len);
    }
#endif
    if (!exe_dir.empty()) {
        auto last_sep = exe_dir.find_last_of("/\\");
        if (last_sep != std::string::npos) exe_dir = exe_dir.substr(0, last_sep);
    }

    if (static_root.empty()) {
        if (!exe_dir.empty()) {
            std::string candidate = exe_dir + "/web";
            struct stat st{};
            if (stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                static_root = candidate;
            }
        }
        if (static_root.empty()) {
            struct stat st{};
            if (stat("web", &st) == 0 && S_ISDIR(st.st_mode)) {
                static_root = "web";
            }
        }
    }

    // 数据库路径
    std::string db_path = options.db_path;
    if (db_path.empty()) {
        const char* home = std::getenv("HOME");
#ifdef _WIN32
        if (!home) home = std::getenv("USERPROFILE");
#endif
        if (home && *home) {
            std::string dir = std::string(home) + "/.openread";
#ifdef _WIN32
            _mkdir(dir.c_str());
#else
            mkdir(dir.c_str(), 0755);
#endif
            db_path = dir + "/openread.db";
        } else {
            db_path = exe_dir.empty() ? "openread.db" : exe_dir + "/openread.db";
        }
    }

    // ── 1. 引擎 ──────────────────────────────────────────────────────
    openread::BookSourceEngine engine;
    engine.setDatabasePath(db_path);
    engine.loadSourcesFromDatabase();

    // 启动时清理已标记为无效/差的书源（上次检测遗留）
    {
        int removed = engine.removeInvalidSources();
        if (removed > 0) {
            std::cout << "[OpenRead] cleaned " << removed
                      << " invalid/poor sources from previous run\n";
        }
    }

    // ── 2. Executors ─────────────────────────────────────────────────
    MainThreadExecutor ui;
    ThreadPoolExecutor worker{4};

    // ── 3. 适配器 + ViewModels ───────────────────────────────────────
    openread::vm::StreamSearchFn search_fn =
        openread::vm::make_engine_stream_search(engine);
    openread::vm::EngineBookshelfBackend bookshelf_backend(engine);
    openread::vm::EngineReaderBackend reader_backend(engine);
    openread::vm::EngineSourceBackend source_backend(engine);

    openread::vm::SearchViewModel svm{ui, worker, search_fn};
    openread::vm::BookshelfViewModel bvm{ui, worker, bookshelf_backend};
    openread::vm::ReaderViewModel rvm{ui, worker, reader_backend};
    openread::vm::SourceViewModel srcvm{ui, worker, source_backend};

    // ── 4. Continuo HTTP 服务 ────────────────────────────────────────
    openread::web::Server svr;
    svr.set_static_root(static_root);
    // Revalidate assets after a build; stale CSS/JS can otherwise mix app versions.
    svr.set_file_request_handler([](const openread::web::Request&,
                                    openread::web::Response& response) {
        response.set_header("Cache-Control", "no-cache");
    });

    // ── 5. 注册 REST 路由（必须在事件循环起来之前，避免路由表读写竞争）──
    openread::web::register_routes(svr, engine, ui, worker, svm, bvm, rvm, srcvm);

    if (!svr.listen(host, port)) {
        std::cerr << "Invalid listen address: " << host << ":" << port << "\n";
        return 1;
    }
    std::thread http_thread([&svr] { svr.run(); });
    const int listening_port = svr.actual_port();
    if (listening_port <= 0) {
        std::cerr << "Failed to start HTTP server on " << host << ":" << port
                  << ". The address may be unavailable or already in use.\n";
        return 1;
    }

    std::cout << "OpenRead Web Server running:\n"
              << "  http://" << host << ":" << listening_port << "\n"
              << "  Backend: C++ (Continuo HTTP/1.1 + ViewModel)\n"
              << "  Static: " << static_root << "\n"
              << "  (Ctrl-C to stop)\n";

    // ── 6. 主循环：pump 图线程 ───────────────────────────────────────
    while (openread::web::g_running.load()) {
        ui.pump_until([] { return false; }, std::chrono::milliseconds(100));
    }

    // ── 7. 退出 ─────────────────────────────────────────────────────
    //
    // 根因：stop() 只投递协作取消，正在跑的长任务（SSE content provider）
    // 与 ThreadPoolExecutor::~ThreadPoolExecutor 的 wait_idle() 都可能
    // 长时间不返回。
    //
    // 方案：先请求停止，再用 _Exit 直接退。OS 自动回收 socket/线程。
    // 这是 Chrome/SQLite 等成熟项目的常见做法——进程退出时不需要
    // 运行全局析构函数，内核回收一切资源。
    svr.stop();
    http_thread.detach();
    std::_Exit(0);
}

/// @file main.cpp
/// @brief OpenRead Web Server 入口。
///
/// 架构：
///   主线程 = Aria 响应式图线程（MainThreadExecutor pump）
///   后台   = ThreadPoolExecutor（worker，搜索/目录/正文在其上执行）
///   HttpAdapter 负责 Aria 绑定协议（SSE 推送响应式状态变更）
///   native_server() 获取底层 httplib::Server&，注册 /api/* REST 路由

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
#include "aria/binding/binding_engine.hpp"
#include "aria/adapters/http/http_adapter.hpp"

#include "json_helpers.h"
#include "port_utils.h"
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
#include <windows.h>
#include <winsock2.h>
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
    using aria::binding::BindingEngine;
    namespace http = aria::adapters::http;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // ── 命令行参数解析 ──────────────────────────────────────────────
    std::uint16_t port = 9091;
    std::string host = "127.0.0.1";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "--host") && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "OpenRead Web Server\n\n"
                      << "Usage: openread [options]\n\n"
                      << "Options:\n"
                      << "  -p, --port PORT   Listen port (default: 9091)\n"
                      << "  --host HOST       Listen address (default: 127.0.0.1)\n"
                      << "  --db PATH         Database path (default: $HOME/.openread/openread.db)\n"
                      << "  --web-root PATH   Frontend static files directory\n"
                      << "  -h, --help        Show this help\n";
            return 0;
        } else if (arg.find_first_not_of("0123456789") == std::string::npos && !arg.empty()) {
            port = static_cast<std::uint16_t>(std::stoi(arg));
        }
    }

    // 静态资源目录解析
    std::string static_root;
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
            if (_NSGetExecutablePath(exe_buf, &sz) == 0) len = sz - 1;
        }
#  endif
        if (len > 0) exe_dir = std::string(exe_buf, len);
    }
#endif
    if (!exe_dir.empty()) {
        auto last_sep = exe_dir.find_last_of("/\\");
        if (last_sep != std::string::npos) exe_dir = exe_dir.substr(0, last_sep);
    }

    if (argc > 2) {
        static_root = argv[2];
    }
#ifdef OPENREAD_WEB_ROOT
    else {
        static_root = OPENREAD_WEB_ROOT;
    }
#endif
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
    std::string db_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--db") && i + 1 < argc) {
            db_path = argv[++i];
        }
    }
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

    // ── 4. HttpAdapter + BindingEngine（SSE 推送）────────────────────
    http::HttpAdapterConfig cfg;
    cfg.host = host;
    cfg.port = port;
    cfg.enable_cors = true;
    cfg.static_root = static_root;

    auto adapter = std::make_shared<http::HttpAdapter>(cfg);
    BindingEngine binding_engine(adapter);

    // 绑定搜索状态到 SSE
    auto& v_keyword = adapter->register_view("keyword", "text");
    auto& v_found   = adapter->register_view("found", "int");
    auto& v_loading = adapter->register_view("loading", "bool");
    binding_engine.bind_text(svm.keyword, v_keyword);
    binding_engine.bind_int_oneway(svm.found_count, v_found);
    binding_engine.bind_bool_oneway(svm.is_searching(), v_loading);

    // ── 0. 端口占用自检 ──────────────────────────────────────────────
    if (openread::web::isPortInUse(host, port)) {
        std::cerr << "[OpenRead] port " << port
                  << " is busy, killing the previous instance...\n";
        openread::web::killProcessOnPort(port);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (openread::web::isPortInUse(host, port)) {
            std::cerr << "[OpenRead] port " << port
                      << " still busy after kill attempt; bind may fail\n";
        } else {
            std::cerr << "[OpenRead] port " << port << " released\n";
        }
    }

    if (!adapter->start()) {
        std::cerr << "Failed to start HTTP server on port " << port << "\n";
        return 1;
    }

    // ── 5. 注册 REST 路由 ───────────────────────────────────────────
    auto& svr = adapter->native_server();
    openread::web::register_routes(svr, engine, ui, worker, svm, bvm, rvm, srcvm);

    std::cout << "OpenRead Web Server running:\n"
              << "  http://" << host << ":" << adapter->actual_port() << "\n"
              << "  Backend: C++ (Aria HttpAdapter + ViewModel)\n"
              << "  Static: " << static_root << "\n"
              << "  (Ctrl-C to stop)\n";

    // ── 6. 主循环：pump 图线程 ───────────────────────────────────────
    while (openread::web::g_running.load()) {
        ui.pump_until([] { return false; }, std::chrono::milliseconds(100));
    }

    // ── 7. 退出 ─────────────────────────────────────────────────────
    //
    // 根因：adapter->stop() 内部 server_thread.join() 等 httplib 线程池
    // 排空所有活跃连接（SSE content provider 等），会永远不返回。
    // ThreadPoolExecutor::~ThreadPoolExecutor 的 wait_idle() 同理。
    //
    // 方案：跳过阻塞的析构，用 _Exit 直接退。OS 自动回收 socket/线程。
    // 这是 Chrome/SQLite 等成熟项目的常见做法——进程退出时不需要
    // 运行全局析构函数，内核回收一切资源。
    std::_Exit(0);
}

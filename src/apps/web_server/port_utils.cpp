/// @file port_utils.cpp
/// @brief 端口检测与进程管理工具实现。

#include "port_utils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#endif

namespace openread::web {

bool isPortInUse(const std::string& host, std::uint16_t port) {
#ifdef _WIN32
    WSADATA wsa;
    static bool wsaInit = false;
    if (!wsaInit) { WSAStartup(MAKEWORD(2, 2), &wsa); wsaInit = true; }
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return false;
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    (void)host;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
    return rc == 0;
}

void killProcessOnPort(std::uint16_t port) {
#ifdef _WIN32
    char cmd[512];
    std::snprintf(cmd, sizeof(cmd),
        "powershell -NoProfile -Command \""
        "Get-NetTCPConnection -LocalPort %u -ErrorAction SilentlyContinue | "
        "Select-Object -ExpandProperty OwningProcess | "
        "ForEach-Object { Stop-Process -Id $_ -Force -ErrorAction SilentlyContinue }\"",
        port);
    std::system(cmd);
#else
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd),
        "lsof -nP -iTCP:%u -sTCP:LISTEN -t 2>/dev/null", port);
    FILE* fp = popen(cmd, "r");
    if (!fp) return;
    std::vector<pid_t> pids;
    char line[64];
    pid_t self = ::getpid();
    while (std::fgets(line, sizeof(line), fp)) {
        try {
            pid_t pid = static_cast<pid_t>(std::stoi(line));
            if (pid > 0 && pid != self) pids.push_back(pid);
        } catch (...) {}
    }
    pclose(fp);
    if (pids.empty()) return;
    for (pid_t pid : pids) ::kill(pid, SIGTERM);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    for (pid_t pid : pids) {
        if (::kill(pid, 0) == 0) ::kill(pid, SIGKILL);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
#endif
}

}  // namespace openread::web

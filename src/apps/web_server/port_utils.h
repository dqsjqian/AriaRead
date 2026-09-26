/// @file port_utils.h
/// @brief 端口检测与进程管理工具。

#pragma once

#include <cstdint>
#include <string>

namespace ariaread::web {

/// 检测目标端口是否已被占用（本地 TCP connect 探测）。
bool isPortInUse(const std::string& host, std::uint16_t port);

/// 检测到端口被占用时，自动找出占用进程并杀掉。
/// macOS/Linux: lsof + SIGTERM/SIGKILL；Windows: PowerShell Get-NetTCPConnection。
void killProcessOnPort(std::uint16_t port);

}  // namespace ariaread::web

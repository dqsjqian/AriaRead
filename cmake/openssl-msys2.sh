#!/usr/bin/env bash
# ──────────────────────────────────────────────
# openssl-msys2.sh — MSYS2/MinGW 下构建/安装 OpenSSL 的辅助脚本
#
# 为什么需要独立脚本：
# BuildOpenSSL.cmake 曾把 `bash -c "export PATH=\"...\" && make ..."` 直接内嵌在
# ExternalProject 的 BUILD_COMMAND 里。CMake 生成 step 脚本时，参数中间的转义
# 引号会触发 "Argument not separated from preceding token by whitespace"，
# 命令参数粘连后被静默丢弃——bash 收到空命令直接退出 0（零输出、零报错），
# OpenSSL 从未被编译，下游链接 libssl.a 时才报 "cannot find"。
# 改用独立脚本 + 分离 argv，彻底绕开 CMake 的引号转义。
#
# 用法：openssl-msys2.sh <build|install> <compiler-dir> <perl-dir> [jobs]
#   build   — make -j<jobs>（默认 4）
#   install — make install_sw
# 工作目录必须是 OpenSSL 的构建目录（ExternalProject 的默认 BINARY_DIR）。
# ──────────────────────────────────────────────
set -euo pipefail

mode="${1:?usage: openssl-msys2.sh <build|install> <compiler-dir> <perl-dir> [jobs]}"
compiler_dir="${2:?missing compiler dir}"
perl_dir="${3:?missing perl dir}"

compiler_unix="$(cygpath -u "$compiler_dir")"
perl_unix="$(cygpath -u "$perl_dir")"
export PATH="${compiler_unix}:${perl_unix}:${PATH}"

case "$mode" in
  build)
    jobs="${4:-4}"
    make -j"$jobs"
    ;;
  install)
    make install_sw
    ;;
  *)
    echo "openssl-msys2.sh: unknown mode: $mode" >&2
    exit 2
    ;;
esac

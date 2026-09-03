#!/usr/bin/env bash
# ──────────────────────────────────────────────
# QuickJS 下载脚本
# 版本：2025-09-13（官方最新稳定版）
#
# 用法：bash third_party/quickjs/setup.sh
# ──────────────────────────────────────────────
set -euo pipefail

QUICKJS_VERSION="2025-09-13"
QUICKJS_URL="https://bellard.org/quickjs/quickjs-${QUICKJS_VERSION}.tar.xz"
TARGET_DIR="$(cd "$(dirname "$0")/.." && pwd)/third_party/quickjs"

echo "📥 Downloading QuickJS ${QUICKJS_VERSION}..."
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

curl -sL "$QUICKJS_URL" -o "$TMPDIR/quickjs.tar.xz"
tar xf "$TMPDIR/quickjs.tar.xz" -C "$TMPDIR"

SRC="$TMPDIR/quickjs-${QUICKJS_VERSION}"
if [ ! -d "$SRC" ]; then
    echo "❌ Failed to extract QuickJS"
    exit 1
fi

echo "📁 Copying source files to ${TARGET_DIR}..."
mkdir -p "$TARGET_DIR"

# 核心 C 源文件
for f in quickjs.c quickjs.h libregexp.c libregexp.h libunicode.c libunicode.h \
         cutils.c cutils.h list.h quickjs-atom.h quickjs-opcode.h \
         libregexp-opcode.h libunicode-table.h unicode_gen_def.h \
         dtoa.c dtoa.h quickjs-libc.c quickjs-libc.h; do
    [ -f "$SRC/$f" ] && cp "$SRC/$f" "$TARGET_DIR/"
done

echo "✅ QuickJS ${QUICKJS_VERSION} ready!"
echo "   Source files: $(ls "$TARGET_DIR"/*.c 2>/dev/null | wc -l | tr -d ' ') .c files"
echo "   Header files: $(ls "$TARGET_DIR"/*.h 2>/dev/null | wc -l | tr -d ' ') .h files"

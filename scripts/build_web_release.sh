#!/bin/bash
# ──────────────────────────────────────────────
# OpenRead Web 版一键构建 & 打包脚本
#
# 构建原生 C++ Web Server（零 Python 依赖，性能最优）
# 全流程：环境检测 → C++ 编译 → 验证 → 打包
#
# 用法：
#   bash scripts/build_web_release.sh              # 增量构建
#   bash scripts/build_web_release.sh --clean      # 全量 clean 构建
#   bash scripts/build_web_release.sh --skip-cmake # 跳过 C++ 编译（仅重新打包）
#
# 产物：
#   release/web/openread           — 原生 C++ Web Server 可执行文件
#   release/web/*.dylib           — 运行时动态库（macOS）
#   release/web/install.sh        — 分发脚本
# ──────────────────────────────────────────────

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RELEASE_DIR="$PROJECT_ROOT/release/web"
BUILD_DIR="$PROJECT_ROOT/build"
OPENREAD_PKG="$PROJECT_ROOT/bindings/web/openread"

# 解析参数
SKIP_CMAKE=false
CLEAN_BUILD=false
for arg in "$@"; do
    case "$arg" in
        --skip-cmake) SKIP_CMAKE=true ;;
        --clean) CLEAN_BUILD=true ;;
    esac
done

# 总阶段数
PHASE_NUM=4

echo ""
echo "╔═══════════════════════════════════════════════╗"
echo "║  📦 OpenRead Web 版 · 原生 C++ 构建 & 打包   ║"
echo "╚═══════════════════════════════════════════════╝"
echo ""

# ══════════════════════════════════════════════
# 阶段 1：环境检测
# ══════════════════════════════════════════════
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🔍 阶段 1/$PHASE_NUM - 环境检测"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

HAS_ERROR=false

# 检查 cmake
if command -v cmake &>/dev/null; then
    CMAKE_VER=$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
    echo "  ✅ cmake $CMAKE_VER"
else
    echo "  ❌ cmake 未安装（brew install cmake）"
    HAS_ERROR=true
fi

# 检查 C++ 编译器
if command -v c++ &>/dev/null || command -v g++ &>/dev/null || command -v clang++ &>/dev/null; then
    CXX_VER=$(c++ --version 2>/dev/null | head -1 || echo "unknown")
    echo "  ✅ C++ 编译器: $CXX_VER"
else
    echo "  ❌ C++ 编译器未安装（xcode-select --install）"
    HAS_ERROR=true
fi

# 检查 web 静态资源
WEB_DIR="$OPENREAD_PKG/web"
if [ -d "$WEB_DIR" ]; then
    WEB_FILES=$(find "$WEB_DIR" -type f | wc -l | tr -d ' ')
    echo "  ✅ Web 静态资源: $WEB_FILES 个文件"
else
    echo "  ❌ Web 静态资源目录不存在: $WEB_DIR"
    HAS_ERROR=true
fi

if [ "$HAS_ERROR" = true ]; then
    echo ""
    echo "❌ 环境检测未通过，请先安装缺失的依赖"
    exit 1
fi

echo ""

# ══════════════════════════════════════════════
# 阶段 2：C++ 编译
# ══════════════════════════════════════════════
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🔨 阶段 2/$PHASE_NUM - C++ 编译"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ "$SKIP_CMAKE" = true ]; then
    echo "  ⏭️  跳过（--skip-cmake）"
else
    # cmake configure（如果 build 目录不存在或 CMakeCache 不存在）
    # 获取当前 macOS 版本作为最低部署目标
    MACOS_VER=$(sw_vers -productVersion 2>/dev/null | cut -d. -f1,2)
    if [ -z "$MACOS_VER" ]; then
        MACOS_VER="13.0"  # 默认最低支持 macOS 13 Ventura
    fi
    echo "  🍎 最低部署目标: macOS $MACOS_VER"

    # --clean 时清理构建目录
    if [ "$CLEAN_BUILD" = true ]; then
        echo "  🗑️  Clean 模式：清理构建目录..."
        rm -rf "$BUILD_DIR"
    fi

    # cmake configure（仅在 CMakeCache 不存在时执行）
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        echo "  📐 cmake configure..."
        cmake \
            -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_VER" \
            -DOPENREAD_BUILD_TESTS=OFF \
            -DOPENREAD_ENFORCE_SELF_CONTAINED=ON \
            -DOPENREAD_USE_SYSTEM_CURL=OFF
    else
        echo "  📐 cmake configure（已有缓存，跳过）"
    fi

    # cmake build（增量编译，仅重新编译变更的文件）
    NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
    echo "  🔧 cmake build openread_web_server（${NPROC} 线程，增量编译）..."
    cmake --build "$BUILD_DIR" --target openread_web_server -j"$NPROC"
    echo "  ✅ C++ 编译完成"
fi

echo ""

# ══════════════════════════════════════════════
# 阶段 3：验证构建产物
# ══════════════════════════════════════════════
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📋 阶段 3/$PHASE_NUM - 验证构建产物"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# 验证 openread_web_server 可执行文件
NATIVE_BIN="$BUILD_DIR/bin/openread_web_server"
if [ ! -f "$NATIVE_BIN" ]; then
    echo "  ❌ 未找到 openread_web_server 可执行文件: $NATIVE_BIN"
    echo "     请检查 CMake 构建日志"
    exit 1
fi
NATIVE_SIZE=$(du -sh "$NATIVE_BIN" | cut -f1)
echo "  📦 可执行文件: openread_web_server ($NATIVE_SIZE)"

# 验证关键方法符号
# 注：catalog_with_cache / content_with_cache 为早期设计阶段的废弃符号，
# 已不存在于代码库，从清单移除以消除误报警告。仅保留真实存在的 register_routes
# 作为产物完整性兜底校验。
REQUIRED_SYMBOLS="register_routes"
MISSING_SYMBOLS=""
for sym in $REQUIRED_SYMBOLS; do
    if ! strings "$NATIVE_BIN" 2>/dev/null | grep -q "$sym"; then
        MISSING_SYMBOLS="$MISSING_SYMBOLS $sym"
    fi
done
if [ -n "$MISSING_SYMBOLS" ]; then
    echo "  ⚠️  可执行文件缺少关键方法:$MISSING_SYMBOLS"
else
    echo "  ✅ 关键方法验证通过"
fi

# 检查动态库依赖
if [[ "$(uname)" == "Darwin" ]]; then
    echo "  🔍 检查动态库依赖..."
    UNSAFE_DEPS=$(otool -L "$NATIVE_BIN" 2>/dev/null | tail -n +2 | grep -v '/usr/lib/' | grep -v '/System/' | grep -v '@rpath' | grep -v '@loader_path' | grep -v '@executable_path' || true)
    if [ -n "$UNSAFE_DEPS" ]; then
        echo "  ⚠️  检测到非系统路径的动态库依赖:"
        echo "$UNSAFE_DEPS" | while read -r line; do
            echo "     $line"
        done
    else
        echo "  ✅ 动态库依赖检查通过（仅依赖系统库 + @rpath）"
    fi

    # 收集需要的 Aria dylib 文件
    DYLIB_DIR="$BUILD_DIR/bin"
    ARIA_DYLIBS=("libaria_http.1.0.0.dylib" "libaria_binding.1.0.0.dylib" "libaria_runtime.1.0.0.dylib")
    for dylib in "${ARIA_DYLIBS[@]}"; do
        if [ -f "$DYLIB_DIR/$dylib" ]; then
            DYLIB_SIZE=$(du -sh "$DYLIB_DIR/$dylib" | cut -f1)
            echo "  📦 $dylib ($DYLIB_SIZE)"
        else
            echo "  ⚠️  未找到 $DYLIB_DIR/$dylib"
        fi
    done
fi

echo ""

# ══════════════════════════════════════════════
# 阶段 4：打包
# ══════════════════════════════════════════════
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📦 阶段 4/$PHASE_NUM - 打包"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

mkdir -p "$RELEASE_DIR"

# 复制主可执行文件
cp "$NATIVE_BIN" "$RELEASE_DIR/openread"
chmod +x "$RELEASE_DIR/openread"
echo "  📦 已复制 openread_web_server → release/web/openread"

# macOS: 复制 Aria 动态库并修正 @rpath
if [[ "$(uname)" == "Darwin" ]]; then
    DYLIB_DIR="$BUILD_DIR/bin"
    ARIA_DYLIBS=("libaria_http.1.dylib" "libaria_binding.1.dylib" "libaria_runtime.1.dylib")
    for dylib in "${ARIA_DYLIBS[@]}"; do
        if [ -f "$DYLIB_DIR/$dylib" ]; then
            cp "$DYLIB_DIR/$dylib" "$RELEASE_DIR/"
            echo "  📦 已复制 $dylib"
        fi
    done

    # 修正主可执行文件的 @rpath → @executable_path（使 dylib 从同目录加载）
    echo "  🔧 修正 @rpath → @executable_path..."
    install_name_tool -add_rpath "@executable_path" "$RELEASE_DIR/openread" 2>/dev/null || true
    # 删除指向 build 目录的旧 rpath（如果有）
    install_name_tool -delete_rpath "$BUILD_DIR/lib" "$RELEASE_DIR/openread" 2>/dev/null || true
    install_name_tool -delete_rpath "$BUILD_DIR/bin" "$RELEASE_DIR/openread" 2>/dev/null || true

    # ad-hoc 签名
    echo "  🍎 对可执行文件和 dylib 进行 ad-hoc 签名..."
    codesign -f -s - "$RELEASE_DIR/openread" 2>/dev/null || true
    for dylib in "$RELEASE_DIR"/libaria_*.dylib; do
        [ -f "$dylib" ] && codesign -f -s - "$dylib" 2>/dev/null || true
    done
fi

# 复制前端静态资源
WEB_SRC="$OPENREAD_PKG/web"
if [ -d "$WEB_SRC" ]; then
    mkdir -p "$RELEASE_DIR/web"
    cp -r "$WEB_SRC/"* "$RELEASE_DIR/web/" 2>/dev/null || true
    WEB_COUNT=$(find "$RELEASE_DIR/web" -type f | wc -l | tr -d ' ')
    echo "  📦 已复制前端静态资源: $WEB_COUNT 个文件"
fi

echo "  ✅ 打包完成"

echo ""

# ══════════════════════════════════════════════
# 签名 & 验证 & 生成分发脚本
# ══════════════════════════════════════════════
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ 签名 & 验证 & 生成分发脚本"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

BINARY="$RELEASE_DIR/openread"
if [ ! -f "$BINARY" ]; then
    echo "❌ 打包失败，请检查上方日志"
    exit 1
fi

SIZE=$(du -sh "$BINARY" | cut -f1)
echo "  📁 产物: release/web/openread ($SIZE)"

# --- 验证二进制架构 ---
echo "  🔍 验证二进制架构..."
ARCH_INFO=$(file "$BINARY")
echo "     $ARCH_INFO"

# --- 验证签名状态 ---
if [[ "$(uname)" == "Darwin" ]]; then
    echo "  🔍 验证签名状态..."
    if codesign -v "$BINARY" 2>/dev/null; then
        echo "  ✅ 签名验证通过"
    else
        echo "  ⚠️  签名验证未通过（对方可能需要手动允许运行）"
    fi
fi

# --- 验证最终二进制的动态库依赖 ---
if [[ "$(uname)" == "Darwin" ]]; then
    echo "  🔍 验证最终二进制动态库依赖..."
    BINARY_UNSAFE_DEPS=$(otool -L "$BINARY" 2>/dev/null | tail -n +2 | grep -v '/usr/lib/' | grep -v '/System/' | grep -v '@rpath' | grep -v '@loader_path' | grep -v '@executable_path' || true)
    if [ -n "$BINARY_UNSAFE_DEPS" ]; then
        echo "  ⚠️  最终二进制存在非系统动态库依赖:"
        echo "$BINARY_UNSAFE_DEPS" | while read -r line; do
            echo "     $line"
        done
    else
        echo "  ✅ 最终二进制动态库依赖检查通过"
    fi
fi

# --- 生成分发用的 install.sh 脚本 ---
INSTALL_SCRIPT="$RELEASE_DIR/install.sh"
cat > "$INSTALL_SCRIPT" << 'INSTALL_EOF'
#!/bin/bash
# ──────────────────────────────────────────────
# OpenRead 安装 & 启动脚本
#
# 首次运行时自动解除 macOS Gatekeeper 隔离属性，
# 之后可直接双击或命令行运行 openread。
#
# 用法：
#   bash install.sh              # 安装并启动（默认端口 9091）
#   bash install.sh 8080         # 安装并启动（指定端口）
# ──────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/openread"
PORT="${1:-9091}"

if [ ! -f "$BINARY" ]; then
    echo "❌ 未找到 openread 可执行文件，请确保 install.sh 和 openread 在同一目录"
    exit 1
fi

# 解除 Gatekeeper 隔离属性（首次需要）
if xattr -l "$BINARY" 2>/dev/null | grep -q "com.apple.quarantine"; then
    echo "🔓 首次运行：解除 macOS 安全隔离..."
    xattr -d com.apple.quarantine "$BINARY" 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "  ✅ 隔离属性已解除"
    else
        echo "  ⚠️  解除失败，尝试使用 sudo..."
        sudo xattr -d com.apple.quarantine "$BINARY"
    fi
fi

# 解除 dylib 的隔离属性
for dylib in "$SCRIPT_DIR"/libaria_*.dylib; do
    [ -f "$dylib" ] && xattr -d com.apple.quarantine "$dylib" 2>/dev/null || true
done

# 确保可执行权限
chmod +x "$BINARY"

echo "🚀 启动 OpenRead（端口: $PORT）..."
echo ""
exec "$BINARY" "$PORT"
INSTALL_EOF

chmod +x "$INSTALL_SCRIPT"
echo "  ✅ 已生成分发脚本: release/web/install.sh"

echo ""

# ══════════════════════════════════════════════
# 完成
# ══════════════════════════════════════════════
echo "═══════════════════════════════════════════════"
echo "✅ 全流程构建完成！"
echo ""
echo "📁 产物目录: release/web/（原生 C++ Web Server）"
echo "   ├── openread       ($SIZE) — 主程序（C++ 原生）"
echo "   ├── libaria_*.dylib        — Aria 动态库"
echo "   ├── web/                   — 前端静态资源"
echo "   └── install.sh             — 安装/启动脚本"
echo ""
echo "🚀 本机使用:"
echo "   ./release/web/openread 9091        # 指定端口"
echo "   ./release/web/openread -p 9091     # 同上"
echo "   ./release/web/openread             # 默认 9091"
echo ""
echo "📋 分发给他人:"
echo "   1. 将 release/web/ 目录整体发送给对方"
echo "   2. 对方运行: bash install.sh         # 首次自动解除安全限制"
echo "   3. 之后可直接: ./openread 9091"

# OpenRead Web Server

一个独立的书源规则引擎 Web 服务，兼容主流书源格式。

## 首次使用（推荐）

```bash
# 运行安装脚本（自动解除 macOS 安全限制 + 启动）
bash install.sh

# 指定端口
bash install.sh 8080
```

> 💡 `install.sh` 会自动解除 macOS Gatekeeper 隔离属性，之后可直接运行 `./openread`。

## 直接运行

```bash
# 赋予执行权限（首次使用）
chmod +x openread

# 启动服务（指定端口）
./openread 9091

# 或者
./openread -p 9091

# 默认端口 9091
./openread
```

启动后浏览器打开：**http://localhost:9091**

## 命令参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `9091` 或 `-p 9091` | 监听端口 | 9091 |
| `--host 0.0.0.0` | 监听地址 | 0.0.0.0 |
| `--db /path/to/db` | 数据库路径 | ~/.openread/openread.db |

## 目录结构

| 文件 | 说明 |
|------|------|
| `openread` | 主程序（独立可执行文件，已 ad-hoc 签名） |
| `install.sh` | 安装/启动脚本（解除 Gatekeeper 隔离） |
| `README.md` | 本说明文件 |

## 使用流程

1. 启动服务，浏览器打开页面
2. 点击「URL加载」导入书源（支持主流格式 JSON）
3. 点击「检测书源」过滤无效源
4. 搜索书籍，阅读正文

## 系统要求

- macOS（arm64 / x86_64）
- 无需安装 Python 或其他依赖

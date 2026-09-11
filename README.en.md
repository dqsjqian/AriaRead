# OpenRead

📖 A cross-platform reading engine, focused on the open Chinese book-source ecosystem.

Built on [Aria](https://github.com/dqsjqian/Aria) (a C++20 reactive MVVM framework); compatible with mainstream book-source formats.

[English](README.en.md) | [简体中文](README.md)

## Screenshots

One C++ core drives two web shapes side by side:

| Shape | Screenshot | Notes |
|---|---|---|
| REST + SSE thin client | ![OpenRead-Web](docs/marketing/images/OpenRead-Web.png) | Browser talks straight to the C++ HTTP shell; Property changes stream out over SSE |
| SSR (server-side rendering) | ![OpenRead-SSR](docs/marketing/images/OpenRead-SSR.png) | Pages rendered on the C++ side; runs with zero JS on the frontend |

## Features

- **C++20 reactive MVVM** — built on the Aria framework: coroutine async, reactive state, ViewModels
- **Cross-platform engine** — CSS3/XPath selectors, a JS runtime bridge, Gumbo HTML5 parsing
- **Self-contained third-party deps** — nlohmann/json, OpenSSL, and libcurl all build from source; zero system dependencies
- **Extended ViewModel layer** — SearchViewModel, BookshelfViewModel, ReaderViewModel, SourceViewModel
- **Engine adapters** — engine_reader_adapter / engine_source_adapter keep engine.h's private dependencies isolated inside .cpp files
- **C++ web server** — Aria HttpAdapter + ViewModels: 39 REST routes, SSE push, zero Python

## Build

```bash
mkdir -p build && cd build
cmake .. -DOPENREAD_BUILD_TESTS=ON
cmake --build .
ctest --output-on-failure
```

## Project layout

```
OpenRead/
├── CMakeLists.txt          # C++20, unified options across platforms
├── include/                # public headers
│   └── openread/
│       └── version.h.in    # version template
├── src/                    # core engine sources
│   ├── engine/             # engine core
│   ├── selector/           # CSS/XPath selectors
│   ├── rule/               # rule analyzer
│   ├── infra/              # infrastructure (HTTP/JS/DB)
│   ├── viewmodels/         # ViewModel layer
│   └── apps/web_server/    # web server (Aria HttpAdapter + REST API)
├── third_party/
│   ├── aria/               # Aria C++20 coroutine MVVM framework (git submodule)
│   ├── curl/               # libcurl, built from source
│   ├── nlohmann_json/      # nlohmann/json
│   ├── openssl/            # OpenSSL
│   ├── quickjs/            # QuickJS
│   └── sqlite3_src/        # SQLite3 amalgamation
├── bindings/web/openread/web/  # frontend static assets
└── tests/                  # unit tests
```

## Tests

Configure, build, and run the tests from the repository root:

```bash
cmake -S . -B build -DOPENREAD_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

CTest registers engine tests and available ViewModel tests. It also registers the relevant debug regressions when Node.js 18+, Python 3.8+, and the Web Server target are available. CMake reports skipped optional dependencies; use `-DOPENREAD_BUILD_WEB_TESTS=OFF` to disable Web regressions.

```bash
# Run only the debug regressions
ctest --test-dir build -R openread-debug --output-on-failure

# Or run them directly; no npm or pip packages are needed
node tests/test_debug_ui.cjs
python3 tests/test_debug_http.py --server build/bin/openread_web_server
```

HTTP tests use local mock sources and an in-memory database to cover console validation and execution limits, successful and failed SSE stages, response limits including gzip decompression, and occupied-port protection. The server path can also be set with the `OPENREAD_WEB_SERVER` environment variable. For another build directory or a multi-configuration generator, use the corresponding executable path.

## Release checklist

- [x] All sources compile from source (no prebuilt binaries)
- [x] `.gitmodules` declares every third-party dependency
- [x] MIT LICENSE
- [x] README.md (Chinese) + README.en.md (English)

## License

MIT License — see [LICENSE](LICENSE).

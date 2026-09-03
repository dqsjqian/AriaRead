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
cmake --build . --target openread-tests
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

All 109 tests pass.

## Release checklist

- [x] All sources compile from source (no prebuilt binaries)
- [x] `.gitmodules` declares every third-party dependency
- [x] MIT LICENSE
- [x] README.md (Chinese) + README.en.md (English)

## License

MIT License — see [LICENSE](LICENSE).

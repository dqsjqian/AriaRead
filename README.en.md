# AriaRead

[Complete dependency update guide](docs/dependency-updates.en.md) — Version pins, selective updates, offline use, rollback and commit steps.

Current version **0.2.2** · Aria **3.1.1**

📖 A cross-platform reading engine, focused on the open Chinese book-source ecosystem.

Built on [Aria](https://github.com/dqsjqian/Aria) (a C++23 reactive MVVM framework); compatible with mainstream book-source formats.

[English](README.en.md) | [简体中文](README.md)

## Screenshots

One C++ core drives two web shapes side by side:

| Shape | Screenshot | Notes |
|---|---|---|
| REST + SSE thin client | ![AriaRead-Web](docs/marketing/images/AriaRead-Web.png) | Browser talks straight to the C++ HTTP shell; Property changes stream out over SSE |
| SSR (server-side rendering) | ![AriaRead-SSR](docs/marketing/images/AriaRead-SSR.png) | Pages rendered on the C++ side; runs with zero JS on the frontend |

## Features

- **C++23 reactive MVVM** — built on the Aria framework: coroutine async, reactive state, ViewModels
- **Cross-platform engine** — CSS3/XPath selectors, a JS runtime bridge, Gumbo HTML5 parsing
- **Platform-aware dependencies** — locked sources and verified component caches; Windows uses Schannel without building OpenSSL, macOS verifies certificates with Apple SecTrust
- **Extended ViewModel layer** — SearchViewModel, BookshelfViewModel, ReaderViewModel, SourceViewModel
- **Engine adapters** — engine_reader_adapter / engine_source_adapter keep engine.h's private dependencies isolated inside .cpp files
- **C++ web server** — Mira (in-house C++23 coroutine networking library) + Aria ViewModels: 39 REST routes, SSE push, zero Python

## Build

Stable toolchain baseline (2026-10-02): GCC **16.2.0**, LLVM Clang **23.1.2**, Xcode **27.0** / AppleClang **21.0**, Visual Studio 2026 stable / MSVC Build Tools **14.51.36247**. C++23 throughout; configuration rejects older compilers and CI no longer retains legacy compatibility jobs.

Requires CMake 3.21+, a C++23 compiler, Git and Python 3.10+. One entry point fetches dependencies, configures, builds and stages the runtime:

```bash
python3 tools/build.py                       # macOS / Linux
python tools/build.py                        # Windows: auto-discovered MSVC x64
python tools/build.py --toolchain mingw      # Windows: explicit MSYS2 UCRT64 / MinGW
python3 tools/build.py --test                # add test dependencies, build and run tests
```

Windows needs Visual Studio C++ tools/SDK, CMake, Git and Python. The default Schannel backend needs no MSYS2, Perl or OpenSSL build. MSVC and MinGW are alternatives, with isolated build/prefix directories and shared source downloads. `--tls-backend openssl` opts into the previous backend and its Perl/make prerequisites.

macOS uses Xcode Command Line Tools and system Perl/make for OpenSSL; Apple SecTrust uses the system keychain. Linux also needs Perl/make and system CA certificates. Node.js 18+ enables Web regressions. Runtime builds skip doctest; `--test` adds it. `--offline`, `--jobs N`, `--build-dir` and `--deps-prefix` control cached builds.

Third-party dependencies use one `dependencies.json` file: source fields and optional
`version` requirements sit beside generated `resolved` versions, immutable commits and archive SHA256 values. Missing lock
entries select the latest stable release. Normal builds reuse the lock, and explicit
versions take priority:

```bash
python3 tools/ci/build_ariaread_deps.py --update
python3 tools/ci/build_ariaread_deps.py --version zlib=1.3.2
python3 tools/ci/build_ariaread_deps.py --offline
python3 tools/ci/build_ariaread_deps.py --only zlib,json
```

The installation cache verifies the lock, recipes/patches, compiler, Release configuration
and installed file contents. Per-component identities rebuild only changed libraries and their affected static consumers. Compiler/ABI changes rebuild the complete prefix; old-format caches need one migration rebuild. Updates preserve
`prefix-backup-*`; failure restores the old prefix and retains `prefix-failed-*` for
inspection. Locally modified installed files or Git caches are never overwritten.
Offline builds require exact locked archives/commits already cached. Partial builds
include prerequisite dependencies; complete the installation before configuring the
application. CMake verifies it offline. For custom installations, pass `--prefix <path>`
to the builder and `-DARIAREAD_DEPS_PREFIX=<path>` to CMake.
QuickJS, Gumbo and sqlite_modern_cpp patches are limited to reviewed upstream versions;
a new unsupported version fails with an explicit recipe-update requirement.

The Aria fetcher verifies the actual Git HEAD and worktree on every run, refuses
to overwrite local edits, and retains the previous checkout in
`build/deps/aria-backup-*` after an update. To consume the pinned commit before
it is published, use `python3 tools/ci/fetch_aria.py --source /path/to/Aria`
(or set `ARIA_SOURCE`). For source development, configure CMake with
`-DARIA_DIR=/path/to/Aria`.

## Running and distributing

The entry point prints the actual executable path. Default runtime directories:

- macOS / Linux: `build/bin/`.
- Windows MSVC: `build/windows-msvc-release/bin/`, or `bin/Release/` with a multi-config generator.
- Windows MinGW: `build/windows-mingw-release/bin/`.

Copy the directory as a unit, including Aria libraries, `web/` and `licenses/`. Assets load beside the executable; use `--web-root bindings/web/ariaread/web` for source-tree assets. `ARIAREAD_BUILD_DIR` and `ARIAREAD_DEPS_PREFIX` remain supported.

`tools/build.py` is the single build entry point on every platform. Windows supports Release only and rejects incompatible configurations early. `--clean` cleans application output while preserving downloaded and compiled dependencies. CI caches verified prefixes and sources, excluding intermediate build trees.

See [dependency responsibilities and Mira boundaries](docs/build-architecture.md).

## Project layout

```
AriaRead/
├── CMakeLists.txt          # C++23, unified options across platforms
├── include/                # public headers
│   └── ariaread/
│       └── version.h.in    # version template
├── src/                    # core engine sources
│   ├── engine/             # engine core
│   ├── selector/           # CSS/XPath selectors
│   ├── rule/               # rule analyzer
│   ├── infra/              # infrastructure (HTTP/JS/DB)
│   ├── viewmodels/         # ViewModel layer
│   └── apps/web_server/    # web server (Mira HTTP/1.1 + REST API)
├── tools/build.py          # unified cross-platform build
├── tools/ci/
│   ├── build_ariaread_deps.py  # sole dependency source: version lock + SHA256
│   └── fetch_aria.py           # fetches Aria at a pinned commit -> build/deps/aria
├── bindings/web/ariaread/web/  # frontend static assets
└── tests/                  # unit tests
```

## Tests

Configure, build, and run the tests from the repository root:

```bash
cmake -S . -B build -DARIAREAD_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

An empty single-config build type defaults to Release. Windows rejects unsupported configurations before reaching CRT-mismatch linker errors. Prefer `python3 tools/build.py --test` to manage the test SDK and configuration together.

CTest registers engine tests, available ViewModel tests, and local dependency-fetch safety regressions. It also registers the relevant debug regressions when Node.js 18+, Python 3.10+, and the Web Server target are available. CMake reports skipped optional dependencies; use `-DARIAREAD_BUILD_WEB_TESTS=OFF` to disable Web regressions.

```bash
# Run only the debug regressions
ctest --test-dir build -R ariaread-debug --output-on-failure

# Or run them directly; no npm or pip packages are needed
node tests/test_debug_ui.cjs
python3 tests/test_debug_http.py --server build/bin/ariaread_web_server
```

HTTP tests use local mock sources and an in-memory database to cover console validation and execution limits, successful and failed SSE stages, response limits including gzip decompression, and occupied-port protection. The server path can also be set with the `ARIAREAD_WEB_SERVER` environment variable. For another build directory or a multi-configuration generator, use the corresponding executable path.

## Release checklist

- [x] All sources compile from source (no prebuilt binaries)
- [x] No git submodules; all dependencies pinned and fetched by tools/ci scripts
- [x] MIT LICENSE
- [x] README.md (Chinese) + README.en.md (English)

## License

MIT License — see [LICENSE](LICENSE).

Own code is MIT-licensed; third-party components retain their licenses. See [Third-Party Notices](THIRD_PARTY_NOTICES.md) for distribution requirements.

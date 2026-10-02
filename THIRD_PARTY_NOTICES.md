# Third-Party Notices

AriaRead's own code is covered by the [MIT License](LICENSE). Dependencies
retain their own terms; the root license does not relicense the whole package.
The source-root `dependencies.json` records the selected versions and source
identities. Explicit overrides must be reviewed using their actual sources.

| Component | Current resolution | License | Use |
|---|---|---|---|
| Aria | 3.1.1 | MIT | Shared framework runtime and bindings |
| Mira | 1.0.0 | MIT | Static HTTP/1.1 transport; its TLS/WebSocket/HTTP2/HTTP3 modules are disabled here |
| nlohmann/json | 3.12.0 | MIT | Compiled header implementation |
| OpenSSL | 4.0.3 | Apache-2.0 | Static curl TLS backend on macOS/Linux; optional on Windows (Schannel is the default) |
| zlib | 1.3.2 | Zlib | Static decompression library |
| curl | 8.22.0 | curl license | Static HTTP client |
| SQLite | 3.53.4 | Public-domain dedication | Static database amalgamation |
| Gumbo | 0.10.1 | Apache-2.0 | Static HTML parser with an MSVC compatibility patch |
| QuickJS | 2026-06-04 | MIT | Static JavaScript engine with an MSVC compatibility patch; unused os/std helper modules excluded |
| sqlite_modern_cpp | 3.2 | MIT | Header wrapper with a compatibility patch |
| doctest | 2.5.3 | MIT; embedded Boost-1.0 portions | Tests only; not linked into the Web server |

The exact source URLs, revisions and archive checksums are retained in the
dependency file and installed dependency receipt. The build copies selected
upstream license texts, any root `NOTICE`/`NOTICE.txt`, JSON's embedded SPDX
attributions, and SQLite's source dedication to the dependency prefix. Gumbo's
modified source files identify the local changes; upstream notices remain.

The runtime profile does not build doctest. Windows' default profile uses the
operating system's Schannel TLS implementation and does not build or incorporate
OpenSSL. The receipt and distribution inventory list the actual selected set;
the table above includes optional components. macOS uses Apple SecTrust for
certificate verification while retaining OpenSSL for TLS transport.

The SDK installation includes `share/licenses/ariaread/`. The Web runtime
directory includes `licenses/`, with the project's and selected Aria's notices
and the built dependencies' licenses. Its `components.json` is a generated
distribution inventory from the actual prefix, not another dependency input;
it excludes private build paths and compiler environment data. Keep this
directory with the executable and libraries. Test-only notices may also be
present when the shared dependency prefix contains doctest.

nlohmann/json includes MIT portions attributed to Niels Lohmann, Evan Nemerson,
Florian Loitsch, Björn Hoehrmann and the Abseil Authors. The generated
`ATTRIBUTIONS.txt` preserves the exact copyright lines from the selected headers.
SQLite's dedication is not an MIT license. The selected OpenSSL and Gumbo
archives have no root NOTICE file; a future version may add one. OpenSSL's
bundled Text::Template is a build tool (GPL-1.0-or-later or Artistic-1.0), not a
library incorporated into the Web executable.

## Runtime and source distribution

The Web target does not use Qt. MinGW DLLs, when copied, are additional components:
libgcc/libstdc++ have their own licenses and the applicable Runtime Library
Exception; winpthreads has separate notices. Available `gcc`, `gcc-libs` and
`winpthreads` license directories from the selected compiler prefix are copied,
but this is not a complete cross-platform runtime audit. Verify the actual DLL
package provenance, license copies and distribution conditions before publishing
a Windows binary bundle. Microsoft runtime redistribution follows Microsoft's
terms; debug CRT files are not general-purpose redistributables.

The source repository and its automatic GitHub archives exclude downloaded
dependency caches. If distributing those complete caches or test executables,
preserve all applicable source/build-tool licenses and embedded notices too.
The current CI uploads failure diagnostics, not application binary packages.
Updating a dependency can change its license or bundled components: review the
actual new release and refresh distribution materials after an update.

Relevant upstream terms: [MIT](https://opensource.org/license/mit),
[Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0),
[curl](https://curl.se/docs/copyright.html),
[zlib](https://zlib.net/zlib_license.html),
[SQLite](https://www.sqlite.org/copyright.html),
[GCC runtime exception](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html),
[Boost-1.0](https://www.boost.org/LICENSE_1_0.txt), and
[Microsoft redistribution terms](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170).

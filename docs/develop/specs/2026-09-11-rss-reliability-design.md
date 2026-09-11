# RSS reliability repair

User goal: subscriptions marked fast/healthy must actually yield readable articles; failures must be visible and retryable. Reference: sibling legado model/rss/Rss.kt, RssParserByRule.kt, and ui/rss/read/ReadRssViewModel.kt.

## Evidence and scope
OpenRead currently tests raw rule fragments instead of parsed articles, drops AnalyzeUrl method/body/headers, gives content JS a different HTTP context, and embeds short content as a remote iframe. These are concrete list/content failures. Standard feed parsing also uses incorrect relative URL joining and may pick Atom self links over article links. JSON URL import incorrectly requires RSS XML markers.

## Options
1. UI-only error handling is small but leaves false ratings and fetch failures.
2. Share the RSS request/list/content pipeline across refresh and checking, then make the reader explicit about content/failure. Recommended: fixes the supported flow with deterministic tests.
3. Port all Legado RSS/WebView capabilities. Too broad for this repair; arbitrary browser JS, login, pagination, and all rule dialects remain separate compatibility work.

## Implementation contract
- Request helper preserves analyzed method, body, headers, source headers and redirect URL; JS AJAX uses the same context with value captures and bounded execution. No changes to sibling repository or user subscriptions.
- Shared list loader handles standard feeds and rule sources, validates real article fields, and is used by refresh and rated checking. Check samples content using the same reader path; latency alone cannot prove readability. Failures do not delete sources or wipe cached articles.
- Reader uses inline feed content/description when available and the source rules when needed. Preserve nonempty short content; unavailable content returns a diagnostic and usable original link instead of blank success. Avoid embedding third-party pages as a fallback.
- Feed fixes cover relative URLs, CDATA, Atom alternate links. Keep scope to tested formats; do not claim full XML or Legado compatibility.
- UI rejects stale async responses, exposes loading/errors/retry, and fetches empty cached sources despite imported timestamps.

## Validation
Deterministic C++ and local HTTP fixtures: fake-fast empty rule output, source/URL headers and POST, JS AJAX content isolation, redirects/relative URLs, Atom alternate links, inline content with inaccessible original, empty/error content, JSON URL import. Node UI tests: short content, iframe safety, out-of-order navigation, retry/empty cache. Run full CTest, macOS build, and smoke representative existing sources using a disposable database. Windows compilation is unavailable on this host.

## Cross-platform boundary
The user explicitly requires business logic to live below the View for future ports. `BookSourceEngine` owns HTTP options, redirects, source JS context, list validation, cache-first loading, content selection/fallback and ratings. `loadRssArticles` returns cached items or fetches an empty cache once, with a structured diagnostic; imported timestamps do not control this decision. `getRssArticles` remains a cache-only query. `getRssArticleContentResult` returns displayable content, a diagnostic and an original URL. Other platforms can call these same C++ APIs.

Web routes only translate requests/results. JavaScript owns DOM rendering, loading indicators, navigation race guards and retries initiated by the user; it no longer chooses description fallback or decides whether the cache needs a fetch. Existing Aria HttpAdapter/BindingEngine integration is reused; no browser dependency or Aria fork is introduced into RSS business code. A future native RSS ViewModel can bind the same engine APIs through Aria.

## Additional regressions addressed
- Debug source selection now loads the source API directly and reports loading/empty/failure states, instead of racing the sidebar's global cache.
- Source sidebar can collapse/expand with a persisted local display preference; the main content uses the released width.
- Static assets require revalidation to prevent new JavaScript from running against stale CSS.
- RSS JSON extras accept boolean flags and null optional values without dropping subsequent parsing rules.
- Public articles with site-wide login widgets remain readable; a password field alone is not proof of a login requirement.

## Real-source verification limits
Testing uses disposable SQLite backups of the supplied database, not writes to the original subscriptions. The five-source comparison found both engine defects and obsolete/upstream-dependent rules. 乐久笑话 can recover readable text despite an obsolete content selector; 61儿歌网 currently serves a maintenance page; 土豆单机游戏 returns HTTP 404 for generated item links; 迅雷榜单 does not yield extractable article content through the imported rule. Those cases must report errors/poor ratings rather than a healthy source based on HTTP latency. Gamer520 is also checked to prevent regression on public content accompanied by a login widget.

The checker samples two articles and is not a guarantee that every historical item is readable. Full browser-only JavaScript execution, all Legado dialects and automatic repair of third-party source definitions remain outside this change. Previously stored ratings change when detection is rerun.

## Verification result
macOS engine, Aria ViewModels and Web server built successfully. All 169 CTest entries passed, including 15 RSS engine cases (116 assertions), Node navigation/retry tests and local HTTP integration. Browser verification found 60 source choices and confirmed sidebar width/persistence. A final real-source probe recovered Gamer520 public content (9,835 characters) with no diagnostic. The RSS rating SSE provider now calls `sink.done()` after its completion event; an integration test verifies one run terminates instead of repeatedly rechecking subscriptions.

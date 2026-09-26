/**
 * reader.js — 阅读会话管理、目录/正文加载
 * 依赖：app.js 中的全局变量（API, currentActiveTab 等）和工具函数（esc, esc2, toast）
 * 依赖：bookshelf.js 中的 isInShelf, showChangeSource, saveProgress
 */

// ──────────────────────────────────────────────
// 活跃阅读会话的持久化索引（localStorage）
// 只存轻量索引：sessionKey + activeView + currentChapterIndex + book 元信息，
// 具体的 chapters/catalogHtml/contentHtml 全部从 C++ 缓存 DB 重新加载（秒开）。
// ──────────────────────────────────────────────
const READER_ACTIVE_SESSION_KEY = 'ariaread_active_session';

/// 把当前活跃会话序列化到 localStorage（容错，任何异常都静默忽略）
function _persistActiveSession() {
    try {
        const s = getActiveReadingSession();
        if (!s || !s.key) {
            localStorage.removeItem(READER_ACTIVE_SESSION_KEY);
            return;
        }
        // 换源预览态不持久化（重启应丢弃临时状态）
        if (s.pendingChangeSource) return;
        const book = s.book || {};
        const payload = {
            key: s.key,
            activeView: s.activeView || 'catalog',
            currentChapterIndex: typeof s.currentChapterIndex === 'number' ? s.currentChapterIndex : -1,
            // 书元信息：重启后用来创建 session（即使书不在书架也能恢复）
            book: {
                name: book.name || book.bookName || '',
                author: book.author || book.bookAuthor || '',
                url: book.url || book.bookUrl || book.book_url || '',
                coverUrl: book.coverUrl || book.cover_url || '',
                sourceName: book.sourceName || '',
                sourceUrl: book.sourceUrl || book.source_url || '',
                sourceIndex: typeof book.sourceIndex === 'number' ? book.sourceIndex : -1,
                intro: book.intro || '',
                kind: book.kind || '',
                isFromShelf: !!book.isFromShelf,
            },
            savedAt: Date.now(),
        };
        localStorage.setItem(READER_ACTIVE_SESSION_KEY, JSON.stringify(payload));
    } catch {}
}

/// 从 localStorage 读出活跃会话索引，失败返回 null
function _loadPersistedActiveSession() {
    try {
        const raw = localStorage.getItem(READER_ACTIVE_SESSION_KEY);
        if (!raw) return null;
        const p = JSON.parse(raw);
        if (!p || !p.key || !p.book || !p.book.url) return null;
        return p;
    } catch { return null; }
}

/// 启动时在阅读 Tab 还原活跃会话（若存在）。返回是否还原成功。
/// 实现：
///   1. 从 localStorage 读 { key, book, activeView, currentChapterIndex }
///   2. 用 book 元信息调 fetchCatalog（走 C++ 缓存 DB，秒开）
///   3. 目录就绪后若 activeView === 'content' 且 chapterIndex 合法，自动切到正文
async function restoreActiveSessionFromStorage() {
    const p = _loadPersistedActiveSession();
    if (!p) return false;
    // 已经还原过/已有活跃会话则跳过
    if (activeReadingSessionKey) return true;

    try {
        // 给 fetchCatalog 喂完整的 book
        const book = { ...p.book };
        // 存活性校验：如果书不在书架（被删了/从未入架），把 isFromShelf 改成 false，
        // 避免 fetchCatalog 走"从书架缓存读"的分支报错
        if (book.isFromShelf) {
            try {
                const stillInShelf = (typeof shelfBookUrls !== 'undefined' && shelfBookUrls.has(book.url)) ||
                                      (typeof isInShelf === 'function' && isInShelf(book.url));
                if (!stillInShelf) {
                    book.isFromShelf = false;
                }
            } catch { book.isFromShelf = false; }
        }
        // 触发正常的目录加载流程（内部会走 C++ 缓存 → 秒开）
        await fetchCatalog(book);

        // 目录到位后，如果上次停在正文，则切到对应章节
        const session = getReadingSession(p.key);
        if (session && p.activeView === 'content' &&
            typeof p.currentChapterIndex === 'number' && p.currentChapterIndex >= 0 &&
            Array.isArray(session.chapters) && p.currentChapterIndex < session.chapters.length) {
            fetchContentByIndex(p.currentChapterIndex, encodeReadingSessionKey(p.key));
        }
        return true;
    } catch {
        // 还原失败（比如书已被删、网络异常），静默清空索引
        try { localStorage.removeItem(READER_ACTIVE_SESSION_KEY); } catch {}
        return false;
    }
}

// ──────────────────────────────────────────────
// 阅读会话（Reading Session）
// ──────────────────────────────────────────────

function buildReadingSessionKey(book) {
    const bookUrl = book?.url || book?.bookUrl || book?.book_url || '';
    const sourceUrl = book?.sourceUrl || book?.source_url || '';
    return bookUrl ? `${bookUrl}@@${sourceUrl}` : '';
}

function createReadingSession(book = {}) {
    const normalizedBook = { ...book };
    if (!normalizedBook.url) normalizedBook.url = normalizedBook.bookUrl || normalizedBook.book_url || '';
    return {
        key: buildReadingSessionKey(normalizedBook),
        book: normalizedBook,
        chapters: [],
        currentChapterIndex: -1,
        activeView: 'catalog',
        catalogHtml: '',
        contentHtml: '',
        catalogAbort: null,
        contentAbort: null,
        pendingChangeSource: null,
    };
}

function getReadingSession(sessionKey = activeReadingSessionKey) {
    return sessionKey ? (readingSessions.get(sessionKey) || null) : null;
}

function getActiveReadingSession() {
    return getReadingSession(activeReadingSessionKey);
}

function syncActiveReadingSessionGlobals() {
    const session = getActiveReadingSession();
    currentBook = session?.book || null;
    currentChapters = session?.chapters || [];
    currentChapterIndex = typeof session?.currentChapterIndex === 'number' ? session.currentChapterIndex : -1;
    cachedCatalogHtml = session?.catalogHtml || '';
    cachedContentHtml = session?.contentHtml || '';
}

function upsertReadingSession(book) {
    const key = buildReadingSessionKey(book);
    if (!key) return null;
    let session = readingSessions.get(key);
    if (!session) {
        session = createReadingSession(book);
        readingSessions.set(key, session);
    } else {
        session.book = {
            ...session.book,
            ...book,
            url: book?.url || book?.bookUrl || book?.book_url || session.book?.url || '',
        };
    }
    session.key = key;
    return session;
}

function setActiveReadingSession(sessionOrKey) {
    const key = typeof sessionOrKey === 'string' ? sessionOrKey : sessionOrKey?.key || '';
    activeReadingSessionKey = key;
    syncActiveReadingSessionGlobals();
    _persistActiveSession();  // 切换活跃会话时持久化
    return getActiveReadingSession();
}

function isSessionActive(sessionOrKey) {
    const key = typeof sessionOrKey === 'string' ? sessionOrKey : sessionOrKey?.key || '';
    return !!key && key === activeReadingSessionKey;
}

function updateReadingSession(sessionKey, patch = {}) {
    const session = getReadingSession(sessionKey);
    if (!session) return null;
    Object.assign(session, patch);
    if (isSessionActive(sessionKey)) syncActiveReadingSessionGlobals();
    // 会影响持久化状态的字段：activeView / currentChapterIndex / book / pendingChangeSource
    if (isSessionActive(sessionKey) &&
        ('activeView' in patch || 'currentChapterIndex' in patch ||
         'book' in patch || 'pendingChangeSource' in patch)) {
        _persistActiveSession();
    }
    return session;
}

function abortReadingSessionRequest(session, fieldName) {
    const ctrl = session?.[fieldName];
    if (!ctrl) return;
    ctrl.abort();
    if (session[fieldName] === ctrl) session[fieldName] = null;
}

function encodeReadingSessionKey(sessionKey) {
    return encodeURIComponent(sessionKey || '');
}

function decodeReadingSessionKey(sessionKey) {
    return decodeURIComponent(sessionKey || '');
}

function showChangeSourceForSession(sessionKey = activeReadingSessionKey) {
    const decodedKey = decodeReadingSessionKey(sessionKey);
    const session = getReadingSession(decodedKey);
    if (session?.book) showChangeSource(session.book);
}

function closeReadingSession(sessionKey) {
    const session = getReadingSession(sessionKey);
    if (!session) return;
    abortReadingSessionRequest(session, 'catalogAbort');
    abortReadingSessionRequest(session, 'contentAbort');
    readingSessions.delete(sessionKey);
    if (activeReadingSessionKey === sessionKey) {
        const nextSession = Array.from(readingSessions.values()).at(-1) || null;
        activeReadingSessionKey = nextSession?.key || '';
        if (nextSession && (!nextSession.activeView || nextSession.activeView === 'content' && !nextSession.contentHtml)) {
            nextSession.activeView = nextSession.contentHtml ? 'content' : 'catalog';
        }
        syncActiveReadingSessionGlobals();
        _persistActiveSession();  // 活跃会话变化，刷新持久化索引
    }
    if (currentActiveTab === 'reader') renderReaderWorkspace();
}

function setReaderView(viewName, sessionKey = activeReadingSessionKey) {
    const decodedKey = decodeReadingSessionKey(sessionKey);
    const session = getReadingSession(decodedKey);
    if (!session) return;
    setActiveReadingSession(decodedKey);
    updateReadingSession(decodedKey, { activeView: viewName === 'content' ? 'content' : 'catalog' });
    if (currentActiveTab !== 'reader') {
        switchTab('reader');
        return;
    }
    renderReaderWorkspace();
}

function isReaderTabActive() {
    return currentActiveTab === 'reader';
}

function isReaderViewVisible(sessionKey, viewName) {
    const session = getReadingSession(sessionKey);
    return isReaderTabActive() && isSessionActive(sessionKey) && (session?.activeView || 'catalog') === viewName;
}

// ──────────────────────────────────────────────
// 阅读工作区渲染
// ──────────────────────────────────────────────

function buildReaderEmptyState() {
    return `
        <div class="empty">
            <div class="icon">📖</div>
            <h3>尚未打开书籍</h3>
            <p>请先从搜索结果或书架中点击一本书，进入它自己的阅读工作区</p>
        </div>`;
}

function buildReaderWorkspaceHtml() {
    const sessions = Array.from(readingSessions.values());
    const activeSession = getActiveReadingSession();
    if (!sessions.length || !activeSession) return buildReaderEmptyState();

    const activeView = activeSession.activeView || 'catalog';
    const contentHtml = activeView === 'content'
        ? (activeSession.contentHtml || '<div class="empty"><div class="icon">📖</div><h3>暂无正文</h3><p>请先从当前书的目录中选择一个章节</p></div>')
        : (activeSession.catalogHtml || '<div class="empty"><div class="icon">📋</div><h3>暂无目录</h3><p>请先等待当前书目录加载完成</p></div>');

    return `
        <div class="reader-workspace">
            <div class="reader-session-tabs">
            ${sessions.map(session => {
                const encodedKey = encodeReadingSessionKey(session.key);
                const isActive = session.key === activeSession.key;
                const title = session.book?.name || '未命名书籍';
                const isPending = !!session.pendingChangeSource;
                return `
                    <button class="reader-session-tab ${isActive ? 'active' : ''} ${isPending ? 'pending-source' : ''}" onclick="setReaderView('${session.activeView || 'catalog'}', '${encodedKey}')" title="${esc2(title)}${isPending ? ' [换源预览]' : ''}">
                        ${isPending ? '<span class="pending-dot">●</span>' : ''}
                        <span class="reader-session-title">${esc(title)}</span>
                        <span class="reader-session-close" onclick="event.stopPropagation();closeReadingSession(decodeURIComponent('${encodedKey}'))">✕</span>
                    </button>`;
            }).join('')}
            </div>
            <div class="reader-subtabs">
                <button class="reader-subtab ${activeView === 'catalog' ? 'active' : ''}" onclick="setReaderView('catalog')">📋 目录</button>
                <button class="reader-subtab ${activeView === 'content' ? 'active' : ''}" onclick="setReaderView('content')">📖 正文</button>
                <div class="reader-book-meta">${esc(activeSession.book?.sourceName || '未标记书源')}</div>
                <!-- D1: 阅读设置入口（仅 content 视图容器内生效，但按钮常驻以便随时调整） -->
                <button class="reader-settings-btn" style="margin-left:auto" onclick="toggleReaderSettings()" title="阅读设置">⚙️</button>
            </div>
            <div class="reader-panel" id="readerPanel">${contentHtml}</div>
        </div>`;
}

function renderReaderWorkspace() {
    const el = document.getElementById('results');
    if (!el) return;
    el.innerHTML = buildReaderWorkspaceHtml();
    // D1: 每次渲染后应用一次用户保存的阅读偏好（主题/字号/字体/行距）
    if (typeof applyReaderPrefsToDom === 'function') applyReaderPrefsToDom();
}

// ──────────────────────────────────────────────
// 目录加载
// ──────────────────────────────────────────────

function _buildCatalogHtml(session, chapters, sourceIndex, sourceName, refreshHint) {
    const catalogBookUrl = session?.book?.url || session?.book?.bookUrl || '';
    const catalogInShelf = isInShelf(catalogBookUrl);
    const sessionKey = session?.key || '';
    const encodedSessionKey = encodeReadingSessionKey(sessionKey);
    const pendingChangeSource = session?.pendingChangeSource || null;

    let topBarHtml = '';
    if (pendingChangeSource) {
        const pData = esc2(JSON.stringify(pendingChangeSource));
        topBarHtml = `
        <div class="change-source-banner">
            <div class="csb-info">
                <span class="csb-label">🔄 换源预览</span>
                <span class="csb-from">${esc(pendingChangeSource.originalBookName || '原书源')}</span>
                <span class="csb-arrow">→</span>
                <span class="csb-to">${esc(sourceName || '新书源')}</span>
            </div>
            <div class="csb-actions">
                <button class="csb-btn csb-confirm" onclick="doChangeSource(${pData}, '${encodedSessionKey}')">✅ 确认换源</button>
                <button class="csb-btn csb-cancel" onclick="closeReadingSession(decodeURIComponent('${encodedSessionKey}'))">✕ 取消</button>
            </div>
        </div>`;
    }

    const catalogShelfBtn = pendingChangeSource ? '' : (catalogInShelf
        ? `<button class="add-shelf-btn in-shelf" style="margin-left:12px" disabled>✓ 已收藏</button>`
        : `<button class="add-shelf-btn" style="margin-left:12px" onclick="event.stopPropagation();addToShelfFromCatalog('${encodedSessionKey}')">+ 书架</button>`);
    const hintHtml = refreshHint
        ? `<span class="catalog-refresh-hint" id="catalogRefreshHint" style="margin-left:8px;font-size:12px;color:var(--text2)">${refreshHint}</span>`
        : '';
    return `
        ${topBarHtml}
        <div class="section-title">📖 ${esc(session?.book?.name)} <span class="badge">${chapters.length} 章</span>
            ${catalogShelfBtn}${hintHtml}
        </div>
        <div class="chapter-list">
            ${chapters.map((c, i) => {
                return `<div class="chapter-item" data-idx="${i}" onclick="fetchContentByIndex(${i}, '${encodedSessionKey}')">${esc(c.title || c.name || '未命名章节')}</div>`;
            }).join('')}
        </div>`;
}

function _isFullyLocalBook(book) {
    if (!book?.kind) return false;
    const kind = book.kind || '';
    const finishedKeywords = ['完结', '完本', '已完结', '已完本', '全本'];
    const isFinished = finishedKeywords.some(kw => kind.includes(kw));
    const totalChapters = book.totalChapters || book.catalogCached || 0;
    const contentCached = book.contentCached || 0;
    return isFinished && totalChapters > 0 && contentCached >= totalChapters;
}

async function fetchCatalog(book) {
    const catalogUrl = book?.url || book?.book_url || book?.bookUrl || '';
    if (!catalogUrl) { toast('该条目缺少目录地址，无法获取目录', 'error'); return; }

    const sourceIndex = book?.sourceIndex;
    const sourceName = book?.sourceName || '';
    const sourceUrl = book?.sourceUrl || book?.source_url || '';
    const isFromShelf = book?.isFromShelf || false;
    const pendingChangeSource = book?.pendingChangeSource || null;
    const sessionBook = { ...book, url: catalogUrl, sourceIndex, sourceName, sourceUrl };
    const session = upsertReadingSession(sessionBook);
    if (!session) { toast('无法创建阅读会话', 'error'); return; }

    setActiveReadingSession(session);
    updateReadingSession(session.key, {
        activeView: 'catalog',
        contentHtml: session.contentHtml || '',
        pendingChangeSource: pendingChangeSource,
    });
    abortReadingSessionRequest(session, 'catalogAbort');
    abortReadingSessionRequest(session, 'contentAbort');

    const bookName = session.book?.name || '未知书籍';
    const loadingCatalogHtml = `<div class="loading">
        <div class="spinner"></div>
        <div class="loading-text">正在加载《${esc(bookName)}》的目录…</div>
        <div class="loading-hint" id="loadingHint" style="display:none">加载较慢，请耐心等待</div>
    </div>`;
    updateReadingSession(session.key, { catalogHtml: loadingCatalogHtml });
    if (currentActiveTab !== 'reader') {
        switchTab('reader');
    } else {
        renderReaderWorkspace();
    }

    // ── 书架中的书：先读缓存瞬间展示，再后台刷新 ──
    if (isFromShelf && sourceUrl) {
        try {
            let cacheReq = `${API}/api/catalog/cached?url=${encodeURIComponent(catalogUrl)}&source_url=${encodeURIComponent(sourceUrl)}`;
            if (sourceName) cacheReq += `&source_name=${encodeURIComponent(sourceName)}`;
            if (Number.isInteger(sourceIndex)) cacheReq += `&source_index=${sourceIndex}`;
            const kind = session.book?.kind || '';
            if (kind) cacheReq += `&kind=${encodeURIComponent(kind)}`;
            const cacheResp = await fetch(cacheReq);
            const cacheData = await cacheResp.json();

            const cachedChapters = Array.isArray(cacheData) ? cacheData : (cacheData?.chapters || []);
            const isFullyLocal = typeof cacheData?.isFullyLocal === 'boolean'
                ? cacheData.isFullyLocal
                : _isFullyLocalBook(session.book);

            if (Array.isArray(cachedChapters) && cachedChapters.length > 0) {
                const mappedChapters = cachedChapters.map((c, i) => ({ ...c, sourceIndex, sourceName, _idx: i }));
                updateReadingSession(session.key, { chapters: mappedChapters, activeView: 'catalog' });

                const refreshHint = isFullyLocal ? '📗 已完结·本地' : '🔄 检查更新中…';
                const activeSession = getReadingSession(session.key);
                const nextCatalogHtml = _buildCatalogHtml(activeSession, mappedChapters, sourceIndex, sourceName, refreshHint);
                updateReadingSession(session.key, { catalogHtml: nextCatalogHtml });
                if (isReaderViewVisible(session.key, 'catalog')) renderReaderWorkspace();

                if (isFullyLocal) return;

                const abortCtrl = new AbortController();
                updateReadingSession(session.key, { catalogAbort: abortCtrl });

                try {
                    let refreshReq = `${API}/api/catalog/refresh?url=${encodeURIComponent(catalogUrl)}`;
                    if (Number.isInteger(sourceIndex)) refreshReq += `&source_index=${sourceIndex}`;
                    if (sourceName) refreshReq += `&source_name=${encodeURIComponent(sourceName)}`;
                    if (sourceUrl) refreshReq += `&source_url=${encodeURIComponent(sourceUrl)}`;

                    const refreshResp = await fetch(refreshReq, { signal: abortCtrl.signal });
                    const freshChapters = await refreshResp.json();
                    const latestSession = getReadingSession(session.key);
                    if (!latestSession || latestSession.catalogAbort !== abortCtrl) return;

                    if (Array.isArray(freshChapters) && freshChapters.length > 0) {
                        const oldCount = latestSession.chapters.length;
                        const newCount = freshChapters.length;
                        const latestMapped = freshChapters.map((c, i) => ({ ...c, sourceIndex, sourceName, _idx: i }));
                        const latestCatalogHtml = _buildCatalogHtml(latestSession, latestMapped, sourceIndex, sourceName,
                            newCount !== oldCount ? `✅ 已更新 (${oldCount}→${newCount}章)` : '✅ 已是最新');
                        // ⚠️ 只更新数据与目录 HTML，**不要**覆盖 activeView——
                        // 用户可能已经在目录加载期间点进了某章节（activeView 变为 'content'），
                        // 这里强行改回 'catalog' + 重渲染会把用户从正文踢回目录，体验极差。
                        updateReadingSession(session.key, {
                            chapters: latestMapped,
                            catalogHtml: latestCatalogHtml,
                            catalogAbort: null,
                        });
                        if (isReaderViewVisible(session.key, 'catalog')) renderReaderWorkspace();
                    } else {
                        const hintText = '⚠️ 刷新失败';
                        const s = getReadingSession(session.key);
                        if (s?.catalogHtml) {
                            updateReadingSession(session.key, { catalogHtml: s.catalogHtml.replace(/(id="catalogRefreshHint"[^>]*>)(.*?)(<\/span>)/, `$1${hintText}$3`) });
                            if (isReaderViewVisible(session.key, 'catalog')) renderReaderWorkspace();
                        }
                    }
                } catch (refreshErr) {
                    if (refreshErr.name !== 'AbortError') {
                        const s = getReadingSession(session.key);
                        if (s?.catalogHtml) {
                            updateReadingSession(session.key, { catalogHtml: s.catalogHtml.replace(/(id="catalogRefreshHint"[^>]*>)(.*?)(<\/span>)/, '$1⚠️ 刷新失败$3') });
                            if (isReaderViewVisible(session.key, 'catalog')) renderReaderWorkspace();
                        }
                    }
                } finally {
                    const s = getReadingSession(session.key);
                    if (s?.catalogAbort === abortCtrl) s.catalogAbort = null;
                    if (isSessionActive(session.key)) syncActiveReadingSessionGlobals();
                }
                return;
            }
        } catch (e) {
            // 缓存读取失败，走常规流程
        }
    }

    // ── 常规流程：直接拉取目录 ──
    const slowTimer = setTimeout(() => {
        if (!isReaderViewVisible(session.key, 'catalog')) return;
        const hint = document.getElementById('loadingHint');
        if (hint) hint.style.display = 'block';
    }, 5000);

    try {
        let req = `${API}/api/catalog?url=${encodeURIComponent(catalogUrl)}`;
        if (Number.isInteger(sourceIndex)) req += `&source_index=${sourceIndex}`;
        if (sourceName) req += `&source_name=${encodeURIComponent(sourceName)}`;
        if (sourceUrl) req += `&source_url=${encodeURIComponent(sourceUrl)}`;

        const r = await fetch(req);
        clearTimeout(slowTimer);
        const chapters = await r.json();
        if (chapters.error) { toast(chapters.error, 'error'); return; }
        if (!chapters.length) {
            const encodedSessionKey = encodeReadingSessionKey(session.key);
            const emptyCatalogHtml = `<div class="empty">
                <div class="icon">📭</div>
                <h3>未获取到目录</h3>
                <p>《${esc(session.book?.name)}》在该书源下没有返回章节，可尝试换一个书源</p>
                <button class="btn btn-primary" style="margin-top:12px" onclick="showChangeSourceForSession('${encodedSessionKey}')">🔄 换源重试</button>
            </div>`;
            updateReadingSession(session.key, { chapters: [], catalogHtml: emptyCatalogHtml, activeView: 'catalog' });
            if (isReaderViewVisible(session.key, 'catalog')) renderReaderWorkspace();
            return;
        }
        const mappedChapters = chapters.map((c, i) => ({ ...c, sourceIndex, sourceName, _idx: i }));
        const activeSession = getReadingSession(session.key);
        const nextCatalogHtml = _buildCatalogHtml(activeSession, mappedChapters, sourceIndex, sourceName, '');
        updateReadingSession(session.key, { chapters: mappedChapters, catalogHtml: nextCatalogHtml, activeView: 'catalog' });
        if (isReaderViewVisible(session.key, 'catalog')) renderReaderWorkspace();
    } catch (e) {
        clearTimeout(slowTimer);
        toast('获取目录失败: ' + e.message, 'error');
    }
}

// ──────────────────────────────────────────────
// 正文加载
// ──────────────────────────────────────────────

async function fetchContent(chapter, sessionKey = activeReadingSessionKey) {
    const decodedSessionKey = decodeReadingSessionKey(sessionKey);
    const session = getReadingSession(decodedSessionKey);
    if (!session || !chapter) return;

    // 防御：章节 url 为空时不要发请求，直接渲染友好提示
    // 触发场景：书源解析出的章节列表存在 url 为空的项，或 localStorage 残留无效会话
    if (!chapter.url || !String(chapter.url).trim()) {
        const encodedSessionKey = encodeReadingSessionKey(session.key);
        const bookDataJson = esc2(JSON.stringify(session.book || {}));
        const errorHtml = `
            <div class="empty">
                <div class="icon">📭</div>
                <h3>该章节无正文链接</h3>
                <p style="margin-bottom:16px">书源解析出的章节没有有效的 URL，建议换源</p>
                ${session.book ? `
                    <button class="btn btn-primary" style="margin-right:6px" onclick='showAutoSource(${bookDataJson})'>🧐 自动换源</button>
                    <button class="btn" onclick="showChangeSourceForSession('${encodedSessionKey}')">🔄 手动换源</button>
                ` : ''}
            </div>`;
        updateReadingSession(session.key, { contentHtml: errorHtml, activeView: 'content' });
        if (isReaderViewVisible(session.key, 'content')) renderReaderWorkspace();
        return;
    }

    setActiveReadingSession(session.key);
    updateReadingSession(session.key, { activeView: 'content' });
    abortReadingSessionRequest(session, 'contentAbort');

    const chapterTitle = chapter?.title || chapter?.name || '未知章节';
    const loadingContentHtml = `<div class="loading">
        <div class="spinner"></div>
        <div class="loading-text">正在加载 ${esc(chapterTitle)}…</div>
        <div class="loading-hint" id="loadingHint" style="display:none">加载较慢，请耐心等待</div>
    </div>`;
    updateReadingSession(session.key, { contentHtml: loadingContentHtml });
    if (currentActiveTab !== 'reader') {
        switchTab('reader');
    } else {
        renderReaderWorkspace();
    }

    const slowTimer = setTimeout(() => {
        if (!isReaderViewVisible(session.key, 'content')) return;
        const hint = document.getElementById('loadingHint');
        if (hint) hint.style.display = 'block';
    }, 5000);

    try {
        let req = `${API}/api/content?url=${encodeURIComponent(chapter.url)}`;
        if (Number.isInteger(chapter?.sourceIndex)) req += `&source_index=${chapter.sourceIndex}`;
        if (chapter?.sourceName) req += `&source_name=${encodeURIComponent(chapter.sourceName)}`;
        if (session.book?.url) req += `&book_url=${encodeURIComponent(session.book.url)}`;
        if (session.book?.sourceUrl) req += `&source_url=${encodeURIComponent(session.book.sourceUrl)}`;
        if (Number.isInteger(chapter?.index)) req += `&chapter_index=${chapter.index}`;

        const abortCtrl = new AbortController();
        updateReadingSession(session.key, { contentAbort: abortCtrl });
        const r = await fetch(req, { signal: abortCtrl.signal });
        clearTimeout(slowTimer);
        const activeSession = getReadingSession(session.key);
        if (!activeSession || activeSession.contentAbort !== abortCtrl) return;

        const d = await r.json();
        if (d.error) {
            toast(d.error, 'error');
            const encodedSessionKey = encodeReadingSessionKey(session.key);
            // D7 场景 (a): 提供"自动换源"入口
            const bookDataJson = esc2(JSON.stringify(activeSession.book || {}));
            const errorHtml = `
                <div class="empty">
                    <div class="icon">⚠️</div>
                    <h3>获取正文失败</h3>
                    <p style="margin-bottom:16px">${esc(d.error).substring(0, 100)}</p>
                    ${activeSession.book ? `
                        <button class="btn btn-primary" style="margin-right:6px" onclick='showAutoSource(${bookDataJson})'>🧐 自动换源</button>
                        <button class="btn" onclick="showChangeSourceForSession('${encodedSessionKey}')">🔄 手动换源</button>
                    ` : ''}
                </div>`;
            updateReadingSession(session.key, { contentHtml: errorHtml, contentAbort: null, activeView: 'content' });
            if (isReaderViewVisible(session.key, 'content')) renderReaderWorkspace();
            return;
        }

        const paragraphs = (d.content || '').split(/\n+/).filter(p => p.trim()).map(p => `<p>${esc(p)}</p>`).join('');
        const encodedSessionKey = encodeReadingSessionKey(session.key);
        const hasPrev = activeSession.currentChapterIndex > 0;
        const hasNext = activeSession.currentChapterIndex < activeSession.chapters.length - 1;
        const navHtml = `
            <div class="reading-nav">
                <button class="btn" ${hasPrev ? `onclick="fetchContentByIndex(${activeSession.currentChapterIndex - 1}, '${encodedSessionKey}')"` : 'disabled'}>⬅ 上一章</button>
                <span class="chapter-info">${esc(chapter.title || chapter.name || '正文')}</span>
                <button class="btn" ${hasNext ? `onclick="fetchContentByIndex(${activeSession.currentChapterIndex + 1}, '${encodedSessionKey}')"` : 'disabled'}>下一章 ➡</button>
            </div>`;
        const bottomNavHtml = `
            <div class="reading-nav" style="margin-top:24px;border-top:1px solid var(--border);border-bottom:none;padding-top:12px">
                <button class="btn" ${hasPrev ? `onclick="fetchContentByIndex(${activeSession.currentChapterIndex - 1}, '${encodedSessionKey}')"` : 'disabled'}>⬅ 上一章</button>
                <button class="btn" onclick="setReaderView('catalog', '${encodedSessionKey}')">📋 目录</button>
                <button class="btn" ${hasNext ? `onclick="fetchContentByIndex(${activeSession.currentChapterIndex + 1}, '${encodedSessionKey}')"` : 'disabled'}>下一章 ➡</button>
            </div>`;

        const nextContentHtml = `${navHtml}
            <div class="content-view" id="readerContentView">${paragraphs || '<p>（空内容）</p>'}</div>${bottomNavHtml}`;
        updateReadingSession(session.key, { contentHtml: nextContentHtml, contentAbort: null, activeView: 'content' });
        if (isReaderViewVisible(session.key, 'content')) renderReaderWorkspace();

        saveProgress(activeSession.book, chapter, activeSession.currentChapterIndex);
        const el = document.getElementById('results');
        if (el) el.scrollTop = 0;

        // 正文获取成功 → 后端已自动缓存，异步刷新书架卡片上的缓存数值
        if (session.book?.url && session.book?.isFromShelf) {
            try {
                const cacheResp = await fetch(`${API}/api/bookshelf/cache_status?book_url=${encodeURIComponent(session.book.url)}`);
                const cacheData = await cacheResp.json();
                if (cacheData.contentCount != null && typeof hashCode === 'function') {
                    const cardId = `card-${hashCode(session.book.url)}`;
                    const card = document.getElementById(cardId);
                    if (card && typeof updateShelfCardCacheByCard === 'function') {
                        const total = cacheData.catalogCount || cacheData.contentCount;
                        updateShelfCardCacheByCard(card, cacheData.contentCount, total);
                    }
                }
            } catch (_) { /* 静默失败，不影响阅读 */ }
        }
    } catch (e) {
        clearTimeout(slowTimer);
        if (e.name === 'AbortError') return;
        toast('获取正文失败: ' + e.message, 'error');
    } finally {
        const latestSession = getReadingSession(session.key);
        if (latestSession?.contentAbort && latestSession.contentAbort.signal?.aborted) latestSession.contentAbort = null;
        if (isSessionActive(session.key)) syncActiveReadingSessionGlobals();
    }
}

function fetchContentByIndex(idx, sessionKey = activeReadingSessionKey) {
    const decodedSessionKey = decodeReadingSessionKey(sessionKey);
    const session = getReadingSession(decodedSessionKey);
    if (!session || idx < 0 || idx >= session.chapters.length) return;
    updateReadingSession(session.key, { currentChapterIndex: idx });
    fetchContent(session.chapters[idx], session.key);
}

// ──────────────────────────────────────────────
// D1: 阅读器体验增强（偏好 / 设置浮层 / 章内搜索 / 进度条）
// ──────────────────────────────────────────────

const READER_PREFS_KEY = 'ariaread_reader_prefs';
const READER_PREFS_DEFAULT = {
    theme: 'default',       // default | night | sepia | day | green
    font: 'sans',           // sans | serif | mono
    fontSize: 16,           // 12 - 28 px
    lineHeight: 1.8,        // 1.4 - 2.4
};

function loadReaderPrefs() {
    try {
        const raw = localStorage.getItem(READER_PREFS_KEY);
        if (!raw) return { ...READER_PREFS_DEFAULT };
        const p = JSON.parse(raw);
        return { ...READER_PREFS_DEFAULT, ...p };
    } catch { return { ...READER_PREFS_DEFAULT }; }
}

function saveReaderPrefs(prefs) {
    try { localStorage.setItem(READER_PREFS_KEY, JSON.stringify(prefs)); } catch {}
}

function applyReaderPrefsToDom() {
    const panel = document.getElementById('readerPanel');
    if (!panel) return;
    const p = loadReaderPrefs();
    if (p.theme && p.theme !== 'default') panel.setAttribute('data-theme', p.theme);
    else panel.removeAttribute('data-theme');
    if (p.font && p.font !== 'sans') panel.setAttribute('data-font', p.font);
    else panel.removeAttribute('data-font');
    panel.style.setProperty('--reader-font-size', (p.fontSize || 16) + 'px');
    panel.style.setProperty('--reader-line-height', String(p.lineHeight || 1.8));
}

function toggleReaderSettings() {
    const existing = document.getElementById('readerSettingsPopup');
    if (existing) { existing.remove(); return; }

    const prefs = loadReaderPrefs();
    const popup = document.createElement('div');
    popup.id = 'readerSettingsPopup';
    popup.className = 'reader-settings-popup';
    popup.innerHTML = `
        <h4>主题</h4>
        <div class="row">
            <div class="swatch" data-theme="default" style="background:#1a1d27" title="深色（默认）"></div>
            <div class="swatch" data-theme="night" style="background:#15171e" title="深夜"></div>
            <div class="swatch" data-theme="sepia" style="background:#f4ecd8" title="羊皮纸"></div>
            <div class="swatch" data-theme="day" style="background:#fbf9f2" title="日间"></div>
            <div class="swatch" data-theme="green" style="background:#cce8cf" title="护眼绿"></div>
        </div>
        <h4>字体</h4>
        <div class="row">
            <span class="chip" data-font="sans">黑体</span>
            <span class="chip" data-font="serif">宋体</span>
            <span class="chip" data-font="mono">等宽</span>
        </div>
        <h4>字号 <span id="readerFontSizeLabel" style="color:var(--text);margin-left:4px">${prefs.fontSize}px</span></h4>
        <div class="row">
            <span class="chip" data-delta-size="-2">A-</span>
            <span class="chip" data-delta-size="-1">-1</span>
            <span class="chip" data-delta-size="1">+1</span>
            <span class="chip" data-delta-size="2">A+</span>
            <span class="chip" data-reset-size="1">默认</span>
        </div>
        <h4>行距 <span id="readerLineHLabel" style="color:var(--text);margin-left:4px">${prefs.lineHeight}</span></h4>
        <div class="row">
            <span class="chip" data-delta-lh="-0.1">紧凑</span>
            <span class="chip" data-delta-lh="0.1">宽松</span>
            <span class="chip" data-reset-lh="1">默认</span>
        </div>`;
    document.body.appendChild(popup);

    // 激活态回显
    popup.querySelectorAll('[data-theme]').forEach(el => {
        if (el.dataset.theme === prefs.theme) el.classList.add('active');
        el.addEventListener('click', () => {
            const p = loadReaderPrefs();
            p.theme = el.dataset.theme;
            saveReaderPrefs(p);
            popup.querySelectorAll('[data-theme]').forEach(x => x.classList.toggle('active', x.dataset.theme === p.theme));
            applyReaderPrefsToDom();
        });
    });
    popup.querySelectorAll('[data-font]').forEach(el => {
        if (el.dataset.font === prefs.font) el.classList.add('active');
        el.addEventListener('click', () => {
            const p = loadReaderPrefs();
            p.font = el.dataset.font;
            saveReaderPrefs(p);
            popup.querySelectorAll('[data-font]').forEach(x => x.classList.toggle('active', x.dataset.font === p.font));
            applyReaderPrefsToDom();
        });
    });
    popup.querySelectorAll('[data-delta-size]').forEach(el => {
        el.addEventListener('click', () => {
            const p = loadReaderPrefs();
            p.fontSize = Math.max(12, Math.min(28, (p.fontSize || 16) + Number(el.dataset.deltaSize)));
            saveReaderPrefs(p);
            document.getElementById('readerFontSizeLabel').textContent = p.fontSize + 'px';
            applyReaderPrefsToDom();
        });
    });
    popup.querySelector('[data-reset-size]')?.addEventListener('click', () => {
        const p = loadReaderPrefs();
        p.fontSize = READER_PREFS_DEFAULT.fontSize;
        saveReaderPrefs(p);
        document.getElementById('readerFontSizeLabel').textContent = p.fontSize + 'px';
        applyReaderPrefsToDom();
    });
    popup.querySelectorAll('[data-delta-lh]').forEach(el => {
        el.addEventListener('click', () => {
            const p = loadReaderPrefs();
            p.lineHeight = Math.max(1.4, Math.min(2.4, Math.round(((p.lineHeight || 1.8) + Number(el.dataset.deltaLh)) * 10) / 10));
            saveReaderPrefs(p);
            document.getElementById('readerLineHLabel').textContent = p.lineHeight;
            applyReaderPrefsToDom();
        });
    });
    popup.querySelector('[data-reset-lh]')?.addEventListener('click', () => {
        const p = loadReaderPrefs();
        p.lineHeight = READER_PREFS_DEFAULT.lineHeight;
        saveReaderPrefs(p);
        document.getElementById('readerLineHLabel').textContent = p.lineHeight;
        applyReaderPrefsToDom();
    });

    // 点击外部关闭
    setTimeout(() => {
        document.addEventListener('click', function onDocClick(e) {
            if (!popup.contains(e.target) && !e.target.closest('.reader-settings-btn')) {
                popup.remove();
                document.removeEventListener('click', onDocClick);
            }
        });
    }, 0);
}




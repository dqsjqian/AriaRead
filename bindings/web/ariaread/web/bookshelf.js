/**
 * bookshelf.js — 书架功能
 * 书架渲染、加入/移出书架、换源、阅读进度
 * 依赖 app.js 中的全局变量和函数
 */

// 书架 bookUrl 缓存，用于判断某书是否已在书架中
let shelfBookUrls = new Set();

// 书架渲染缓存（避免每次切换 tab 都重新请求 API）
let _shelfHtmlCache = '';
let _shelfDataDirty = true;  // 标记书架数据是否需要刷新

// 标记书架数据需要刷新（加入/移出/换源/下载完成后调用）
function markShelfDirty() { _shelfDataDirty = true; }

// 刷新书架缓存（从 /api/bookshelf 拉取所有 bookUrl）
async function refreshShelfCache() {
    try {
        const r = await fetch(`${API}/api/bookshelf`);
        const d = await r.json();
        shelfBookUrls = new Set((d.books || []).map(b => b.bookUrl));
        markShelfDirty();  // 数据变了，下次渲染时刷新
    } catch (e) {
        // 静默失败，不影响主流程
    }
}

// 判断某本书是否已在书架中
function isInShelf(bookUrl) {
    return shelfBookUrls.has(bookUrl);
}

// ──────────────────────────────────────────────
// 书架渲染
// ──────────────────────────────────────────────

async function renderBookshelf() {
    const el = document.getElementById('results');

    // 如果有缓存且数据未变，直接使用缓存 HTML（瞬间渲染）
    if (_shelfHtmlCache && !_shelfDataDirty) {
        el.innerHTML = _shelfHtmlCache;
        restoreDownloadProgressBars();
        return;
    }

    el.innerHTML = '<div class="loading"><div class="spinner"></div><div class="loading-text">正在加载书架…</div></div>';
    try {
        const r = await fetch(`${API}/api/bookshelf`);
        if (currentActiveTab !== 'bookshelf') return; // 用户已切走
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }
        if (currentActiveTab !== 'bookshelf') return; // 再次检查
        const books = d.books || [];
        if (books.length === 0) {
            _shelfHtmlCache = `
                <div class="shelf-empty-hint">
                    <div class="icon">📚</div>
                    <h3 style="color:var(--text);margin-bottom:8px">书架空空如也</h3>
                    <p>搜索书籍后，点击「+ 书架」收藏到这里</p>
                </div>`;
            _shelfDataDirty = false;
            el.innerHTML = _shelfHtmlCache;
            return;
        }
        _shelfHtmlCache = `
            <div class="section-title">📚 我的书架 <span class="badge">${books.length}</span> <button class="btn btn-cache-all" onclick="cacheAllBooks()">⬇️ 缓存全部</button></div>
            <div class="bookshelf-grid" id="shelfGrid">
                ${books.map(b => renderShelfCard(b)).join('')}
            </div>`;
        _shelfDataDirty = false;
        el.innerHTML = _shelfHtmlCache;
        // 恢复正在下载的书的进度条（DOM 重建后重新绑定）
        restoreDownloadProgressBars();
    } catch (e) {
        _shelfHtmlCache = '';
        _shelfDataDirty = true;
        toast('加载书架失败: ' + e.message, 'error');
    }
}

function renderShelfCard(b) {
    const progressText = b.progress
        ? `读到: ${esc(b.progress.chapterTitle || '第' + b.progress.chapterIndex + '章')}`
        : '尚未阅读';
    const coverHtml = b.coverUrl
        ? `<img src="${esc(b.coverUrl)}" onerror="this.parentNode.innerHTML='📖'" loading="lazy">`
        : '📖';
    const updateDot = b.hasUpdate ? 'show' : '';
    // 从 kind 中解析分类标签和连载状态
    const kindTags = parseKindTags(b.kind || '');
    let tagsHtml = '';
    if (kindTags.length > 0) {
        tagsHtml = `<div class="shelf-tags">${kindTags.map(t =>
            `<span class="shelf-tag ${t.type}">${esc(t.label)}</span>`
        ).join('')}</div>`;
    }
    // 缓存状态显示
    const catalogCached = b.catalogCached || 0;
    const contentCached = b.contentCached || 0;
    const totalChapters = b.totalChapters || catalogCached || 0;
    let cacheText = '';
    if (totalChapters > 0) {
        if (contentCached >= totalChapters) {
            cacheText = `<span class="cache-status cache-full">✅ 已缓存全部 ${contentCached} 章</span>`;
        } else if (contentCached > 0) {
            cacheText = `<span class="cache-status cache-partial">📦 已缓存 ${contentCached}/${totalChapters} 章</span>`;
        } else if (catalogCached > 0) {
            cacheText = `<span class="cache-status cache-catalog">📋 目录已缓存</span>`;
        }
    }
    // 下载菜单项文案（结合完结状态和缓存状态）
    const isFullyCached = totalChapters > 0 && contentCached >= totalChapters;
    const hasUpdate = b.hasUpdate;
    const hasCachedContent = contentCached > 0;
    const isFinished = kindTags.some(t => t.type === 'finished');
    const cardId = `card-${hashCode(b.bookUrl)}`;
    let dlMenuHtml = '';
    if (isFullyCached && !hasUpdate && isFinished) {
        // 已完结 + 已全量缓存 → 不显示下载菜单项
        dlMenuHtml = '';
    } else if (isFullyCached && !hasUpdate) {
        // 连载中/未知 + 已全量缓存 + 无更新 → 显示但禁用
        dlMenuHtml = `<div class="shelf-menu-item disabled">✅ 已是最新</div>`;
    } else if (isFullyCached && hasUpdate) {
        // 有更新 → 可下载新章节
        dlMenuHtml = `<div class="shelf-menu-item" onclick="event.stopPropagation();closeAllMenus();startDownload('${cardId}','${esc2(b.bookUrl)}','${esc2(b.sourceUrl)}','${esc2(b.sourceName)}')">🔄 下载新章节</div>`;
    } else if (hasCachedContent) {
        dlMenuHtml = `<div class="shelf-menu-item" onclick="event.stopPropagation();closeAllMenus();startDownload('${cardId}','${esc2(b.bookUrl)}','${esc2(b.sourceUrl)}','${esc2(b.sourceName)}')">⬇️ 继续下载</div>`;
    } else {
        dlMenuHtml = `<div class="shelf-menu-item" onclick="event.stopPropagation();closeAllMenus();startDownload('${cardId}','${esc2(b.bookUrl)}','${esc2(b.sourceUrl)}','${esc2(b.sourceName)}')">⬇️ 下载缓存</div>`;
    }
    const bookData = esc2(JSON.stringify({
        name: b.bookName, author: b.bookAuthor, url: b.bookUrl,
        coverUrl: b.coverUrl, sourceName: b.sourceName, sourceUrl: b.sourceUrl,
        sourceIndex: -1, intro: b.intro, kind: b.kind,
        contentCached: contentCached, totalChapters: totalChapters,
        catalogCached: catalogCached, isFromShelf: true
    }));
    return `<div class="shelf-card" id="${cardId}" onclick='openShelfBook(${bookData})'>
        <div class="update-dot ${updateDot}"></div>
        <button class="shelf-more-btn" onclick="event.stopPropagation();toggleShelfMenu(this)" title="更多操作">⋯</button>
        <div class="shelf-menu" style="display:none">
            <div class="shelf-menu-item" onclick="event.stopPropagation();closeAllMenus();showChangeSource(${bookData})">🔄 换源</div>
            <div class="shelf-menu-item" onclick="event.stopPropagation();closeAllMenus();showAutoSource(${bookData})">🧐 自动换源</div>
            ${dlMenuHtml}
            <div class="shelf-menu-item ${hasCachedContent ? '' : 'disabled'}" onclick="event.stopPropagation();closeAllMenus();exportBookTxt('${esc2(b.bookUrl)}','${esc2(b.sourceUrl)}')">📄 导出TXT</div>
            <div class="shelf-menu-item ${hasCachedContent ? '' : 'disabled'}" onclick="event.stopPropagation();closeAllMenus();clearCache('${esc2(b.bookUrl)}','${esc2(b.sourceUrl)}','${cardId}')">🧹 清空缓存</div>
            <div class="shelf-menu-divider"></div>
            <div class="shelf-menu-item danger" onclick="event.stopPropagation();closeAllMenus();removeFromShelf('${esc2(b.bookUrl)}','${esc2(b.sourceUrl)}')">🗑️ 移出书架</div>
        </div>
        <div class="cover-wrap">
            <div class="cover">${coverHtml}</div>
            <span class="shelf-source">📖 ${esc(b.sourceName || '未知书源')}</span>
        </div>
        <div class="shelf-info">
            <div class="shelf-title">${esc(b.bookName)}</div>
            <div class="shelf-author">作者：${esc(b.bookAuthor) || '未知'}</div>
            ${tagsHtml}
            <div class="shelf-progress">${progressText}</div>
            ${cacheText}
        </div>
    </div>`;
}

// ── 工具函数 ──

// 从 kind 字符串中解析分类标签和连载状态
// 常见格式："玄幻·连载中"、"都市|完结"、"玄幻,连载"、"科幻 完本"
function parseKindTags(kind) {
    if (!kind || !kind.trim()) return [];
    // 按常见分隔符拆分
    const parts = kind.split(/[·|,，、/\s]+/).map(s => s.trim()).filter(Boolean);
    const tags = [];
    const statusKeywords = {
        '连载中': 'ongoing', '连载': 'ongoing', '更新中': 'ongoing',
        '完结': 'finished', '完本': 'finished', '已完结': 'finished', '已完本': 'finished', '全本': 'finished',
    };
    for (const part of parts) {
        let matched = false;
        for (const [kw, type] of Object.entries(statusKeywords)) {
            if (part.includes(kw)) {
                tags.push({ label: kw === part ? kw : part, type });
                matched = true;
                break;
            }
        }
        if (!matched && part.length <= 8) {
            // 当作分类标签
            tags.push({ label: part, type: 'genre' });
        }
    }
    return tags;
}

function hashCode(str) {
    let h = 0;
    for (let i = 0; i < str.length; i++) {
        h = ((h << 5) - h + str.charCodeAt(i)) | 0;
    }
    return (h >>> 0).toString(36);
}

// ── 更多菜单控制 ──
function toggleShelfMenu(btn) {
    const card = btn.closest('.shelf-card');
    const menu = card.querySelector('.shelf-menu');
    const isOpen = menu.style.display !== 'none';
    closeAllMenus();
    if (!isOpen) {
        card.style.zIndex = '50';
        menu.style.display = 'block';
        // 点击其他地方关闭菜单
        setTimeout(() => document.addEventListener('click', _closeMenuHandler, { once: true }), 0);
    }
}
function _closeMenuHandler() { closeAllMenus(); }
function closeAllMenus() {
    document.querySelectorAll('.shelf-menu').forEach(m => m.style.display = 'none');
    document.querySelectorAll('.shelf-card').forEach(c => c.style.zIndex = '');
}

// ──────────────────────────────────────────────
// 加入/移出书架
// ──────────────────────────────────────────────

async function addToShelf(btnEl, book) {
    try {
        const r = await fetch(`${API}/api/bookshelf/add`, {
            method: 'POST', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({
                bookName: book.name || '',
                bookAuthor: book.author || '',
                coverUrl: book.cover_url || book.coverUrl || '',
                bookUrl: book.url || book.bookUrl || '',
                sourceName: book.sourceName || '',
                sourceUrl: book.sourceUrl || '',
                intro: book.intro || '',
                kind: book.kind || '',
                lastChapter: book.last_chapter || book.lastChapter || '',
            })
        });
        const d = await r.json();
        if (d.exists) {
            toast('📚 已在书架中', 'success');
            btnEl.textContent = '✓ 已收藏';
            btnEl.classList.add('in-shelf');
            btnEl.disabled = true;
        } else if (d.ok) {
            toast('📚 已加入书架', 'success');
            btnEl.textContent = '✓ 已收藏';
            btnEl.classList.add('in-shelf');
            btnEl.disabled = true;
            // 更新书架缓存
            const bUrl = book.url || book.bookUrl || '';
            if (bUrl) shelfBookUrls.add(bUrl);
            markShelfDirty();
        } else {
            toast(d.error || '加入失败', 'error');
        }
    } catch (e) { toast('加入书架失败: ' + e.message, 'error'); }
}

async function removeFromShelf(bookUrl, sourceUrl) {
    if (!confirm('确定要从书架移除吗？')) return;
    try {
        const r = await fetch(`${API}/api/bookshelf/remove`, {
            method: 'DELETE', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({ bookUrl, sourceUrl })
        });
        const d = await r.json();
        if (d.ok) {
            toast('🗑️ 已从书架移除', 'success');
            // 更新书架缓存
            shelfBookUrls.delete(bookUrl);
            markShelfDirty();
            renderBookshelf();
        } else {
            toast(d.error || '移除失败', 'error');
        }
    } catch (e) { toast('移除失败: ' + e.message, 'error'); }
}

function openShelfBook(book) {
    fetchCatalog(book);
}

function addToShelfFromCatalog(sessionKey = activeReadingSessionKey) {
    const session = typeof getReadingSession === 'function' ? getReadingSession(sessionKey) : null;
    const book = session?.book || currentBook;
    if (!book) { toast('无法获取书籍信息', 'error'); return; }
    // 找到目录页中的「+ 书架」按钮并直接操作
    const el = document.getElementById('results');
    const btn = el ? el.querySelector('.add-shelf-btn') : null;
    if (btn && !btn.disabled) {
        addToShelf(btn, book).then(() => {
            if (session && typeof updateReadingSession === 'function') {
                updateReadingSession(session.key, { catalogHtml: el.innerHTML });
            } else {
                cachedCatalogHtml = el.innerHTML;
            }
        });
    } else {
        // 兜底：用临时按钮
        const tmpBtn = document.createElement('button');
        tmpBtn.className = 'add-shelf-btn';
        addToShelf(tmpBtn, book);
    }
}

// ──────────────────────────────────────────────
// 换源
// ──────────────────────────────────────────────

async function showChangeSource(book) {
    const el = document.getElementById('results');
    el.innerHTML = '<div class="loading"><div class="spinner"></div><div class="loading-text">正在搜索可用书源…</div></div>';
    switchTab('search');

    // 用书名作为搜索关键词
    const keyword = book.name || '';
    const bookAuthor = (book.author || book.bookAuthor || '').trim().toLowerCase();
    if (!keyword) { toast('书名为空，无法换源', 'error'); return; }

    try {
        const url = `${API}/api/search/all?q=${encodeURIComponent(keyword)}&match_name=1&match_author=0&match_intro=0`;
        const evtSource = new EventSource(url);
        let candidates = [];

        el.innerHTML = `
            <div class="section-title">🔄 换源 - ${esc(book.name)} <span class="badge" id="changeSourceCount">搜索中...</span></div>
            <p style="color:var(--text2);font-size:13px;margin-bottom:16px">正在搜索可用书源，请稍候...</p>
            <div id="changeSourceList" class="change-source-modal"></div>`;

        evtSource.addEventListener('source_result', (e) => {
            const d = JSON.parse(e.data);
            if (d.books && d.books.length > 0) {
                const bookNameNorm = keyword.trim().toLowerCase();
                const matched = d.books.filter(b => {
                    const n1 = (b.name || '').trim().toLowerCase();
                    // 书名必须匹配
                    const nameMatch = n1 === bookNameNorm || n1.includes(bookNameNorm) || bookNameNorm.includes(n1);
                    if (!nameMatch) return false;
                    // 如果原书有作者名，则同时用作者名过滤（允许部分匹配）
                    if (bookAuthor) {
                        const a1 = (b.author || '').trim().toLowerCase();
                        if (a1 && !a1.includes(bookAuthor) && !bookAuthor.includes(a1)) return false;
                    }
                    return true;
                });
                for (const b of matched) {
                    if (b.sourceName === book.sourceName && (b.url || b.bookUrl) === book.url) continue;
                    candidates.push({
                        ...b,
                        sourceIndex: d.index,
                        sourceName: d.name,
                        sourceUrl: d.url,
                        latency: d.latency,
                    });
                }
                renderChangeSourceList(candidates, book);
            }
        });

        evtSource.addEventListener('search_done', () => {
            evtSource.close();
            const countEl = document.getElementById('changeSourceCount');
            if (countEl) countEl.textContent = `${candidates.length} 个可用书源`;
            if (candidates.length === 0) {
                const listEl = document.getElementById('changeSourceList');
                if (listEl) listEl.innerHTML = '<p style="color:var(--text2);padding:20px;text-align:center">未找到其他可用书源</p>';
            }
        });

        evtSource.onerror = () => {
            evtSource.close();
            const countEl = document.getElementById('changeSourceCount');
            if (countEl) countEl.textContent = `${candidates.length} 个可用书源`;
        };
    } catch (e) { toast('换源搜索失败: ' + e.message, 'error'); }
}

function renderChangeSourceList(candidates, originalBook) {
    const listEl = document.getElementById('changeSourceList');
    if (!listEl) return;
    listEl.innerHTML = candidates.map((c, i) => {
        const latencyText = typeof c.latency === 'number' ? `${c.latency}ms` : '';
        const cData = esc2(JSON.stringify(c));
        const origData = esc2(JSON.stringify(originalBook));
        return `<div class="source-option" onclick='previewChangeSource(${cData}, ${origData})'>
            <div class="so-name">📖 ${esc(c.sourceName || '未知书源')}</div>
            <div class="so-meta">${esc(c.name)} · ${esc(c.author || '未知')} ${latencyText ? '· ' + latencyText : ''}</div>
        </div>`;
    }).join('');
}

// 预览新书源目录（不立即换源，只是临时打开目录界面）
function previewChangeSource(newSource, originalBook) {
    fetchCatalog({
        name: newSource.name || originalBook.name,
        author: newSource.author || originalBook.author,
        url: newSource.url || newSource.bookUrl,
        sourceName: newSource.sourceName,
        sourceUrl: newSource.sourceUrl,
        sourceIndex: newSource.sourceIndex,
        // 换源预览标记：携带原书信息，供确认换源时使用
        pendingChangeSource: {
            originalBookUrl: originalBook.url || originalBook.bookUrl,
            originalSourceUrl: originalBook.sourceUrl || '',
            newSourceName: newSource.sourceName || '',
            newSourceUrl: newSource.sourceUrl || '',
            newBookUrl: newSource.url || newSource.bookUrl || '',
            newSourceIndex: newSource.sourceIndex ?? -1,
            originalBookName: originalBook.name || originalBook.bookName || '',
        },
    });
}

// 确认换源（在预览目录界面点击"确认换源"按钮后调用）
async function doChangeSource(pendingInfo, previewSessionKey) {
    if (!pendingInfo) { toast('换源信息缺失', 'error'); return; }
    try {
        const r = await fetch(`${API}/api/bookshelf/change_source`, {
            method: 'PUT', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({
                bookUrl: pendingInfo.originalBookUrl,
                oldSourceUrl: pendingInfo.originalSourceUrl,
                newSourceName: pendingInfo.newSourceName,
                newSourceUrl: pendingInfo.newSourceUrl,
                newBookUrl: pendingInfo.newBookUrl,
                newSourceIndex: pendingInfo.newSourceIndex ?? -1,
            })
        });
        const d = await r.json();
        if (d.ok) {
            toast('✅ 换源成功，开始全量缓存…', 'success');
            markShelfDirty();
            // 关闭预览 session
            if (previewSessionKey) closeReadingSession(decodeURIComponent(previewSessionKey));
            // 立即触发全量缓存
            const cardId = `card-${hashCode(pendingInfo.newBookUrl)}`;
            startDownload(cardId, pendingInfo.newBookUrl, pendingInfo.newSourceUrl, pendingInfo.newSourceName);
        } else {
            toast(d.error || '换源失败', 'error');
        }
    } catch (e) { toast('换源失败: ' + e.message, 'error'); }
}

// ──────────────────────────────────────────────
// 阅读进度
// ──────────────────────────────────────────────

async function saveProgress(book, chapter, chapterIndex) {
    if (!book || !chapter) return;
    try {
        await fetch(`${API}/api/bookshelf/progress`, {
            method: 'PUT', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({
                bookUrl: book.url || book.bookUrl || '',
                sourceUrl: book.sourceUrl || '',
                chapterIndex: chapterIndex || 0,
                chapterUrl: chapter.url || '',
                chapterTitle: chapter.title || chapter.name || '',
                readPercent: 0,
            })
        });
    } catch (e) { /* 静默失败 */ }
}

// ──────────────────────────────────────────────────
// 全量下载缓存
// ──────────────────────────────────────────────────

// 正在下载的 bookUrl 集合（防止重复点击）
// Map<bookUrl, { evtSource, cardId, done, total, cached, failed, finished, error }>
let downloadingBooks = new Map();

// 缓存全部：自动对所有未缓存完毕的书逐本执行下载
let _cacheAllRunning = false;
async function cacheAllBooks() {
    if (_cacheAllRunning) { toast('正在缓存中，请稍候', 'error'); return; }
    _cacheAllRunning = true;
    try {
        const r = await fetch(`${API}/api/bookshelf`);
        const d = await r.json();
        const books = d.books || [];
        // 筛选未缓存完毕的书
        // C++ 层 downloadBook 支持通过 sourceName 匹配书源，sourceUrl 为空也能工作
        const needCache = books.filter(b => {
            // sourceUrl 和 sourceName 都为空才跳过（无法定位书源）
            if (!b.sourceUrl && !b.sourceName) return false;
            if (downloadingBooks.has(b.bookUrl)) return false;
            const total = b.totalChapters || 0;
            const catalog = b.catalogCached || 0;
            const content = b.contentCached || 0;
            // 用 totalChapters 和 catalogCached 中较大的作为目标章节数
            const target = Math.max(total, catalog);
            // 正文没缓存完 → 需要缓存
            if (target > 0 && content < target) return true;
            // 没有目录，也没有正文 → 需要缓存（下载时会先拉目录）
            if (catalog === 0 && content === 0) return true;
            return false;
        });
        if (needCache.length === 0) {
            toast('✅ 所有书籍均已缓存完毕', 'success');
            _cacheAllRunning = false;
            return;
        }
        toast(`⬇️ 开始缓存 ${needCache.length} 本书…`, 'success');
        // 逐本启动下载（同步调用，EventSource 是异步的不会阻塞）
        let started = 0;
        for (const b of needCache) {
            const cardId = `card-${hashCode(b.bookUrl)}`;
            try {
                startDownload(cardId, b.bookUrl, b.sourceUrl, b.sourceName || '');
                started++;
                toast(`📗 已启动: ${b.bookName} (${started}/${needCache.length})`, 'success');
            } catch (ex) {
                toast(`❌ 启动 ${b.bookName} 失败: ${ex.message}`, 'error');
            }
        }
        toast(`✅ 共启动 ${started}/${needCache.length} 本书的缓存`, 'success');
    } catch (e) {
        toast('缓存全部失败: ' + e.message, 'error');
    }
    _cacheAllRunning = false;
}

// 确保卡片上有 cache-status 元素，返回 { card, statusEl }，找不到卡片则返回 null
function ensureCacheStatus(cardId) {
    const card = document.getElementById(cardId);
    if (!card) return null;
    let statusEl = card.querySelector('.cache-status');
    if (!statusEl) {
        const progressEl = card.querySelector('.shelf-progress');
        if (progressEl) {
            statusEl = document.createElement('span');
            statusEl.className = 'cache-status cache-partial';
            progressEl.after(statusEl);
        }
    }
    return statusEl ? { card, statusEl } : null;
}

// 根据下载状态刷新指定卡片的缓存状态文案（不再使用独立进度条）
function syncProgressBarUI(cardId, state) {
    const els = ensureCacheStatus(cardId);
    if (!els) return;
    const { card, statusEl } = els;
    if (state.finished) {
        if (state.error) {
            statusEl.className = 'cache-status cache-partial';
            statusEl.textContent = '❌ 下载失败';
        } else {
            // 下载完成，恢复正常的缓存状态文案
            updateShelfCardCacheByCard(card, state.cached, state.total);
        }
    } else if (state.total > 0) {
        const pct = Math.round(state.done / state.total * 100);
        const failText = state.failed > 0 ? ` ❌${state.failed}` : '';
        statusEl.className = 'cache-status cache-partial';
        statusEl.textContent = `⏳ 下载中 ${state.cached}/${state.total} (${pct}%)${failText}`;
    } else {
        statusEl.className = 'cache-status cache-partial';
        statusEl.textContent = '⏳ 准备中...';
    }
}

// 书架重新渲染后，恢复所有正在下载的进度条
function restoreDownloadProgressBars() {
    for (const [bookUrl, state] of downloadingBooks) {
        syncProgressBarUI(state.cardId, state);
    }
}

// 从菜单触发下载（通过 cardId 找到卡片）
function startDownload(cardId, bookUrl, sourceUrl, sourceName) {
    if (downloadingBooks.has(bookUrl)) {
        toast('该书正在下载中', 'error');
        return;
    }
    const els = ensureCacheStatus(cardId);
    // 即使找不到 DOM 卡片也继续下载，只是不显示进度
    if (els) {
        els.statusEl.className = 'cache-status cache-partial';
        els.statusEl.textContent = '⏳ 准备中...';
    }

    const url = `${API}/api/bookshelf/download?book_url=${encodeURIComponent(bookUrl)}&source_url=${encodeURIComponent(sourceUrl)}&source_name=${encodeURIComponent(sourceName)}`;
    const evtSource = new EventSource(url);
    // 保存下载状态（包含 cardId，方便 DOM 重建后重新绑定）
    const dlState = { evtSource, cardId, done: 0, total: 0, cached: 0, failed: 0, finished: false, error: false };
    downloadingBooks.set(bookUrl, dlState);

    evtSource.addEventListener('download_start', (e) => {
        const d = JSON.parse(e.data);
        dlState.total = d.total;
        syncProgressBarUI(cardId, dlState);
    });

    evtSource.addEventListener('download_progress', (e) => {
        const d = JSON.parse(e.data);
        dlState.done = d.done;
        dlState.total = d.total;
        dlState.cached = d.cached;
        dlState.failed = d.failed;
        syncProgressBarUI(cardId, dlState);
    });

    evtSource.addEventListener('download_done', (e) => {
        const d = JSON.parse(e.data);
        evtSource.close();
        dlState.done = d.total;
        dlState.total = d.total;
        dlState.cached = d.cached;
        dlState.finished = true;
        syncProgressBarUI(cardId, dlState);
        toast(`⬇️ 下载完成: ${d.cached}/${d.total} 章`, 'success');
        // 实时更新菜单中的下载按钮
        const card = document.getElementById(cardId);
        if (card) updateShelfCardMenuAfterDownload(card, d.cached, d.total);
        // 延迟清理状态
        setTimeout(() => { downloadingBooks.delete(bookUrl); }, 2500);
    });

    evtSource.addEventListener('download_error', (e) => {
        if (dlState.finished) return;
        const d = JSON.parse(e.data);
        evtSource.close();
        dlState.finished = true;
        dlState.error = true;
        syncProgressBarUI(cardId, dlState);
        toast(d.error || '下载失败', 'error');
        setTimeout(() => { downloadingBooks.delete(bookUrl); }, 2500);
    });

    evtSource.onerror = () => {
        if (dlState.finished) return;  // download_done 已处理，忽略后续 onerror
        evtSource.close();
        dlState.finished = true;
        dlState.error = true;
        syncProgressBarUI(cardId, dlState);
        toast('下载连接中断', 'error');
        setTimeout(() => { downloadingBooks.delete(bookUrl); }, 2500);
    };
}

// 实时更新书架卡片上的缓存状态文本
function updateShelfCardCacheByCard(card, cached, total) {
    if (!card) return;
    let statusEl = card.querySelector('.cache-status');
    if (!statusEl) {
        const progressEl = card.querySelector('.shelf-progress');
        if (progressEl) {
            statusEl = document.createElement('span');
            progressEl.after(statusEl);
        }
    }
    if (statusEl) {
        if (cached >= total && total > 0) {
            statusEl.className = 'cache-status cache-full';
            statusEl.textContent = `✅ 已缓存全部 ${cached} 章`;
        } else if (cached > 0) {
            statusEl.className = 'cache-status cache-partial';
            statusEl.textContent = `📦 已缓存 ${cached}/${total} 章`;
        }
    }
}

// 下载完成后实时更新菜单中的下载按钮
function updateShelfCardMenuAfterDownload(card, cached, total) {
    if (!card) return;
    const menu = card.querySelector('.shelf-menu');
    if (!menu) return;
    // 找到下载相关的菜单项（包含"下载"或"已是最新"文字的项）
    const menuItems = menu.querySelectorAll('.shelf-menu-item');
    for (const item of menuItems) {
        const text = item.textContent;
        if (text.includes('下载') || text.includes('已是最新')) {
            if (cached >= total && total > 0) {
                item.className = 'shelf-menu-item disabled';
                item.textContent = '✅ 已是最新';
                item.onclick = null;
            }
            break;
        }
    }
}

// ── 清空缓存 ──
async function clearCache(bookUrl, sourceUrl, cardId) {
    if (!confirm('确定要清空该书的所有缓存吗？')) return;
    try {
        const r = await fetch(`${API}/api/bookshelf/clear_cache`, {
            method: 'DELETE', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({ bookUrl, sourceUrl })
        });
        const d = await r.json();
        if (d.ok) {
            toast('🧹 缓存已清空', 'success');
            // 更新卡片上的缓存状态
            const card = document.getElementById(cardId);
            if (card) {
                const statusEl = card.querySelector('.cache-status');
                if (statusEl) statusEl.remove();
            }
        } else {
            toast(d.error || '清空失败', 'error');
        }
    } catch (e) { toast('清空缓存失败: ' + e.message, 'error'); }
}

// ──────────────────────────────────────────────
// D6: 导出 TXT
// ──────────────────────────────────────────────
async function exportBookTxt(bookUrl, sourceUrl) {
    if (!bookUrl) return;
    try {
        toast('📄 正在导出…', 'success');
        const url = `${API}/api/bookshelf/export_txt?book_url=${encodeURIComponent(bookUrl)}&source_url=${encodeURIComponent(sourceUrl || '')}`;
        const r = await fetch(url);
        if (!r.ok) {
            const d = await r.json().catch(() => ({ error: '导出失败' }));
            toast(d.error || '导出失败', 'error');
            return;
        }
        const disposition = r.headers.get('Content-Disposition') || '';
        let filename = 'book.txt';
        const m = disposition.match(/filename\*?=(?:UTF-8'')?([^;]+)/i);
        if (m) {
            try { filename = decodeURIComponent(m[1].trim().replace(/^"|"$/g, '')); } catch {}
        }
        const blob = await r.blob();
        const dlUrl = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = dlUrl;
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(dlUrl);
        toast('📄 已导出 ' + filename, 'success');
    } catch (e) {
        toast('导出失败: ' + e.message, 'error');
    }
}

// ──────────────────────────────────────────────
// D7: 智能自动换源（场景 b：书架主动触发 / 场景 a：由 reader.js 失败时调用）
// ──────────────────────────────────────────────
let _autoSrcEvtSource = null;

function showAutoSource(book) {
    // book 可能来自书架卡片（键为 bookName/bookAuthor）或搜索结果（键为 name/author）
    const bookName = book.bookName || book.name || '';
    const bookAuthor = book.bookAuthor || book.author || '';
    const excludeSourceUrl = book.sourceUrl || '';
    if (!bookName) { toast('缺少书名，无法自动换源', 'error'); return; }

    // 构建 modal
    let modal = document.getElementById('autoSrcModal');
    if (modal) modal.remove();
    modal = document.createElement('div');
    modal.id = 'autoSrcModal';
    modal.className = 'auto-src-modal';
    modal.innerHTML = `
        <div class="auto-src-panel">
            <h3>🧐 自动换源 —《${esc(bookName)}》${bookAuthor ? ' · ' + esc(bookAuthor) : ''}</h3>
            <div class="auto-src-status" id="autoSrcStatus">正在多源搜索，已找到 <b id="autoSrcCount">0</b> 个候选…</div>
            <div id="autoSrcList"></div>
            <div style="text-align:right;margin-top:12px">
                <button class="btn" onclick="closeAutoSource()">关闭</button>
            </div>
        </div>`;
    modal.addEventListener('click', (e) => { if (e.target === modal) closeAutoSource(); });
    document.body.appendChild(modal);

    // 原书信息保留给 renderChangeSourceList 用（直接复用既有预览流程）
    const originalBook = {
        name: bookName,
        author: bookAuthor,
        url: book.bookUrl || book.url,
        sourceName: book.sourceName,
        sourceUrl: book.sourceUrl,
        bookName, bookAuthor,
    };

    const candidates = [];
    const url = `${API}/api/bookshelf/auto_source?book_name=${encodeURIComponent(bookName)}&book_author=${encodeURIComponent(bookAuthor)}&exclude_source_url=${encodeURIComponent(excludeSourceUrl)}`;
    const es = new EventSource(url);
    _autoSrcEvtSource = es;

    es.addEventListener('auto_source_candidate', (e) => {
        const c = JSON.parse(e.data);
        // 过滤：不推荐原书源
        if (c.sourceUrl && c.sourceUrl === excludeSourceUrl) return;
        candidates.push(c);
        // 按 matchScore 降序、latency 升序
        candidates.sort((a, b) => {
            if ((b.matchScore || 0) !== (a.matchScore || 0)) return (b.matchScore || 0) - (a.matchScore || 0);
            const la = typeof a.latency === 'number' ? a.latency : 99999;
            const lb = typeof b.latency === 'number' ? b.latency : 99999;
            return la - lb;
        });
        _renderAutoSrcList(candidates, originalBook);
    });
    es.addEventListener('auto_source_done', (e) => {
        const d = JSON.parse(e.data);
        const statusEl = document.getElementById('autoSrcStatus');
        if (statusEl) {
            statusEl.innerHTML = `✅ 搜索完成，共 <b>${candidates.length}</b> 个可用候选`;
        }
        es.close();
        _autoSrcEvtSource = null;
    });
    es.onerror = () => {
        es.close();
        _autoSrcEvtSource = null;
    };
}

function _renderAutoSrcList(candidates, originalBook) {
    const listEl = document.getElementById('autoSrcList');
    const countEl = document.getElementById('autoSrcCount');
    if (countEl) countEl.textContent = candidates.length;
    if (!listEl) return;
    listEl.innerHTML = candidates.map((c, i) => {
        const latency = typeof c.latency === 'number' ? `${c.latency}ms` : '';
        // 把 c 包装成 previewChangeSource 要的结构
        const payload = {
            name: c.name, author: c.author, url: c.bookUrl,
            sourceName: c.sourceName, sourceUrl: c.sourceUrl, sourceIndex: c.sourceIndex,
        };
        const cData = esc2(JSON.stringify(payload));
        const origData = esc2(JSON.stringify(originalBook));
        return `<div class="auto-src-item" onclick='closeAutoSource();previewChangeSource(${cData}, ${origData})'>
            <span class="auto-src-score">${c.matchScore || 0}</span>
            <div class="auto-src-main">
                <div class="auto-src-title">📖 ${esc(c.sourceName || '未知书源')}</div>
                <div class="auto-src-meta">${esc(c.name)} · ${esc(c.author || '未知作者')}</div>
            </div>
            ${latency ? `<span class="auto-src-latency">${latency}</span>` : ''}
        </div>`;
    }).join('');
}

function closeAutoSource() {
    if (_autoSrcEvtSource) { _autoSrcEvtSource.close(); _autoSrcEvtSource = null; }
    const modal = document.getElementById('autoSrcModal');
    if (modal) modal.remove();
}

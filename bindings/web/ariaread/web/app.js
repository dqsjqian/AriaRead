/**
 * app.js — AriaRead 核心入口
 * 全局变量、工具函数、初始化、Tab 切换
 *
 * 模块加载顺序（index.html 中）：
 *   app.js → sources.js → search.js → reader.js → bookshelf.js
 */

const API = '';

// ── 书源状态 ──
let sourcesData = [];
let totalSourceCount = 0;
let sourceValidity = {};
let sourceLatency = {};
let isValidating = false;
let validateEvtSource = null;

// ── 搜索状态 ──
let allBooks = [];
let bookCardNodeMap = new Map();
let isSearching = false;
let searchEvtSource = null;
let lastSearchQuery = '';
let lastSearchFilters = { matchName: true, matchAuthor: false, matchIntro: false };

// ── 阅读状态（兼容旧引用，由 reader.js 的 syncActiveReadingSessionGlobals 维护） ──
let currentBook = null;
let currentChapters = [];
let currentChapterIndex = -1;
let cachedCatalogHtml = '';
let cachedContentHtml = '';
let currentActiveTab = 'bookshelf';
let readingSessions = new Map();
let activeReadingSessionKey = '';

// ──────────────────────────────────────────────
// 工具函数
// ──────────────────────────────────────────────

function stripHtml(str) {
    return String(str || '').replace(/<[^>]+>/g, '');
}

function esc(str) {
    if (!str) return '';
    const d = document.createElement('div');
    d.textContent = stripHtml(str);
    return d.innerHTML;
}

function esc2(str) {
    // HTML-escape + 换行符转义：防止含 \n 的 sourceUrl 在 onclick 属性中断裂
    return String(str || '')
        .replace(/&/g,'&amp;')
        .replace(/'/g,'&#39;')
        .replace(/"/g,'&quot;')
        .replace(/</g,'&lt;')
        .replace(/>/g,'&gt;')
        .replace(/\n/g,'&#10;')
        .replace(/\r/g,'&#13;');
}

function toast(msg, type = 'success') {
    const t = document.createElement('div');
    t.className = `toast ${type}`;
    t.textContent = msg;
    document.body.appendChild(t);
    setTimeout(() => t.remove(), 3000);
}

// ──────────────────────────────────────────────
// 用户偏好持久化（localStorage）
// ──────────────────────────────────────────────

function savePrefs() {
    try {
        localStorage.setItem('ariaread_prefs', JSON.stringify({
            matchName: document.getElementById('matchName')?.checked ?? true,
            matchAuthor: document.getElementById('matchAuthor')?.checked ?? false,
            matchIntro: document.getElementById('matchIntro')?.checked ?? false,
            selectedSources: Array.from(selectedSourceNames).filter(n => n !== '__NONE__'),
        }));
    } catch(e) {}
}

function loadPrefs() {
    try {
        const raw = localStorage.getItem('ariaread_prefs');
        if (!raw) return;
        const prefs = JSON.parse(raw);
        if (typeof prefs.matchName === 'boolean') {
            const el = document.getElementById('matchName');
            if (el) el.checked = prefs.matchName;
        }
        if (typeof prefs.matchAuthor === 'boolean') {
            const el = document.getElementById('matchAuthor');
            if (el) el.checked = prefs.matchAuthor;
        }
        if (typeof prefs.matchIntro === 'boolean') {
            const el = document.getElementById('matchIntro');
            if (el) el.checked = prefs.matchIntro;
        }
        if (Array.isArray(prefs.selectedSources)) {
            selectedSourceNames = new Set(prefs.selectedSources);
            updateSourcePickerBtn();
        }
    } catch(e) {}
}

// ──────────────────────────────────────────────
// 关闭服务
// ──────────────────────────────────────────────

async function shutdownServer() {
    if (!confirm('确定要关闭 AriaRead 服务吗？')) return;
    try {
        document.getElementById('statusText').textContent = '正在关闭...';
        document.getElementById('statusDot').style.background = 'var(--yellow, orange)';
        document.getElementById('shutdownBtn').disabled = true;
        await fetch(`${API}/api/shutdown`, { method: 'POST' });
    } catch {
        // 服务已关闭，连接断开是正常的
    }
    document.getElementById('statusText').textContent = '服务已关闭';
    document.getElementById('statusDot').style.background = 'var(--red)';
}

// ──────────────────────────────────────────────
// Tab 切换
// ──────────────────────────────────────────────

function switchTab(name) {
    if (typeof invalidateRssNavigation === 'function') invalidateRssNavigation();
    if (name !== 'debug' && typeof stopSourceDebug === 'function') stopSourceDebug();
    currentActiveTab = name;
    // E6: 记住最后浏览的 Tab，刷新页面不回默认
    try { localStorage.setItem('ariaread_last_tab', name); } catch {}
    document.querySelectorAll('.tab').forEach(t => t.classList.toggle('active', t.dataset.tab === name));

    if (name === 'bookshelf') { renderBookshelf(); return; }

    if (name === 'search') {
        const el = document.getElementById('results');
        if (Array.isArray(allBooks) && allBooks.length > 0) {
            el.innerHTML = `
                <div class="section-title">搜索结果 <span class="badge" id="resultCount">${allBooks.length}</span></div>
                <div class="book-grid" id="bookGrid"></div>`;
            renderBooks(allBooks, lastSearchQuery, lastSearchFilters || getSearchFilters());
        } else {
            el.innerHTML = `
                <div class="empty">
                    <div class="icon">🔍</div>
                    <h3>开始搜索</h3>
                    <p>在上方输入书名，点击搜索</p>
                </div>`;
        }
        return;
    }

    syncActiveReadingSessionGlobals();

    if (name === 'reader') {
        // 如果当前没有任何活跃会话，尝试从 localStorage 还原"上次在读的书"
        const hasAnySession = typeof readingSessions !== 'undefined' && readingSessions.size > 0;
        if (!hasAnySession &&
            typeof _loadPersistedActiveSession === 'function' &&
            _loadPersistedActiveSession()) {
            // 先显示还原中的 loading 占位，避免空白闪烁
            const el = document.getElementById('results');
            if (el) {
                el.innerHTML = `<div class="loading">
                    <div class="spinner"></div>
                    <div class="loading-text">正在恢复上次的阅读进度…</div>
                </div>`;
            }
            // 异步还原（内部会自动调 fetchCatalog → 触发 renderReaderWorkspace）
            restoreActiveSessionFromStorage().then((ok) => {
                if (!ok && currentActiveTab === 'reader') renderReaderWorkspace();
            });
            return;
        }
        renderReaderWorkspace();
        return;
    }

    if (name === 'console') {
        document.getElementById('results').innerHTML = `
            <div class="section-title">⚡ JavaScript 控制台</div>
            <p style="color:var(--text2)">独立 JS 环境，默认限时 1 秒、内存 32 MiB；不提供网络或文件访问。</p>
            <textarea class="console-input" id="jsInput" placeholder="// 输入 JS 代码"></textarea>
            <button class="btn btn-primary" id="jsRunBtn" style="margin-top:8px" onclick="evalJs()">执行</button>
            <div class="console-output" id="jsOutput">// 输出</div>`;
    }

    if (name === 'rss') {
        if (typeof renderRssPanel === 'function') renderRssPanel();
        return;
    }

    if (name === 'debug') {
        if (typeof renderDebugPanel === 'function') renderDebugPanel();
        return;
    }
}

// ──────────────────────────────────────────────
// JS 控制台
// ──────────────────────────────────────────────

async function evalJs() {
    const code = document.getElementById('jsInput')?.value || '';
    const el = document.getElementById('jsOutput');
    const button = document.getElementById('jsRunBtn');
    if (!el || button?.disabled) return;
    if (!code.trim()) { el.textContent = '请输入 JS 代码'; return; }
    if (button) button.disabled = true;
    el.textContent = '执行中...';
    try {
        const r = await fetch(`${API}/api/eval`, {
            method: 'POST', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({code})
        });
        const d = await r.json();
        const lines = Array.isArray(d.logs) ? [...d.logs] : [];
        if (d.logsTruncated) lines.push('[日志已截断]');
        lines.push(d.error ? `错误：${d.error}` : (d.result ?? '(undefined)'));
        if (d.errorTruncated) lines.push('[错误已截断]');
        if (d.resultTruncated) lines.push('[结果已截断]');
        if (typeof d.elapsedMs === 'number') lines.push(`耗时：${d.elapsedMs}ms`);
        el.textContent = lines.join('\n');
    } catch (e) { el.textContent = 'Error: ' + e.message; }
    finally { if (button) button.disabled = false; }
}

// ──────────────────────────────────────────────
// 初始化
// ──────────────────────────────────────────────

function setSourceSidebarCollapsed(collapsed) {
    const sidebar = document.getElementById('sourceSidebar');
    const toggle = document.getElementById('sidebarToggle');
    if (!sidebar || !toggle) return;
    sidebar.hidden = collapsed;
    toggle.textContent = collapsed ? '▶ 展开书源' : '◀ 收起书源';
    toggle.setAttribute('aria-expanded', String(!collapsed));
    toggle.title = collapsed ? '展开书源列表' : '收起书源列表';
    try { localStorage.setItem('ariaread_sidebar_collapsed', String(collapsed)); } catch {}
}

function toggleSourceSidebar() {
    const sidebar = document.getElementById('sourceSidebar');
    if (sidebar) setSourceSidebarCollapsed(!sidebar.hidden);
}

async function init() {
    let collapsed = false;
    try { collapsed = localStorage.getItem('ariaread_sidebar_collapsed') === 'true'; } catch {}
    setSourceSidebarCollapsed(collapsed);
    loadPrefs();
    try {
        const r = await fetch(`${API}/api/health`);
        const d = await r.json();
        document.getElementById('statusText').textContent = `v${d.version} · 就绪`;
        document.getElementById('statusDot').style.background = 'var(--green)';
        refreshSources();
        // 等书架缓存先就绪，这样 switchTab('reader') 还原活跃会话时
        // isInShelf 的存活性校验结果才准确（否则 shelfBookUrls 还是空 Set）
        await refreshShelfCache();
        // E6: 优先恢复上次 Tab（缺省 bookshelf）
        let lastTab = 'bookshelf';
        try {
            const saved = localStorage.getItem('ariaread_last_tab');
            if (saved && ['bookshelf', 'search', 'reader', 'rss', 'debug', 'console'].includes(saved)) {
                lastTab = saved;
            }
        } catch {}
        switchTab(lastTab);
        pollShelfCheckStatus();
    } catch {
        document.getElementById('statusText').textContent = '连接失败';
        document.getElementById('statusDot').style.background = 'var(--red)';
    }
}

function pollShelfCheckStatus() {
    let pollTimer = setInterval(async () => {
        try {
            const r = await fetch(`${API}/api/bookshelf/check_status`);
            const d = await r.json();
            if (d.done) {
                clearInterval(pollTimer);
                if (d.updated > 0) {
                    toast(`📡 检测到 ${d.updated} 本书有更新`, 'success');
                    const activeTab = document.querySelector('.tab.active');
                    if (activeTab && activeTab.dataset.tab === 'bookshelf') {
                        renderBookshelf();
                    }
                    refreshShelfCache();
                }
            }
        } catch {
            clearInterval(pollTimer);
        }
    }, 3000);
}

// 启动
init();

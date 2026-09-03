/**
 * rss.js — D8: RSS 订阅源管理
 * 依赖：app.js 中的全局变量（API, toast, esc, esc2）
 */

// ── RSS 状态 ──
let rssSourcesData = [];
let rssArticlesData = [];
let rssCurrentView = 'sources';   // sources | articles | reader
let rssCurrentSource = null;
let rssCurrentArticle = null;
let rssIsFetching = false;

// ──────────────────────────────────────────────
// 主面板渲染
// ──────────────────────────────────────────────

function renderRssPanel() {
    const el = document.getElementById('results');
    if (!el) return;

    if (rssCurrentView === 'sources') {
        renderRssSourcesView(el);
    } else if (rssCurrentView === 'articles') {
        renderRssArticlesView(el);
    } else if (rssCurrentView === 'reader') {
        renderRssReaderView(el);
    }
}

// ──────────────────────────────────────────────
// RSS 源列表视图
// ──────────────────────────────────────────────

function renderRssSourcesView(container) {
    container.innerHTML = `
        <div class="section-title" style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">
            <span>📡 RSS 订阅源</span>
            <button class="btn" id="rssCheckBtn" onclick="checkRssSources()" ${rssIsFetching ? 'disabled' : ''}>🔍 检测</button>
            <div class="rss-menu-wrap" style="position:relative">
                <button class="btn" onclick="toggleRssMenu(event)">⚙️ 更多 ▾</button>
                <div id="rssMenu" class="rss-menu" style="display:none;position:absolute;top:110%;left:0;z-index:50;min-width:160px;background:var(--surface);border:1px solid var(--border);border-radius:8px;box-shadow:0 6px 24px rgba(0,0,0,.18);padding:6px;overflow:hidden">
                    <button class="rss-menu-item" onclick="closeRssMenu();showRssAddModal()">➕ 添加源</button>
                    <button class="rss-menu-item" onclick="closeRssMenu();showRssImportModal()">📥 导入</button>
                    <button class="rss-menu-item" onclick="closeRssMenu();exportRssSources()">📤 导出</button>
                    <button class="rss-menu-item" onclick="closeRssMenu();clearInvalidRssSources()">🧹 清空无效</button>
                    <button class="rss-menu-item rss-menu-danger" onclick="closeRssMenu();clearAllRssSources()">🗑️ 清空列表</button>
                </div>
            </div>
            <div id="rssCheckProgress" style="display:none;flex:1;min-width:120px;align-items:center;gap:8px">
                <div class="spinner" style="width:12px;height:12px;border:2px solid var(--border);border-top-color:var(--orange);border-radius:50%;animation:spin 0.8s linear infinite;flex-shrink:0"></div>
                <span id="rssCheckNum" style="font-size:12px;color:var(--orange);font-weight:600;white-space:nowrap">0/0</span>
                <div class="progress-bar" style="height:4px;flex:1;min-width:40px"><div id="rssCheckFill" class="fill" style="width:0%"></div></div>
                <span id="rssCheckStats" style="font-size:11px;color:var(--text2);white-space:nowrap"></span>
            </div>
            <span id="rssSourceCount" style="margin-left:auto;font-size:12px;color:var(--text2)">共 ${rssSourcesData.length} 个源</span>
        </div>
        <div class="rss-source-list" id="rssSourceList">
            <div class="loading"><div class="spinner"></div><div class="loading-text">加载中...</div></div>
        </div>
    `;
    refreshRssSources();
}

// ── 设置菜单开关 ──
function toggleRssMenu(ev) {
    if (ev) ev.stopPropagation();
    const m = document.getElementById('rssMenu');
    if (!m) return;
    const show = m.style.display === 'none';
    m.style.display = show ? 'block' : 'none';
    if (show) {
        setTimeout(() => document.addEventListener('click', closeRssMenuOnce, { once: true }), 0);
    }
}
function closeRssMenu() {
    const m = document.getElementById('rssMenu');
    if (m) m.style.display = 'none';
}
function closeRssMenuOnce() { closeRssMenu(); }

// 检测状态辅助
function rssCheckDone() {
    rssIsFetching = false;
    const btn = document.getElementById('rssCheckBtn');
    if (btn) btn.disabled = false;
}

async function refreshRssSources() {
    try {
        const r = await fetch(`${API}/api/rss/sources`);
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }
        rssSourcesData = d.sources || [];
        renderRssSourceList();
    } catch (e) {
        console.error('refreshRssSources:', e);
        document.getElementById('rssSourceList').innerHTML = `
            <div class="empty"><div class="icon">📡</div><h3>加载失败</h3><p>${esc(e.message)}</p></div>`;
    }
}

function renderRssSourceList() {
    const el = document.getElementById('rssSourceList');
    const countEl = document.getElementById('rssSourceCount');
    if (countEl) countEl.textContent = `共 ${rssSourcesData.length} 个源`;
    if (!el) return;
    if (!rssSourcesData.length) {
        el.innerHTML = `
            <div class="empty">
                <div class="icon">📡</div>
                <h3>暂无 RSS 订阅源</h3>
                <p>点击「添加源」输入 RSS 地址</p>
            </div>`;
        return;
    }

    // 按状态分组
    const groups = {
        excellent: { label: '优质', color: '#16a34a', bg: '#eaf3de', items: [] },
        good:      { label: '良好', color: '#2563eb', bg: '#e6f1fb', items: [] },
        unknown:   { label: '待检测', color: '#6b7280', bg: '#f3f4f6', items: [] },
        poor:      { label: '差', color: '#d97706', bg: '#fef3c7', items: [] },
        invalid:   { label: '无效', color: '#dc2626', bg: '#fee2e2', items: [] },
    };

    rssSourcesData.forEach(s => {
        const v = s.validity || 'unknown';
        if (groups[v]) groups[v].items.push(s);
        else groups.unknown.items.push(s);
    });

    // 组内按延迟升序排列（无延迟的排最后）
    for (const group of Object.values(groups)) {
        group.items.sort((a, b) => {
            const la = (a.latencyMs != null && a.latencyMs >= 0) ? a.latencyMs : Infinity;
            const lb = (b.latencyMs != null && b.latencyMs >= 0) ? b.latencyMs : Infinity;
            return la - lb;
        });
    }

    // 计算分组平均延迟
    function avgLatency(items) {
        const valid = items.filter(s => s.latencyMs != null && s.latencyMs >= 0);
        if (!valid.length) return null;
        return Math.round(valid.reduce((sum, s) => sum + s.latencyMs, 0) / valid.length);
    }

    let html = '';
    for (const [key, group] of Object.entries(groups)) {
        if (!group.items.length) continue;
        const avg = avgLatency(group.items);
        const avgText = avg != null ? `avg ${avg}ms` : '';

        html += `<div class="rss-group">
            <div class="rss-group-header" style="background:${group.bg};border-left:3px solid ${group.color}">
                <span class="rss-group-label" style="color:${group.color}">${group.label} (${group.items.length})</span>
                ${avgText ? `<span class="rss-group-avg" style="color:${group.color}">${avgText}</span>` : ''}
            </div>
            <div class="rss-group-grid">`;

        for (const s of group.items) {
            const lastUpdate = s.lastUpdateTime
                ? new Date(s.lastUpdateTime * 1000).toLocaleString('zh-CN')
                : '从未更新';
            const latencyText = (s.latencyMs != null && s.latencyMs >= 0) ? `${s.latencyMs}ms` : '';

            html += `<div class="rss-card" data-url="${esc2(s.sourceUrl)}" data-name="${esc2(s.sourceName)}">
                <div class="rss-card-top">
                    <span class="rss-card-name">${esc(s.sourceName)}</span>
                    <span class="rss-card-dot" style="background:${group.color}"></span>
                </div>
                <div class="rss-card-meta">${esc(s.sourceGroup || '')}</div>
                <div class="rss-card-bottom">
                    <span class="rss-card-time">${lastUpdate}</span>
                    <span class="rss-card-latency" style="color:${group.color}">${latencyText}</span>
                    <div class="rss-card-actions">
                        <button class="btn-icon rss-btn-refresh" title="刷新">↻</button>
                        <button class="btn-icon btn-icon-danger rss-btn-delete" title="删除">✕</button>
                    </div>
                </div>
            </div>`;
        }

        html += `</div></div>`;
    }

    el.innerHTML = html;

    // 事件委托：避免 inline onclick 在含特殊字符的 URL 上失效
    el.addEventListener('click', e => {
        const card = e.target.closest('.rss-card');
        if (!card) return;
        const url = card.dataset.url;
        const name = card.dataset.name;
        if (e.target.closest('.rss-btn-delete')) {
            deleteRssSource(url, name);
        } else if (e.target.closest('.rss-btn-refresh')) {
            fetchRssSource(url);
        } else {
            openRssArticles(url);
        }
    });
}

// 评级徽章：优(绿)/良(蓝)/差(橙)/无效(红)/待检测(灰)
function rssValidityBadge(validity, latencyMs) {
    const map = {
        excellent: ['优', '#16a34a'],
        good:      ['良', '#2563eb'],
        poor:      ['差', '#d97706'],
        invalid:   ['无效', '#dc2626'],
        unknown:   ['待检测', '#9ca3af'],
    };
    const v = map[validity] || map.unknown;
    const ms = (latencyMs != null && latencyMs >= 0) ? ` ${latencyMs}ms` : '';
    return `<span class="rss-badge" style="display:inline-block;font-size:11px;padding:1px 6px;border-radius:6px;color:#fff;background:${v[1]};margin-right:6px;vertical-align:1px">${v[0]}${ms}</span>`;
}

// ──────────────────────────────────────────────
// 文章列表视图
// ──────────────────────────────────────────────

async function openRssArticles(sourceUrl) {
    rssCurrentSource = rssSourcesData.find(s => s.sourceUrl === sourceUrl) || null;
    if (!rssCurrentSource) return;
    rssCurrentView = 'articles';
    renderRssPanel();
    // 如果该源从未更新过（lastUpdateTime 为 0 或不存在），自动抓取
    if (!rssCurrentSource.lastUpdateTime || rssCurrentSource.lastUpdateTime === 0) {
        rssIsFetching = false;
        await fetchRssSource(sourceUrl);
    }
}

function renderRssArticlesView(container) {
    const source = rssCurrentSource;
    container.innerHTML = `
        <div class="section-title" style="display:flex;align-items:center;gap:12px">
            <button class="btn" onclick="rssCurrentView='sources';renderRssPanel()">← 返回</button>
            <span>${esc(source.sourceName)}</span>
            <button class="btn" onclick="fetchRssSource('${esc2(source.sourceUrl)}')">🔄 刷新</button>
            <button class="btn" style="color:var(--red,#dc2626)" onclick="deleteAndBack('${esc2(source.sourceUrl)}','${esc2(source.sourceName)}')">🗑️ 删除</button>
            <span style="margin-left:auto;font-size:12px;color:var(--text2)">
                <a href="${esc2(source.sourceUrl)}" target="_blank" style="color:var(--text2)">源地址 ↗</a>
            </span>
        </div>
        <div class="rss-article-list" id="rssArticleList">
            <div class="loading"><div class="spinner"></div><div class="loading-text">加载内容...</div></div>
        </div>
    `;
    loadRssArticles(source.sourceUrl);
}

async function loadRssArticles(sourceUrl, page = 1) {
    try {
        const r = await fetch(`${API}/api/rss/articles?source_url=${encodeURIComponent(sourceUrl)}&page=${page}&page_size=50`);
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }
        rssArticlesData = d.articles || [];
        renderRssArticleList(d.total || 0, page);
    } catch (e) {
        console.error('loadRssArticles:', e);
        document.getElementById('rssArticleList').innerHTML = `
            <div class="empty"><div class="icon">📄</div><h3>加载失败</h3><p>${esc(e.message)}</p></div>`;
    }
}

function renderRssArticleList(total, page) {
    const el = document.getElementById('rssArticleList');
    if (!el) return;
    if (!rssArticlesData.length) {
        el.innerHTML = `
            <div class="empty">
                <div class="icon">📄</div>
                <h3>暂无内容</h3>
                <p>点击「刷新」抓取最新内容</p>
            </div>`;
        return;
    }

    el.innerHTML = rssArticlesData.map(a => {
        const dateStr = a.pubDate
            ? new Date(a.pubDate * 1000).toLocaleString('zh-CN')
            : '';
        const hasImage = a.image ? `<div class="rss-article-thumb"><img src="${esc2(a.image)}" loading="lazy" onerror="this.style.display='none'"></div>` : '';
        return `<div class="rss-article-card" onclick="openRssArticle(${a.id})">
            ${hasImage}
            <div class="rss-article-info">
                <div class="rss-article-title">${esc(a.title)}</div>
                <div class="rss-article-meta">${dateStr}</div>
                <div class="rss-article-desc">${esc(stripHtml(a.description || '').substring(0, 200))}</div>
            </div>
        </div>`;
    }).join('');
}

// ──────────────────────────────────────────────
// 文章阅读视图
// ──────────────────────────────────────────────

async function openRssArticle(articleId) {
    try {
        const r = await fetch(`${API}/api/rss/article?id=${articleId}`);
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }
        rssCurrentArticle = d;
        rssCurrentView = 'reader';
        renderRssPanel();
    } catch (e) {
        toast('加载内容失败: ' + e.message, 'error');
    }
}

function renderRssReaderView(container) {
    const art = rssCurrentArticle;
    if (!art) {
        container.innerHTML = `<div class="empty"><div class="icon">📄</div><h3>内容不存在</h3></div>`;
        return;
    }

    const dateStr = art.pubDate
        ? new Date(art.pubDate * 1000).toLocaleString('zh-CN')
        : '';
    const content = art.content || art.description || '';

    // 判断正文是否为「完整 HTML 页面」（含 script/style/head/meta/html 等页面级标签）。
    // legado 复杂图片源（如壁纸喵）的 ruleContent 返回一整套带 ViewerJS + 瀑布流脚本的
    // HTML 页面，若直接 innerHTML 注入主文档，其全局 <style>/<script> 会污染并破坏整个
    // App 布局。对齐 legado 的 WebView 隔离，这类内容用 <iframe srcdoc> 沙箱渲染。
    const isFullPage = /<\s*(script|style|head|meta|title|link|!doctype|html|body)\b/i.test(content);

    // 检测是否为 SPA 应用（内容太短，只有一个空壳 div）
    // 这种情况下，服务器端抓取的 HTML 无法渲染，需要直接加载原网页
    const isSpaShell = content.length < 500 && /<div\s+id=["'](?:app|root|__next)["']\s*><\/div>/i.test(content);

    const imageHeader = art.image
        ? `<img src="${esc2(art.image)}" style="max-width:100%;border-radius:8px;margin-bottom:16px" onerror="this.style.display='none'">`
        : '';

    // 仅当存在真实的、http(s) 外链时才显示「原文」按钮：
    // 空 link 会让 <a href=""> 解析成当前页（localhost）；
    // 占位 link（源URL#item-N）非真实原文，也不应显示。
    const hasOriginal = art.link
        && /^https?:\/\//i.test(art.link)
        && art.link.indexOf('#item-') === -1;
    const originalBtn = hasOriginal
        ? `<a class="btn" href="${esc2(art.link)}" target="_blank" rel="noopener">原文 ↗</a>`
        : '';

    let bodyHtml;
    if (hasOriginal && (isSpaShell || content.length < 200)) {
        // SPA 应用或内容太短：直接用 iframe 加载原网页 URL（对齐 legado WebView 行为）
        // 这样可以让浏览器执行 JavaScript，渲染出完整内容
        bodyHtml = `<iframe class="rss-reader-frame" src="${esc2(art.link)}"
            sandbox="allow-scripts allow-same-origin allow-popups"
            style="width:100%;height:calc(100vh - 160px);border:0;border-radius:8px;background:#fff"></iframe>`;
    } else if (isFullPage) {
        // 沙箱渲染：iframe 与主文档完全隔离（独立 DOM/CSS/JS），允许脚本执行以支持
        // 瀑布流懒加载、图片查看器等。高度自适应，铺满阅读区。
        bodyHtml = `<iframe class="rss-reader-frame" srcdoc="${esc2(content)}"
            sandbox="allow-scripts allow-same-origin allow-popups"
            style="width:100%;height:calc(100vh - 160px);border:0;border-radius:8px;background:#fff"></iframe>`;
    } else {
        bodyHtml = `<div class="rss-reader-body">${content}</div>`;
    }

    container.innerHTML = `
        <div class="rss-reader-header">
            <button class="btn" onclick="rssCurrentView='articles';renderRssPanel()">← 返回列表</button>
            ${originalBtn}
        </div>
        <div class="rss-reader-content">
            <h1 class="rss-reader-title">${esc(art.title)}</h1>
            <div class="rss-reader-meta">${dateStr}</div>
            ${isFullPage ? '' : imageHeader}
            ${bodyHtml}
        </div>
    `;
}

// ──────────────────────────────────────────────
// 添加 RSS 源 Modal
// ──────────────────────────────────────────────

function showRssAddModal() {
    const modalId = 'rssAddModal';
    let modal = document.getElementById(modalId);
    if (!modal) {
        modal = document.createElement('div');
        modal.id = modalId;
        modal.className = 'modal-overlay';
        modal.style.display = 'none';
        modal.innerHTML = `
            <div class="modal">
                <h3>📡 添加 RSS 订阅源</h3>
                <div style="margin-bottom:8px;color:var(--text2);font-size:13px">
                    支持标准 RSS / Atom 源，如：<br>
                    https://rsshub.app/bilibili/user/video/xxxx<br>
                    https://www.v2ex.com/index.xml
                </div>
                <input type="text" id="rssAddUrl" placeholder="RSS 源地址">
                <input type="text" id="rssAddName" placeholder="名称（可选，留空自动使用地址）">
                <input type="text" id="rssAddGroup" placeholder="分组（可选）">
                <div class="modal-actions">
                    <button class="btn" onclick="document.getElementById('rssAddModal').style.display='none'">取消</button>
                    <button class="btn btn-primary" onclick="submitRssAdd()">添加</button>
                </div>
            </div>
        `;
        document.body.appendChild(modal);
    }
    document.getElementById('rssAddUrl').value = '';
    document.getElementById('rssAddName').value = '';
    document.getElementById('rssAddGroup').value = '';
    modal.style.display = 'flex';
}

async function submitRssAdd() {
    const url = document.getElementById('rssAddUrl').value.trim();
    const name = document.getElementById('rssAddName').value.trim();
    const group = document.getElementById('rssAddGroup').value.trim();
    if (!url) { toast('请输入 RSS 源地址', 'error'); return; }

    document.getElementById('rssAddModal').style.display = 'none';
    toast('⬇️ 正在添加并抓取...', 'success');

    try {
        // 1. 添加源
        const r1 = await fetch(`${API}/api/rss/sources`, {
            method: 'POST', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({sourceUrl: url, sourceName: name || url, sourceGroup: group}),
        });
        const d1 = await r1.json();
        if (d1.error) { toast(d1.error, 'error'); return; }

        // 2. 立即抓取
        const r2 = await fetch(`${API}/api/rss/fetch`, {
            method: 'POST', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({sourceUrl: url}),
        });
        const d2 = await r2.json();
        if (d2.error) {
            toast('源已添加，但抓取失败: ' + d2.error, 'error');
        } else {
            toast(`✅ 添加成功，抓到 ${d2.count || 0} 篇内容`, 'success');
        }

        await refreshRssSources();
    } catch (e) {
        toast('添加失败: ' + e.message, 'error');
    }
}

// ──────────────────────────────────────────────
// 导入 RSS 源
// ──────────────────────────────────────────────

function showRssImportModal() {
    const modalId = 'rssImportModal';
    let modal = document.getElementById(modalId);
    if (!modal) {
        modal = document.createElement('div');
        modal.id = modalId;
        modal.className = 'modal-overlay';
        modal.style.display = 'none';
        modal.innerHTML = `
            <div class="modal" style="max-width:520px">
                <h3>📥 导入 RSS 订阅源</h3>
                <div style="margin-bottom:12px;color:var(--text2);font-size:13px">
                    支持标准 JSON 文件 / URL / 粘贴文本<br>
                    可导入单条 JSON 对象或 JSON 数组
                </div>
                <div style="display:flex;gap:8px;margin-bottom:12px">
                    <button class="btn btn-small" id="rssImportTabFile" onclick="switchRssImportTab('file')">📁 文件</button>
                    <button class="btn btn-small" id="rssImportTabUrl" onclick="switchRssImportTab('url')">🌐 URL</button>
                    <button class="btn btn-small" id="rssImportTabText" onclick="switchRssImportTab('text')">📋 粘贴</button>
                </div>
                <div id="rssImportPanelFile">
                    <input type="file" id="rssImportFileInput" accept=".json" style="display:none" onchange="onRssFileSelected(this)">
                    <div style="display:flex;gap:8px;align-items:center;margin-bottom:8px">
                        <button class="btn btn-small" onclick="document.getElementById('rssImportFileInput').click()">📁 选择文件</button>
                        <span id="rssImportFileName" style="font-size:13px;color:var(--text2)">未选择文件</span>
                    </div>
                    <input type="hidden" id="rssImportFileContent">
                </div>
                <div id="rssImportPanelUrl" style="display:none">
                    <input type="text" id="rssImportUrl" placeholder="JSON 文件 URL">
                </div>
                <div id="rssImportPanelText" style="display:none">
                    <textarea id="rssImportText" rows="6" placeholder="粘贴订阅源 JSON..."></textarea>
                </div>
                <div class="modal-actions">
                    <button class="btn" onclick="document.getElementById('rssImportModal').style.display='none'">取消</button>
                    <button class="btn btn-primary" onclick="submitRssImport()">导入</button>
                </div>
            </div>
        `;
        document.body.appendChild(modal);
    }
    const fileInput = document.getElementById('rssImportFileInput');
    if (fileInput) fileInput.value = '';
    const fileName = document.getElementById('rssImportFileName');
    if (fileName) fileName.textContent = '未选择文件';
    const fileContent = document.getElementById('rssImportFileContent');
    if (fileContent) fileContent.value = '';
    document.getElementById('rssImportUrl').value = '';
    document.getElementById('rssImportText').value = '';
    switchRssImportTab('file');
    modal.style.display = 'flex';
}

function switchRssImportTab(tab) {
    ['file','url','text'].forEach(t => {
        const panel = document.getElementById('rssImportPanel' + t.charAt(0).toUpperCase() + t.slice(1));
        const btn = document.getElementById('rssImportTab' + t.charAt(0).toUpperCase() + t.slice(1));
        if (panel) panel.style.display = t === tab ? 'block' : 'none';
        if (btn) btn.style.opacity = t === tab ? '1' : '0.5';
    });
    rssImportCurrentTab = tab;
}

let rssImportCurrentTab = 'file';

function onRssFileSelected(input) {
    const file = input.files[0];
    if (!file) return;
    document.getElementById('rssImportFileName').textContent = file.name;
    const reader = new FileReader();
    reader.onload = function(e) {
        document.getElementById('rssImportFileContent').value = e.target.result;
    };
    reader.readAsText(file);
}

async function submitRssImport() {
    const modal = document.getElementById('rssImportModal');
    let payload = null;
    let endpoint = '';

    if (rssImportCurrentTab === 'file') {
        const content = document.getElementById('rssImportFileContent').value.trim();
        if (!content) { toast('请先选择 JSON 文件', 'error'); return; }
        payload = {json: content};
        endpoint = '/api/rss/import/json';
    } else if (rssImportCurrentTab === 'url') {
        const url = document.getElementById('rssImportUrl').value.trim();
        if (!url) { toast('请输入 URL', 'error'); return; }
        payload = {url};
        endpoint = '/api/rss/import/url';
    } else {
        const text = document.getElementById('rssImportText').value.trim();
        if (!text) { toast('请输入 JSON 内容', 'error'); return; }
        payload = {json: text};
        endpoint = '/api/rss/import/json';
    }

    modal.style.display = 'none';
    toast('📥 正在导入...', 'success');

    try {
        const r = await fetch(`${API}${endpoint}`, {
            method: 'POST', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify(payload),
        });
        const d = await r.json();
        if (d.error) {
            toast('导入失败: ' + d.error, 'error');
        } else {
            toast(`✅ 成功导入 ${d.count || 0} 个 RSS 源`, 'success');
            await refreshRssSources();
        }
    } catch (e) {
        toast('导入失败: ' + e.message, 'error');
    }
}

// ──────────────────────────────────────────────
// 导出 RSS 源
// ──────────────────────────────────────────────

function exportRssSources() {
    if (!rssSourcesData.length) { toast('没有可导出的源', 'info'); return; }
    const json = JSON.stringify(rssSourcesData, null, 2);
    const blob = new Blob([json], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `openread-rss-sources-${new Date().toISOString().slice(0,10)}.json`;
    a.click();
    URL.revokeObjectURL(url);
    toast(`📤 已导出 ${rssSourcesData.length} 个源`, 'success');
}

// ──────────────────────────────────────────────
// 删除 / 刷新
// ──────────────────────────────────────────────

async function deleteRssSource(url, name) {
    try {
        const r = await fetch(`${API}/api/rss/sources`, {
            method: 'DELETE', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({sourceUrl: url}),
        });
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }
        toast(`🗑️ 已删除「${name}」`, 'success');
        await refreshRssSources();
    } catch (e) {
        toast('删除失败: ' + e.message, 'error');
    }
}

async function deleteAndBack(url, name) {
    await deleteRssSource(url, name);
    rssCurrentView = 'sources';
    renderRssPanel();
}

async function fetchRssSource(url) {
    if (rssIsFetching) return;
    rssIsFetching = true;
    toast('🔄 正在抓取...', 'success');
    try {
        const r = await fetch(`${API}/api/rss/fetch`, {
            method: 'POST', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({sourceUrl: url}),
        });
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); }
        else { toast(`✅ 抓到 ${d.count || 0} 篇内容`, 'success'); }

        if (rssCurrentView === 'articles' && rssCurrentSource && rssCurrentSource.sourceUrl === url) {
            await loadRssArticles(url);
        } else {
            await refreshRssSources();
        }
    } catch (e) {
        toast('抓取失败: ' + e.message, 'error');
    } finally {
        rssIsFetching = false;
    }
}

let rssCheckEvtSource = null;

async function clearAllRssSources() {
    if (rssSourcesData.length === 0) { toast('列表已经是空的', 'info'); return; }
    if (!confirm(`确定要清空所有 RSS 订阅源吗？\n共 ${rssSourcesData.length} 个源及其内容将被全部删除，不可恢复。`)) return;
    try {
        const r = await fetch(`${API}/api/rss/clear`, {method: 'DELETE'});
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }
        toast(`🗑️ 已清空 ${rssSourcesData.length} 个 RSS 源`, 'success');
        rssSourcesData = [];
        renderRssSourceList();
    } catch (e) {
        toast('清空失败: ' + e.message, 'error');
    }
}

// 清空被评级为"无效"的源
async function clearInvalidRssSources() {
    const invalidCount = rssSourcesData.filter(s => s.validity === 'invalid').length;
    if (invalidCount === 0) { toast('没有「无效」源，先点「检测」评级', 'info'); return; }
    if (!confirm(`确定要清空 ${invalidCount} 个「无效」RSS 源吗？\n相关内容也会被一并删除，不可恢复。`)) return;
    try {
        const r = await fetch(`${API}/api/rss/clear-invalid`, {method: 'DELETE'});
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }
        toast(`🧹 已清空 ${d.removed || 0} 个无效源`, 'success');
        await refreshRssSources();
    } catch (e) {
        toast('清空无效失败: ' + e.message, 'error');
    }
}

async function checkRssSources() {
    // 防止卡死：如果上次检测的 EventSource 还在，先关闭
    if (rssCheckEvtSource) {
        rssCheckEvtSource.close();
        rssCheckEvtSource = null;
    }
    if (rssIsFetching) {
        // 可能是上次异常退出未重置，强制恢复
        rssIsFetching = false;
    }

    // 若列表未加载，先拉取数据，避免头部显示"共0个源"
    if (rssSourcesData.length === 0) {
        await refreshRssSources();
    }

    rssIsFetching = true;

    // 禁用检测按钮
    const checkBtn = document.getElementById('rssCheckBtn');
    if (checkBtn) checkBtn.disabled = true;

    const progress = document.getElementById('rssCheckProgress');
    const num = document.getElementById('rssCheckNum');
    const fill = document.getElementById('rssCheckFill');
    const stats = document.getElementById('rssCheckStats');
    if (progress) progress.style.display = 'flex';
    if (num) num.textContent = '0/0';
    if (fill) fill.style.width = '0%';
    if (stats) stats.textContent = '';

    let validCount = 0, invalidCount = 0;

    // 刷新按钮 disabled 状态
    if (rssCurrentView === 'sources') renderRssSourceList();

    try {
        const evtSource = new EventSource(`${API}/api/rss/check/stream`);
        rssCheckEvtSource = evtSource;

        const handle = (e) => {
            let d; try { d = JSON.parse(e.data); } catch (_) { return; }
            if (d.finished) {
                evtSource.close();
                rssCheckEvtSource = null;
                const ex = d.excellent||0, gd = d.good||0, pr = d.poor||0, iv = d.invalid||0;
                toast(`检测完成：优${ex} 良${gd} 差${pr} 无效${iv}`, iv > 0 ? 'warning' : 'success');
                if (progress) progress.style.display = 'none';
                rssCheckDone();
                if (rssCurrentView === 'sources') refreshRssSources();
                return;
            }
            if (d.error) {
                evtSource.close();
                rssCheckEvtSource = null;
                toast('检测失败: ' + d.error, 'error');
                if (progress) progress.style.display = 'none';
                rssCheckDone();
                if (rssCurrentView === 'sources') refreshRssSources();
                return;
            }
            const done = d.done || 0;
            const total = d.total || 1;
            if (num) num.textContent = `${done}/${total}`;
            if (fill) fill.style.width = `${Math.round(done / total * 100)}%`;
            if (stats) stats.textContent =
                `优${d.excellent||0} 良${d.good||0} 差${d.poor||0} 无效${d.invalid||0}`;
        };

        evtSource.addEventListener('check_progress', handle);
        evtSource.addEventListener('check_done', handle);
        evtSource.onmessage = handle;

        evtSource.onerror = () => {
            evtSource.close();
            rssCheckEvtSource = null;
            if (progress) progress.style.display = 'none';
            rssCheckDone();
            if (rssCurrentView === 'sources') refreshRssSources();
        };
    } catch (e) {
        toast('检测失败: ' + e.message, 'error');
        if (progress) progress.style.display = 'none';
        rssCheckDone();
        if (rssCurrentView === 'sources') refreshRssSources();
    }
}

async function fetchAllRssSources() {
    if (rssIsFetching || !rssSourcesData.length) return;
    rssIsFetching = true;
    toast('🔄 正在刷新全部 RSS 源...', 'success');
    let okCount = 0, failCount = 0;
    for (const s of rssSourcesData) {
        try {
            const r = await fetch(`${API}/api/rss/fetch`, {
                method: 'POST', headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({sourceUrl: s.sourceUrl}),
            });
            const d = await r.json();
            if (d.error) failCount++;
            else okCount++;
        } catch (e) {
            failCount++;
        }
    }
    rssIsFetching = false;
    toast(`🔄 刷新完成：成功 ${okCount} 个，失败 ${failCount} 个`, okCount > 0 ? 'success' : 'error');
    await refreshRssSources();
}

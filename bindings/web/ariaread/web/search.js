/**
 * search.js — 搜索、书籍卡片渲染、书源多选面板
 * 依赖：app.js 中的全局变量和工具函数
 * 依赖：sources.js 中的 updateSourceInData, getValidPickerSources
 * 依赖：bookshelf.js 中的 isInShelf, addToShelf
 * 依赖：reader.js 中的 fetchCatalog
 */

// ──────────────────────────────────────────────
// 搜索
// ──────────────────────────────────────────────

function onSearchInputChange() {
    const q = document.getElementById('searchInput').value.trim();
    const btn = document.getElementById('searchBtn');
    if (isSearching && q && q !== lastSearchQuery) {
        btn.disabled = false;
        btn.textContent = '🔍 搜索';
    }
}

function cancelCurrentSearch() {
    if (searchEvtSource) { searchEvtSource.close(); searchEvtSource = null; }
    isSearching = false;
    const btn = document.getElementById('searchBtn');
    const stopBtn = document.getElementById('stopSearchBtn');
    const progress = document.getElementById('searchProgress');
    if (btn) { btn.disabled = false; btn.textContent = '🔍 搜索'; }
    if (stopBtn) stopBtn.style.display = 'none';
    if (progress) progress.style.display = 'none';  // 隐藏 spinner
}

function getSearchFilters() {
    const byName = document.getElementById('matchName')?.checked ?? true;
    const byAuthor = document.getElementById('matchAuthor')?.checked ?? false;
    const byIntro = document.getElementById('matchIntro')?.checked ?? false;
    return {
        matchName: byName || (!byName && !byAuthor && !byIntro),
        matchAuthor: byAuthor,
        matchIntro: byIntro,
    };
}

function normalizeKeyword(s) {
    return String(s || '').toLowerCase().replace(/[\s\t\r\n\-_\.·•|【】\[\]\(\)（）]/g, '');
}

function computeLocalMatchScore(book, rawKeyword, filters) {
    const kw = normalizeKeyword(rawKeyword);
    if (!kw) return 0;
    const name = normalizeKeyword(book.name || '');
    const author = normalizeKeyword(book.author || '');
    const intro = normalizeKeyword(book.intro || '');

    let score = 0;
    if (filters.matchName && name) {
        if (name === kw) score = Math.max(score, 1000);
        else if (name.startsWith(kw)) score = Math.max(score, 850);
        else if (name.includes(kw)) score = Math.max(score, 700);
    }
    if (filters.matchAuthor && author) {
        if (author === kw) score = Math.max(score, 520);
        else if (author.includes(kw)) score = Math.max(score, 420);
    }
    if (filters.matchIntro && intro.includes(kw)) {
        score = Math.max(score, 220);
    }
    return score;
}

function sortBooksByRelevance(books, rawKeyword, filters) {
    const kw = normalizeKeyword(rawKeyword);
    const list = books.map((b, idx) => {
        const backendScore = Number.isFinite(Number(b.matchScore)) ? Number(b.matchScore) : 0;
        const localScore = computeLocalMatchScore(b, kw, filters);
        const score = Math.max(backendScore, localScore);
        const nameNorm = normalizeKeyword(b.name || '');
    const nameLenGap = Math.abs(nameNorm.length - kw.length);
    const latency = Number.isFinite(Number(b.latency)) ? Number(b.latency) : 999999;
    return { ...b, _score: score, _nameLenGap: nameLenGap, _latency: latency, _idx: idx };
    });

    list.sort((a, b) => {
        if (a._score !== b._score) return b._score - a._score;
        if (a._nameLenGap !== b._nameLenGap) return a._nameLenGap - b._nameLenGap;
        if (a._latency !== b._latency) return a._latency - b._latency;
        return a._idx - b._idx;
    });

    return list;
}

async function doSearchAll() {
    const q = document.getElementById('searchInput').value.trim();
    if (!q) return;
    if (isSearching && q === lastSearchQuery) return;
    if (isSearching) cancelCurrentSearch();

    const selectedNames = getSelectedSourceNames();
    if (selectedNames.length > 0 && !isAllSourcesSelected()) {
        return doSearchWithSelectedSources(q, selectedNames);
    }
    if (selectedSourceNames.has('__NONE__')) {
        toast('请先选择至少一个书源', 'error');
        return;
    }

    const filters = getSearchFilters();
    lastSearchQuery = q;
    lastSearchFilters = { ...filters };
    isSearching = true;
    allBooks = [];
    bookCardNodeMap.clear();
    switchTab('search');

    const el = document.getElementById('results');
    const btn = document.getElementById('searchBtn');
    const progress = document.getElementById('searchProgress');
    const progressText = document.getElementById('progressText');
    const stopBtn = document.getElementById('stopSearchBtn');

    btn.disabled = true;
    btn.textContent = '⏳ 搜索中...';
    progress.style.display = 'flex';
    if (stopBtn) stopBtn.style.display = 'inline-block';

    el.innerHTML = `
        <div class="section-title">搜索结果 <span class="badge" id="resultCount">0</span></div>
        <div class="book-grid" id="bookGrid"></div>`;

    let doneSources = 0, totalSources = 0;

    try {
        const url = `${API}/api/search/all?q=${encodeURIComponent(q)}&match_name=${filters.matchName ? 1 : 0}&match_author=${filters.matchAuthor ? 1 : 0}&match_intro=${filters.matchIntro ? 1 : 0}`;
        const evtSource = new EventSource(url);
        searchEvtSource = evtSource;

        evtSource.addEventListener('search_start', (e) => {
            const d = JSON.parse(e.data);
            totalSources = d.totalSources;
            progressText.textContent = `0/${totalSources} 书源 · 0 结果`;
        });

        evtSource.addEventListener('source_result', (e) => {
            const d = JSON.parse(e.data);
            const sourceUrl = d.url || (sourcesData[d.index] ? sourcesData[d.index].url : '');
            updateSourceInData(sourceUrl, d.index, 'good', d.latency);
            doneSources++;
            if (d.books && d.books.length > 0) {
                const booksWithLatency = d.books.map(b => ({ ...b, latency: d.latency, sourceUrl: d.url || '' }));
                allBooks = allBooks.concat(booksWithLatency);
                renderBooks(allBooks, q, filters);
            }
            document.getElementById('resultCount').textContent = allBooks.length;
            progressText.textContent = `${doneSources}/${totalSources} 书源 · ${allBooks.length} 结果`;
        });

        evtSource.addEventListener('source_error', (e) => {
            doneSources++;
            progressText.textContent = `${doneSources}/${totalSources} 书源 · ${allBooks.length} 结果`;
        });

        evtSource.onerror = () => {
            evtSource.close();
            searchEvtSource = null;
            renderBooks(allBooks, q, filters);
            finishSearch(btn, progress, totalSources, allBooks.length);
        };
    } catch (e) {
        toast('搜索失败: ' + e.message, 'error');
        finishSearch(btn, progress, 0, 0);
    }
}

function getBookKey(b) {
    return `${b.sourceIndex || ''}|${b.sourceName || ''}|${b.url || ''}|${b.name || ''}`;
}

function ensureBookCardNode(book) {
    const key = getBookKey(book);
    let node = bookCardNodeMap.get(key);
    if (!node) {
        node = document.createElement('div');
        node.className = 'book-card';
        bookCardNodeMap.set(key, node);
    }

    const nextHash = JSON.stringify({
        name: book.name || '', author: book.author || '',
        sourceName: book.sourceName || '', intro: book.intro || '', url: book.url || '',
    });

    if (node.dataset.renderHash !== nextHash) {
        const bookUrl = book.url || book.bookUrl || '';
        const alreadyInShelf = isInShelf(bookUrl);
        const shelfBtnHtml = alreadyInShelf
            ? `<button class="add-shelf-btn in-shelf" disabled>✓ 已收藏</button>`
            : `<button class="add-shelf-btn" onclick="event.stopPropagation();addToShelf(this, ${esc2(JSON.stringify(book))})">+ 书架</button>`;
        node.innerHTML = `
            <div class="title">${esc(book.name)}</div>
            <div class="author">${esc(book.author) || '未知'}</div>
            <span class="source-tag">📖 ${esc(book.sourceName)}</span>
            ${shelfBtnHtml}
            <div class="intro">${esc(book.intro) || '暂无简介'}</div>`;
        node.dataset.renderHash = nextHash;
    }

    node.onclick = () => fetchCatalog(book);
    return node;
}

function renderBooks(books, keyword, filters) {
    const grid = document.getElementById('bookGrid');
    if (!grid) return;

    const sorted = sortBooksByRelevance(books, keyword, filters);
    const usedKeys = new Set();
    const fragment = document.createDocumentFragment();

    for (const b of sorted) {
        const key = getBookKey(b);
        usedKeys.add(key);
        fragment.appendChild(ensureBookCardNode(b));
    }

    for (const [key, node] of bookCardNodeMap.entries()) {
        if (!usedKeys.has(key)) {
            if (node.parentNode) node.parentNode.removeChild(node);
            bookCardNodeMap.delete(key);
        }
    }

    grid.appendChild(fragment);
}

function finishSearch(btn, progress, totalSources, totalBooks) {
    isSearching = false;
    btn.disabled = false;
    btn.textContent = '🔍 搜索';
    progress.style.display = 'none';
    const stopBtn = document.getElementById('stopSearchBtn');
    if (stopBtn) stopBtn.style.display = 'none';

    if (totalBooks === 0) {
        document.getElementById('results').innerHTML = `<div class="empty"><div class="icon">🔍</div><h3>没有找到结果</h3><p>共搜索 ${totalSources} 个书源，换个关键词试试</p></div>`;
    }
    refreshSources();
}

// ──────────────────────────────────────────────
// 搜索历史持久化（自定义下拉，支持 Enter + X 删除）
// ──────────────────────────────────────────────

const SEARCH_HISTORY_KEYS = {
    source: 'ariaread_source_search_history',
    book:   'ariaread_book_search_history',
};

function getSearchHistory(key) {
    try {
        const arr = JSON.parse(localStorage.getItem(key) || '[]');
        return Array.isArray(arr) ? arr : [];
    } catch { return []; }
}

function saveSearchHistory(key, query) {
    if (!query) return;
    const arr = getSearchHistory(key);
    const idx = arr.indexOf(query);
    if (idx !== -1) arr.splice(idx, 1);   // 去重，移到最前
    arr.unshift(query);
    if (arr.length > 20) arr.length = 20;  // 最多保留 20 条
    localStorage.setItem(key, JSON.stringify(arr));
}

function deleteSearchHistoryItem(key, query) {
    const arr = getSearchHistory(key);
    const idx = arr.indexOf(query);
    if (idx !== -1) {
        arr.splice(idx, 1);
        localStorage.setItem(key, JSON.stringify(arr));
    }
}

// 渲染历史下拉
function renderHistoryDropdown(type) {
    const key = SEARCH_HISTORY_KEYS[type];
    const dropdownId = type === 'source' ? 'sourceHistoryDropdown' : 'bookHistoryDropdown';
    const inputId = type === 'source' ? 'sourceSearchInput' : 'searchInput';
    const dropdown = document.getElementById(dropdownId);
    if (!dropdown) return;
    const items = getSearchHistory(key);
    if (!items.length) {
        dropdown.innerHTML = '<div class="history-empty">暂无搜索记录</div>';
        return;
    }
    dropdown.innerHTML = items.map(item =>
        `<div class="history-item" data-value="${esc2(item)}">
            <span class="history-item-text">${esc(item)}</span>
            <span class="history-item-del" title="删除">✕</span>
        </div>`
    ).join('');
    // 绑定事件
    dropdown.querySelectorAll('.history-item').forEach(el => {
        const val = el.dataset.value;
        // 点击文本 → 填入搜索框并触发搜索
        el.querySelector('.history-item-text').addEventListener('mousedown', (e) => {
            e.preventDefault(); // 阻止 blur
            document.getElementById(inputId).value = val;
            hideHistoryDropdown(type);
            if (type === 'book') doSearchAll();
            else { filterSourceList(); saveSearchHistory(SEARCH_HISTORY_KEYS.source, val); }
        });
        // 点击 X → 删除该条记录
        el.querySelector('.history-item-del').addEventListener('mousedown', (e) => {
            e.preventDefault(); // 阻止 blur
            e.stopPropagation();
            deleteSearchHistoryItem(key, val);
            renderHistoryDropdown(type);
        });
    });
}

function showHistoryDropdown(type) {
    renderHistoryDropdown(type);
    const dropdownId = type === 'source' ? 'sourceHistoryDropdown' : 'bookHistoryDropdown';
    const dropdown = document.getElementById(dropdownId);
    if (dropdown) dropdown.classList.add('show');
}

function hideHistoryDropdown(type) {
    // 延迟隐藏，让 mousedown 事件先触发
    setTimeout(() => {
        const dropdownId = type === 'source' ? 'sourceHistoryDropdown' : 'bookHistoryDropdown';
        const dropdown = document.getElementById(dropdownId);
        if (dropdown) dropdown.classList.remove('show');
    }, 150);
}

// 书源搜索框：回车时保存历史
(function() {
    const el = document.getElementById('sourceSearchInput');
    if (!el) return;
    el.addEventListener('keydown', function(e) {
        if (e.key === 'Enter') {
            const q = el.value.trim();
            if (q) saveSearchHistory(SEARCH_HISTORY_KEYS.source, q);
        }
    });
})();

// 书籍搜索：搜索执行时保存历史（hook doSearchAll）
(function() {
    const origFn = doSearchAll;
    doSearchAll = function() {
        const q = document.getElementById('searchInput')?.value?.trim();
        if (q) saveSearchHistory(SEARCH_HISTORY_KEYS.book, q);
        return origFn.apply(this, arguments);
    };
})();

// ──────────────────────────────────────────────
// 书源多选面板
// ──────────────────────────────────────────────

let selectedSourceNames = new Set();

function toggleSourcePicker() {
    const panel = document.getElementById('sourcePickerPanel');
    const isOpen = panel.style.display !== 'none';
    panel.style.display = isOpen ? 'none' : 'flex';
    if (!isOpen) {
        updateSourcePickerList();
        document.getElementById('spSearch').value = '';
        setTimeout(() => document.addEventListener('click', _closeSourcePickerHandler, { once: true }), 0);
    }
}

function _closeSourcePickerHandler(e) {
    const panel = document.getElementById('sourcePickerPanel');
    const btn = document.getElementById('sourcePickerBtn');
    if (panel && !panel.contains(e.target) && btn && !btn.contains(e.target)) {
        panel.style.display = 'none';
    } else if (panel && panel.style.display !== 'none') {
        setTimeout(() => document.addEventListener('click', _closeSourcePickerHandler, { once: true }), 0);
    }
}

function updateSourcePicker() {
    updateSourcePickerBtn();
}

function updateSourcePickerBtn() {
    const btn = document.getElementById('sourcePickerBtn');
    if (!btn) return;
    if (selectedSourceNames.size === 0) {
        const valid = getValidPickerSources();
        btn.textContent = `📚 全部书源(${valid.length}) ▾`;
        btn.classList.remove('has-selection');
    } else if (selectedSourceNames.has('__NONE__')) {
        btn.textContent = '📚 未选书源 ▾';
        btn.classList.remove('has-selection');
    } else {
        btn.textContent = `📚 已选 ${selectedSourceNames.size} 个 ▾`;
        btn.classList.add('has-selection');
    }
}

function getValidPickerSources() {
    return sourcesData.filter(s => s.search_url && s.validity !== 'invalid' && s.validity !== 'poor');
}

function isAllSourcesSelected() {
    if (selectedSourceNames.size === 0) return true;
    const valid = getValidPickerSources();
    return valid.length > 0 && valid.every(s => selectedSourceNames.has(s.name));
}

function updateSourcePickerList(filter) {
    const list = document.getElementById('spList');
    if (!list) return;
    const kw = (filter || '').toLowerCase();
    const validSources = getValidPickerSources();
    const filtered = kw ? validSources.filter(s => (s.name || '').toLowerCase().includes(kw) || (s.group || '').toLowerCase().includes(kw)) : validSources;
    const allMode = selectedSourceNames.size === 0;

    list.innerHTML = filtered.map(s => {
        const checked = (allMode || selectedSourceNames.has(s.name)) ? 'checked' : '';
        const grade = s.validity === 'excellent' ? '⭐' : s.validity === 'good' ? '✅' : '📖';
        return `<label class="sp-item"><input type="checkbox" ${checked} onchange="toggleSourceSelection('${esc2(s.name)}', this.checked)"><span class="sp-name">${esc(s.name)}</span><span class="sp-grade">${grade}</span></label>`;
    }).join('');

    if (!filtered.length) {
        list.innerHTML = '<div style="padding:12px;text-align:center;color:var(--text2);font-size:12px">无匹配书源</div>';
    }

    updateSourcePickerCount();
}

function filterSourcePicker() {
    const kw = document.getElementById('spSearch')?.value || '';
    updateSourcePickerList(kw);
}

function toggleSourceSelection(name, checked) {
    if (selectedSourceNames.size === 0 && !checked) {
        const valid = getValidPickerSources();
        for (const s of valid) selectedSourceNames.add(s.name);
    }
    if (selectedSourceNames.has('__NONE__') && checked) {
        selectedSourceNames.delete('__NONE__');
    }
    if (checked) {
        selectedSourceNames.add(name);
    } else {
        selectedSourceNames.delete(name);
    }
    const valid = getValidPickerSources();
    if (valid.length > 0 && valid.every(s => selectedSourceNames.has(s.name))) {
        selectedSourceNames.clear();
    }
    updateSourcePickerCount();
    updateSourcePickerBtn();
}

function selectAllSources() {
    selectedSourceNames.clear();
    const kw = document.getElementById('spSearch')?.value || '';
    updateSourcePickerList(kw);
    updateSourcePickerBtn();
}

function deselectAllSources() {
    selectedSourceNames.clear();
    selectedSourceNames.add('__NONE__');
    const kw = document.getElementById('spSearch')?.value || '';
    updateSourcePickerList(kw);
    updateSourcePickerBtn();
}

function selectExcellentSources() {
    selectedSourceNames.clear();
    const excellent = sourcesData.filter(s => s.search_url && s.validity === 'excellent');
    if (excellent.length === 0) { toast('暂无优质书源', 'error'); return; }
    excellent.forEach(s => selectedSourceNames.add(s.name));
    const kw = document.getElementById('spSearch')?.value || '';
    updateSourcePickerList(kw);
    updateSourcePickerBtn();
}

function updateSourcePickerCount() {
    const el = document.getElementById('spCount');
    if (!el) return;
    if (selectedSourceNames.size === 0) {
        const valid = getValidPickerSources();
        el.textContent = `已选全部 ${valid.length} 个`;
    } else if (selectedSourceNames.has('__NONE__')) {
        el.textContent = '已选 0 个';
    } else {
        el.textContent = `已选 ${selectedSourceNames.size} 个`;
    }
}

function confirmSourcePicker() {
    document.getElementById('sourcePickerPanel').style.display = 'none';
    updateSourcePickerBtn();
    savePrefs();
}

function getSelectedSourceNamesForSave() {
    return Array.from(selectedSourceNames).filter(n => n !== '__NONE__');
}

function getSelectedSourceNames() {
    if (selectedSourceNames.size === 0) return [];
    if (selectedSourceNames.has('__NONE__')) return [];
    return Array.from(selectedSourceNames).filter(n => n !== '__NONE__');
}

// ──────────────────────────────────────────────
// 指定书源搜索（多选，SSE）
// ──────────────────────────────────────────────

async function doSearchWithSelectedSources(keyword, sourceNames) {
    isSearching = true;
    allBooks = [];
    bookCardNodeMap.clear();
    switchTab('search');

    const el = document.getElementById('results');
    const btn = document.getElementById('searchBtn');
    const progress = document.getElementById('searchProgress');
    const progressText = document.getElementById('progressText');
    const stopBtn = document.getElementById('stopSearchBtn');

    btn.disabled = true;
    btn.textContent = '⏳ 搜索中...';
    progress.style.display = 'flex';
    if (stopBtn) stopBtn.style.display = 'inline-block';
    lastSearchQuery = keyword;

    el.innerHTML = `
        <div class="section-title">搜索结果（${sourceNames.length} 个书源）<span class="badge" id="resultCount">0</span></div>
        <div class="book-grid" id="bookGrid"></div>`;

    let doneSources = 0, totalSources = 0;
    const filters = getSearchFilters();

    try {
        const url = `${API}/api/search/selected?q=${encodeURIComponent(keyword)}&source_names=${encodeURIComponent(sourceNames.join(','))}&match_name=${filters.matchName ? 1 : 0}&match_author=${filters.matchAuthor ? 1 : 0}&match_intro=${filters.matchIntro ? 1 : 0}`;
        const evtSource = new EventSource(url);
        searchEvtSource = evtSource;

        evtSource.addEventListener('search_start', (e) => {
            const d = JSON.parse(e.data);
            totalSources = d.totalSources;
            progressText.textContent = `0/${totalSources} 书源 · 0 结果`;
        });

        evtSource.addEventListener('source_result', (e) => {
            const d = JSON.parse(e.data);
            doneSources++;
            if (d.books && d.books.length > 0) {
                allBooks = allBooks.concat(d.books);
                renderBooks(allBooks, keyword, filters);
            }
            document.getElementById('resultCount').textContent = allBooks.length;
            progressText.textContent = `${doneSources}/${totalSources} 书源 · ${allBooks.length} 结果`;
        });

        evtSource.addEventListener('source_error', (e) => {
            doneSources++;
            progressText.textContent = `${doneSources}/${totalSources} 书源 · ${allBooks.length} 结果`;
        });

        evtSource.addEventListener('search_done', (e) => {
            evtSource.close();
            searchEvtSource = null;
            renderBooks(allBooks, keyword, filters);
            finishSearch(btn, progress, totalSources, allBooks.length);
        });

        evtSource.onerror = () => {
            evtSource.close();
            searchEvtSource = null;
            renderBooks(allBooks, keyword, filters);
            finishSearch(btn, progress, totalSources, allBooks.length);
        };
    } catch (e) {
        toast('搜索失败: ' + e.message, 'error');
        finishSearch(btn, progress, 0, 0);
    }
}

// ──────────────────────────────────────────────
// 单源搜索（保留）
// ──────────────────────────────────────────────

async function doSearch(sourceIndex) {
    const q = document.getElementById('searchInput').value.trim();
    if (!q) return;
    const el = document.getElementById('results');
    el.innerHTML = '<div class="loading"><div class="spinner"></div></div>';
    switchTab('search');
    try {
        const r = await fetch(`${API}/api/search?q=${encodeURIComponent(q)}&source=${sourceIndex}`);
        const books = await r.json();
        if (books.error) { toast(books.error, 'error'); el.innerHTML = ''; return; }
        if (!books.length) { el.innerHTML = '<div class="empty"><div class="icon">🔍</div><h3>没有找到结果</h3><p>换个关键词试试</p></div>'; return; }
        el.innerHTML = `
            <div class="section-title">搜索结果 <span class="badge">${books.length}</span></div>
            <div class="book-grid">
                ${books.map(b => `
                    <div class="book-card" onclick='fetchCatalog(${JSON.stringify(b)})'>
                        <div class="title">${esc(b.name)}</div>
                        <div class="author">${esc(b.author) || '未知'}</div>
                        <div class="intro">${esc(b.intro) || '暂无简介'}</div>
                    </div>
                `).join('')}
            </div>`;
    } catch (e) { toast('搜索失败: ' + e.message, 'error'); }
}

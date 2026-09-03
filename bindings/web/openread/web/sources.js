/**
 * sources.js — 书源列表、书源操作、书源验证
 * 依赖：app.js 中的全局变量（API, sourcesData 等）和工具函数（esc, esc2, toast）
 */

// ──────────────────────────────────────────────
// 书源列表渲染
// ──────────────────────────────────────────────

// 书源搜索关键词
let sourceSearchKeyword = '';

function filterSourceList() {
    const input = document.getElementById('sourceSearchInput');
    sourceSearchKeyword = (input ? input.value : '').trim().toLowerCase();
    renderSources(sourcesData);
}

async function refreshSources() {
    try {
        const r = await fetch(`${API}/api/sources`);
        const d = await r.json();
        sourcesData = d.sources || [];
        totalSourceCount = d.totalCount || sourcesData.length;
        renderSources(sourcesData);
        document.getElementById('validateBtn').disabled = !sourcesData.length;
        if (d.stats && !isValidating) {
            showStatsSummary(d.stats);
        }
        updateSourcePicker();
    } catch(e) {
        console.error('refreshSources:', e);
    }
}

const VALIDITY_ORDER = { excellent: 0, good: 1, unknown: 2, poor: 3, invalid: 4 };

function sortSources(sources) {
    return sources.map((s, i) => ({...s, _origIndex: i}))
        .sort((a, b) => {
            const va = VALIDITY_ORDER[a.validity] ?? 5;
            const vb = VALIDITY_ORDER[b.validity] ?? 5;
            if (va !== vb) return va - vb;
            const la = (typeof a.latency === 'number') ? a.latency : 99999;
            const lb = (typeof b.latency === 'number') ? b.latency : 99999;
            return la - lb;
        });
}

function renderSources(sources) {
    const el = document.getElementById('sourceList');
    if (!sources || !sources.length) {
        el.innerHTML = '<div class="empty"><div class="icon">📂</div><h3>暂无书源</h3><p>点击「URL加载」输入书源地址</p></div>';
        return;
    }

    // 过滤掉差和无效的书源，只显示待检测、优、良
    const filtered = sources.filter(s => {
        const v = s.validity || 'unknown';
        if (v === 'poor' || v === 'invalid') return false;
        // 搜索关键词过滤
        if (sourceSearchKeyword) {
            const name = (s.name || '').toLowerCase();
            const group = (s.group || '').toLowerCase();
            const url = (s.url || '').toLowerCase();
            if (!name.includes(sourceSearchKeyword) && !group.includes(sourceSearchKeyword) && !url.includes(sourceSearchKeyword)) {
                return false;
            }
        }
        return true;
    });

    if (!filtered.length) {
        if (sourceSearchKeyword) {
            el.innerHTML = `<div class="empty"><div class="icon">🔍</div><h3>未找到匹配的书源</h3><p>尝试其他关键词</p></div>`;
        } else {
            el.innerHTML = '<div class="empty"><div class="icon">📂</div><h3>暂无可用书源</h3><p>所有书源均为差或无效状态</p></div>';
        }
        return;
    }

    const sorted = sortSources(filtered);

    el.innerHTML = sorted.map(s => {
        const v = s.validity || 'unknown';
        const origIdx = s._origIndex;
        let icon = '', cls = v, gradeTag = '';
        if (v === 'excellent') { icon = '⭐'; gradeTag = '<span class="grade-tag excellent">优</span>'; }
        else if (v === 'good') { icon = '✅'; gradeTag = '<span class="grade-tag good">良</span>'; }
        else { icon = s.search_url ? '📖' : '📕'; cls = 'unknown'; }

        let rightInfo = '';
        if (v !== 'unknown') {
            const hasLatency = typeof s.latency === 'number';
            const latencyText = hasLatency ? `${s.latency}ms` : '';
            rightInfo = `<span class="source-status">${icon}${gradeTag}${hasLatency ? ` <span class="latency">${latencyText}</span>` : ''}</span>`;
        }
        return `<div class="source-item ${cls}" data-orig-index="${origIdx}">
            <div style="flex:1;min-width:0">
                <div class="name">${esc(s.name || '未命名')}</div>
                <div class="meta">${s.group || s.url || ''}</div>
            </div>
            ${rightInfo}
            <button class="source-del-btn" onclick="event.stopPropagation();deleteSource('${esc2(s.url)}','${esc2(s.name)}')" title="删除此书源">✕</button>
        </div>`;
    }).join('');
}

// ──────────────────────────────────────────────
// 书源操作菜单
// ──────────────────────────────────────────────

function toggleSourceMenu(btn) {
    const menu = document.getElementById('sourceMenu');
    const isOpen = menu.style.display !== 'none';
    menu.style.display = isOpen ? 'none' : 'block';
    if (!isOpen) {
        setTimeout(() => document.addEventListener('click', _closeSourceMenuHandler, { once: true }), 0);
    }
}
function _closeSourceMenuHandler() { closeSourceMenu(); }
function closeSourceMenu() {
    document.getElementById('sourceMenu').style.display = 'none';
}

function showUrlModal() {
    document.getElementById('urlModal').style.display = 'flex';
    loadUrlHistory();
}
function closeUrlModal() { document.getElementById('urlModal').style.display = 'none'; }
function showLoadModal() {
    _pendingFileContent = null;
    _fileReading = false;
    document.getElementById('fileInput').value = '';
    document.getElementById('fileInputName').textContent = '未选择文件';
    document.getElementById('filePath').value = '';
    const loadBtn = document.querySelector('#loadModal .btn-primary');
    if (loadBtn) loadBtn.disabled = false;
    document.getElementById('loadModal').style.display = 'flex';
    // 路径输入时清除文件选择状态
    document.getElementById('filePath').oninput = () => {
        if (document.getElementById('filePath').value.trim()) {
            _pendingFileContent = null;
            _fileReading = false;
            document.getElementById('fileInput').value = '';
            document.getElementById('fileInputName').textContent = '未选择文件';
        }
    };
}
function showRawModal() { document.getElementById('rawModal').style.display = 'flex'; }

function fillPresetUrl(url) {
    document.getElementById('sourceUrl').value = url;
}

async function loadUrlHistory() {
    const container = document.getElementById('urlHistoryTags');
    if (!container) return;
    try {
        const r = await fetch(`${API}/api/url_history`);
        const d = await r.json();
        const urls = d.urls || [];
        if (!urls.length) {
            container.innerHTML = '<span class="empty-hint">暂无历史记录</span>';
            return;
        }
        container.innerHTML = urls.map(u => {
            let display = u.replace(/^https?:\/\//, '');
            if (display.length > 50) display = display.substring(0, 47) + '...';
            return `<span class="tag" onclick="fillPresetUrl('${esc2(u)}')" title="${esc2(u)}">${esc(display)}<span class="tag-del" onclick="event.stopPropagation();deleteUrlHistory('${esc2(u)}')" title="删除">✕</span></span>`;
        }).join('');
    } catch (e) {
        container.innerHTML = '<span class="empty-hint">加载失败</span>';
    }
}

async function deleteUrlHistory(url) {
    try {
        await fetch(`${API}/api/url_history`, {
            method: 'DELETE',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({url})
        });
        loadUrlHistory();
    } catch (e) {
        toast('删除失败', 'error');
    }
}

async function loadUrl() {
    const url = document.getElementById('sourceUrl').value.trim();
    if (!url) return;
    closeUrlModal();
    toast('⬇️ 正在后台下载书源...', 'success');
    try {
        const ctrl = new AbortController();
        const timer = setTimeout(() => ctrl.abort(), 30000);
        const r = await fetch(`${API}/api/sources/url`, {
            method: 'POST', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({url}), signal: ctrl.signal
        });
        clearTimeout(timer);
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }
        toast(`✅ 加载了 ${d.loaded} 个书源（共 ${d.total}）`, 'success');
        await refreshSources();
        autoValidate();
    } catch(e) {
        if (e.name === 'AbortError') toast('⏰ 下载超时（30s）', 'error');
        else toast('加载失败: ' + e.message, 'error');
    }
}

let _pendingFileContent = null;
let _fileReading = false;

function onFileSelected(input) {
    const file = input.files[0];
    const nameEl = document.getElementById('fileInputName');
    const loadBtn = document.querySelector('#loadModal .btn-primary');
    if (!file) { nameEl.textContent = '未选择文件'; _pendingFileContent = null; _fileReading = false; if (loadBtn) loadBtn.disabled = false; return; }
    nameEl.textContent = `⏳ 读取中: ${file.name}`;
    _fileReading = true;
    _pendingFileContent = null;
    if (loadBtn) loadBtn.disabled = true;
    // 同时清除路径输入，避免歧义
    document.getElementById('filePath').value = '';
    const reader = new FileReader();
    reader.onload = () => {
        _pendingFileContent = reader.result;
        _fileReading = false;
        nameEl.textContent = `✅ ${file.name} (${(reader.result.length / 1024).toFixed(1)}KB)`;
        if (loadBtn) loadBtn.disabled = false;
    };
    reader.onerror = () => { toast('读取文件失败', 'error'); _pendingFileContent = null; _fileReading = false; nameEl.textContent = '❌ 读取失败'; if (loadBtn) loadBtn.disabled = false; };
    reader.readAsText(file, 'UTF-8');
}

async function loadFile() {
    // 如果文件还在读取中，提示等待
    if (_fileReading) { toast('文件正在读取中，请稍候...', 'warning'); return; }
    if (_pendingFileContent) {
        document.getElementById('loadModal').style.display = 'none';
        toast('📁 正在加载书源文件...', 'success');
        try {
            const r = await fetch(`${API}/api/sources/raw`, {
                method: 'POST', headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({json: _pendingFileContent})
            });
            const d = await r.json();
            _pendingFileContent = null;
            document.getElementById('fileInput').value = '';
            document.getElementById('fileInputName').textContent = '未选择文件';
            if (d.error) { toast(d.error, 'error'); return; }
            toast(`✅ 加载了 ${d.loaded} 个书源`, 'success');
            await refreshSources();
            autoValidate();
        } catch(e) { toast('加载失败: ' + e.message, 'error'); }
        return;
    }
    const path = document.getElementById('filePath').value.trim();
    if (!path) { toast('请选择文件或输入文件路径', 'error'); return; }
    document.getElementById('loadModal').style.display = 'none';
    toast('📁 正在加载书源文件...', 'success');
    try {
        const r = await fetch(`${API}/api/sources/load`, {
            method: 'POST', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({path})
        });
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }
        toast(`✅ 加载了 ${d.loaded} 个书源`, 'success');
        await refreshSources();
        autoValidate();
    } catch(e) { toast('加载失败: ' + e.message, 'error'); }
}

async function loadRaw() {
    const json = document.getElementById('rawJson').value.trim();
    if (!json) return;
    document.getElementById('rawModal').style.display = 'none';
    toast('📝 正在加载书源...', 'success');
    try {
        const r = await fetch(`${API}/api/sources/raw`, {
            method: 'POST', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({json})
        });
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }
        toast(`✅ 加载了 ${d.loaded} 个书源`, 'success');
        await refreshSources();
        autoValidate();
    } catch(e) { toast('加载失败: ' + e.message, 'error'); }
}

async function deleteSource(url, name) {
    if (!confirm(`确定要删除书源「${name}」吗？`)) return;
    try {
        const r = await fetch(`${API}/api/sources/remove`, {
            method: 'DELETE',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({url})
        });
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }
        toast(`🗑️ 已删除「${name}」`, 'success');
        await refreshSources();
    } catch (e) {
        toast('删除失败: ' + e.message, 'error');
    }
}

async function exportSources() {
    try {
        const r = await fetch(`${API}/api/sources/export`);
        if (!r.ok) {
            const d = await r.json();
            toast(d.error || '导出失败', 'error');
            return;
        }
        const blob = await r.blob();
        if (blob.size <= 2) {
            toast('暂无优/良书源可导出', 'error');
            return;
        }
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `book_sources_${new Date().toISOString().slice(0,10)}.json`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
        toast('📤 书源已导出', 'success');
    } catch (e) {
        toast('导出失败: ' + e.message, 'error');
    }
}

async function cleanInvalidSources() {
    if (isValidating || isSearching) { toast('请等待当前任务结束后再清理', 'error'); return; }
    if (!confirm('确定要清理所有差和无效的书源吗？')) return;
    try {
        const r = await fetch(`${API}/api/sources/invalid`, { method: 'DELETE' });
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }
        toast(`🧹 已清理 ${d.count || 0} 个无效书源`, 'success');
        await refreshSources();
    } catch (e) {
        toast('清理失败: ' + e.message, 'error');
    }
}

async function clearSources() {
    if (isValidating || isSearching) { toast('请等待当前任务结束后再清空', 'error'); return; }
    if (!confirm('确定要清空全部书源吗？')) return;
    try {
        const r = await fetch(`${API}/api/sources/clear`, { method: 'DELETE' });
        const d = await r.json();
        if (d.error) { toast(d.error, 'error'); return; }

        sourcesData = [];
        totalSourceCount = 0;
        sourceValidity = {};
        sourceLatency = {};
        allBooks = [];
        bookCardNodeMap.clear();
        lastSearchQuery = '';

        if (validateEvtSource) { validateEvtSource.close(); validateEvtSource = null; }
        if (searchEvtSource) { searchEvtSource.close(); searchEvtSource = null; }
        isValidating = false;
        isSearching = false;

        await refreshSources();
        document.getElementById('results').innerHTML = `
            <div class="empty">
                <div class="icon">🔍</div>
                <h3>开始搜索</h3>
                <p>加载书源后，点击「检测书源」过滤无效源，再搜索</p>
            </div>`;
        toast(`🗑️ 已清空 ${d.count || 0} 个书源`, 'success');
    } catch (e) {
        toast('清空失败: ' + e.message, 'error');
    }
}

// ──────────────────────────────────────────────
// 书源验证（SSE）
// ──────────────────────────────────────────────

function autoValidate() {
    if (!isValidating) validateSources();
}

function updateSourceInData(sourceUrl, fallbackIndex, status, latency, sourceInfo) {
    let target = null;
    if (sourceUrl) {
        target = sourcesData.find(s => s.url === sourceUrl) || null;
    }
    if (!target && Number.isInteger(fallbackIndex) && sourcesData[fallbackIndex]) {
        target = sourcesData[fallbackIndex];
    }
    if (!target) {
        if (sourceUrl && sourceInfo) {
            sourcesData.push({
                name: sourceInfo.name || '未命名',
                url: sourceUrl,
                group: sourceInfo.group || '',
                search_url: sourceInfo.search_url || '',
                validity: status,
                latency: (typeof latency === 'number') ? latency : null,
            });
        }
        return;
    }
    target.validity = status;
    // Bug2 修复：搜索延迟不应覆盖验证延迟
    // 验证延迟 = 搜索延迟 + 目录验证延迟，更准确反映等级
    // 只有当新延迟更大（更准确）或旧延迟不存在时才更新
    if (typeof latency === 'number') {
        if (typeof target.latency !== 'number' || latency > target.latency) {
            target.latency = latency;
        }
    }
}

let validateRefreshTimer = null;

function startValidateRefresh() {
    stopValidateRefresh();
    validateRefreshTimer = setInterval(() => { renderSources(sourcesData); }, 1000);
}

function stopValidateRefresh() {
    if (validateRefreshTimer) { clearInterval(validateRefreshTimer); validateRefreshTimer = null; }
}

function updateSummaryText(validCount, invalidCount, poorCount) {
    const excellentCount = Object.values(sourceValidity).filter(v=>v==='excellent').length;
    const goodCount = Object.values(sourceValidity).filter(v=>v==='good').length;
    const pc = poorCount ?? Object.values(sourceValidity).filter(v=>v==='poor').length;
    document.getElementById('summaryInline').innerHTML = `⭐<b style="color:var(--green)">${excellentCount}</b> ✅<b style="color:var(--blue)">${goodCount}</b> ⚠️<b style="color:var(--orange)">${pc}</b> ❌<b style="color:var(--red)">${invalidCount}</b>`;
}

function showStatsSummary(stats) {
    const excellent = stats.excellent || 0;
    const good = stats.good || 0;
    const poor = stats.poor || 0;
    const invalid = stats.invalid || 0;
    const unknown = stats.unknown || 0;
    const hasChecked = (excellent + good + poor + invalid) > 0;

    const progress = document.getElementById('validateProgress');
    const progressLine = document.getElementById('validateProgressLine');
    const summaryInline = document.getElementById('summaryInline');
    const summaryGrid = document.getElementById('summaryGrid');

    if (hasChecked || unknown > 0) {
        progress.style.display = 'flex';
        if (progressLine) progressLine.style.display = 'none';
        summaryInline.style.display = 'none';
        summaryGrid.style.display = 'block';
        document.getElementById('summaryRow1').innerHTML = `⭐ 优 <b style="color:var(--green)">${excellent}</b> &nbsp;&nbsp; ✅ 良 <b style="color:var(--blue)">${good}</b> &nbsp;&nbsp; ⚠️ 差 <b style="color:var(--orange)">${poor}</b>`;
        document.getElementById('summaryRow2').innerHTML = `📖 未知 <b>${unknown}</b> &nbsp;&nbsp; ❌ 无效 <b style="color:var(--red)">${invalid}</b>`;
    } else {
        progress.style.display = 'none';
    }
}

async function validateSources() {
    if (isValidating) return;
    isValidating = true;

    const btn = document.getElementById('validateBtn');
    const progress = document.getElementById('validateProgress');
    const progressLine = document.getElementById('validateProgressLine');

    btn.disabled = true;
    btn.textContent = '⏳ 验证中...';
    progress.style.display = 'flex';
    if (progressLine) progressLine.style.display = 'flex';

    sourceValidity = {};
    sourceLatency = {};
    document.getElementById('validateNum').textContent = '0/0';
    document.getElementById('validateFill').style.width = '0%';
    document.getElementById('summaryInline').innerHTML = '⭐0 ✅0 ❌0';
    document.getElementById('summaryInline').style.display = 'flex';
    document.getElementById('summaryGrid').style.display = 'none';

    const spinner = document.getElementById('validateSpinner');
    if (spinner) spinner.style.display = '';

    startValidateRefresh();

    let validCount = 0, invalidCount = 0, poorCount = 0, total = 0;

    try {
        const evtSource = new EventSource(`${API}/api/sources/validate`);
        validateEvtSource = evtSource;

        evtSource.addEventListener('validate_start', (e) => {
            const d = JSON.parse(e.data);
            total = d.total || 0;
            document.getElementById('validateNum').textContent = `0/${total}`;
        });

        evtSource.addEventListener('source_valid', (e) => {
            const d = JSON.parse(e.data);
            validCount++;
            const grade = d.grade || 'good';
            const sourceUrl = d.url || (sourcesData[d.index] ? sourcesData[d.index].url : '');
            if (sourceUrl) sourceValidity[sourceUrl] = grade;
            if (sourceUrl && typeof d.latency === 'number') sourceLatency[sourceUrl] = d.latency;
            updateSourceInData(sourceUrl, d.index, grade, d.latency, {
                name: d.name || '', group: d.group || '', search_url: d.search_url || '',
            });
            updateSummaryText(validCount, invalidCount, poorCount);
        });

        evtSource.addEventListener('source_removed', (e) => {
            const d = JSON.parse(e.data);
            const grade = d.reason === 'poor' ? 'poor' : 'invalid';
            if (d.reason === 'poor') poorCount++;
            else invalidCount++;
            const sourceUrl = d.url || '';
            if (sourceUrl) {
                sourceValidity[sourceUrl] = grade;
                // 从列表中移除差和无效的书源（实时过滤）
                sourcesData = sourcesData.filter(s => s.url !== sourceUrl);
            }
            updateSummaryText(validCount, invalidCount, poorCount);
        });

        evtSource.addEventListener('validate_progress', (e) => {
            const d = JSON.parse(e.data);
            const done = d.done || 0;
            const t = d.total || total || 0;
            total = t;
            const pct = t > 0 ? Math.round(done / t * 100) : 0;
            document.getElementById('validateFill').style.width = pct + '%';
            document.getElementById('validateNum').textContent = `${done}/${t}`;
        });

        evtSource.addEventListener('validate_done', (e) => {
            const d = JSON.parse(e.data);
            evtSource.close();
            validateEvtSource = null;
            stopValidateRefresh();
            finishValidate(btn, progress, d.valid ?? validCount, invalidCount, poorCount, total);
        });

        evtSource.onerror = () => {
            evtSource.close();
            validateEvtSource = null;
            stopValidateRefresh();
            finishValidate(btn, progress, validCount, invalidCount, poorCount, total);
        };
    } catch (e) {
        toast('验证失败: ' + e.message, 'error');
        stopValidateRefresh();
        finishValidate(btn, progress, 0, 0, 0, 0);
    }
}

async function finishValidate(btn, progress, valid, invalid, poor, total) {
    isValidating = false;
    btn.disabled = false;
    btn.textContent = '🛡️ 检测书源';

    const spinner = document.getElementById('validateSpinner');
    if (spinner) spinner.style.display = 'none';

    const excellentCount = Object.values(sourceValidity).filter(v=>v==='excellent').length;
    const goodCount = Object.values(sourceValidity).filter(v=>v==='good').length;

    toast(`🛡️ 验证完成：⭐${excellentCount} ✅${goodCount} ⚠️${poor} ❌${invalid}`, valid > 0 ? 'success' : 'error');
    await refreshSources();
}

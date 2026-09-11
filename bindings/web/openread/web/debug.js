/**
 * debug.js — D5: 书源调试页
 * 选一个书源 + 输入测试关键词，SSE 分步拉取"搜索 → 目录 → 正文"，实时展示每步耗时/结果/错误。
 */

let _debugEvtSource = null;

function renderDebugPanel() {
    stopSourceDebug();
    const el = document.getElementById('results');
    if (!el) return;

    el.innerHTML = `
        <div class="section-title">🔧 书源调试</div>
        <p style="color:var(--text2);font-size:13px;margin-bottom:14px">
            选一个书源 + 输入测试关键词，实时查看搜索/目录/正文三步的 HTTP 请求、解析结果与耗时。
        </p>
        <div class="debug-toolbar">
            <select id="debugSourceSelect" class="debug-input">
                <option value="">正在加载书源…</option>
            </select>
            <button class="btn" onclick="loadDebugSources()">刷新书源</button>
            <input type="text" id="debugKeyword" class="debug-input" placeholder="测试关键词" value="我">
            <button class="btn btn-primary" onclick="runSourceDebug()">▶ 开始调试</button>
            <button class="btn" id="debugStopBtn" onclick="stopSourceDebug()" style="display:none">⏹ 停止</button>
        </div>
        <div id="debugSteps" style="display:flex;flex-direction:column;gap:12px"></div>
    `;
    loadDebugSources();
}

let _debugSourcesRequest = 0;
async function loadDebugSources() {
    const select = document.getElementById('debugSourceSelect');
    if (!select) return;
    const request = ++_debugSourcesRequest;
    const selected = select.value;
    select.disabled = true;
    select.innerHTML = '<option value="">正在加载书源…</option>';
    try {
        const response = await fetch(`${API}/api/sources`);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const data = await response.json();
        if (data.error) throw new Error(data.error);
        if (request !== _debugSourcesRequest || document.getElementById('debugSourceSelect') !== select) return;
        const available = (data.sources || []).filter(source => source.search_url);
        select.innerHTML = `<option value="">${available.length ? '-- 选择书源 --' : '暂无可搜索书源，请先导入'}</option>` +
            available.map(source => {
                const latency = typeof source.latency === 'number' && source.latency >= 0 ? ` · ${source.latency}ms` : '';
                return `<option value="${esc2(source.url)}">${esc2(source.name)}${latency}${source.validity ? ` [${esc2(source.validity)}]` : ''}</option>`;
            }).join('');
        if (available.some(source => source.url === selected)) select.value = selected;
    } catch (error) {
        if (request !== _debugSourcesRequest || document.getElementById('debugSourceSelect') !== select) return;
        select.innerHTML = `<option value="">书源加载失败：${esc2(error.message)}，请刷新书源</option>`;
    } finally {
        if (request === _debugSourcesRequest && document.getElementById('debugSourceSelect') === select) select.disabled = false;
    }
}

function stopSourceDebug() {
    if (_debugEvtSource) {
        _debugEvtSource.close();
        _debugEvtSource = null;
    }
    const btn = document.getElementById('debugStopBtn');
    if (btn) btn.style.display = 'none';
}

function _debugAppendStep(title, bodyHtml, color) {
    const root = document.getElementById('debugSteps');
    if (!root) return;
    const card = document.createElement('div');
    card.className = 'debug-step';
    card.innerHTML = `
        <div class="debug-step-title" style="color:${color || 'var(--accent2)'}">${title}</div>
        <div class="debug-step-body">${bodyHtml}</div>
    `;
    root.appendChild(card);
    card.scrollIntoView({ behavior: 'smooth', block: 'end' });
}

async function runSourceDebug() {
    const sourceUrl = document.getElementById('debugSourceSelect')?.value;
    const keyword = (document.getElementById('debugKeyword')?.value || '').trim() || '我';
    if (!sourceUrl) { toast('请先选择一个书源', 'error'); return; }

    // 清空旧结果
    const root = document.getElementById('debugSteps');
    if (root) root.innerHTML = '';
    stopSourceDebug();

    const btn = document.getElementById('debugStopBtn');
    if (btn) btn.style.display = '';

    const url = `${API}/api/source/debug?source_url=${encodeURIComponent(sourceUrl)}&q=${encodeURIComponent(keyword)}`;
    const es = new EventSource(url);
    _debugEvtSource = es;

    es.addEventListener('debug_info', (e) => {
        if (_debugEvtSource !== es) return;
        const d = JSON.parse(e.data);
        _debugAppendStep('ℹ️ 书源信息',
            `<div><b>名称：</b>${esc(d.sourceName || '')}</div>
             <div><b>URL：</b>${esc(d.sourceUrl || '')}</div>
             <div><b>searchUrl：</b><code>${esc(d.searchUrl || '')}</code></div>`,
            'var(--blue)');
    });

    es.addEventListener('debug_http', (e) => {
        if (_debugEvtSource !== es) return;
        const d = JSON.parse(e.data);
        _debugAppendStep('HTTP 请求',
            `<pre>${esc2(d.method || 'GET')} ${esc2(d.url || '')}\n状态：${esc2(String(d.status))} · ${esc2(String(d.elapsedMs))}ms · ${esc2(String(d.bytes))} 字节${d.error ? '\n' + esc2(d.error) : ''}</pre>`,
            d.error ? 'var(--red)' : 'var(--blue)');
    });

    es.addEventListener('debug_search', (e) => {
        if (_debugEvtSource !== es) return;
        const d = JSON.parse(e.data);
        if (d.error) {
            _debugAppendStep('❌ 搜索失败', `<pre>${esc(d.error)}</pre>`, 'var(--red)');
            return;
        }
        const sample = (d.sample || []).map((b, i) =>
            `<li><b>${i + 1}. ${esc(b.name)}</b> · ${esc(b.author || '未知')}<br>
             <span style="color:var(--text2)">${esc(b.url || '').substring(0, 120)}</span></li>`
        ).join('');
        _debugAppendStep(`🔍 搜索结果（${d.count} 条，${d.elapsedMs}ms）`,
            `<ul style="margin-left:20px">${sample || '<li>无样本</li>'}</ul>`,
            'var(--accent2)');
    });

    es.addEventListener('debug_catalog', (e) => {
        if (_debugEvtSource !== es) return;
        const d = JSON.parse(e.data);
        if (d.error) {
            _debugAppendStep('❌ 目录获取失败', `<pre>${esc(d.error)}</pre>`, 'var(--red)');
            return;
        }
        const sample = (d.sample || []).map(c =>
            `<li>[${c.index}] ${esc(c.title || '(空)')}<br>
             <span style="color:var(--text2)">${esc(c.url || '').substring(0, 120)}</span></li>`
        ).join('');
        _debugAppendStep(`📋 目录（《${esc(d.bookName || '')}》${d.count} 章，${d.elapsedMs}ms）`,
            `<ul style="margin-left:20px">${sample || '<li>无样本</li>'}</ul>`,
            'var(--accent2)');
    });

    es.addEventListener('debug_content', (e) => {
        if (_debugEvtSource !== es) return;
        const d = JSON.parse(e.data);
        if (d.error) {
            _debugAppendStep('❌ 正文获取失败', `<pre>${esc(d.error)}</pre>`, 'var(--red)');
            return;
        }
        _debugAppendStep(`正文（${esc2(d.chapterTitle || '')}，${d.length} 字节，${d.elapsedMs}ms）`,
            `<pre style="max-height:280px;overflow:auto;white-space:pre-wrap">${esc2(d.preview || '')}${d.truncated ? '…' : ''}</pre>`,
            'var(--green)');
    });

    es.addEventListener('debug_done', () => {
        if (_debugEvtSource !== es) return;
        _debugAppendStep('✅ 调试完成', '<div style="color:var(--text2)">全部步骤已执行</div>', 'var(--green)');
        stopSourceDebug();
    });

    es.addEventListener('debug_error', (e) => {
        if (_debugEvtSource !== es) return;
        const d = JSON.parse(e.data);
        const stages = {debug_search: '搜索', debug_catalog: '目录', debug_content: '正文'};
        _debugAppendStep(`${stages[d.stage] || '调试'}失败`, `<pre>${esc2(d.error || '未知错误')}</pre>`, 'var(--red)');
        stopSourceDebug();
    });

    es.onerror = () => {
        if (_debugEvtSource !== es) return;
        _debugAppendStep('连接中断', '<div>调试连接已中断，请检查服务状态或重新选择书源。</div>', 'var(--red)');
        stopSourceDebug();
    };
}

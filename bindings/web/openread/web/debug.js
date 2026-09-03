/**
 * debug.js — D5: 书源调试页
 * 选一个书源 + 输入测试关键词，SSE 分步拉取"搜索 → 目录 → 正文"，实时展示每步耗时/结果/错误。
 */

let _debugEvtSource = null;

function renderDebugPanel() {
    const el = document.getElementById('results');
    if (!el) return;

    // 从 sourcesData 过滤出有 searchUrl 的书源
    const available = (sourcesData || []).filter(s => s.search_url);
    const optionsHtml = available.map(s => {
        const latency = typeof s.latency === 'number' ? ` · ${s.latency}ms` : '';
        const grade = s.validity ? ` [${s.validity}]` : '';
        return `<option value="${esc2(s.name)}">${esc(s.name)}${latency}${grade}</option>`;
    }).join('');

    el.innerHTML = `
        <div class="section-title">🔧 书源调试</div>
        <p style="color:var(--text2);font-size:13px;margin-bottom:14px">
            选一个书源 + 输入测试关键词，实时查看搜索/目录/正文三步的 HTTP 请求、解析结果与耗时。
        </p>
        <div class="debug-toolbar">
            <select id="debugSourceSelect" class="debug-input">
                <option value="">-- 选择书源 --</option>
                ${optionsHtml}
            </select>
            <input type="text" id="debugKeyword" class="debug-input" placeholder="测试关键词" value="我">
            <button class="btn btn-primary" onclick="runSourceDebug()">▶ 开始调试</button>
            <button class="btn" id="debugStopBtn" onclick="stopSourceDebug()" style="display:none">⏹ 停止</button>
        </div>
        <div id="debugSteps" style="display:flex;flex-direction:column;gap:12px"></div>
    `;
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
    const sourceName = document.getElementById('debugSourceSelect')?.value;
    const keyword = (document.getElementById('debugKeyword')?.value || '').trim() || '我';
    if (!sourceName) { toast('请先选择一个书源', 'error'); return; }

    // 清空旧结果
    const root = document.getElementById('debugSteps');
    if (root) root.innerHTML = '';
    stopSourceDebug();

    const btn = document.getElementById('debugStopBtn');
    if (btn) btn.style.display = '';

    const url = `${API}/api/source/debug?source_name=${encodeURIComponent(sourceName)}&q=${encodeURIComponent(keyword)}`;
    const es = new EventSource(url);
    _debugEvtSource = es;

    es.addEventListener('debug_info', (e) => {
        const d = JSON.parse(e.data);
        _debugAppendStep('ℹ️ 书源信息',
            `<div><b>名称：</b>${esc(d.sourceName || '')}</div>
             <div><b>URL：</b>${esc(d.sourceUrl || '')}</div>
             <div><b>searchUrl：</b><code>${esc(d.searchUrl || '')}</code></div>`,
            'var(--blue)');
    });

    es.addEventListener('debug_search', (e) => {
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
        const d = JSON.parse(e.data);
        if (d.error) {
            _debugAppendStep('❌ 正文获取失败', `<pre>${esc(d.error)}</pre>`, 'var(--red)');
            return;
        }
        _debugAppendStep(`📖 正文（${esc(d.chapterTitle || '')}，${d.length} 字，${d.elapsedMs}ms）`,
            `<pre style="max-height:280px;overflow:auto;white-space:pre-wrap">${esc(d.preview || '')}…</pre>`,
            'var(--green)');
    });

    es.addEventListener('debug_done', () => {
        _debugAppendStep('✅ 调试完成', '<div style="color:var(--text2)">全部步骤已执行</div>', 'var(--green)');
        stopSourceDebug();
    });

    es.addEventListener('debug_error', (e) => {
        const d = JSON.parse(e.data);
        _debugAppendStep('❌ 调试出错', `<pre>${esc(d.error || '未知错误')}</pre>`, 'var(--red)');
        stopSourceDebug();
    });

    es.onerror = () => {
        stopSourceDebug();
    };
}

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const elements = new Map();
const element = id => {
    if (!elements.has(id)) elements.set(id, {
        value: '', textContent: '', innerHTML: '', disabled: false, style: {},
        appendChild() {}, scrollIntoView() {},
    });
    return elements.get(id);
};
class FakeEventSource {
    static instances = [];
    constructor(url) {
        this.url = url;
        this.handlers = {};
        this.closed = false;
        FakeEventSource.instances.push(this);
    }
    addEventListener(name, handler) { this.handlers[name] = handler; }
    close() { this.closed = true; }
    emit(name, data = {}) { this.handlers[name]?.({data: JSON.stringify(data)}); }
}
const context = vm.createContext({
    document: {
        getElementById: element,
        querySelectorAll: () => [],
        createElement: () => element('created'),
    },
    localStorage: {setItem() {}},
    EventSource: FakeEventSource,
    syncActiveReadingSessionGlobals() {},
    fetch: async () => ({json: async () => ({ok: true, result: '', logs: ['hello'], elapsedMs: 2})}),
});
const root = path.resolve(__dirname, '../bindings/web/ariaread/web');
const app = fs.readFileSync(path.join(root, 'app.js'), 'utf8').replace(/\ninit\(\);\s*$/, '\n');
vm.runInContext(app, context);
vm.runInContext(fs.readFileSync(path.join(root, 'debug.js'), 'utf8'), context);
const cards = [];
context._debugAppendStep = (...args) => cards.push(args);

(async () => {
    element('debugSourceSelect').value = 'https://source.test/?x=1&y=2';
    element('debugKeyword').value = '测试';
    await context.runSourceDebug();
    const first = FakeEventSource.instances.at(-1);
    assert.match(first.url, /source_url=https%3A%2F%2Fsource.test/);
    await context.runSourceDebug();
    const second = FakeEventSource.instances.at(-1);
    assert.equal(first.closed, true);
    first.emit('debug_done');
    first.onerror();
    assert.equal(second.closed, false);
    assert.equal(cards.length, 0);
    second.emit('debug_http', {url: '<script>', method: 'GET', status: 200, elapsedMs: 1, bytes: 2});
    assert.match(cards.at(-1)[1], /&lt;script&gt;/);
    second.emit('debug_error', {stage: 'debug_catalog', error: '<bad>'});
    assert.equal(cards.at(-1)[0], '目录失败');
    assert.match(cards.at(-1)[1], /&lt;bad&gt;/);
    assert.equal(second.closed, true);
    await context.runSourceDebug();
    const third = FakeEventSource.instances.at(-1);
    context.switchTab('console');
    assert.equal(third.closed, true);
    element('jsInput').value = "console.log('hello')";
    await context.evalJs();
    assert.match(element('jsOutput').textContent, /hello/);
    assert.match(element('jsOutput').textContent, /耗时：2ms/);
    assert.equal(element('jsRunBtn').disabled, false);
    context.fetch = async () => ({json: async () => ({ok: false, error: 'long error', errorTruncated: true})});
    await context.evalJs();
    assert.match(element('jsOutput').textContent, /错误：long error/);
    assert.match(element('jsOutput').textContent, /\[错误已截断\]/);
    context.fetch = async () => { throw new Error('offline'); };
    await context.evalJs();
    assert.match(element('jsOutput').textContent, /offline/);
    assert.equal(element('jsRunBtn').disabled, false);
    // The debug panel must work before the global source cache is populated.
    vm.runInContext('sourcesData = []', context);
    let deliver;
    context.fetch = () => new Promise(resolve => { deliver = resolve; });
    const loading = context.loadDebugSources();
    assert.equal(element('debugSourceSelect').disabled, true);
    deliver({ok: true, json: async () => ({sources: [
        {url: 'https://source.test', name: '<Source>', search_url: '/search', latency: 12},
        {url: 'https://browse.test', name: 'Browse only', search_url: ''},
    ]})});
    await loading;
    assert.match(element('debugSourceSelect').innerHTML, /&lt;Source&gt;/);
    assert.doesNotMatch(element('debugSourceSelect').innerHTML, /Browse only/);
    context.fetch = async () => { throw new Error('offline'); };
    await context.loadDebugSources();
    assert.match(element('debugSourceSelect').innerHTML, /书源加载失败/);
    assert.equal(element('debugSourceSelect').disabled, false);
    context.fetch = async () => ({ok: true, json: async () => ({sources: []})});
    await context.loadDebugSources();
    assert.match(element('debugSourceSelect').innerHTML, /暂无可搜索书源/);
    console.log('调试页面会话隔离、HTML 转义、控制台日志与错误恢复测试通过');
})().catch(error => { console.error(error); process.exitCode = 1; });

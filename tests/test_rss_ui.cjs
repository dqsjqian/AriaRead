const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const nodes = new Map();
const node = id => {
    if (!nodes.has(id)) nodes.set(id, {innerHTML:'',style:{},textContent:''});
    return nodes.get(id);
};
const escape = value => String(value ?? '').replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;').replaceAll('"','&quot;').replaceAll("'",'&#39;');
const ctx = vm.createContext({document:{getElementById:node},API:'',esc:escape,esc2:escape,stripHtml:s=>s,toast(){},console});
vm.runInContext(fs.readFileSync(path.join(__dirname,'../bindings/web/ariaread/web/rss.js'),'utf8'),ctx);
const run = text => vm.runInContext(text,ctx);
(async()=>{
    run(`rssCurrentSource={sourceUrl:'https://a.test',sourceName:'A'}; rssArticlesData=[{id:1,title:'A'},{id:2,title:'B'}]`);
    const pending = [];
    ctx.fetch = () => new Promise(resolve=>pending.push(resolve));
    const first = ctx.openRssArticle(1);
    assert.match(node('results').innerHTML,/正在加载正文/);
    const second = ctx.openRssArticle(2);
    pending[1]({ok:true,json:async()=>({id:2,title:'B',link:'https://b.test',content:'<p>Hi</p>'})}); await second;
    pending[0]({ok:true,json:async()=>({id:1,title:'A',content:'old'})}); await first;
    assert.equal(run('rssCurrentArticle.id'),2);
    assert.match(node('results').innerHTML,/Hi/);
    assert.match(node('results').innerHTML,/srcdoc=/);
    assert.doesNotMatch(node('results').innerHTML,/allow-same-origin|<iframe[^>]*\ssrc=/);
    ctx.fetch = async()=>({ok:true,json:async()=>({id:2,title:'B',link:'https://b.test',content:'',contentError:'HTTP 403'})});
    await ctx.openRssArticle(2);
    assert.match(node('results').innerHTML,/HTTP 403/); assert.match(node('results').innerHTML,/重试/);
    ctx.fetch = async()=>({ok:true,json:async()=>({id:2,title:'B',content:'Recovered'})});
    await ctx.openRssArticle(2);
    assert.match(node('results').innerHTML,/Recovered/); assert.doesNotMatch(node('results').innerHTML,/HTTP 403/);
    // Opening a source delegates cache/fetch policy to the portable engine.
    run(`rssCurrentView='articles'; rssCurrentSource={sourceUrl:'https://empty.test',sourceName:'Empty',lastUpdateTime:123};`);
    ctx.fetch = async(url,options)=>{
        assert.equal(options,undefined);
        assert.match(url,/load=1/);
        return {ok:true,json:async()=>({articles:[{id:3,title:'Fetched'}],total:1})};
    };
    await ctx.loadRssArticles('https://empty.test',1,true);
    assert.match(node('rssArticleList').innerHTML,/Fetched/);
    ctx.fetch = async()=>({ok:true,json:async()=>({articles:[{id:3,title:'Cached'}],total:1,error:'Channel failed'})});
    await ctx.loadRssArticles('https://empty.test',1,true);
    assert.match(node('rssArticleList').innerHTML,/Cached/);
    assert.match(node('rssArticleList').innerHTML,/Channel failed/);
    // Slow list A cannot overwrite a later list B.
    ctx.fetch = () => new Promise(resolve=>pending.push(resolve));
    run(`rssCurrentSource={sourceUrl:'https://a.test'}`);
    const listA=ctx.loadRssArticles('https://a.test');
    run(`rssCurrentSource={sourceUrl:'https://b.test'}`);
    const listB=ctx.loadRssArticles('https://b.test');
    pending[3]({ok:true,json:async()=>({articles:[{id:4,title:'New B'}]})}); await listB;
    pending[2]({ok:true,json:async()=>({articles:[{id:5,title:'Old A'}]})}); await listA;
    assert.match(node('rssArticleList').innerHTML,/New B/); assert.doesNotMatch(node('rssArticleList').innerHTML,/Old A/);
    // Switching away from RSS must invalidate pending article requests.
    ctx.fetch = () => new Promise(resolve=>pending.push(resolve));
    const away=ctx.openRssArticle(4);
    ctx.invalidateRssNavigation();
    node('results').innerHTML='Search tab';
    pending[4]({ok:true,json:async()=>({id:4,title:'Too late',content:'Old RSS'})}); await away;
    assert.equal(node('results').innerHTML,'Search tab');
    console.log('RSS reader, recovery, empty cache and navigation races passed');
})().catch(error=>{console.error(error);process.exitCode=1});

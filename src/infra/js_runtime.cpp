#include "openread/js_runtime.h"
#include <quickjs.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <map>
#include <mutex>

namespace openread {

using json = nlohmann::json;

// ──────────────────────────────────────────────
// QuickJS 运行时实现
// ──────────────────────────────────────────────
class JsRuntime::Impl {
public:
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    std::string lastError;
    JsLogFunc logFunc;
    std::chrono::milliseconds executionTimeout{10000};
    std::chrono::steady_clock::time_point deadline;
    unsigned int executionDepth = 0;
    bool timedOut = false;
    bool interrupted = false;
    std::function<bool()> interruptCallback;

    static int interruptHandler(JSRuntime*, void* opaque) {
        auto* impl = static_cast<Impl*>(opaque);
        if (impl->executionDepth == 0) return 0;
        if (std::chrono::steady_clock::now() >= impl->deadline) {
            impl->timedOut = true;
        }
        if (!impl->timedOut && !impl->interrupted && impl->interruptCallback) {
            try {
                impl->interrupted = impl->interruptCallback();
            } catch (...) {
                impl->interrupted = true;
            }
        }
        return impl->timedOut || impl->interrupted ? 1 : 0;
    }

    struct ExecutionScope {
        Impl& impl;
        explicit ExecutionScope(Impl& runtime) : impl(runtime) {
            if (impl.executionDepth++ == 0) {
                impl.deadline = std::chrono::steady_clock::now() + impl.executionTimeout;
                impl.timedOut = false;
                impl.interrupted = false;
                impl.lastError.clear();
            }
        }
        ~ExecutionScope() { --impl.executionDepth; }
    };

    std::string interruptionError() const {
        return timedOut ? "JavaScript execution timed out"
                        : "JavaScript execution interrupted";
    }

    void captureException() {
        JSValue exception = JS_GetException(ctx);
        const char* str = (timedOut || interrupted) ? nullptr : JS_ToCString(ctx, exception);
        lastError = (timedOut || interrupted) ? interruptionError()
                                             : (str ? str : "Unknown JS error");
        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, exception);
        // An exception can itself throw (or time out) while being converted to text.
        if (JS_HasException(ctx)) {
            JSValue formattingException = JS_GetException(ctx);
            JS_FreeValue(ctx, formattingException);
        }
    }

    // 真实绑定
    JsRuntime::HttpFunc httpFunc;
    std::function<std::string(const std::string&)> varGetter;
    std::function<void(const std::string&, const std::string&)> varSetter;
    std::map<std::string, std::string> varTable;   // 内置变量表（无外部 accessors 时）
    mutable std::mutex varMutex;

    // 选择器桥接（java.getString/getElements 用）
    JsRuntime::SelectorFunc selectorFunc;
    std::string currentContent;

    Impl() {
        rt = JS_NewRuntime();
        if (!rt) {
            throw std::runtime_error("Failed to create QuickJS runtime");
        }

        ctx = JS_NewContext(rt);
        if (!ctx) {
            JS_FreeRuntime(rt);
            throw std::runtime_error("Failed to create QuickJS context");
        }

        // 默认内存限制
        JS_SetMemoryLimit(rt, 256 * 1024 * 1024);  // 256MB
        JS_SetMaxStackSize(rt, 4 * 1024 * 1024);    // 4MB

        JS_SetInterruptHandler(rt, interruptHandler, this);
        JS_SetContextOpaque(ctx, this);
        registerNativeLog();
        registerNativeBridges();
        initConsole();
    }

    ~Impl() {
        if (ctx) JS_FreeContext(ctx);
        if (rt) JS_FreeRuntime(rt);
    }

    void initConsole() {
        const char* consoleJs = R"(
            var console = {
                log: function() {
                    var msg = Array.prototype.slice.call(arguments).join(' ');
                    if (typeof __nativeLog === 'function') {
                        __nativeLog(msg);
                    }
                },
                error: function() {
                    var msg = '[ERROR] ' + Array.prototype.slice.call(arguments).join(' ');
                    if (typeof __nativeLog === 'function') {
                        __nativeLog(msg);
                    }
                },
                warn: function() {
                    var msg = '[WARN] ' + Array.prototype.slice.call(arguments).join(' ');
                    if (typeof __nativeLog === 'function') {
                        __nativeLog(msg);
                    }
                }
            };
        )";

        JS_Eval(ctx, consoleJs, strlen(consoleJs), "<console>", JS_EVAL_TYPE_GLOBAL);
    }

    /// 注册 __nativeLog 函数，让 JS 的 console.log / java.log 输出到 C++
    void registerNativeLog() {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue fn = JS_NewCFunction(ctx, &Impl::jsNativeLog, "__nativeLog", 1);
        JS_SetPropertyStr(ctx, global, "__nativeLog", fn);
        JS_FreeValue(ctx, global);
    }

    /// C 回调：__nativeLog(msg) → logFunc
    static JSValue jsNativeLog(JSContext* c, JSValueConst /*this_val*/,
                               int argc, JSValueConst* argv) {
        auto* self = static_cast<Impl*>(JS_GetContextOpaque(c));
        if (self && self->logFunc && argc >= 1) {
            const char* s = JS_ToCString(c, argv[0]);
            if (s) {
                self->logFunc(s);
                JS_FreeCString(c, s);
            }
        }
        return JS_UNDEFINED;
    }

    // ──────────────────────────────────────────────
    // 真实桥接：__http / __varget / __varput
    // 在 JS 侧由 stubJava 包装成 java.ajax/get/post/getString + @get/@put
    // ──────────────────────────────────────────────
    void registerNativeBridges() {
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "__http",
                          JS_NewCFunction(ctx, &Impl::jsHttp, "__http", 4));
        JS_SetPropertyStr(ctx, global, "__varget",
                          JS_NewCFunction(ctx, &Impl::jsVarGet, "__varget", 1));
        JS_SetPropertyStr(ctx, global, "__varput",
                          JS_NewCFunction(ctx, &Impl::jsVarPut, "__varput", 2));
        JS_SetPropertyStr(ctx, global, "__select",
                          JS_NewCFunction(ctx, &Impl::jsSelect, "__select", 2));
        JS_FreeValue(ctx, global);
    }

    /// __select(content, rule) → JSON 数组字符串（解析结果列表）。
    /// content 为空时使用 currentContent。
    static JSValue jsSelect(JSContext* c, JSValueConst /*this_val*/,
                            int argc, JSValueConst* argv) {
        auto* self = static_cast<Impl*>(JS_GetContextOpaque(c));
        if (!self || !self->selectorFunc) return JS_NewString(c, "[]");
        auto arg = [&](int i) -> std::string {
            if (i >= argc) return "";
            const char* s = JS_ToCString(c, argv[i]);
            std::string r = s ? s : "";
            if (s) JS_FreeCString(c, s);
            return r;
        };
        std::string content = arg(0);
        std::string rule = arg(1);
        if (content.empty()) content = self->currentContent;
        std::vector<std::string> out;
        try {
            out = self->selectorFunc(content, rule);
        } catch (...) {}
        json j = json::array();
        for (auto& s : out) j.push_back(s);
        return JS_NewString(c, j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
    }

    /// __http(url, method, headersJson, body) → 响应体字符串
    static JSValue jsHttp(JSContext* c, JSValueConst /*this_val*/,
                          int argc, JSValueConst* argv) {
        auto* self = static_cast<Impl*>(JS_GetContextOpaque(c));
        if (!self || !self->httpFunc) return JS_NewString(c, "");

        auto arg = [&](int i) -> std::string {
            if (i >= argc) return "";
            const char* s = JS_ToCString(c, argv[i]);
            std::string r = s ? s : "";
            if (s) JS_FreeCString(c, s);
            return r;
        };
        std::string url = arg(0);
        std::string method = arg(1); if (method.empty()) method = "GET";
        std::string headers = arg(2); if (headers.empty()) headers = "{}";
        std::string body = arg(3);

        std::string resp;
        try { resp = self->httpFunc(url, method, headers, body); } catch (...) { resp = ""; }
        return JS_NewString(c, resp.c_str());
    }

    /// __varget(key) → value
    static JSValue jsVarGet(JSContext* c, JSValueConst /*this_val*/,
                            int argc, JSValueConst* argv) {
        auto* self = static_cast<Impl*>(JS_GetContextOpaque(c));
        if (!self || argc < 1) return JS_NewString(c, "");
        const char* k = JS_ToCString(c, argv[0]);
        std::string key = k ? k : "";
        if (k) JS_FreeCString(c, k);
        std::string val;
        if (self->varGetter) val = self->varGetter(key);
        else {
            std::lock_guard<std::mutex> lk(self->varMutex);
            auto it = self->varTable.find(key);
            if (it != self->varTable.end()) val = it->second;
        }
        return JS_NewString(c, val.c_str());
    }

    /// __varput(key, value)
    static JSValue jsVarPut(JSContext* c, JSValueConst /*this_val*/,
                            int argc, JSValueConst* argv) {
        auto* self = static_cast<Impl*>(JS_GetContextOpaque(c));
        if (!self || argc < 2) return JS_UNDEFINED;
        const char* k = JS_ToCString(c, argv[0]);
        const char* v = JS_ToCString(c, argv[1]);
        std::string key = k ? k : "";
        std::string val = v ? v : "";
        if (k) JS_FreeCString(c, k);
        if (v) JS_FreeCString(c, v);
        if (self->varSetter) self->varSetter(key, val);
        else {
            std::lock_guard<std::mutex> lk(self->varMutex);
            self->varTable[key] = val;
        }
        return JS_UNDEFINED;
    }

    /// 将 JSValue 转为字符串。
    /// - null / undefined：返回空串
    /// - 数组 / 对象：JSON.stringify
    /// - 其他（数字/布尔）：转字符串
    std::string valueToString(JSValue v) {
        if (JS_IsString(v)) {
            const char* s = JS_ToCString(ctx, v);
            std::string r = s ? s : "";
            JS_FreeCString(ctx, s);
            return r;
        }
        if (JS_IsNull(v) || JS_IsUndefined(v)) {
            return "";
        }
        if (JS_IsObject(v)) {
            // 用 JSON.stringify 序列化（取全局 JSON.stringify）
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue jsonObj = JS_GetPropertyStr(ctx, global, "JSON");
            JS_FreeValue(ctx, global);
            if (JS_IsException(jsonObj)) return "";
            JSValue stringify = JS_GetPropertyStr(ctx, jsonObj, "stringify");
            if (JS_IsException(stringify)) {
                JS_FreeValue(ctx, jsonObj);
                return "";
            }
            JSValue argv[1] = { JS_DupValue(ctx, v) };
            JSValue r = JS_Call(ctx, stringify, jsonObj, 1, argv);
            std::string out;
            if (!JS_IsException(r) && JS_IsString(r)) {
                const char* s = JS_ToCString(ctx, r);
                out = s ? s : "";
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, argv[0]);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, stringify);
            JS_FreeValue(ctx, jsonObj);
            return out;
        }
        // 数字 / 布尔等
        const char* s = JS_ToCString(ctx, v);
        std::string r = s ? s : "";
        JS_FreeCString(ctx, s);
        return r;
    }
};

// ──────────────────────────────────────────────
// 构造/析构
// ──────────────────────────────────────────────
JsRuntime::JsRuntime() : pImpl(std::make_unique<Impl>()) {}

JsRuntime::~JsRuntime() = default;

// ──────────────────────────────────────────────
// 执行 JS
// ──────────────────────────────────────────────
std::string JsRuntime::eval(const std::string& code,
                             const std::string& filename) {
    Impl::ExecutionScope scope(*pImpl);
    if (Impl::interruptHandler(pImpl->rt, pImpl.get())) {
        pImpl->lastError = pImpl->interruptionError();
        return "";
    }
    JSValue result = JS_Eval(pImpl->ctx, code.c_str(), code.length(),
                             filename.c_str(), JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(result)) {
        pImpl->captureException();
        JS_FreeValue(pImpl->ctx, result);
        return "";
    }

    std::string ret = pImpl->valueToString(result);
    JS_FreeValue(pImpl->ctx, result);
    if (JS_HasException(pImpl->ctx)) {
        pImpl->captureException();
        return "";
    }
    if (Impl::interruptHandler(pImpl->rt, pImpl.get())) {
        pImpl->lastError = pImpl->interruptionError();
        return "";
    }

    return ret;
}

// ──────────────────────────────────────────────
// 执行书源规则解析 JS（上下文对齐 legado AnalyzeRule.evalJS）
// ──────────────────────────────────────────────
std::string JsRuntime::evalRuleJs(const std::string& jsCode,
                                  const std::string& result,
                                  const std::string& baseUrl,
                                  const std::string& key,
                                  int page) {
    Impl::ExecutionScope scope(*pImpl);
    if (jsCode.empty()) return result;

    // java / source —— 与 legado JsExtensions 行为对齐。
    // ajax/get/post/getString 通过 __http 原生桥接发起真实请求；
    // java.put/get 走 __varput/__varget（@put/@get 变量表）。
    static const char* stubJava = R"(
var __putResult = undefined;
function __mkHeaders(h){ try { return (h && typeof h==='object') ? JSON.stringify(h) : (h||'{}'); } catch(e){ return '{}'; } }
// ── base64（UTF-8 安全）──
var __B64='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function __utf8Encode(str){
  var out=[];
  for(var i=0;i<str.length;i++){
    var c=str.charCodeAt(i);
    if(c<0x80){out.push(c);}
    else if(c<0x800){out.push(0xC0|(c>>6),0x80|(c&0x3F));}
    else if(c>=0xD800&&c<0xDC00&&i+1<str.length){
      var c2=str.charCodeAt(++i); var cp=0x10000+((c&0x3FF)<<10)+(c2&0x3FF);
      out.push(0xF0|(cp>>18),0x80|((cp>>12)&0x3F),0x80|((cp>>6)&0x3F),0x80|(cp&0x3F));
    } else {out.push(0xE0|(c>>12),0x80|((c>>6)&0x3F),0x80|(c&0x3F));}
  }
  return out;
}
function __utf8Decode(bytes){
  var out='',i=0;
  while(i<bytes.length){
    var c=bytes[i++];
    if(c<0x80){out+=String.fromCharCode(c);}
    else if(c>=0xC0&&c<0xE0){out+=String.fromCharCode(((c&0x1F)<<6)|(bytes[i++]&0x3F));}
    else if(c>=0xE0&&c<0xF0){out+=String.fromCharCode(((c&0x0F)<<12)|((bytes[i++]&0x3F)<<6)|(bytes[i++]&0x3F));}
    else {var cp=((c&0x07)<<18)|((bytes[i++]&0x3F)<<12)|((bytes[i++]&0x3F)<<6)|(bytes[i++]&0x3F);
      cp-=0x10000; out+=String.fromCharCode(0xD800+(cp>>10),0xDC00+(cp&0x3FF));}
  }
  return out;
}
function __b64encode(str){
  var b=__utf8Encode(''+str),out='';
  for(var i=0;i<b.length;i+=3){
    var n=(b[i]<<16)|((i+1<b.length?b[i+1]:0)<<8)|(i+2<b.length?b[i+2]:0);
    out+=__B64[(n>>18)&63]+__B64[(n>>12)&63];
    out+=(i+1<b.length)?__B64[(n>>6)&63]:'=';
    out+=(i+2<b.length)?__B64[n&63]:'=';
  }
  return out;
}
function __b64decode(str){
  var s=(''+str).replace(/[^A-Za-z0-9+/=]/g,''),bytes=[];
  for(var i=0;i<s.length;i+=4){
    var e=[__B64.indexOf(s[i]),__B64.indexOf(s[i+1]),__B64.indexOf(s[i+2]),__B64.indexOf(s[i+3])];
    var n=(e[0]<<18)|(e[1]<<12)|((e[2]&63)<<6)|(e[3]&63);
    bytes.push((n>>16)&0xFF);
    if(s[i+2]!=='='&&e[2]>=0)bytes.push((n>>8)&0xFF);
    if(s[i+3]!=='='&&e[3]>=0)bytes.push(n&0xFF);
  }
  return __utf8Decode(bytes);
}
function __selArr(rule, content){
  try { return JSON.parse(__select(content===undefined?'':(''+content), ''+rule)); } catch(e){ return []; }
}
var java = {
  put: function(k, v) {
    if (k === 'result' || k === 'url') __putResult = v;
    try { __varput('' + k, (v===null||v===undefined)?'':(''+v)); } catch(e){}
    return v;
  },
  get: function(k) { try { return __varget('' + k); } catch(e){ return ''; } },
  ajax: function(url) {
    var u = (url instanceof Array) ? url[0] : url;
    try { return __http('' + u, 'GET', '{}', ''); } catch(e){ return ''; }
  },
  post: function(url, body, headers) {
    try { return __http('' + url, 'POST', __mkHeaders(headers), body?(''+body):''); } catch(e){ return ''; }
  },
  getString: function(urlOrRule, content) {
    // legado 双义：若像 URL 则发 HTTP；否则当作选择器对当前/传入内容求值。
    var s = (urlOrRule instanceof Array) ? urlOrRule[0] : urlOrRule; s=''+s;
    if (/^https?:\/\//i.test(s)) { try { return __http(s, 'GET', '{}', ''); } catch(e){ return ''; } }
    var a = __selArr(s, content); return a.length? (''+a[0]) : '';
  },
  getStrings: function(rule, content){ return __selArr(''+rule, content); },
  getElements: function(rule, content){ return __selArr(''+rule, content); },
  getElement: function(rule, content){ var a=__selArr(''+rule, content); return a.length?(''+a[0]):''; },
  ajaxAll: function(urlList) {
    var out = [];
    if (urlList instanceof Array) {
      for (var i=0;i<urlList.length;i++){ try { out.push(__http(''+urlList[i],'GET','{}','')); } catch(e){ out.push(''); } }
    }
    return out;
  },
  connect: function(url) { try { return __http('' + url, 'GET', '{}', ''); } catch(e){ return ''; } },
  log: function(msg) { if (typeof __nativeLog === 'function') __nativeLog('' + msg); return msg; },
  toString: function(x) { return (x === null || x === undefined) ? '' : ('' + x); },
  base64Decode: function(s){ try { return __b64decode(s); } catch(e){ return ''; } },
  base64Encode: function(s){ try { return __b64encode(s); } catch(e){ return ''; } },
  base64DecodeToByteArray: function(s){ try { return __b64decode(s); } catch(e){ return ''; } },
  encodeURI: function(s, enc){ try { return encodeURIComponent(''+s); } catch(e){ return ''+s; } },
  toURL: function(s){ return ''+s; },
  timeFormat: function(t){ try { return new Date(parseInt(t)).toISOString(); } catch(e){ return ''+t; } },
  md5Encode: function(s){ return ''+s; },
  digestHex: function(s){ return ''+s; },
  cache: { get: function(k) { try { return __varget('cache_' + k); } catch(e){ return ''; } },
           put: function(k, v) { try { __varput('cache_' + k, '' + v); } catch(e){} } },
  cookie: { getCookie: function(k) { return ''; }, setCookie: function(k, v) { } },
  getByteArray: function(urlStr) { return []; }
};
var source = { get: function(k){ try { return __varget('source_' + k); } catch(e){ return ''; } },
               put: function(k,v){ try { __varput('source_' + k, '' + v); } catch(e){} },
               getVariable: function(){ try { return __varget('source_variable'); } catch(e){ return ''; } },
               key:'', bookSourceUrl:'', bookSourceName:'', sourceComment:'' };
var cookie = java.cookie;
var cache = java.cache;
)";

    // 读取已透传的 sourceComment（优先外部 accessor，回退内置变量表）
    std::string srcComment;
    if (pImpl->varGetter) {
        srcComment = pImpl->varGetter("source_sourceComment");
    } else {
        std::lock_guard<std::mutex> lk(pImpl->varMutex);
        auto it = pImpl->varTable.find("source_sourceComment");
        if (it != pImpl->varTable.end()) srcComment = it->second;
    }

    std::string wrappedCtx =
        "var baseUrl = " + json(baseUrl).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
        "var key = " + json(key).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
        "var page = " + std::to_string(page) + ";\n"
        "var result = " + json(result).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
        "var src = result;\n"
        + std::string(stubJava) + "\n"
        "source.key = " + json(key).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
        "source.bookSourceUrl = " + json(baseUrl).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
        // sourceComment（书源/订阅源备注）：很多 legado 复杂源把核心变量定义放在
        // sourceComment 里，规则 JS 首句 eval(String(source.sourceComment)) 引导。
        // 此前硬编码为空，导致这类源整段逻辑失效，这里从变量表透传真实值。
        "source.sourceComment = " + json(srcComment).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n"
        "__putResult = undefined;\n"
        "undefined;\n";

    // 第一步：注入上下文（全局变量在同一 ctx 中持久）
    pImpl->currentContent = result;   // java.getString/getElements 默认查询此内容
    eval(wrappedCtx, "<ruleJsCtx>");
    if (!pImpl->lastError.empty()) return "";

    // 第二步：直接求值用户脚本。QuickJS GLOBAL 模式返回最后一个表达式/语句的值，
    // 因此 legado 风格的"最后一个表达式即结果"无需显式 return。
    std::string scriptOut = eval(jsCode, "<ruleJs>");
    if (!pImpl->lastError.empty()) return "";

    // 第三步：优先级——脚本返回非空 > java.put('result',..) > 原 result
    if (!scriptOut.empty()) {
        return scriptOut;
    }
    std::string putResult = eval(
        "(__putResult !== undefined && __putResult !== null) ? ('' + __putResult) : ''",
        "<ruleJsPut>");
    if (!pImpl->lastError.empty()) return "";
    if (!putResult.empty()) {
        return putResult;
    }
    return result;
}

// ──────────────────────────────────────────────
// 注入全局函数
// ──────────────────────────────────────────────
void JsRuntime::injectFunction(const std::string& name,
                                std::function<std::string(const std::vector<std::string>&)> func) {
    // TODO: 使用 JS_NewCFunctionData 注册 C++ 函数
    // 当前简化实现：注入一个 JS 占位符
    (void)name;
    (void)func;
}

void JsRuntime::injectObject(const std::string& name, const std::string& jsonValue) {
    std::string code = "var " + name + " = " + jsonValue + ";";
    eval(code, "<inject>");
}

std::string JsRuntime::getGlobal(const std::string& name) {
    // 通过 JS_Eval 获取全局变量
    std::string code = "JSON.stringify(" + name + ")";
    return eval(code, "<getGlobal>");
}

void JsRuntime::setGlobal(const std::string& name, const std::string& value) {
    std::string code = "var " + name + " = " + value + ";";
    eval(code, "<setGlobal>");
}

// ──────────────────────────────────────────────
// 配置
// ──────────────────────────────────────────────
void JsRuntime::setLogCallback(JsLogFunc func) {
    pImpl->logFunc = func;
}

std::string JsRuntime::getLastError() const {
    return pImpl->lastError;
}

void JsRuntime::setMemoryLimit(size_t bytes) {
    if (pImpl->rt) {
        JS_SetMemoryLimit(pImpl->rt, bytes);
    }
}

void JsRuntime::setStackSize(size_t bytes) {
    if (pImpl->rt) {
        JS_SetMaxStackSize(pImpl->rt, bytes);
    }
}

void JsRuntime::setExecutionTimeout(int timeoutMs) {
    if (timeoutMs <= 0) {
        throw std::invalid_argument("JavaScript execution timeout must be positive");
    }
    pImpl->executionTimeout = std::chrono::milliseconds(timeoutMs);
}

void JsRuntime::setInterruptCallback(std::function<bool()> callback) {
    pImpl->interruptCallback = std::move(callback);
}

void* JsRuntime::rawContext() const {
    return pImpl->ctx;
}

// ──────────────────────────────────────────────
// 真实绑定：HTTP 与变量表
// ──────────────────────────────────────────────
void JsRuntime::setHttpFunc(HttpFunc func) {
    pImpl->httpFunc = std::move(func);
}

void JsRuntime::setVariableAccessors(std::function<std::string(const std::string&)> getter,
                                     std::function<void(const std::string&, const std::string&)> setter) {
    pImpl->varGetter = std::move(getter);
    pImpl->varSetter = std::move(setter);
}

void JsRuntime::setSelectorFunc(SelectorFunc func) {
    pImpl->selectorFunc = std::move(func);
}

void JsRuntime::setCurrentContent(const std::string& content) {
    pImpl->currentContent = content;
}

std::string JsRuntime::getCurrentContent() const {
    return pImpl->currentContent;
}

std::string JsRuntime::getVariable(const std::string& key) const {
    if (pImpl->varGetter) return pImpl->varGetter(key);
    std::lock_guard<std::mutex> lk(pImpl->varMutex);
    auto it = pImpl->varTable.find(key);
    return it != pImpl->varTable.end() ? it->second : "";
}

void JsRuntime::putVariable(const std::string& key, const std::string& value) {
    if (pImpl->varSetter) { pImpl->varSetter(key, value); return; }
    std::lock_guard<std::mutex> lk(pImpl->varMutex);
    pImpl->varTable[key] = value;
}

} // namespace openread

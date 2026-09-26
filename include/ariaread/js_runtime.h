#pragma once
/// @file js_runtime.h
/// @brief QuickJS 运行时封装 —— JS 执行引擎

#include "ariaread/types.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace ariaread {

// ──────────────────────────────────────────────
/// QuickJS 运行时封装
// ──────────────────────────────────────────────
class JsRuntime {
public:
    JsRuntime();
    ~JsRuntime();

    // 禁止拷贝
    JsRuntime(const JsRuntime&) = delete;
    JsRuntime& operator=(const JsRuntime&) = delete;

    /// 执行 JS 代码，返回结果字符串
    /// @param code JS 代码
    /// @param filename 文件名（用于错误定位）
    /// @return 执行结果字符串，异常时返回空串
    std::string eval(const std::string& code,
                     const std::string& filename = "<eval>");

    /// 执行书源「规则解析」JS（与 legado AnalyzeRule.evalJS 上下文对齐）。
    /// 注入以下绑定供用户脚本使用：
    ///   result   —— 上一步规则的解析结果（字符串）
    ///   src      —— 当前页面/片段原始内容（== result 的别名，兼容 legado）
    ///   baseUrl  —— 当前页面 URL
    ///   key      —— 搜索关键词
    ///   page     —— 页码
    ///   java     —— 简化桩对象（put/get/log/ajax/...，put('result',v) 可改写结果）
    /// 用户脚本最后一个表达式的值即为返回结果；若脚本未返回字符串，则回退到 java.put
    /// 设置的 result，再回退到原 result。
    /// @param jsCode  用户 JS 代码（不含 @js:/<js> 包裹）
    /// @param result  传入的初始 result（通常是被解析内容）
    /// @param baseUrl 页面 URL
    /// @param key     关键词
    /// @param page    页码
    std::string evalRuleJs(const std::string& jsCode,
                           const std::string& result,
                           const std::string& baseUrl = "",
                           const std::string& key = "",
                           int page = 1);

    /// 注入全局函数（C++ → JS）
    /// @param name 函数名
    /// @param func 实现：接收参数列表，返回字符串
    void injectFunction(const std::string& name,
                        std::function<std::string(const std::vector<std::string>&)> func);

    /// 注入全局对象（JSON 字符串）
    void injectObject(const std::string& name, const std::string& jsonValue);

    /// 获取全局变量值
    std::string getGlobal(const std::string& name);

    /// 设置全局变量
    void setGlobal(const std::string& name, const std::string& value);

    /// 设置日志回调（console.log 输出）
    void setLogCallback(JsLogFunc func);

    // ──────────────────────────────────────────────
    // 与 legado JsExtensions 对齐的真实绑定
    // ──────────────────────────────────────────────

    /// HTTP 回调签名：(url, method, headersJson, body) → 响应体
    using HttpFunc = std::function<std::string(const std::string& url,
                                               const std::string& method,
                                               const std::string& headersJson,
                                               const std::string& body)>;

    /// 设置 java.ajax / java.get / java.post / java.getString 真正发起请求的回调。
    /// 不设置时这些方法返回空串（与旧桩行为兼容）。
    void setHttpFunc(HttpFunc func);

    /// 设置 @get/@put 使用的变量存取回调。
    /// getter(key)->value，setter(key,value)。不设置时使用内置进程内变量表。
    void setVariableAccessors(std::function<std::string(const std::string&)> getter,
                              std::function<void(const std::string&, const std::string&)> setter);

    /// 选择器回调签名：(content, rule) → 解析结果列表。
    /// 供书源 JS 中 java.getString(rule) / java.getElements(rule) 调用，
    /// 让脚本能用 AriaRead 选择器引擎查询「当前正文/片段」。
    using SelectorFunc = std::function<std::vector<std::string>(
        const std::string& content, const std::string& rule)>;

    /// 设置选择器回调。同时建议配合 setCurrentContent 提供"当前内容"。
    /// 不设置时 java.getString/getElements 退化为空结果（保持兼容）。
    void setSelectorFunc(SelectorFunc func);

    /// 设置/获取「当前内容」——java.getString(rule)/getElements(rule) 不显式传内容时，
    /// 默认对此内容求值（对齐 legado：脚本里 java.getString('.x@text') 查询当前 element）。
    void setCurrentContent(const std::string& content);
    std::string getCurrentContent() const;

    /// 直接读写内置变量表（当未注入外部 accessors 时使用）
    std::string getVariable(const std::string& key) const;
    void putVariable(const std::string& key, const std::string& value);

    /// 获取最后一次错误
    std::string getLastError() const;

    /// 设置内存限制（字节）
    void setMemoryLimit(size_t bytes);

    /// 设置栈大小限制（字节）
    void setStackSize(size_t bytes);

    /// 设置脚本执行超时（毫秒，默认 10000，必须为正数）。
    /// 中断 JS 执行；同步原生回调仍需自行设置超时。
    void setExecutionTimeout(int timeoutMs);

    /// 设置执行前及执行中的中断检查；返回 true 或抛出异常时中断本次脚本。
    /// 传入空回调可清除检查；同步原生回调仍需自行响应取消。
    void setInterruptCallback(std::function<bool()> callback);

    /// 获取原始 JSContext（高级用法，供 JsEvalSelector 使用）
    void* rawContext() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace ariaread

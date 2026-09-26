#pragma once

#include <doctest/doctest.h>
#include <quickjs.h>
#include "../src/apps/web_server/startup_options.h"
#include "ariaread/js_runtime.h"

#include <chrono>
#include <initializer_list>
#include <stdexcept>

namespace {

ariaread::web::StartupOptions parseOptions(std::initializer_list<const char*> args) {
    return ariaread::web::parseStartupOptions(static_cast<int>(args.size()), args.begin());
}

TEST_CASE("StartupOptions - 默认值和帮助") {
    const auto options = parseOptions({"ariaread"});
    CHECK(options.port == 9091);
    CHECK(options.host == "127.0.0.1");
    CHECK(options.web_root.empty());
    CHECK(options.db_path.empty());
    CHECK_FALSE(options.help);
    CHECK(parseOptions({"ariaread", "--help"}).help);
    CHECK(parseOptions({"ariaread", "-h"}).help);
}

TEST_CASE("StartupOptions - 命名参数互不干扰") {
    auto options = parseOptions({"ariaread", "--port", "9092"});
    CHECK(options.port == 9092);
    CHECK(options.web_root.empty());
    options = parseOptions({"ariaread", "--db", "123", "--web-root", "web assets",
                            "--host", "0.0.0.0", "-p", "65535"});
    CHECK(options.port == 65535);
    CHECK(options.db_path == "123");
    CHECK(options.web_root == "web assets");
    CHECK(options.host == "0.0.0.0");
    CHECK(parseOptions({"ariaread", "--port", "1"}).port == 1);
}

TEST_CASE("StartupOptions - 兼容位置参数及混合参数") {
    auto options = parseOptions({"ariaread", "9093", "web", "books.db"});
    CHECK(options.port == 9093);
    CHECK(options.web_root == "web");
    CHECK(options.db_path == "books.db");
    options = parseOptions({"ariaread", "--host", "::1", "--port", "9094", "web"});
    CHECK(options.port == 9094);
    CHECK(options.host == "::1");
    CHECK(options.web_root == "web");
    CHECK(parseOptions({"ariaread", "9091", "--", "-web"}).web_root == "-web");
}

TEST_CASE("StartupOptions - 拒绝无效端口及参数") {
    for (const char* port : {"0", "-1", "65536", "999999999999999999999", "abc", "80x", "1.5", ""}) {
        CHECK_THROWS_AS(parseOptions({"ariaread", "--port", port}), std::invalid_argument);
    }
    for (const char* option : {"--port", "-p", "--host", "--db", "--web-root"}) {
        CHECK_THROWS_AS(parseOptions({"ariaread", option}), std::invalid_argument);
        CHECK_THROWS_AS(parseOptions({"ariaread", option, "--help"}), std::invalid_argument);
        CHECK_THROWS_AS(parseOptions({"ariaread", option, ""}), std::invalid_argument);
    }
    CHECK_THROWS_AS(parseOptions({"ariaread", "--unknown"}), std::invalid_argument);
    CHECK_THROWS_AS(parseOptions({"ariaread", "9091", "web", "db", "extra"}), std::invalid_argument);
}

TEST_CASE("JsRuntime - 死循环超时后仍可复用") {
    ariaread::JsRuntime runtime;
    runtime.setExecutionTimeout(20);
    const auto started = std::chrono::steady_clock::now();
    CHECK(runtime.eval("while (true) {}").empty());
    CHECK(runtime.getLastError().find("timed out") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(runtime.eval("21 * 2") == "42");
    CHECK(runtime.getLastError().empty());
    CHECK(runtime.eval("try { while (true) {} } catch (e) { 'ignored'; }").empty());
    CHECK(runtime.getLastError().find("timed out") != std::string::npos);
}

TEST_CASE("JsRuntime - 规则超时不会回退原文且错误不污染后续执行") {
    ariaread::JsRuntime runtime;
    runtime.setExecutionTimeout(50);
    CHECK(runtime.evalRuleJs("while (true) {}", "original").empty());
    CHECK(runtime.getLastError().find("timed out") != std::string::npos);
    CHECK(runtime.evalRuleJs("result + '!'", "original") == "original!");
    CHECK(runtime.getLastError().empty());
    CHECK(runtime.evalRuleJs("java.put('result', 'saved'); void 0;", "original") == "saved");
    CHECK(runtime.evalRuleJs("void 0", "original") == "original");
    CHECK(runtime.eval("throw new Error('broken')").empty());
    CHECK(runtime.getLastError().find("broken") != std::string::npos);
    CHECK(runtime.eval("'recovered'") == "recovered");
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - 结果转换期间同样受超时约束") {
    ariaread::JsRuntime runtime;
    runtime.setExecutionTimeout(20);
    CHECK(runtime.eval("({toString() { while (true) {} }, toJSON() { while (true) {} }})").empty());
    CHECK(runtime.getLastError().find("timed out") != std::string::npos);
    CHECK_FALSE(JS_HasException(static_cast<JSContext*>(runtime.rawContext())));
    CHECK(runtime.eval("({ok: true})") == R"({"ok":true})");
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - 序列化异常被报告和消费且不会回退原文") {
    ariaread::JsRuntime runtime;
    auto* ctx = static_cast<JSContext*>(runtime.rawContext());
    CHECK(runtime.eval("({toJSON() { throw new Error('serialize failed'); }})").empty());
    CHECK(runtime.getLastError().find("serialize failed") != std::string::npos);
    CHECK_FALSE(JS_HasException(ctx));
    CHECK(runtime.evalRuleJs(
        "java.put('result', 'saved'); ({toJSON() { throw new Error('rule failed'); }})",
        "original").empty());
    CHECK(runtime.getLastError().find("rule failed") != std::string::npos);
    CHECK_FALSE(JS_HasException(ctx));
    CHECK(runtime.eval("var circular = {}; circular.self = circular; circular").empty());
    CHECK(runtime.getLastError().find("TypeError") != std::string::npos);
    CHECK_FALSE(JS_HasException(ctx));
    CHECK(runtime.evalRuleJs("({ok: true})", "original") == R"({"ok":true})");
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - 读取序列化函数异常保留原始错误") {
    ariaread::JsRuntime runtime;
    CHECK(runtime.eval(R"(
        var savedStringify = JSON.stringify;
        Object.defineProperty(JSON, 'stringify', {
            configurable: true,
            get() { throw new Error('stringifier inaccessible'); }
        });
        ({value: 1});
    )").empty());
    CHECK(runtime.getLastError().find("stringifier inaccessible") != std::string::npos);
    CHECK_FALSE(JS_HasException(static_cast<JSContext*>(runtime.rawContext())));
    CHECK(runtime.eval(R"(
        Object.defineProperty(JSON, 'stringify', {value: savedStringify});
        ({value: 2});
    )") == R"({"value":2})");
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - 异常格式化再次异常或超时均被消费") {
    ariaread::JsRuntime runtime;
    auto* ctx = static_cast<JSContext*>(runtime.rawContext());
    CHECK(runtime.eval("throw {toString() { throw new Error('format failed'); }}").empty());
    CHECK_FALSE(runtime.getLastError().empty());
    CHECK_FALSE(JS_HasException(ctx));
    runtime.setExecutionTimeout(20);
    CHECK(runtime.eval("throw {toString() { while (true) {} }}").empty());
    CHECK(runtime.getLastError().find("timed out") != std::string::npos);
    CHECK_FALSE(JS_HasException(ctx));
    CHECK(runtime.eval("({recovered: true})") == R"({"recovered":true})");
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - 执行前中断不执行脚本且清除回调后可复用") {
    ariaread::JsRuntime runtime;
    runtime.eval("var ran = false;");
    runtime.setInterruptCallback([] { return true; });
    CHECK(runtime.eval("ran = true;").empty());
    CHECK(runtime.getLastError() == "JavaScript execution interrupted");
    CHECK_FALSE(JS_HasException(static_cast<JSContext*>(runtime.rawContext())));
    runtime.setInterruptCallback({});
    CHECK(runtime.eval("ran") == "false");
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - 执行中中断不会被脚本捕获吞掉") {
    ariaread::JsRuntime runtime;
    int checks = 0;
    runtime.setInterruptCallback([&] { return ++checks >= 3; });
    CHECK(runtime.eval("try { while (true) {} } catch (e) { 'ignored'; }").empty());
    CHECK(checks >= 3);
    CHECK(runtime.getLastError() == "JavaScript execution interrupted");
    CHECK_FALSE(JS_HasException(static_cast<JSContext*>(runtime.rawContext())));
    runtime.setInterruptCallback({});
    CHECK(runtime.eval("21 * 2") == "42");
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - 中断回调抛出的异常不会跨越 QuickJS 边界") {
    ariaread::JsRuntime runtime;
    int checks = 0;
    runtime.setInterruptCallback([&] {
        if (++checks >= 3) throw std::runtime_error("cancelled");
        return false;
    });
    std::string result;
    CHECK_NOTHROW(result = runtime.eval("while (true) {}"));
    CHECK(result.empty());
    CHECK(runtime.getLastError() == "JavaScript execution interrupted");
    CHECK_FALSE(JS_HasException(static_cast<JSContext*>(runtime.rawContext())));
    runtime.setInterruptCallback({});
    CHECK(runtime.eval("'ok'") == "ok");
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - 序列化时同样响应中断且错误不污染后续执行") {
    ariaread::JsRuntime runtime;
    bool cancelled = false;
    runtime.setLogCallback([&](const std::string&) { cancelled = true; });
    runtime.setInterruptCallback([&] { return cancelled; });
    CHECK(runtime.eval("({toJSON() { console.log('cancel'); while (true) {} }})").empty());
    CHECK(runtime.getLastError() == "JavaScript execution interrupted");
    CHECK_FALSE(JS_HasException(static_cast<JSContext*>(runtime.rawContext())));
    cancelled = false;
    CHECK(runtime.eval("({ok: true})") == R"({"ok":true})");
    CHECK(runtime.getLastError().empty());
}

TEST_CASE("JsRuntime - 拒绝非正数超时") {
    ariaread::JsRuntime runtime;
    CHECK_THROWS_AS(runtime.setExecutionTimeout(0), std::invalid_argument);
    CHECK_THROWS_AS(runtime.setExecutionTimeout(-1), std::invalid_argument);
    CHECK_NOTHROW(runtime.setExecutionTimeout(100));
    CHECK(runtime.eval("1 + 1") == "2");
}

}  // namespace

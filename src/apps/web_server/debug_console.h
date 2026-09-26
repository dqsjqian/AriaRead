#pragma once

#include "ariaread/js_runtime.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace ariaread::web {

inline std::string debugTextPrefix(const std::string& text, size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    while (maxBytes > 0 && (static_cast<unsigned char>(text[maxBytes]) & 0xc0) == 0x80) {
        --maxBytes;
    }
    return text.substr(0, maxBytes);
}

inline nlohmann::json evaluateDebugScript(const nlohmann::json& body) {
    if (!body.is_object() || !body.contains("code") || !body["code"].is_string()) {
        throw std::invalid_argument("code must be a string");
    }
    const auto code = body["code"].get<std::string>();
    if (code.empty()) throw std::invalid_argument("Missing code");
    if (code.size() > 64 * 1024) throw std::length_error("Script exceeds 64 KiB");
    int timeoutMs = 1000;
    if (body.contains("timeoutMs")) {
        const auto& value = body["timeoutMs"];
        if (!value.is_number_integer() || value < 1 || value > 5000) {
            throw std::invalid_argument("timeoutMs must be an integer between 1 and 5000");
        }
        timeoutMs = value.get<int>();
    }

    nlohmann::json logs = nlohmann::json::array();
    size_t logBytes = 0;
    bool logsTruncated = false;
    JsRuntime runtime;
    runtime.setMemoryLimit(32 * 1024 * 1024);
    runtime.setStackSize(512 * 1024);
    runtime.setExecutionTimeout(timeoutMs);
    runtime.setLogCallback([&](const std::string& message) {
        if (logs.size() >= 100 || logBytes >= 16 * 1024) {
            logsTruncated = true;
            return;
        }
        auto text = debugTextPrefix(message, 16 * 1024 - logBytes);
        logsTruncated = logsTruncated || text.size() != message.size();
        logBytes += text.size();
        logs.push_back(text);
    });
    const auto started = std::chrono::steady_clock::now();
    const auto result = runtime.eval(code, "<console>");
    const auto error = runtime.getLastError();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    return {{"ok", error.empty()}, {"result", debugTextPrefix(result, 64 * 1024)},
            {"error", debugTextPrefix(error, 64 * 1024)}, {"logs", logs}, {"elapsedMs", elapsed},
            {"logsTruncated", logsTruncated}, {"resultTruncated", result.size() > 64 * 1024},
            {"errorTruncated", error.size() > 64 * 1024}};
}

}  // namespace ariaread::web

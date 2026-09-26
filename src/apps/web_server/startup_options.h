#pragma once

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>

namespace ariaread::web {

struct StartupOptions {
    std::uint16_t port = 9091;
    std::string host = "127.0.0.1";
    std::string web_root;
    std::string db_path;
    bool help = false;
};

inline StartupOptions parseStartupOptions(int argc, const char* const* argv) {
    StartupOptions options;
    bool port_set = false;
    bool web_root_set = false;
    bool db_path_set = false;
    bool positional_only = false;

    auto parse_port = [](const std::string& value) -> std::uint16_t {
        unsigned int port = 0;
        auto result = std::from_chars(value.data(), value.data() + value.size(), port);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
            port == 0 || port > 65535) {
            throw std::invalid_argument("Invalid port (expected 1-65535): " + value);
        }
        return static_cast<std::uint16_t>(port);
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (!positional_only && (arg == "-h" || arg == "--help")) {
            options.help = true;
        } else if (!positional_only && arg == "--") {
            positional_only = true;
        } else if (!positional_only &&
                   (arg == "-p" || arg == "--port" || arg == "--host" ||
                    arg == "--web-root" || arg == "--db")) {
            if (i + 1 >= argc || !argv[i + 1][0] || argv[i + 1][0] == '-') {
                throw std::invalid_argument("Missing value for " + arg);
            }
            std::string value = argv[++i];
            if (arg == "-p" || arg == "--port") {
                options.port = parse_port(value);
                port_set = true;
            } else if (arg == "--host") {
                options.host = value;
            } else if (arg == "--web-root") {
                options.web_root = value;
                web_root_set = true;
            } else {
                options.db_path = value;
                db_path_set = true;
            }
        } else if (!positional_only && !arg.empty() && arg.front() == '-') {
            throw std::invalid_argument("Unknown option: " + arg);
        } else if (!port_set) {
            options.port = parse_port(arg);
            port_set = true;
        } else if (!web_root_set && !arg.empty()) {
            options.web_root = arg;
            web_root_set = true;
        } else if (!db_path_set && !arg.empty()) {
            options.db_path = arg;
            db_path_set = true;
        } else {
            throw std::invalid_argument("Unexpected argument: " + arg);
        }
    }
    return options;
}

}  // namespace ariaread::web

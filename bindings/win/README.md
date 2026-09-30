# AriaRead Windows 接入

此目录保留平台接入说明，目前没有独立的原生应用壳。可运行的应用入口是 Web 服务；构建和运行步骤见[项目说明](../../README.md)。

- [C API](../../include/ariaread/bridge.h)：面向 Swift、Objective-C、C 和其他 FFI 调用方，返回字符串须用 `ariaread_free_string` 释放。
- [C++ 包装](../../include/ariaread/wrapper.hpp)：类型为 `ariaread::EngineWrapper`。
- [ViewModel](../../src/viewmodels/include/ariaread/vm/)：可供界面层复用的书架、阅读、搜索与书源状态。

这些接口由当前工程构建；本目录不提供单独的构建入口或已打包的 SDK。

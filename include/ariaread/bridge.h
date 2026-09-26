#pragma once
/// @file bridge.h
/// @brief AriaRead C API —— 跨平台 FFI 桥接层
///
/// 纯 C 接口，供 iOS(Swift/OC)、Android(JNI)、Qt(C++)、Python(ctypes) 调用
/// 所有返回 char* 的函数，调用者需用 ariaread_free_string() 释放

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// ──────────────────────────────────────────────
// 类型定义
// ──────────────────────────────────────────────

/// 不透明引擎指针
typedef void* AriaReadEngine;

/// HTTP 回调函数类型
/// @param url     请求URL
/// @param method  请求方法 (GET/POST)
/// @param headers 请求头 (JSON)
/// @param body    请求体
/// @param userData 用户数据指针
/// @return 响应体字符串（需用 malloc 分配，由 AriaRead 调用 ariaread_free_string 释放）
typedef char* (*AriaReadHttpCallback)(
    const char* url,
    const char* method,
    const char* headers,
    const char* body,
    void* userData
);

/// 日志回调函数类型
typedef void (*AriaReadLogCallback)(const char* message, void* userData);

// ──────────────────────────────────────────────
// 生命周期
// ──────────────────────────────────────────────

/// 创建引擎实例
AriaReadEngine ariaread_engine_create();

/// 销毁引擎实例
void ariaread_engine_destroy(AriaReadEngine engine);

// ──────────────────────────────────────────────
// 配置
// ──────────────────────────────────────────────

/// 设置 HTTP 回调
void ariaread_engine_set_http_callback(
    AriaReadEngine engine,
    AriaReadHttpCallback callback,
    void* userData
);

/// 设置日志回调
void ariaread_engine_set_log_callback(
    AriaReadEngine engine,
    AriaReadLogCallback callback,
    void* userData
);

// ──────────────────────────────────────────────
// 书源管理
// ──────────────────────────────────────────────

/// 加载单个书源（JSON 字符串）
/// @return 0 成功，-1 失败
int ariaread_engine_load_source(AriaReadEngine engine, const char* json);

/// 加载书源数组
/// @return 成功加载的书源数量，-1 失败
int ariaread_engine_load_sources(AriaReadEngine engine, const char* jsonArray);

/// 从文件加载书源
/// @return 成功加载的书源数量，-1 失败
int ariaread_engine_load_sources_from_file(AriaReadEngine engine, const char* filePath);

/// 选择当前书源（按索引）
int ariaread_engine_select_source(AriaReadEngine engine, int index);

/// 选择当前书源（按名称）
int ariaread_engine_select_source_by_name(AriaReadEngine engine, const char* name);

/// 获取已加载书源数量
int ariaread_engine_source_count(AriaReadEngine engine);

// ──────────────────────────────────────────────
// 核心操作
// ──────────────────────────────────────────────

/// 搜索书籍
/// @return JSON 数组字符串（需用 ariaread_free_string 释放）
char* ariaread_engine_search(AriaReadEngine engine, const char* keyword);

/// 获取目录
/// @return JSON 数组字符串（需用 ariaread_free_string 释放）
char* ariaread_engine_get_catalog(AriaReadEngine engine, const char* bookUrl);

/// 获取正文
/// @return 正文字符串（需用 ariaread_free_string 释放）
char* ariaread_engine_get_content(AriaReadEngine engine, const char* chapterUrl);

/// 按指定书源获取目录（优先 sourceName，其次 sourceIndex）
/// @return JSON 数组字符串（需用 ariaread_free_string 释放）
char* ariaread_engine_get_catalog_for_source(
    AriaReadEngine engine,
    const char* bookUrl,
    int sourceIndex,
    const char* sourceName
);

/// 按指定书源获取正文（优先 sourceName，其次 sourceIndex）
/// @return 正文字符串（需用 ariaread_free_string 释放）
char* ariaread_engine_get_content_for_source(
    AriaReadEngine engine,
    const char* chapterUrl,
    int sourceIndex,
    const char* sourceName
);

/// 清空全部书源（内存+数据库）
/// @return 删除数量
int ariaread_engine_clear_all_sources(AriaReadEngine engine);

// ──────────────────────────────────────────────
// 书架管理
// ──────────────────────────────────────────────

/// 添加书籍到书架
/// @param bookJson JSON 字符串 {bookName, bookAuthor, coverUrl, bookUrl, sourceName, sourceUrl, intro, kind, lastChapter}
/// @return 新插入的 id，已存在返回 -1
int64_t ariaread_bookshelf_add(AriaReadEngine engine, const char* bookJson);

/// 从书架移除书籍
/// @return 0 成功，-1 失败
int ariaread_bookshelf_remove(AriaReadEngine engine, const char* bookUrl, const char* sourceUrl);

/// 获取书架列表
/// @return JSON 数组字符串（需用 ariaread_free_string 释放）
char* ariaread_bookshelf_list(AriaReadEngine engine);

/// 检查书籍是否在书架中
/// @return 1 在书架中，0 不在
int ariaread_bookshelf_is_in(AriaReadEngine engine, const char* bookUrl, const char* sourceUrl);

/// 保存阅读进度
/// @param progressJson JSON 字符串 {bookUrl, sourceUrl, chapterIndex, chapterUrl, chapterTitle, readPercent}
/// @return 0 成功，-1 失败
int ariaread_bookshelf_progress_save(AriaReadEngine engine, const char* progressJson);

/// 获取阅读进度
/// @return JSON 字符串（需用 ariaread_free_string 释放）
char* ariaread_bookshelf_progress_get(AriaReadEngine engine, const char* bookUrl, const char* sourceUrl);

/// 换源
/// @return 0 成功，-1 失败
int ariaread_bookshelf_change_source(AriaReadEngine engine,
    const char* bookUrl, const char* oldSourceUrl,
    const char* newSourceName, const char* newSourceUrl, const char* newBookUrl);

/// 检查单本书更新
/// @return 1 有更新，0 无更新，-1 失败
int ariaread_bookshelf_check_update(AriaReadEngine engine,
    const char* bookUrl, const char* sourceUrl,
    int sourceIndex, const char* sourceName);

/// 更新书架条目的最新章节信息
/// @return 0 成功，-1 失败
int ariaread_bookshelf_update_last_chapter(AriaReadEngine engine,
    const char* bookUrl, const char* sourceUrl,
    const char* lastChapter, int totalChapters, int hasUpdate);

/// 全量下载缓存（同步，无回调版本）
/// @return JSON 字符串 {"total":N, "cached":N, "failed":N}，调用者需 ariaread_free_string
char* ariaread_bookshelf_download(AriaReadEngine engine,
    const char* bookUrl, const char* sourceUrl,
    int sourceIndex, const char* sourceName);

/// 批量更新检测（同步，无回调版本）
/// @return 有更新的书籍数量，-1 失败
int ariaread_bookshelf_check_all_updates(AriaReadEngine engine);

// ──────────────────────────────────────────────
// 并发操作
// ──────────────────────────────────────────────

/// 不透明取消令牌指针
typedef void* AriaReadCancelToken;

/// 并发验证回调（逐源通知，线程安全）
/// @param sourceIndex 书源索引
/// @param sourceName  书源名称
/// @param validity    取值与 ariaread::SourceValidity 枚举保持一致：
///                     0=Unknown, 1=Excellent, 2=Good, 3=Poor, 4=Invalid
/// @param latencyMs   响应延迟（毫秒）
/// @param detail      附加信息
/// @param userData    用户数据指针
typedef void (*AriaReadValidateCallback)(
    int sourceIndex,
    const char* sourceName,
    int validity,
    int latencyMs,
    const char* detail,
    void* userData
);

/// 并发搜索回调（逐源推送结果，线程安全）
/// @param sourceIndex 书源索引
/// @param sourceName  书源名称
/// @param booksJson   搜索结果 JSON 数组字符串（无需释放，回调内有效）
/// @param latencyMs   响应延迟（毫秒）
/// @param error       错误信息（空字符串表示成功）
/// @param userData    用户数据指针
typedef void (*AriaReadSearchCallback)(
    int sourceIndex,
    const char* sourceName,
    const char* booksJson,
    int latencyMs,
    const char* error,
    void* userData
);

/// 并发完成回调
/// @param count1  验证时=有效数, 搜索时=总书数
/// @param count2  验证时=无效数, 搜索时=总源数
/// @param count3  验证时=删除数, 搜索时=错误数
/// @param userData 用户数据指针
typedef void (*AriaReadDoneCallback)(
    int count1,
    int count2,
    int count3,
    void* userData
);

/// 创建取消令牌
AriaReadCancelToken ariaread_cancel_token_create();

/// 触发取消
void ariaread_cancel_token_cancel(AriaReadCancelToken token);

/// 销毁取消令牌
void ariaread_cancel_token_destroy(AriaReadCancelToken token);

/// 并发验证书源（C++ 线程池，无效自动删除，有效持久化）
/// @param engine      引擎实例
/// @param testQuery   测试关键词（UTF-8）
/// @param timeoutMs   单源超时毫秒
/// @param concurrency 并发线程数
/// @param callback    逐源验证结果回调
/// @param doneCallback 全部完成回调
/// @param userData    用户数据指针
/// @param cancelToken 取消令牌（可为 NULL）
void ariaread_engine_validate_concurrent(
    AriaReadEngine engine,
    const char* testQuery,
    int timeoutMs,
    int concurrency,
    AriaReadValidateCallback callback,
    AriaReadDoneCallback doneCallback,
    void* userData,
    AriaReadCancelToken cancelToken
);

/// 并发搜索所有有效书源（C++ 线程池，结果逐源推送）
/// @param engine      引擎实例
/// @param keyword     搜索关键词（UTF-8）
/// @param concurrency 并发线程数
/// @param callback    逐源搜索结果回调
/// @param doneCallback 全部完成回调
/// @param userData    用户数据指针
/// @param cancelToken 取消令牌（可为 NULL）
void ariaread_engine_search_all_concurrent(
    AriaReadEngine engine,
    const char* keyword,
    int concurrency,
    AriaReadSearchCallback callback,
    AriaReadDoneCallback doneCallback,
    void* userData,
    AriaReadCancelToken cancelToken,
    int matchName,
    int matchAuthor,
    int matchIntro
);

/// 设置数据库路径（启用持久化）
void ariaread_engine_set_database_path(AriaReadEngine engine, const char* dbPath);

/// 从数据库加载书源
/// @return 加载的书源数量
int ariaread_engine_load_sources_from_database(AriaReadEngine engine);

/// 删除所有无效书源
/// @return 删除数量
int ariaread_engine_remove_invalid_sources(AriaReadEngine engine);

/// 设置默认并发数
void ariaread_engine_set_concurrency(AriaReadEngine engine, int n);

// ──────────────────────────────────────────────
// 状态查询
// ──────────────────────────────────────────────

/// 获取当前书源信息
/// @return JSON 字符串（需用 ariaread_free_string 释放）
char* ariaread_engine_get_source_info(AriaReadEngine engine);

/// 获取书源列表摘要（过滤无效源、映射 validity 字符串、兜底 latency）
/// @return JSON 字符串（需用 ariaread_free_string 释放）
///   格式: {"sources":[{name,url,group,searchUrl,exploreUrl,validity,latency},...], "validCount":N, "totalCount":N}
char* ariaread_engine_get_source_list(AriaReadEngine engine);

/// 导出优+良书源为 JSON 字符串
/// @return JSON 数组字符串（需用 ariaread_free_string 释放）
char* ariaread_engine_export_good_sources(AriaReadEngine engine);

/// 获取最后错误
/// @return 错误字符串（无需释放，下次调用前有效）
const char* ariaread_engine_get_last_error(AriaReadEngine engine);

/// 检查引擎是否可用
int ariaread_engine_is_available(AriaReadEngine engine);

// ──────────────────────────────────────────────
// 内存管理
// ──────────────────────────────────────────────

/// 释放引擎返回的字符串
void ariaread_free_string(char* str);

// ──────────────────────────────────────────────
// 版本信息
// ──────────────────────────────────────────────

/// 获取版本号
const char* ariaread_version();

#ifdef __cplusplus
}
#endif

# SQLite3 amalgamation — 跨平台编译，零外部依赖

add_library(sqlite3 STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/sqlite3.c
)

target_include_directories(sqlite3
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# SQLite3 编译选项
target_compile_definitions(sqlite3
    PRIVATE
        SQLITE_ENABLE_FTS5=1              # 全文搜索（未来书源全文检索）
        SQLITE_THREADSAFE=1               # 多线程安全
        SQLITE_OMIT_LOAD_EXTENSION=1      # 不需要动态加载扩展
)

# 关闭所有警告（sqlite3.c 是第三方代码）
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)
else()
    target_compile_options(sqlite3 PRIVATE -w)
endif()

# 链接系统依赖
if(UNIX)
    target_link_libraries(sqlite3 PRIVATE pthread dl m)
endif()

if(WIN32)
    target_link_libraries(sqlite3 PRIVATE ws2_32)
endif()

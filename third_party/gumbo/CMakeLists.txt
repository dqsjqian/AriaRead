# Gumbo HTML5 解析器（Google 开源，纯 C99，跨平台零依赖）
# 源码来自 https://github.com/google/gumbo-parser v0.10.1

set(GUMBO_SOURCES
    attribute.c
    char_ref.c
    error.c
    parser.c
    string_buffer.c
    string_piece.c
    tag.c
    tokenizer.c
    utf8.c
    util.c
    vector.c
)

add_library(gumbo STATIC ${GUMBO_SOURCES})
target_include_directories(gumbo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT MSVC)
    target_compile_options(gumbo PRIVATE -w)  # 抑制第三方库警告
endif()

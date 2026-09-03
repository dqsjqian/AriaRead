# OpenRead Unit Tests

# 使用 doctest（轻量级测试框架）
include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
)
# CMake 4.x 兼容
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(doctest)

# ──────────────────────────────────────────────
# 引擎测试
# ──────────────────────────────────────────────
set(ENGINE_TEST_SOURCES
    test_main.cpp
    test_source_parser.cpp
    test_selector.cpp
    test_css_xpath.cpp
    test_engine_utils.cpp
    test_engine_core.cpp
    test_database.cpp
    test_engine_integration.cpp
    test_engine_validate.cpp
    test_engine_bookshelf.cpp
)

add_executable(openread-tests ${ENGINE_TEST_SOURCES})

target_include_directories(openread-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
        ${CMAKE_SOURCE_DIR}/third_party/gumbo
        SYSTEM ${CMAKE_SOURCE_DIR}/third_party/sqlite_modern_cpp/hdr
)

target_link_libraries(openread-tests
    PRIVATE openread
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE doctest::doctest
    PRIVATE sqlite3
)

# 注册测试
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(openread-tests)

# ──────────────────────────────────────────────
# ViewModel 测试（仅当 Aria + openread_vm 可用时构建）
# ──────────────────────────────────────────────
if(TARGET openread_vm)
    set(VM_TEST_SOURCES
        test_main.cpp
        test_search_view_model.cpp
        test_bookshelf_view_model.cpp
        test_search_end_to_end.cpp
        test_reader_view_model.cpp
        test_source_view_model.cpp
    )

    add_executable(openread-vm-tests ${VM_TEST_SOURCES})

    target_include_directories(openread-vm-tests
        PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            # Aria binding 的测试辅助（FakeAdapter/FakeView），用于端到端示例
            ${CMAKE_SOURCE_DIR}/third_party/aria/modules/binding/tests
            SYSTEM ${CMAKE_SOURCE_DIR}/third_party/sqlite_modern_cpp/hdr
    )

    target_link_libraries(openread-vm-tests
        PRIVATE
            openread_vm
            doctest::doctest
            nlohmann_json::nlohmann_json
    )

    if(NOT MSVC)
        target_compile_options(openread-vm-tests PRIVATE -Wno-deprecated-declarations)
    endif()

    include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
    doctest_discover_tests(openread-vm-tests)
endif()

# 测试注册辅助函数。
#
# 每个测试源文件生成一个独立可执行文件并注册到 CTest。
# 测试断言使用 Unity（vendored 到 tests/vendor/unity/，MIT）。

set(TOKENPROBE_UNITY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vendor/unity/unity.c
)

function(tokenprobe_add_test target)
    cmake_parse_arguments(TOKENPROBE_TEST
        ""
        ""
        "SOURCES;LIBS"
        ${ARGN}
    )

    if(NOT TOKENPROBE_TEST_SOURCES)
        message(FATAL_ERROR "tokenprobe_add_test(${target}) requires SOURCES")
    endif()

    add_executable(${target}
        ${TOKENPROBE_TEST_SOURCES}
        ${TOKENPROBE_UNITY_SOURCES}
    )

    target_include_directories(${target}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenprobe
            ${TOKENPROBE_GENERATED_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/vendor/unity
    )

    if(TOKENPROBE_TEST_LIBS)
        target_link_libraries(${target} PRIVATE ${TOKENPROBE_TEST_LIBS})
    endif()

    tokenprobe_target_defaults(${target})

    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_test(NAME ${target} COMMAND $<TARGET_FILE:${target}>)
    set_tests_properties(${target} PROPERTIES LABELS "unit")
    list(APPEND TOKENPROBE_TEST_TARGETS ${target})
    set(TOKENPROBE_TEST_TARGETS "${TOKENPROBE_TEST_TARGETS}" PARENT_SCOPE)
endfunction()

set(TOKENPROBE_TEST_TARGETS "")

# 架构门禁：include/ 的每个公共函数声明都必须在 src/ 中有对应定义。
# Python 解释器为必需项：缺少它时这些门禁会悄悄从 CTest 清单中消失，
# 测试套件仍会以更少的用例报告通过。
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME tokenprobe_decl_def_check
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_decl_def.py
            ${CMAKE_CURRENT_SOURCE_DIR}
            --min-declarations 1
            --min-definitions 1)
    set_tests_properties(tokenprobe_decl_def_check PROPERTIES LABELS "architecture")

    add_test(NAME tokenprobe_layers_check
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_layers.py
            ${CMAKE_CURRENT_SOURCE_DIR}
            --min-public-headers 2
            --min-providers 1)
    set_tests_properties(tokenprobe_layers_check PROPERTIES LABELS "architecture")

    add_test(NAME tokenprobe_gate_behaviors
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_gate_behaviors.py)
    set_tests_properties(tokenprobe_gate_behaviors PROPERTIES LABELS "architecture")

    add_test(NAME tokenprobe_docs_check
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_docs.py
            ${CMAKE_CURRENT_SOURCE_DIR}
            --mode all)
    set_tests_properties(tokenprobe_docs_check PROPERTIES LABELS "documentation")

    add_test(NAME tokenprobe_doc_paths_check
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_doc_paths.py
            ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(tokenprobe_doc_paths_check PROPERTIES LABELS "documentation")

    # 版本一致性：CMakeLists 中的 project VERSION、生成的 tokenprobe/version.h、
    # CLI 的 --version 输出以及 CHANGELOG 最新段落必须一致。TokenProbe 有意
    # 不在自己的头文件中保留版本字面量，因此该门禁只需检查唯一事实来源。
    if(TARGET tokenprobe)
        add_test(NAME tokenprobe_version_consistency
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_version_consistency.py
                ${CMAKE_CURRENT_SOURCE_DIR}
                --build-dir ${CMAKE_BINARY_DIR}
                --exe $<TARGET_FILE:tokenprobe>
        )
        set_tests_properties(tokenprobe_version_consistency PROPERTIES
            LABELS "architecture"
            DEPENDS tokenprobe)
    endif()
else()
    message(FATAL_ERROR
        "Python3 interpreter not found, but TOKENPROBE_BUILD_TESTING is ON and "
        "the architecture gates require it. Install Python 3, or reconfigure "
        "with -DTOKENPROBE_BUILD_TESTING=OFF to build without the test suite.")
endif()

# ---------------------------------------------------------------------------
# 单元测试
# ---------------------------------------------------------------------------

tokenprobe_add_test(tokenprobe_decimal_tests
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_decimal.c
    LIBS
        tokenprobe_core
)

# 这两个测试包含 tp_internal.h 并调用内部解析器。它们链接对象库而不是发布库，
# 因为这些符号有意不导出到共享库，也不属于公共 ABI。
tokenprobe_add_test(tokenprobe_json_tests
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_json.c
    LIBS
        tokenprobe_objects
)

tokenprobe_add_test(tokenprobe_url_tests
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_url.c
    LIBS
        tokenprobe_objects
)

tokenprobe_add_test(tokenprobe_deepseek_tests
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_provider_deepseek.c
    LIBS
        tokenprobe_core
)

tokenprobe_add_test(tokenprobe_context_tests
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_context.c
    LIBS
        tokenprobe_core
)

# ---------------------------------------------------------------------------
# CLI 冒烟测试
# ---------------------------------------------------------------------------

if(TARGET tokenprobe)
    add_test(NAME tokenprobe_cli_help COMMAND $<TARGET_FILE:tokenprobe> --help)
    set_tests_properties(tokenprobe_cli_help PROPERTIES
        LABELS "smoke"
        PASS_REGULAR_EXPRESSION "Usage:")

    add_test(NAME tokenprobe_cli_version COMMAND $<TARGET_FILE:tokenprobe> --version)
    set_tests_properties(tokenprobe_cli_version PROPERTIES
        LABELS "smoke"
        PASS_REGULAR_EXPRESSION "[0-9]+\\.[0-9]+\\.[0-9]+")

    # 列出供应商不应需要网络传输层或 API key：
    # 该测试验证供应商注册表独立于其他部分。
    add_test(NAME tokenprobe_cli_list COMMAND $<TARGET_FILE:tokenprobe> --list)
    set_tests_properties(tokenprobe_cli_list PROPERTIES
        LABELS "smoke"
        PASS_REGULAR_EXPRESSION "deepseek")

    # 没有 key 的探测必须以文档规定的退出码失败，且不得发起请求。
    # 使用一个保证未设置的环境变量。
    add_test(NAME tokenprobe_cli_missing_key
        COMMAND $<TARGET_FILE:tokenprobe> --provider deepseek)
    set_tests_properties(tokenprobe_cli_missing_key PROPERTIES
        LABELS "smoke"
        ENVIRONMENT "DEEPSEEK_API_KEY="
        WILL_FAIL TRUE)

    add_test(NAME tokenprobe_cli_unknown_provider
        COMMAND $<TARGET_FILE:tokenprobe> --provider no_such_provider)
    set_tests_properties(tokenprobe_cli_unknown_provider PROPERTIES
        LABELS "smoke"
        WILL_FAIL TRUE)

    # mock 构建没有平台传输层，CLI 无法完成成功路径。
    if(NOT TOKENPROBE_HTTP_BACKEND STREQUAL "mock")
        # 机器可读文档是宿主消费的接口，因此通过解析来验证，而不是匹配文本模式。
        # 由 tests/test_cli_output.py 驱动：它在回环地址上提供预置响应，用供应商
        # 提供的文本检验转义规则，并逐字段比较两种格式，避免任一方单独偏移。
        add_test(NAME tokenprobe_cli_output
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli_output.py
                $<TARGET_FILE:tokenprobe>)
        set_tests_properties(tokenprobe_cli_output PROPERTIES LABELS "smoke")

        # 平台传输层必须真正应用 --timeout：服务器只接受连接、不响应。
        add_test(NAME tokenprobe_cli_timeout
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_timeout.py
                $<TARGET_FILE:tokenprobe>)
        set_tests_properties(tokenprobe_cli_timeout PROPERTIES
            LABELS "smoke"
            TIMEOUT 30)
    endif()
endif()

add_custom_target(tokenprobe_tests
    DEPENDS
        ${TOKENPROBE_TEST_TARGETS}
)

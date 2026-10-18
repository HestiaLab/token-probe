# 库与 CLI 的源文件清单。
#
# 分层由代码审查保证，而不是由构建系统强制：
#
#   core/       内存池、字符串、精确十进制、JSON、URL、结果、上下文
#   transport   每个 HTTP 后端一个文件；唯一接触操作系统或第三方库的代码
#   providers/  每个供应商一个文件，把该供应商的 JSON 转换为
#               tokenprobe_metric 记录
#
# 供应商驱动不得包含操作系统头文件，核心层不得包含传输层实现。
#
# 这些源文件只编译一次，进入 `tokenprobe_objects` 对象库；原因见 CMakeLists.txt。

set(TOKENPROBE_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenprobe/tp_arena.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenprobe/tp_str.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenprobe/tp_decimal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenprobe/tp_json.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenprobe/tp_url.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenprobe/tp_result.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenprobe/tp_ctx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenprobe/tp_http_mock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenprobe/tp_provider_deepseek.c
)

if(TOKENPROBE_HTTP_BACKEND STREQUAL "winhttp")
    list(APPEND TOKENPROBE_CORE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenprobe/tp_http_winhttp.c
    )
elseif(TOKENPROBE_HTTP_BACKEND STREQUAL "openssl")
    list(APPEND TOKENPROBE_CORE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenprobe/tp_http_openssl.c
    )
endif()

set(TOKENPROBE_CLI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
)

# 第三方依赖查找。
#
# TokenProbe 有意保持很小的依赖面。唯一的构建期第三方依赖是 OpenSSL，
# 且只用于非 Windows 主机：
#
#   * Windows —— WinHTTP 是操作系统组件，不需要从平台 SDK 之外下载或链接任何东西。
#   * Linux   —— libc 不提供 TLS 实现，因此 TLS 库不可避免。已排除的方案：
#               自带一份 TLS 栈（CVE 维护负担过重）、在项目内实现 TLS 1.3
#               （不适合手写）、或发布不含 HTTPS 的版本（会明文发送 API key）。
#
# OpenSSL 从不出现在公共头文件中，且以 PRIVATE 方式链接，因此也不会出现在
# 已安装目标的 INTERFACE 属性里。TokenProbe::tokenprobe_core 的消费方
# 不会继承该依赖。

if(TOKENPROBE_HTTP_BACKEND STREQUAL "auto")
    if(WIN32)
        set(TOKENPROBE_HTTP_BACKEND "winhttp" CACHE STRING "" FORCE)
    else()
        set(TOKENPROBE_HTTP_BACKEND "openssl" CACHE STRING "" FORCE)
    endif()
endif()

if(TOKENPROBE_HTTP_BACKEND STREQUAL "winhttp" AND NOT WIN32)
    message(FATAL_ERROR
        "TOKENPROBE_HTTP_BACKEND=winhttp requires Windows; use openssl on POSIX hosts.")
endif()

if(TOKENPROBE_HTTP_BACKEND STREQUAL "openssl" AND WIN32)
    message(FATAL_ERROR
        "TOKENPROBE_HTTP_BACKEND=openssl is not wired up for Windows in this version. "
        "Use winhttp, which needs no external dependency.")
endif()

if(TOKENPROBE_HTTP_BACKEND STREQUAL "openssl")
    find_package(OpenSSL 1.1.1 REQUIRED)
endif()

find_package(Threads REQUIRED)

# 传输层所需的链接库，只解析一次，供对象库和最终的 tokenprobe_core 共同使用。
set(TOKENPROBE_TRANSPORT_LIBS Threads::Threads)
if(TOKENPROBE_HTTP_BACKEND STREQUAL "winhttp")
    list(APPEND TOKENPROBE_TRANSPORT_LIBS winhttp)
elseif(TOKENPROBE_HTTP_BACKEND STREQUAL "openssl")
    list(APPEND TOKENPROBE_TRANSPORT_LIBS OpenSSL::SSL OpenSSL::Crypto)
endif()
# include() 与调用方共享作用域，变量直接可见；不要用 PARENT_SCOPE
# （顶层目录没有父作用域，会触发 CMake 警告）。
set(TOKENPROBE_TRANSPORT_LIBS "${TOKENPROBE_TRANSPORT_LIBS}")

message(STATUS "TokenProbe: HTTP transport = ${TOKENPROBE_HTTP_BACKEND}")

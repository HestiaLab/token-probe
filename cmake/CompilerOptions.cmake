function(tokenprobe_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
        )
    endif()
endfunction()

function(tokenprobe_target_sanitizers target)
    if(TOKENPROBE_ENABLE_ASAN)
        if(MSVC)
            target_compile_options(${target} PRIVATE /fsanitize=address)
            target_link_options(${target} PRIVATE /fsanitize=address)
        else()
            target_compile_options(${target} PRIVATE -fsanitize=address)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    endif()

    if(TOKENPROBE_ENABLE_UBSAN)
        if(NOT MSVC)
            target_compile_options(${target} PRIVATE -fsanitize=undefined)
            target_link_options(${target} PRIVATE -fsanitize=undefined)
        else()
            message(WARNING
                "TOKENPROBE_ENABLE_UBSAN is ON, but UBSAN is not configured for MSVC in this template.")
        endif()
    endif()
endfunction()

function(tokenprobe_target_coverage target)
    if(NOT TOKENPROBE_ENABLE_COVERAGE)
        return()
    endif()

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${target} PRIVATE --coverage -O0 -g)
        target_link_options(${target} PRIVATE --coverage)
    else()
        message(WARNING
            "TOKENPROBE_ENABLE_COVERAGE is ON, but coverage flags are only configured for GCC and Clang-like compilers.")
    endif()
endfunction()

function(tokenprobe_target_defaults target)
    target_compile_features(${target} PRIVATE c_std_11)

    # CMAKE_C_EXTENSIONS 已关闭，编译器运行在严格 ISO 模式（-std=c11，而非
    # gnu11），因此 glibc 会把所有 POSIX 符号藏在特性测试宏之后。POSIX 传输层
    # 需要 getaddrinfo() 和 clock_gettime()，测试需要 setenv()。显式声明这些
    # 宏是可移植的做法，也能让公共头文件不必包含平台条件编译。
    #
    # 有意设为 PRIVATE：消费方不能因为链接本库而被改写自己的特性测试宏。
    if(NOT WIN32)
        target_compile_definitions(${target} PRIVATE
            _POSIX_C_SOURCE=200809L
            _DEFAULT_SOURCE
        )
    endif()

    tokenprobe_target_warnings(${target})
    tokenprobe_target_sanitizers(${target})
    tokenprobe_target_coverage(${target})
endfunction()

# 把平台传输层的依赖链接到目标上。对 tokenprobe_core 使用 PRIVATE，
# 消费方不会继承 OpenSSL 或 winhttp。
function(tokenprobe_target_transport target)
    if(TOKENPROBE_HTTP_BACKEND STREQUAL "winhttp")
        target_compile_definitions(${target} PRIVATE TOKENPROBE_USE_WINHTTP=1)
        target_link_libraries(${target} PRIVATE winhttp)
    elseif(TOKENPROBE_HTTP_BACKEND STREQUAL "openssl")
        target_compile_definitions(${target} PRIVATE TOKENPROBE_USE_OPENSSL=1)
        target_link_libraries(${target} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    else()
        target_compile_definitions(${target} PRIVATE TOKENPROBE_NO_PLATFORM_TRANSPORT=1)
    endif()

    target_link_libraries(${target} PRIVATE Threads::Threads)
endfunction()

#ifndef TOKENPROBE_COMPILER_H
#define TOKENPROBE_COMPILER_H

/*
 * 编译器与平台检测宏。
 *
 * 覆盖四种编译器（MSVC、GCC、Clang、AppleClang）和三种平台
 * （Windows、Linux、macOS），是编写可移植 C 代码的参考写法。
 */

/* ---- 编译器族检测 ---- */

#define TOKENPROBE_COMPILER_MSVC 0
#define TOKENPROBE_COMPILER_APPLECLANG 0
#define TOKENPROBE_COMPILER_CLANG 0
#define TOKENPROBE_COMPILER_GCC 0

#if defined(_MSC_VER)
#undef TOKENPROBE_COMPILER_MSVC
#define TOKENPROBE_COMPILER_MSVC 1
#elif defined(__clang__) && defined(__APPLE__)
#undef TOKENPROBE_COMPILER_APPLECLANG
#define TOKENPROBE_COMPILER_APPLECLANG 1
#elif defined(__clang__)
#undef TOKENPROBE_COMPILER_CLANG
#define TOKENPROBE_COMPILER_CLANG 1
#elif defined(__GNUC__)
#undef TOKENPROBE_COMPILER_GCC
#define TOKENPROBE_COMPILER_GCC 1
#else
#warning "Unknown compiler. Portable macros may not be available."
#endif

/* ---- 平台检测 ---- */

#define TOKENPROBE_PLATFORM_WINDOWS 0
#define TOKENPROBE_PLATFORM_MACOS 0
#define TOKENPROBE_PLATFORM_LINUX 0

#if defined(_WIN32)
#undef TOKENPROBE_PLATFORM_WINDOWS
#define TOKENPROBE_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
#undef TOKENPROBE_PLATFORM_MACOS
#define TOKENPROBE_PLATFORM_MACOS 1
#elif defined(__linux__)
#undef TOKENPROBE_PLATFORM_LINUX
#define TOKENPROBE_PLATFORM_LINUX 1
#endif

/* ---- 可移植的函数属性 ---- */

#if TOKENPROBE_COMPILER_MSVC
#define TOKENPROBE_NORETURN __declspec(noreturn)
#define TOKENPROBE_DEPRECATED(msg) __declspec(deprecated(msg))
#else
#define TOKENPROBE_NORETURN __attribute__((__noreturn__))
#define TOKENPROBE_DEPRECATED(msg) __attribute__((__deprecated__(msg)))
#endif

#endif /* TOKENPROBE_COMPILER_H */

cmake_minimum_required(VERSION 3.21...3.31)

project(TokenProbe
    VERSION 0.0.1
    DESCRIPTION "Probe AI provider API credit, spend and quota from C"
    LANGUAGES C
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(TOKENPROBE_IS_TOP_LEVEL_DEFAULT ON)
else()
    set(TOKENPROBE_IS_TOP_LEVEL_DEFAULT OFF)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# 传输后端。"auto" 在 Windows 上选择 winhttp（操作系统组件，因此 Windows 构建
# 不链接平台 SDK 之外的任何东西），其他平台选择 openssl。
set(TOKENPROBE_HTTP_BACKEND "auto" CACHE STRING
    "HTTP transport: auto, winhttp, openssl or mock (build with no platform transport)")
set_property(CACHE TOKENPROBE_HTTP_BACKEND PROPERTY STRINGS auto winhttp openssl mock)

option(TOKENPROBE_ENABLE_ASAN "Enable AddressSanitizer where supported" OFF)
option(TOKENPROBE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer where supported" OFF)
option(TOKENPROBE_ENABLE_COVERAGE "Enable coverage flags where supported" OFF)
option(TOKENPROBE_BUILD_CLI "Build the tokenprobe command line tool" ${TOKENPROBE_IS_TOP_LEVEL_DEFAULT})
option(TOKENPROBE_BUILD_TESTING "Build CTest tests" ${TOKENPROBE_IS_TOP_LEVEL_DEFAULT})
option(TOKENPROBE_INSTALL "Generate install rules" ${TOKENPROBE_IS_TOP_LEVEL_DEFAULT})
option(TOKENPROBE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(TOKENPROBE_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tokenprobe_version.h.in"
    "${TOKENPROBE_GENERATED_INCLUDE_DIR}/tokenprobe/version.h"
    @ONLY
)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompilerOptions.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sources.cmake")

# 源文件只编译一次，进入对象库，然后被两处使用：
#
#   * tokenprobe_core（静态或共享）——发布的制品，只导出公共 ABI；
#   * 测试内部解析器（JSON、URL）的用例——它们无法链接共享库，
#     因为这些符号有意不导出。
#
# 只编译一次既保留了这类测试，又不必为了测试而扩大公共 ABI。
add_library(tokenprobe_objects OBJECT ${TOKENPROBE_CORE_SOURCES})
set_target_properties(tokenprobe_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(tokenprobe_objects PRIVATE TOKENPROBE_BUILDING_LIBRARY)
if(BUILD_SHARED_LIBS)
    # 对象库先于 tokenprobe_core 编译；shared 构建必须在这里定义
    # TOKENPROBE_SHARED，否则 MSVC 不会生成 dllexport/导入库。
    target_compile_definitions(tokenprobe_objects PRIVATE TOKENPROBE_SHARED)
endif()
target_include_directories(tokenprobe_objects
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${TOKENPROBE_GENERATED_INCLUDE_DIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
tokenprobe_target_defaults(tokenprobe_objects)

# 对象库不会传播自身的链接依赖，因此这里把传输层库作为 PUBLIC 使用要求附加。
# 否则，直接使用 tokenprobe_objects 的目标（内部解析器测试）会链接不到
# WinHTTP 或 OpenSSL。
target_link_libraries(tokenprobe_objects PUBLIC ${TOKENPROBE_TRANSPORT_LIBS})

if(TOKENPROBE_HTTP_BACKEND STREQUAL "winhttp")
    target_compile_definitions(tokenprobe_objects PRIVATE TOKENPROBE_USE_WINHTTP=1)
elseif(TOKENPROBE_HTTP_BACKEND STREQUAL "openssl")
    target_compile_definitions(tokenprobe_objects PRIVATE TOKENPROBE_USE_OPENSSL=1)
else()
    target_compile_definitions(tokenprobe_objects PRIVATE TOKENPROBE_NO_PLATFORM_TRANSPORT=1)
endif()

add_library(tokenprobe_core $<TARGET_OBJECTS:tokenprobe_objects>)
add_library(TokenProbe::tokenprobe_core ALIAS tokenprobe_core)

target_compile_features(tokenprobe_core PUBLIC c_std_11)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(tokenprobe_core PUBLIC TOKENPROBE_SHARED)
endif()

set_target_properties(tokenprobe_core PROPERTIES
    # MSVC 的导入库属于 ARCHIVE；不显式指定时链接行只写裸文件名，
    # 在 shared 构建中会找不到 tokenprobe_core.lib。
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# 只暴露 INTERFACE 包含路径：库自身的源文件已由 tokenprobe_objects 编译，
# 把 -I.../src 暴露给消费方会让他们包含未安装的私有头文件。
target_include_directories(tokenprobe_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${TOKENPROBE_GENERATED_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(tokenprobe_core PRIVATE ${TOKENPROBE_TRANSPORT_LIBS})
tokenprobe_target_defaults(tokenprobe_core)

if(TOKENPROBE_BUILD_CLI)
    if(WIN32)
        set(TOKENPROBE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
        set(TOKENPROBE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
        set(TOKENPROBE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/res/tokenprobe.rc.in"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/tokenprobe.rc"
            @ONLY
        )
    endif()
    add_executable(tokenprobe ${TOKENPROBE_CLI_SOURCES})
    if(WIN32)
        target_sources(tokenprobe PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/generated/tokenprobe.rc"
        )
    endif()
    target_link_libraries(tokenprobe PRIVATE tokenprobe_core)
    tokenprobe_target_defaults(tokenprobe)
    set_target_properties(tokenprobe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

if(TOKENPROBE_BUILD_TESTING)
    enable_testing()
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Tests.cmake")
endif()

if(TOKENPROBE_INSTALL)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Packaging.cmake")
endif()

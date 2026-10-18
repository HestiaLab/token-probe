cmake_minimum_required(VERSION 3.21...3.31)

project(TokenProbeSubprojectSmoke LANGUAGES C)

if(NOT DEFINED TOKENPROBE_SOURCE_DIR)
    message(FATAL_ERROR "TOKENPROBE_SOURCE_DIR must point at the TokenProbe source tree")
endif()

add_subdirectory("${TOKENPROBE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/tokenprobe")

if(NOT TARGET TokenProbe::tokenprobe_core)
    message(FATAL_ERROR "Expected TokenProbe::tokenprobe_core alias target")
endif()

# A consumer that pulls the project in with add_subdirectory() must not inherit
# the CLI, the test suite or install rules.
foreach(option_name IN ITEMS
        TOKENPROBE_BUILD_CLI
        TOKENPROBE_BUILD_TESTING
        TOKENPROBE_INSTALL)
    if(${option_name})
        message(FATAL_ERROR "${option_name} should default to OFF for subproject builds")
    endif()
endforeach()

foreach(target_name IN ITEMS
        tokenprobe
        tokenprobe_decimal_tests
        tokenprobe_tests)
    if(TARGET ${target_name})
        message(FATAL_ERROR "${target_name} should not be created by default for subproject builds")
    endif()
endforeach()

add_executable(subproject_smoke main.c)
target_link_libraries(subproject_smoke PRIVATE TokenProbe::tokenprobe_core)

enable_testing()
add_test(NAME subproject_smoke COMMAND subproject_smoke)

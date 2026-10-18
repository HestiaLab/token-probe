# CPack 打包，所有平台共用。
#
# 生成本项目所用的模板会在 Windows 上提供 WiX MSI、其他平台使用 CPack。
# 这里不再拆分：TokenProbe 是库加 CLI，主要作为库被消费，ZIP/TGZ 制品比
# 安装程序更合适。若将来需要签名 MSI，再重新评估。

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/InstallRules.cmake")

set(CPACK_PACKAGE_INSTALL_DIRECTORY "TokenProbe")
set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY ${PROJECT_DESCRIPTION})
set(CPACK_PACKAGE_VENDOR "TokenProbe contributors")
set(CPACK_PACKAGE_CONTACT "TokenProbe contributors")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
if(PROJECT_HOMEPAGE_URL)
    set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
endif()

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(CPACK_GENERATOR "TGZ")
else()
    # 打包工具可用时优先产出 DEB/RPM，同时始终提供 TGZ。
    find_program(TOKENPROBE_DPKG_EXECUTABLE dpkg)
    find_program(TOKENPROBE_RPMBUILD_EXECUTABLE rpmbuild)
    set(CPACK_GENERATOR "TGZ")
    if(TOKENPROBE_DPKG_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "DEB")
        set(CPACK_DEBIAN_PACKAGE_MAINTAINER "TokenProbe contributors")
        set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
        set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libssl3 | libssl1.1")
    endif()
    if(TOKENPROBE_RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
        set(CPACK_RPM_PACKAGE_LICENSE "MIT")
        set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
        set(CPACK_RPM_PACKAGE_REQUIRES "openssl-libs")
    endif()
endif()

set(CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set(CPACK_SOURCE_IGNORE_FILES
    "/build/"
    "/_CPack_Packages/"
    "/\\.git/"
    "/\\.vs/"
    "/\\.cache/"
    "/dist/"
    "/out/"
    "\\\\.swp$"
    "\\\\.swo$"
    "\\\\.user$"
    "\\\\.idx$"
)

include(CPack)

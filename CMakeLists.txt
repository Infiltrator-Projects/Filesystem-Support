# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEM_SUPPORT_VERSION)
string(STRIP "${FILESYSTEM_SUPPORT_VERSION}" FILESYSTEM_SUPPORT_VERSION)
if(NOT FILESYSTEM_SUPPORT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain major.minor.patch")
endif()

project(FilesystemSupport VERSION "${FILESYSTEM_SUPPORT_VERSION}" LANGUAGES C CXX)
include(CTest)
include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FILESYSTEM_SUPPORT_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

set(INFILTRATR_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/infiltratr-common")
set(INFILTRATR_COMMON_EXPECTED_VERSION "1.19.35")
set(INFILTRATR_COMMON_EXPECTED_COMMIT "7cc5de3de0e94ed2cfcff0840bbb5346eb5c9c9f")

if(NOT EXISTS "${INFILTRATR_COMMON_DIR}/VERSION")
    message(FATAL_ERROR "Pinned Infiltratr Common submodule is required")
endif()

file(READ "${INFILTRATR_COMMON_DIR}/VERSION" INFILTRATR_COMMON_VERSION)
string(STRIP "${INFILTRATR_COMMON_VERSION}" INFILTRATR_COMMON_VERSION)
if(NOT INFILTRATR_COMMON_VERSION STREQUAL INFILTRATR_COMMON_EXPECTED_VERSION)
    message(FATAL_ERROR
        "Filesystem Support requires Common ${INFILTRATR_COMMON_EXPECTED_VERSION}; found ${INFILTRATR_COMMON_VERSION}")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    find_package(Git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${INFILTRATR_COMMON_DIR}" rev-parse HEAD
        RESULT_VARIABLE COMMON_GIT_RESULT
        OUTPUT_VARIABLE INFILTRATR_COMMON_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT COMMON_GIT_RESULT EQUAL 0 OR
       NOT INFILTRATR_COMMON_COMMIT STREQUAL INFILTRATR_COMMON_EXPECTED_COMMIT)
        message(FATAL_ERROR
            "Filesystem Support requires Common commit ${INFILTRATR_COMMON_EXPECTED_COMMIT}; found ${INFILTRATR_COMMON_COMMIT}")
    endif()
endif()

set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_WARNINGS_AS_ERRORS ON CACHE BOOL "" FORCE)
add_subdirectory("${INFILTRATR_COMMON_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/infiltratr-common" EXCLUDE_FROM_ALL)

set(FILESYSTEM_SUPPORT_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow)

function(filesystem_support_configure target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(FILESYSTEM_SUPPORT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang" OR
           CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${target} PRIVATE ${FILESYSTEM_SUPPORT_WARNING_FLAGS})
        if(FILESYSTEM_SUPPORT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Filesystem-neutral native mechanisms. Platform-specific adapters provide I/O
# and operating-system integration; filesystem semantics must not depend on
# Linux VFS or Windows IFS objects.
add_library(infiltratr-fs-native-core STATIC
    native/core/status.c
    native/core/io.c
    native/core/byte_reader.c
    native/core/filesystem.c)
target_include_directories(infiltratr-fs-native-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/include")
target_link_libraries(infiltratr-fs-native-core
    PUBLIC InfiltratrCommon::Portable)
filesystem_support_configure(infiltratr-fs-native-core)

# Canonical EXT2 on-disk semantics. This exact source is also compiled into
# the Linux and Windows adapters; OS-specific code must not reimplement it.
add_library(infiltratr-fs-ext2-core STATIC
    native/filesystems/ext2/core/ext2_core.c
    native/filesystems/ext2/core/ext2_engine.c)
target_include_directories(infiltratr-fs-ext2-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ext2/core")
filesystem_support_configure(infiltratr-fs-ext2-core)


# Canonical EXT3 feature/format policy. Linux and future Windows adapters
# consume this exact implementation rather than maintaining independent rules.
add_library(infiltratr-fs-ext3-core STATIC
    native/filesystems/ext3/core/ext3_core.c)
target_include_directories(infiltratr-fs-ext3-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ext3/core")
filesystem_support_configure(infiltratr-fs-ext3-core)

# Canonical EXT4 feature/format policy shared by every host adapter.
add_library(infiltratr-fs-ext4-core STATIC
    native/filesystems/ext4/core/ext4_core.c)
target_include_directories(infiltratr-fs-ext4-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ext4/core")
filesystem_support_configure(infiltratr-fs-ext4-core)

# Canonical Amiga OFS and FFS format classification.
add_library(infiltratr-fs-ofs-core STATIC
    native/filesystems/ofs/core/ofs_core.c
    native/filesystems/ofs/core/ofs_primitives.c)
target_include_directories(infiltratr-fs-ofs-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ofs/core")
filesystem_support_configure(infiltratr-fs-ofs-core)

add_library(infiltratr-fs-ffs-core STATIC
    native/filesystems/ffs/core/ffs_core.c
    native/filesystems/ffs/core/ffs_primitives.c)
target_include_directories(infiltratr-fs-ffs-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ffs/core")
filesystem_support_configure(infiltratr-fs-ffs-core)

# Canonical Amiga SFS format/layout policy shared by host adapters.
add_library(infiltratr-fs-sfs-core STATIC
    native/filesystems/sfs/core/sfs_core.c)
target_include_directories(infiltratr-fs-sfs-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/sfs/core")
filesystem_support_configure(infiltratr-fs-sfs-core)

# Canonical Amiga SFS2 format policy. SFS2 is deliberately independent
# from the SFS core because its on-disk structures are not 1:1 compatible.
add_library(infiltratr-fs-sfs2-core STATIC
    native/filesystems/sfs2/core/sfs2_core.c)
target_include_directories(infiltratr-fs-sfs2-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/sfs2/core")
filesystem_support_configure(infiltratr-fs-sfs2-core)

# Canonical PFS format/geometry policy used by the PFS3 implementation.
add_library(infiltratr-fs-pfs3-core STATIC
    native/filesystems/pfs3/core/pfs3_core.c)
target_include_directories(infiltratr-fs-pfs3-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/pfs3/core")
filesystem_support_configure(infiltratr-fs-pfs3-core)

# The catalogue is intentionally platform-neutral. Linux package/probe policy
# is layered on top of it; the Windows manager consumes the same identities.
add_library(filesystem-support-catalog STATIC
    src/catalog.cpp
    src/platform_support.cpp)
target_include_directories(filesystem-support-catalog
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(filesystem-support-catalog
    PUBLIC InfiltratrCommon::Portable)
target_compile_definitions(filesystem-support-catalog
    PUBLIC FILESYSTEM_SUPPORT_VERSION="${PROJECT_VERSION}")
filesystem_support_configure(filesystem-support-catalog)

if(WIN32)
    # Native Windows management shell. It deliberately exposes the complete
    # catalogue while keeping unqualified filesystem modules non-installable.
    add_executable(filesystem-support WIN32
        windows/manager/main.cpp)
    target_link_libraries(filesystem-support
        PRIVATE filesystem-support-catalog Comctl32)
    target_compile_definitions(filesystem-support
        PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    filesystem_support_configure(filesystem-support)

    install(TARGETS filesystem-support
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

if(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
    pkg_check_modules(GIO2 REQUIRED IMPORTED_TARGET gio-2.0)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0>=3.22)

    add_library(infiltratr-fs-userspace STATIC
        native/platform/userspace/file_io.c)
    target_include_directories(infiltratr-fs-userspace
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/native/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/native/platform/userspace")
    target_link_libraries(infiltratr-fs-userspace
        PUBLIC infiltratr-fs-native-core InfiltratrCommon::Common)
    filesystem_support_configure(infiltratr-fs-userspace)

    add_executable(fsinspect
        tools/fsinspect/main.c)
    target_link_libraries(fsinspect PRIVATE infiltratr-fs-userspace)
    filesystem_support_configure(fsinspect)

    add_library(filesystem-support-core STATIC
        src/probe.cpp)
    target_include_directories(filesystem-support-core
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(filesystem-support-core
        PUBLIC filesystem-support-catalog PkgConfig::GLIB2)
    filesystem_support_configure(filesystem-support-core)

    add_library(filesystem-support-actions STATIC
        src/installer.cpp)
    target_include_directories(filesystem-support-actions
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(filesystem-support-actions
        PUBLIC filesystem-support-core PkgConfig::GIO2)
    filesystem_support_configure(filesystem-support-actions)

    add_executable(filesystem-support
        src/main.cpp)
    target_link_libraries(filesystem-support
        PRIVATE filesystem-support-actions InfiltratrCommon::Portable PkgConfig::GTK3)
    filesystem_support_configure(filesystem-support)

    install(TARGETS filesystem-support
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES data/org.infiltrator.FilesystemSupport.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

    # Project-owned native Linux filesystem modules are built against the
    # running kernel at install time. Ship the exact source needed by the
    # privileged helper rather than pretending a generic prebuilt .ko is
    # portable across kernel ABIs.
    install(PROGRAMS packaging/linux/native-module-helper.sh
        DESTINATION "lib/infiltrator-filesystem-support"
        RENAME native-module-helper)

    foreach(native_fs IN ITEMS ext2 ext3 ext4 ofs ffs sfs)
        install(DIRECTORY
            "native/filesystems/${native_fs}/core"
            "native/filesystems/${native_fs}/linux"
            DESTINATION "lib/infiltrator-filesystem-support/native/filesystems/${native_fs}"
            PATTERN "*.o" EXCLUDE
            PATTERN "*.ko" EXCLUDE
            PATTERN "*.mod" EXCLUDE
            PATTERN "*.mod.c" EXCLUDE
            PATTERN "Module.symvers" EXCLUDE
            PATTERN "modules.order" EXCLUDE)
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(filesystem-support-catalog-test
        tests/catalog_test.cpp)
    target_link_libraries(filesystem-support-catalog-test
        PRIVATE filesystem-support-catalog)
    filesystem_support_configure(filesystem-support-catalog-test)
    add_test(
        NAME filesystem-support-catalog
        COMMAND filesystem-support-catalog-test
                "${CMAKE_CURRENT_SOURCE_DIR}/docs/FILESYSTEM_SUPPORT_MATRIX.md")

    add_executable(filesystem-support-native-core-test
        tests/native_core_test.c)
    target_link_libraries(filesystem-support-native-core-test
        PRIVATE infiltratr-fs-native-core)
    filesystem_support_configure(filesystem-support-native-core-test)
    add_test(
        NAME filesystem-support-native-core
        COMMAND filesystem-support-native-core-test)

    add_executable(filesystem-support-ext2-core-test
        tests/ext2_core_test.c)
    target_link_libraries(filesystem-support-ext2-core-test
        PRIVATE infiltratr-fs-ext2-core)
    filesystem_support_configure(filesystem-support-ext2-core-test)
    add_test(
        NAME filesystem-support-ext2-core
        COMMAND filesystem-support-ext2-core-test)

    add_executable(filesystem-support-ext2-engine-test
        tests/ext2_engine_test.c)
    target_link_libraries(filesystem-support-ext2-engine-test
        PRIVATE infiltratr-fs-ext2-core)
    filesystem_support_configure(filesystem-support-ext2-engine-test)
    add_test(
        NAME filesystem-support-ext2-engine
        COMMAND filesystem-support-ext2-engine-test)

    add_executable(filesystem-support-ext3-core-test
        tests/ext3_core_test.c)
    target_link_libraries(filesystem-support-ext3-core-test
        PRIVATE infiltratr-fs-ext3-core)
    filesystem_support_configure(filesystem-support-ext3-core-test)
    add_test(
        NAME filesystem-support-ext3-core
        COMMAND filesystem-support-ext3-core-test)

    add_executable(filesystem-support-ext4-core-test
        tests/ext4_core_test.c)
    target_link_libraries(filesystem-support-ext4-core-test
        PRIVATE infiltratr-fs-ext4-core)
    filesystem_support_configure(filesystem-support-ext4-core-test)
    add_test(
        NAME filesystem-support-ext4-core
        COMMAND filesystem-support-ext4-core-test)

    add_executable(filesystem-support-ofs-core-test
        tests/ofs_core_test.c)
    target_link_libraries(filesystem-support-ofs-core-test
        PRIVATE infiltratr-fs-ofs-core)
    filesystem_support_configure(filesystem-support-ofs-core-test)
    add_test(NAME filesystem-support-ofs-core COMMAND filesystem-support-ofs-core-test)

    add_executable(filesystem-support-ofs-primitives-test
        tests/ofs_primitives_test.c)
    target_link_libraries(filesystem-support-ofs-primitives-test
        PRIVATE infiltratr-fs-ofs-core)
    filesystem_support_configure(filesystem-support-ofs-primitives-test)
    add_test(NAME filesystem-support-ofs-primitives COMMAND filesystem-support-ofs-primitives-test)

    add_executable(filesystem-support-ffs-core-test
        tests/ffs_core_test.c)
    target_link_libraries(filesystem-support-ffs-core-test
        PRIVATE infiltratr-fs-ffs-core)
    filesystem_support_configure(filesystem-support-ffs-core-test)
    add_test(NAME filesystem-support-ffs-core COMMAND filesystem-support-ffs-core-test)

    add_executable(filesystem-support-ffs-primitives-test
        tests/ffs_primitives_test.c)
    target_link_libraries(filesystem-support-ffs-primitives-test
        PRIVATE infiltratr-fs-ffs-core)
    filesystem_support_configure(filesystem-support-ffs-primitives-test)
    add_test(NAME filesystem-support-ffs-primitives COMMAND filesystem-support-ffs-primitives-test)

    add_executable(filesystem-support-sfs-core-test
        tests/sfs_core_test.c)
    target_link_libraries(filesystem-support-sfs-core-test
        PRIVATE infiltratr-fs-sfs-core)
    filesystem_support_configure(filesystem-support-sfs-core-test)
    add_test(
        NAME filesystem-support-sfs-core
        COMMAND filesystem-support-sfs-core-test)

    add_executable(filesystem-support-sfs2-core-test
        tests/sfs2_core_test.c)
    target_link_libraries(filesystem-support-sfs2-core-test
        PRIVATE infiltratr-fs-sfs2-core)
    filesystem_support_configure(filesystem-support-sfs2-core-test)
    add_test(
        NAME filesystem-support-sfs2-core
        COMMAND filesystem-support-sfs2-core-test)

    add_executable(filesystem-support-pfs3-core-test
        tests/pfs3_core_test.c)
    target_link_libraries(filesystem-support-pfs3-core-test
        PRIVATE infiltratr-fs-pfs3-core)
    filesystem_support_configure(filesystem-support-pfs3-core-test)
    add_test(
        NAME filesystem-support-pfs3-core
        COMMAND filesystem-support-pfs3-core-test)

    add_executable(filesystem-support-windows-catalog-test
        tests/windows_catalog_test.cpp)
    target_link_libraries(filesystem-support-windows-catalog-test
        PRIVATE filesystem-support-catalog)
    filesystem_support_configure(filesystem-support-windows-catalog-test)
    add_test(
        NAME filesystem-support-windows-catalog
        COMMAND filesystem-support-windows-catalog-test)

    if(UNIX)
        add_executable(filesystem-support-action-policy-test
            tests/action_policy_test.cpp)
        target_link_libraries(filesystem-support-action-policy-test
            PRIVATE filesystem-support-actions)
        filesystem_support_configure(filesystem-support-action-policy-test)
        add_test(
            NAME filesystem-support-action-policy
            COMMAND filesystem-support-action-policy-test)
    endif()
endif()

if(UNIX)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "infiltrator-filesystem-support")
    set(CPACK_PACKAGE_VENDOR "Infiltrator Projects")
    set(CPACK_PACKAGE_CONTACT "Shannon Smith")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Discover and install filesystem support on Debian")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME "Filesystem-Support-${PROJECT_VERSION}-amd64")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Shannon Smith")
    set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "policykit-1, apt, dpkg, kmod, mokutil")
    include(CPack)
endif()

# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEMS_VERSION)
string(STRIP "${FILESYSTEMS_VERSION}" FILESYSTEMS_VERSION)
if(NOT FILESYSTEMS_VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+$")
    message(FATAL_ERROR "VERSION must contain major.minor.patch")
endif()

project(Filesystems VERSION "${FILESYSTEMS_VERSION}" LANGUAGES C CXX)
include(CTest)
include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FILESYSTEMS_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

# Native deployment policy owns only filesystem/module membership. Desktop
# integration has an independent authority in data/desktop-filesystems.tsv and
# must never influence which kernel modules are built, packaged or managed.
set(FILESYSTEMS_NATIVE_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/data/native-filesystems.tsv")
file(STRINGS "${FILESYSTEMS_NATIVE_MANIFEST}"
    FILESYSTEMS_NATIVE_MANIFEST_LINES)
set(FILESYSTEMS_NATIVE_FILESYSTEMS)
set(FILESYSTEMS_NATIVE_MODULES)
foreach(native_line IN LISTS FILESYSTEMS_NATIVE_MANIFEST_LINES)
    string(STRIP "${native_line}" native_line)
    if(native_line STREQUAL "" OR native_line MATCHES "^#")
        continue()
    endif()

    string(REPLACE "\t" ";" native_fields "${native_line}")
    list(LENGTH native_fields native_field_count)
    if(NOT native_field_count EQUAL 2)
        message(FATAL_ERROR
            "Invalid native filesystem manifest row: ${native_line}")
    endif()

    list(GET native_fields 0 native_fs)
    list(GET native_fields 1 native_module)
    if(NOT native_fs MATCHES "^[A-Za-z0-9_.+-]+$" OR
       NOT native_module MATCHES "^[A-Za-z0-9_.+-]+$")
        message(FATAL_ERROR
            "Invalid native filesystem/module token: ${native_line}")
    endif()

    list(FIND FILESYSTEMS_NATIVE_FILESYSTEMS
        "${native_fs}" native_duplicate_index)
    if(NOT native_duplicate_index EQUAL -1)
        message(FATAL_ERROR
            "Duplicate native filesystem in manifest: ${native_fs}")
    endif()
    list(FIND FILESYSTEMS_NATIVE_MODULES
        "${native_module}" module_duplicate_index)
    if(NOT module_duplicate_index EQUAL -1)
        message(FATAL_ERROR
            "Duplicate native module in manifest: ${native_module}")
    endif()

    list(APPEND FILESYSTEMS_NATIVE_FILESYSTEMS "${native_fs}")
    list(APPEND FILESYSTEMS_NATIVE_MODULES "${native_module}")
    set("FILESYSTEMS_NATIVE_MODULE_${native_fs}" "${native_module}")
endforeach()
if(NOT FILESYSTEMS_NATIVE_FILESYSTEMS)
    message(FATAL_ERROR "Native filesystem deployment manifest is empty")
endif()

# Generate the C++ native policy directly from the same deployment manifest.
# catalog.cpp therefore cannot acquire a second, independently maintained list
# of native filesystem IDs or kernel-module names.
set(FILESYSTEMS_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${FILESYSTEMS_GENERATED_DIR}")
set(FILESYSTEMS_NATIVE_POLICY_HEADER
    "${FILESYSTEMS_GENERATED_DIR}/native_filesystems.generated.hpp")
list(LENGTH FILESYSTEMS_NATIVE_FILESYSTEMS
    FILESYSTEMS_NATIVE_FILESYSTEM_COUNT)
set(native_policy_header
    "// Generated from data/native-filesystems.tsv; do not edit.\n#pragma once\n\n#include <array>\n#include <string_view>\n\nnamespace filesystems {\n\nstruct NativeFilesystemPolicy {\n    std::string_view filesystem_id;\n    std::string_view module;\n};\n\ninline constexpr std::array<NativeFilesystemPolicy, ${FILESYSTEMS_NATIVE_FILESYSTEM_COUNT}> kNativeFilesystemPolicies = {{\n")
foreach(native_fs IN LISTS FILESYSTEMS_NATIVE_FILESYSTEMS)
    set(native_module "${FILESYSTEMS_NATIVE_MODULE_${native_fs}}")
    string(APPEND native_policy_header
        "    {\"${native_fs}\", \"${native_module}\"},\n")
endforeach()
string(APPEND native_policy_header "}};\n\n} // namespace filesystems\n")
file(WRITE "${FILESYSTEMS_NATIVE_POLICY_HEADER}" "${native_policy_header}")

set(INFILTRATR_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/infiltratr-common")
set(INFILTRATR_COMMON_EXPECTED_VERSION "1.19.46")

if(NOT EXISTS "${INFILTRATR_COMMON_DIR}/VERSION")
    message(FATAL_ERROR "Pinned Libraries submodule is required")
endif()

file(READ "${INFILTRATR_COMMON_DIR}/VERSION" INFILTRATR_COMMON_VERSION)
string(STRIP "${INFILTRATR_COMMON_VERSION}" INFILTRATR_COMMON_VERSION)
if(NOT INFILTRATR_COMMON_VERSION STREQUAL INFILTRATR_COMMON_EXPECTED_VERSION)
    message(FATAL_ERROR
        "Filesystems requires Common ${INFILTRATR_COMMON_EXPECTED_VERSION}; found ${INFILTRATR_COMMON_VERSION}")
endif()

# Git itself owns the exact Common revision through the submodule gitlink. Do
# not duplicate that SHA in CMake: compare the checked-out submodule directly
# with the commit recorded by this Filesystems revision.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    find_package(Git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                rev-parse "HEAD:src/infiltratr-common"
        RESULT_VARIABLE COMMON_GITLINK_RESULT
        OUTPUT_VARIABLE INFILTRATR_COMMON_GITLINK
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${INFILTRATR_COMMON_DIR}" rev-parse HEAD
        RESULT_VARIABLE COMMON_CHECKOUT_RESULT
        OUTPUT_VARIABLE INFILTRATR_COMMON_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT COMMON_GITLINK_RESULT EQUAL 0 OR
       NOT COMMON_CHECKOUT_RESULT EQUAL 0 OR
       NOT INFILTRATR_COMMON_COMMIT STREQUAL INFILTRATR_COMMON_GITLINK)
        message(FATAL_ERROR
            "Libraries checkout ${INFILTRATR_COMMON_COMMIT} does not match repository gitlink ${INFILTRATR_COMMON_GITLINK}")
    endif()
endif()

set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_WARNINGS_AS_ERRORS ON CACHE BOOL "" FORCE)
add_subdirectory("${INFILTRATR_COMMON_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/infiltratr-common" EXCLUDE_FROM_ALL)

set(FILESYSTEMS_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow)

function(filesystems_configure target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(FILESYSTEMS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang" OR
           CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${target} PRIVATE ${FILESYSTEMS_WARNING_FLAGS})
        if(FILESYSTEMS_WARNINGS_AS_ERRORS)
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
filesystems_configure(infiltratr-fs-native-core)

# Canonical EXT2 on-disk semantics. This exact source is also compiled into
# the Linux and Windows adapters; OS-specific code must not reimplement it.
add_library(infiltratr-fs-ext2-core STATIC
    native/filesystems/ext2/core/ext2_core.c
    native/filesystems/ext2/core/ext2_engine.c)
target_include_directories(infiltratr-fs-ext2-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ext2/core")
filesystems_configure(infiltratr-fs-ext2-core)

# Canonical EXT3 feature/format policy. Linux and future Windows adapters
# consume this exact implementation rather than maintaining independent rules.
add_library(infiltratr-fs-ext3-core STATIC
    native/filesystems/ext3/core/ext3_core.c)
target_include_directories(infiltratr-fs-ext3-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ext3/core")
filesystems_configure(infiltratr-fs-ext3-core)

# Canonical EXT4 feature/format policy shared by every host adapter.
add_library(infiltratr-fs-ext4-core STATIC
    native/filesystems/ext4/core/ext4_core.c)
target_include_directories(infiltratr-fs-ext4-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ext4/core")
filesystems_configure(infiltratr-fs-ext4-core)

# Canonical Amiga OFS and FFS format classification.
add_library(infiltratr-fs-ofs-core STATIC
    native/filesystems/ofs/core/ofs_core.c
    native/filesystems/ofs/core/ofs_primitives.c)
target_include_directories(infiltratr-fs-ofs-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ofs/core")
filesystems_configure(infiltratr-fs-ofs-core)

add_library(infiltratr-fs-ffs-core STATIC
    native/filesystems/ffs/core/ffs_core.c
    native/filesystems/ffs/core/ffs_primitives.c)
target_include_directories(infiltratr-fs-ffs-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ffs/core")
filesystems_configure(infiltratr-fs-ffs-core)

# Canonical Amiga SFS format/layout policy shared by host adapters.
add_library(infiltratr-fs-sfs-core STATIC
    native/filesystems/sfs/core/sfs_core.c)
target_include_directories(infiltratr-fs-sfs-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/sfs/core")
filesystems_configure(infiltratr-fs-sfs-core)

# Canonical Amiga SFS2 format policy. SFS2 is deliberately independent
# from the SFS core because its on-disk structures are not 1:1 compatible.
add_library(infiltratr-fs-sfs2-core STATIC
    native/filesystems/sfs2/core/sfs2_core.c)
target_include_directories(infiltratr-fs-sfs2-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/sfs2/core")
filesystems_configure(infiltratr-fs-sfs2-core)

# Canonical PFS format/geometry policy used by the PFS3 implementation.
add_library(infiltratr-fs-pfs3-core STATIC
    native/filesystems/pfs3/core/pfs3_core.c)
target_include_directories(infiltratr-fs-pfs3-core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/pfs3/core")
filesystems_configure(infiltratr-fs-pfs3-core)

# Stable on-media identity is a platform-neutral authority. Keep it physically
# separate from Linux package/provider policy so non-Linux consumers cannot
# acquire Debian concerns through a target-level dependency.
add_library(filesystems-identity STATIC
    src/platform_support.cpp)
target_include_directories(filesystems-identity
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(filesystems-identity
    PUBLIC InfiltratrCommon::Portable)
target_compile_definitions(filesystems-identity
    PUBLIC FILESYSTEMS_VERSION="${PROJECT_VERSION}")
filesystems_configure(filesystems-identity)

# Linux catalogue materializes stable identity with Linux provider/package and
# native-deployment policy. This target is therefore intentionally Linux-aware.
add_library(filesystems-catalog STATIC
    src/catalog.cpp)
target_include_directories(filesystems-catalog
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${FILESYSTEMS_GENERATED_DIR}")
target_link_libraries(filesystems-catalog
    PUBLIC filesystems-identity InfiltratrCommon::Portable)
filesystems_configure(filesystems-catalog)

if(WIN32)
    # Native Windows management shell consumes stable filesystem identity only;
    # Debian package/provider policy is not part of its dependency graph.
    add_executable(filesystems WIN32
        windows/manager/main.cpp)
    target_link_libraries(filesystems
        PRIVATE filesystems-identity Comctl32)
    target_compile_definitions(filesystems
        PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    filesystems_configure(filesystems)

    install(TARGETS filesystems
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
    filesystems_configure(infiltratr-fs-userspace)

    add_executable(fsinspect
        tools/fsinspect/main.c)
    target_link_libraries(fsinspect PRIVATE infiltratr-fs-userspace)
    filesystems_configure(fsinspect)

    add_library(filesystems-core STATIC
        src/probe.cpp
        src/action_policy.cpp)
    target_include_directories(filesystems-core
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(filesystems-core
        PUBLIC filesystems-catalog PkgConfig::GLIB2)
    filesystems_configure(filesystems-core)

    add_library(filesystems-actions STATIC
        src/installer.cpp
        src/desktop_actions.cpp)
    target_include_directories(filesystems-actions
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(filesystems-actions
        PUBLIC filesystems-core PkgConfig::GIO2)
    filesystems_configure(filesystems-actions)

    add_executable(filesystems
        src/main.cpp)
    target_link_libraries(filesystems
        PRIVATE filesystems-actions InfiltratrCommon::Portable PkgConfig::GTK3)
    filesystems_configure(filesystems)

    install(TARGETS filesystems
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES data/org.infiltrator.Filesystems.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

    # Project-owned native Linux filesystem modules are built against the
    # running kernel at install time. The coordinator and kernel transaction
    # helper are separate installed programs so desktop policy can never leak
    # into the kernel rollback implementation.
    install(PROGRAMS packaging/linux/native-module-helper.sh
        DESTINATION "lib/filesystems"
        RENAME native-module-helper)
    install(PROGRAMS packaging/linux/kernel-module-helper.sh
        DESTINATION "lib/filesystems"
        RENAME kernel-module-helper)
    install(FILES "${FILESYSTEMS_NATIVE_MANIFEST}"
        DESTINATION "lib/filesystems"
        RENAME native-filesystems.tsv)

    # Shared kernel-safe adapter source is compiled directly into each
    # filesystem module that consumes it. It is not installed as a shared
    # runtime module and therefore cannot couple the .ko lifecycles together.
    install(DIRECTORY "native/platform/linux_kernel/"
        DESTINATION "lib/filesystems/native/platform/linux_kernel"
        FILES_MATCHING PATTERN "*.h")

    foreach(native_fs IN LISTS FILESYSTEMS_NATIVE_FILESYSTEMS)
        install(DIRECTORY
            "native/filesystems/${native_fs}/core"
            "native/filesystems/${native_fs}/linux"
            DESTINATION "lib/filesystems/native/filesystems/${native_fs}"
            PATTERN "*.o" EXCLUDE
            PATTERN "*.ko" EXCLUDE
            PATTERN "*.mod" EXCLUDE
            PATTERN "*.mod.c" EXCLUDE
            PATTERN "Module.symvers" EXCLUDE
            PATTERN "modules.order" EXCLUDE)
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(filesystems-catalog-test
        tests/catalog_test.cpp)
    target_link_libraries(filesystems-catalog-test
        PRIVATE filesystems-catalog)
    filesystems_configure(filesystems-catalog-test)
    add_test(
        NAME filesystems-catalog
        COMMAND filesystems-catalog-test
                "${CMAKE_CURRENT_SOURCE_DIR}/docs/FILESYSTEMS_MATRIX.md"
                "${FILESYSTEMS_NATIVE_MANIFEST}")

    add_executable(filesystems-native-core-test
        tests/native_core_test.c)
    target_link_libraries(filesystems-native-core-test
        PRIVATE infiltratr-fs-native-core)
    filesystems_configure(filesystems-native-core-test)
    add_test(
        NAME filesystems-native-core
        COMMAND filesystems-native-core-test)

    add_executable(filesystems-ext2-core-test
        tests/ext2_core_test.c)
    target_link_libraries(filesystems-ext2-core-test
        PRIVATE infiltratr-fs-ext2-core)
    filesystems_configure(filesystems-ext2-core-test)
    add_test(
        NAME filesystems-ext2-core
        COMMAND filesystems-ext2-core-test)

    add_executable(filesystems-ext2-engine-test
        tests/ext2_engine_test.c)
    target_link_libraries(filesystems-ext2-engine-test
        PRIVATE infiltratr-fs-ext2-core)
    filesystems_configure(filesystems-ext2-engine-test)
    add_test(
        NAME filesystems-ext2-engine
        COMMAND filesystems-ext2-engine-test)

    add_executable(filesystems-ext3-core-test
        tests/ext3_core_test.c)
    target_link_libraries(filesystems-ext3-core-test
        PRIVATE infiltratr-fs-ext3-core)
    filesystems_configure(filesystems-ext3-core-test)
    add_test(
        NAME filesystems-ext3-core
        COMMAND filesystems-ext3-core-test)

    add_executable(filesystems-ext4-core-test
        tests/ext4_core_test.c)
    target_link_libraries(filesystems-ext4-core-test
        PRIVATE infiltratr-fs-ext4-core)
    filesystems_configure(filesystems-ext4-core-test)
    add_test(
        NAME filesystems-ext4-core
        COMMAND filesystems-ext4-core-test)

    add_executable(filesystems-ofs-core-test
        tests/ofs_core_test.c)
    target_link_libraries(filesystems-ofs-core-test
        PRIVATE infiltratr-fs-ofs-core)
    filesystems_configure(filesystems-ofs-core-test)
    add_test(NAME filesystems-ofs-core COMMAND filesystems-ofs-core-test)

    add_executable(filesystems-ofs-primitives-test
        tests/ofs_primitives_test.c)
    target_link_libraries(filesystems-ofs-primitives-test
        PRIVATE infiltratr-fs-ofs-core)
    filesystems_configure(filesystems-ofs-primitives-test)
    add_test(NAME filesystems-ofs-primitives COMMAND filesystems-ofs-primitives-test)

    add_executable(filesystems-ffs-core-test
        tests/ffs_core_test.c)
    target_link_libraries(filesystems-ffs-core-test
        PRIVATE infiltratr-fs-ffs-core)
    filesystems_configure(filesystems-ffs-core-test)
    add_test(NAME filesystems-ffs-core COMMAND filesystems-ffs-core-test)

    add_executable(filesystems-ffs-primitives-test
        tests/ffs_primitives_test.c)
    target_link_libraries(filesystems-ffs-primitives-test
        PRIVATE infiltratr-fs-ffs-core)
    filesystems_configure(filesystems-ffs-primitives-test)
    add_test(NAME filesystems-ffs-primitives COMMAND filesystems-ffs-primitives-test)

    add_executable(filesystems-sfs-core-test
        tests/sfs_core_test.c)
    target_link_libraries(filesystems-sfs-core-test
        PRIVATE infiltratr-fs-sfs-core)
    filesystems_configure(filesystems-sfs-core-test)
    add_test(
        NAME filesystems-sfs-core
        COMMAND filesystems-sfs-core-test)

    add_executable(filesystems-sfs2-core-test
        tests/sfs2_core_test.c)
    target_link_libraries(filesystems-sfs2-core-test
        PRIVATE infiltratr-fs-sfs2-core)
    filesystems_configure(filesystems-sfs2-core-test)
    add_test(
        NAME filesystems-sfs2-core
        COMMAND filesystems-sfs2-core-test)

    add_executable(filesystems-pfs3-core-test
        tests/pfs3_core_test.c)
    target_link_libraries(filesystems-pfs3-core-test
        PRIVATE infiltratr-fs-pfs3-core)
    filesystems_configure(filesystems-pfs3-core-test)
    add_test(
        NAME filesystems-pfs3-core
        COMMAND filesystems-pfs3-core-test)

    add_executable(filesystems-windows-catalog-test
        tests/windows_catalog_test.cpp)
    target_link_libraries(filesystems-windows-catalog-test
        PRIVATE filesystems-identity)
    filesystems_configure(filesystems-windows-catalog-test)
    add_test(
        NAME filesystems-windows-catalog
        COMMAND filesystems-windows-catalog-test)

    if(UNIX)
        add_executable(filesystems-action-policy-test
            tests/action_policy_test.cpp)
        target_link_libraries(filesystems-action-policy-test
            PRIVATE filesystems-actions)
        filesystems_configure(filesystems-action-policy-test)
        add_test(
            NAME filesystems-action-policy
            COMMAND filesystems-action-policy-test)

        add_executable(filesystems-desktop-actions-test
            tests/desktop_actions_test.cpp)
        target_link_libraries(filesystems-desktop-actions-test
            PRIVATE filesystems-actions)
        filesystems_configure(filesystems-desktop-actions-test)
        add_test(
            NAME filesystems-desktop-actions
            COMMAND filesystems-desktop-actions-test)
    endif()
endif()

if(UNIX)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "filesystems")
    set(CPACK_PACKAGE_VENDOR "Infiltrator Projects")
    set(CPACK_PACKAGE_CONTACT "Shannon Smith")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Filesystems - discover and install filesystem support on Debian")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME "Filesystems-${PROJECT_VERSION}-amd64")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Shannon Smith")
    set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "policykit-1, apt, dpkg, kmod, mokutil")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/amiga-desktop-detection.cmake")
    include(CPack)
endif()

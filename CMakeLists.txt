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
set(INFILTRATR_COMMON_EXPECTED_VERSION "1.19.22")
set(INFILTRATR_COMMON_EXPECTED_COMMIT "302c44eb7436803dee020667453a9a0681da8bbf")

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

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0>=3.22)

set(FILESYSTEM_SUPPORT_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow)

function(filesystem_support_configure target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${target} PRIVATE ${FILESYSTEM_SUPPORT_WARNING_FLAGS})
        if(FILESYSTEM_SUPPORT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_library(filesystem-support-core STATIC
    src/catalog.cpp
    src/probe.cpp)
target_include_directories(filesystem-support-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(filesystem-support-core PUBLIC InfiltratrCommon::Portable PkgConfig::GLIB2)
target_compile_definitions(filesystem-support-core PUBLIC FILESYSTEM_SUPPORT_VERSION="${PROJECT_VERSION}")
filesystem_support_configure(filesystem-support-core)

add_executable(filesystem-support src/main.cpp)
target_link_libraries(filesystem-support PRIVATE filesystem-support-core InfiltratrCommon::Portable PkgConfig::GTK3)
filesystem_support_configure(filesystem-support)

install(TARGETS filesystem-support RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES data/org.infiltrator.FilesystemSupport.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

if(BUILD_TESTING)
    add_executable(filesystem-support-catalog-test tests/catalog_test.cpp)
    target_link_libraries(filesystem-support-catalog-test PRIVATE filesystem-support-core)
    filesystem_support_configure(filesystem-support-catalog-test)
    add_test(NAME filesystem-support-catalog COMMAND filesystem-support-catalog-test)
endif()

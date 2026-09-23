# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ExtFS VERSION 0.9.3 LANGUAGES C)

add_library(extfs STATIC
    core/extfs.c
    core/extfs_classic_resize.c
    core/extfs_resize_dispatch.c
)
set_source_files_properties(core/extfs.c PROPERTIES
    COMPILE_DEFINITIONS "extfs_resize_file_ext2_direct=extfs_resize_file_ext2_legacy_direct;extfs_resize_file_ext3_journaled_direct=extfs_resize_file_ext3_journaled_legacy_direct"
)
set_source_files_properties(core/extfs_classic_resize.c PROPERTIES
    COMPILE_DEFINITIONS "extfs_resize_file_ext2_direct=extfs_resize_file_ext2_single_indirect_impl;extfs_resize_file_ext3_journaled_direct=extfs_resize_file_ext3_single_indirect_impl"
)
target_include_directories(extfs PUBLIC include)
target_compile_features(extfs PUBLIC c_std_11)

add_executable(extfs-tool tools/extfs-tool.c)
target_link_libraries(extfs-tool PRIVATE extfs)

# The destructive image mutator is a qualification-only POSIX harness. Keep it
# out of ordinary Windows CMake builds; the native Windows driver has its own
# WDK project and qualification path.
if(UNIX)
    add_executable(extfs-mutate-test tests/mutate_image.c)
    target_link_libraries(extfs-mutate-test PRIVATE extfs)
endif()

enable_testing()
add_executable(test-extfs tests/test_extfs.c)
target_link_libraries(test-extfs PRIVATE extfs)
add_test(NAME extfs-unit COMMAND test-extfs)

add_executable(test-single-indirect tests/test_single_indirect.c)
target_link_libraries(test-single-indirect PRIVATE extfs)
add_test(NAME extfs-single-indirect COMMAND test-single-indirect)

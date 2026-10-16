# SPDX-License-Identifier: GPL-3.0-or-later
# Linux desktop media identification for project-native Amiga filesystems.
# Kernel deployment and desktop presentation deliberately have separate policy
# manifests. Desktop policy may reference a native filesystem, but native
# deployment never depends on desktop membership or presentation metadata.

if(NOT TARGET fsinspect)
    message(FATAL_ERROR "Amiga desktop detection requires the fsinspect target")
endif()

set(FILESYSTEMS_DESKTOP_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/data/desktop-filesystems.tsv")
file(STRINGS "${FILESYSTEMS_DESKTOP_MANIFEST}"
    FILESYSTEMS_DESKTOP_MANIFEST_LINES)
set(FILESYSTEMS_AMIGA_DESKTOP_FILESYSTEMS)
foreach(desktop_line IN LISTS FILESYSTEMS_DESKTOP_MANIFEST_LINES)
    string(STRIP "${desktop_line}" desktop_line)
    if(desktop_line STREQUAL "" OR desktop_line MATCHES "^#")
        continue()
    endif()

    string(REPLACE "\t" ";" desktop_fields "${desktop_line}")
    list(LENGTH desktop_fields desktop_field_count)
    if(NOT desktop_field_count EQUAL 2)
        message(FATAL_ERROR
            "Invalid desktop filesystem manifest row: ${desktop_line}")
    endif()

    list(GET desktop_fields 0 desktop_fs)
    list(GET desktop_fields 1 desktop_variant)
    if(NOT desktop_fs MATCHES "^[A-Za-z0-9_.+-]+$" OR
       NOT desktop_variant MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "Invalid desktop filesystem policy: ${desktop_line}")
    endif()

    list(FIND FILESYSTEMS_AMIGA_DESKTOP_FILESYSTEMS
        "${desktop_fs}" desktop_duplicate_index)
    if(NOT desktop_duplicate_index EQUAL -1)
        message(FATAL_ERROR
            "Duplicate desktop filesystem in manifest: ${desktop_fs}")
    endif()

    # Desktop support is an optional consumer of a native filesystem. This is
    # the only permitted direction of dependency between these policy sets.
    list(FIND FILESYSTEMS_NATIVE_FILESYSTEMS
        "${desktop_fs}" native_index)
    if(native_index EQUAL -1)
        message(FATAL_ERROR
            "Desktop filesystem ${desktop_fs} is not a managed native filesystem")
    endif()

    list(APPEND FILESYSTEMS_AMIGA_DESKTOP_FILESYSTEMS "${desktop_fs}")
    set("FILESYSTEMS_DESKTOP_VARIANT_${desktop_fs}"
        "${desktop_variant}")
endforeach()
if(NOT FILESYSTEMS_AMIGA_DESKTOP_FILESYSTEMS)
    message(FATAL_ERROR "Desktop filesystem manifest is empty")
endif()

target_link_libraries(fsinspect
    PRIVATE
        infiltratr-fs-ofs-core
        infiltratr-fs-ffs-core
        infiltratr-fs-sfs-core
        infiltratr-fs-sfs2-core
        infiltratr-fs-pfs3-core)

install(TARGETS fsinspect
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
find_package(Threads REQUIRED)
set(desktop_templates "lib/filesystems/desktop/templates")
install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/desktop-integration.sh"
    DESTINATION "lib/filesystems" RENAME desktop-integration)
install(FILES "${FILESYSTEMS_DESKTOP_MANIFEST}"
    DESTINATION "lib/filesystems"
    RENAME desktop-filesystems.tsv)
install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/gnome-disks-wrapper"
    DESTINATION "${desktop_templates}" RENAME gnome-disks)
install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/nemo-wrapper"
    DESTINATION "${desktop_templates}" RENAME nemo)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/desktop-rules/"
    DESTINATION "${desktop_templates}" FILES_MATCHING PATTERN "*.rules")
foreach(desktop_fs IN LISTS FILESYSTEMS_AMIGA_DESKTOP_FILESYSTEMS)
    set(desktop_variant_variable "FILESYSTEMS_DESKTOP_VARIANT_${desktop_fs}")
    set(desktop_variant "${${desktop_variant_variable}}")
    if(NOT desktop_variant MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "Missing desktop variant for ${desktop_fs}")
    endif()
    set(desktop_target "filesystems-disks-${desktop_fs}")
    add_library(${desktop_target} SHARED
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/udisks-amiga-names.c")
    target_link_libraries(${desktop_target} PRIVATE PkgConfig::GLIB2 PkgConfig::GIO2 ${CMAKE_DL_LIBS} Threads::Threads)
    target_compile_definitions(${desktop_target} PRIVATE IFS_NAMES_VARIANT=${desktop_variant})
    set_target_properties(${desktop_target} PROPERTIES PREFIX "" OUTPUT_NAME "${desktop_fs}"
        C_VISIBILITY_PRESET hidden LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/desktop-templates")
    filesystems_configure(${desktop_target})
    install(TARGETS ${desktop_target} LIBRARY DESTINATION "${desktop_templates}")
    if(BUILD_TESTING)
        add_library(${desktop_target}-test SHARED
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/udisks-amiga-names.c")
        target_link_libraries(${desktop_target}-test PRIVATE PkgConfig::GLIB2 PkgConfig::GIO2 ${CMAKE_DL_LIBS} Threads::Threads)
        target_compile_definitions(${desktop_target}-test PRIVATE
            IFS_NAMES_VARIANT=${desktop_variant}
            IFS_DESKTOP_TESTING=1)
        set_target_properties(${desktop_target}-test PROPERTIES PREFIX "" OUTPUT_NAME "${desktop_fs}"
            C_VISIBILITY_PRESET hidden LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/desktop-test-templates")
        filesystems_configure(${desktop_target}-test)
    endif()
endforeach()

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # Exercise the real detector without creating fake modules under the
    # host's /lib/modules. Production binaries have no runtime root override.
    add_executable(fsinspect-desktop-test
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/fsinspect/main.c")
    get_target_property(amiga_detector_libraries fsinspect LINK_LIBRARIES)
    target_link_libraries(fsinspect-desktop-test PRIVATE ${amiga_detector_libraries})
    target_compile_definitions(fsinspect-desktop-test PRIVATE
        IFS_NATIVE_MODULES_ROOT="${CMAKE_CURRENT_BINARY_DIR}/desktop-test-modules")
    filesystems_configure(fsinspect-desktop-test)
    add_test(
        NAME filesystems-amiga-detection
        COMMAND sh
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/amiga_detection_test.sh"
            "$<TARGET_FILE:fsinspect-desktop-test>"
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/desktop-test-modules")
    add_test(NAME filesystems-desktop-install
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/desktop_install_test.py"
            "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME filesystems-native-helper-boundary
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_helper_boundary_test.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME filesystems-forensic-ownership-boundary
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ownership_boundary_test.py"
            "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME filesystems-desktop-manifest-boundary
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/desktop_manifest_boundary_test.py"
            "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# The detector is invoked by udev at runtime. Reload and retrigger block-device
# events on package install/removal so already-connected Amiga media is updated
# immediately instead of requiring a reboot or physical replug.
string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS
    ", udev, util-linux, libudisks2-0, gnome-disk-utility")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/prerm"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postinst"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postrm")
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)

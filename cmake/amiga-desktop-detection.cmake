# SPDX-License-Identifier: GPL-3.0-or-later
# Linux desktop media identification for the project-native Amiga filesystems.
# Keep this integration separate from the filesystem implementations: the
# kernel modules own mount semantics, while fsinspect publishes ID_FS_* to the
# userspace storage stack before libblkid/udisks evaluate the device.

if(NOT TARGET fsinspect)
    message(FATAL_ERROR "Amiga desktop detection requires the fsinspect target")
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
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/59-infiltrator-filesystems.rules"
    DESTINATION "lib/udev/rules.d")

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME filesystem-support-amiga-detection
        COMMAND sh
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/amiga_detection_test.sh"
            "$<TARGET_FILE:fsinspect>"
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# The detector is invoked by udev at runtime. Reload and retrigger block-device
# events on package install/removal so already-connected Amiga media is updated
# immediately instead of requiring a reboot or physical replug.
string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS
    ", udev, infiltrator-filesystem-support-udisks (>= ${PROJECT_VERSION})")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postinst"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postrm")
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)

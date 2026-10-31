cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" MAGIC_DOCK_VERSION_TEXT)
string(STRIP "${MAGIC_DOCK_VERSION_TEXT}" MAGIC_DOCK_VERSION_TEXT)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" MAGIC_DOCK_VERSION_TRIPLE "${MAGIC_DOCK_VERSION_TEXT}")
if(NOT MAGIC_DOCK_VERSION_TRIPLE)
    message(FATAL_ERROR "version.txt must start with major.minor.patch")
endif()

string(TOLOWER "${MAGIC_DOCK_VERSION_TEXT}" MAGIC_DOCK_VERSION_SLUG)
string(REGEX REPLACE "[ \t_]+" "-" MAGIC_DOCK_VERSION_SLUG "${MAGIC_DOCK_VERSION_SLUG}")
string(REGEX REPLACE "[^a-z0-9.-]" "" MAGIC_DOCK_VERSION_SLUG "${MAGIC_DOCK_VERSION_SLUG}")

project(magic-dock VERSION ${MAGIC_DOCK_VERSION_TRIPLE} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

find_package(Qt6 6.9 REQUIRED COMPONENTS Core Gui Widgets)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-cursor)

set(MAGIC_DOCK_PROTOS
    wlr-layer-shell-unstable-v1
    ext-foreign-toplevel-list-v1
    cosmic-toplevel-info-unstable-v1
    cosmic-toplevel-management-unstable-v1
    fractional-scale-v1
    viewporter
    xdg-shell
    ext-workspace-v1
    cosmic-workspace-unstable-v1
    ext-image-capture-source-v1
    ext-image-copy-capture-v1)

set(MAGIC_DOCK_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${MAGIC_DOCK_GEN_DIR}")
set(MAGIC_DOCK_PROTO_SOURCES)
foreach(proto ${MAGIC_DOCK_PROTOS})
    set(xml "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${proto}.xml")
    set(header "${MAGIC_DOCK_GEN_DIR}/${proto}-client.h")
    set(code "${MAGIC_DOCK_GEN_DIR}/${proto}-protocol.c")
    add_custom_command(
        OUTPUT "${header}" "${code}"
        COMMAND "${WAYLAND_SCANNER}" client-header "${xml}" "${header}"
        COMMAND "${WAYLAND_SCANNER}" private-code "${xml}" "${code}"
        # wayland-scanner names a field "namespace", which is a C++ keyword.
        COMMAND sed -i "s/\\bnamespace\\b/ns_name/g" "${header}"
        DEPENDS "${xml}"
        VERBATIM)
    list(APPEND MAGIC_DOCK_PROTO_SOURCES "${code}")
    set_source_files_properties("${code}" PROPERTIES LANGUAGE C)
endforeach()

add_executable(magic-dock
    src/main.cpp
    src/dock.cpp
    src/apps.cpp
    src/backend.cpp
    src/settings.cpp
    src/marks.cpp
    ${MAGIC_DOCK_PROTO_SOURCES})
target_include_directories(magic-dock PRIVATE src "${MAGIC_DOCK_GEN_DIR}")
target_compile_definitions(magic-dock PRIVATE
    QT_NO_KEYWORDS
    "MAGIC_DOCK_VERSION=\"${MAGIC_DOCK_VERSION_TEXT}\"")
target_compile_options(magic-dock PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -fvisibility=hidden
    $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>)
target_link_libraries(magic-dock PRIVATE Qt6::Widgets Qt6::Gui Qt6::Core ${WAYLAND_LIBRARIES} pthread)
target_include_directories(magic-dock PRIVATE ${WAYLAND_INCLUDE_DIRS})
target_link_directories(magic-dock PRIVATE ${WAYLAND_LIBRARY_DIRS})

if(NOT WIN32)
    set_target_properties(magic-dock PROPERTIES
        BUILD_RPATH "\$ORIGIN/../lib;${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBS}"
        INSTALL_RPATH "\$ORIGIN/../lib")
    target_link_options(magic-dock PRIVATE -Wl,--disable-new-dtags -rdynamic)
endif()

set(CPACK_PACKAGE_NAME "magic-dock")
set(CPACK_PACKAGE_VENDOR "Mira (The White Witch)")
set(CPACK_PACKAGE_VERSION "${MAGIC_DOCK_VERSION_TRIPLE}")
set(CPACK_PACKAGE_FILE_NAME "magic-dock-${MAGIC_DOCK_VERSION_SLUG}-linux-x86_64")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A dock for the COSMIC desktop")
set(CPACK_PACKAGE_DESCRIPTION "MAGIC Dock ${MAGIC_DOCK_VERSION_TEXT}.")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ")
include(CPack)

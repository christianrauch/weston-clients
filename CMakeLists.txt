cmake_minimum_required(VERSION 2.8)
project(weston-clients)

include_directories(.)
include_directories(shared)
include_directories(/usr/include/cairo/)

include(FindPkgConfig)

## generate protocol implementation

# find Wayland protocols
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

# find 'wayland-scanner' executable
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)

# xdg-shell
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml" xdg-shell-client-protocol.h)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml" xdg-shell-protocol.c
    DEPENDS xdg-shell-client-protocol.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
list(APPEND PROT_SOURCES xdg-shell-protocol.c)

# text-input-unstable-v3
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/text-cursor-position-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header "${WAYLAND_PROTOCOLS_DIR}/unstable/text-input/text-input-unstable-v3.xml" text-cursor-position-client-protocol.h)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/text-cursor-position-client-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code "${WAYLAND_PROTOCOLS_DIR}/unstable/text-input/text-input-unstable-v3.xml" text-cursor-position-client-protocol.c
    DEPENDS text-cursor-position-client-protocol.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
list(APPEND PROT_SOURCES text-cursor-position-client-protocol.c)

# pointer-constraints-unstable-v1-client-protocol
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/pointer-constraints-unstable-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header "${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml" pointer-constraints-unstable-v1-client-protocol.h)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/pointer-constraints-unstable-v1-client-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code "${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml" pointer-constraints-unstable-v1-client-protocol.c
    DEPENDS pointer-constraints-unstable-v1-client-protocol.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
list(APPEND PROT_SOURCES pointer-constraints-unstable-v1-client-protocol.c)


##
add_library(ttk window.c ${PROT_SOURCES})


add_executable(image image.c)

target_link_libraries(image -lcairo)
target_link_libraries(image ttk)

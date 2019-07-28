cmake_minimum_required(VERSION 2.8)
project(weston-clients)

include_directories(.)
include_directories(shared)

include(FindPkgConfig)

## generate protocol implementation

# find Wayland protocols
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

# find 'wayland-scanner' executable
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)

include_directories(${CMAKE_CURRENT_BINARY_DIR})

macro(gen_proto DEF NAME)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${NAME}.h
        COMMAND ${WAYLAND_SCANNER} client-header "${DEF}.xml" ${NAME}.h)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${NAME}.c
        COMMAND ${WAYLAND_SCANNER} private-code "${DEF}.xml" ${NAME}.c
        DEPENDS ${NAME}.h)
    list(APPEND PROT_SOURCES ${NAME}.c)
endmacro(gen_proto)

macro(gen_std_proto DEF NAME)
    gen_proto("${WAYLAND_PROTOCOLS_DIR}/${DEF}" ${NAME})
endmacro(gen_std_proto)

gen_std_proto("stable/xdg-shell/xdg-shell" xdg-shell-client-protocol)
gen_std_proto("unstable/pointer-constraints/pointer-constraints-unstable-v1" pointer-constraints-unstable-v1-client-protocol)
gen_std_proto("unstable/relative-pointer/relative-pointer-unstable-v1" relative-pointer-unstable-v1-client-protocol)
gen_std_proto("unstable/text-input/text-input-unstable-v1" text-input-unstable-v1-client-protocol)

gen_proto(${CMAKE_CURRENT_SOURCE_DIR}"/protocol/text-cursor-position" text-cursor-position-client-protocol)

## toolkit library

pkg_check_modules(wayland-client REQUIRED wayland-client)
include_directories(${wayland-client_INCLUDE_DIRS})

pkg_check_modules(wayland-cursor REQUIRED wayland-cursor)
include_directories(${wayland-cursor_INCLUDE_DIRS})

pkg_check_modules(xkbcommon REQUIRED xkbcommon)
include_directories(${xkbcommon_INCLUDE_DIRS})

pkg_check_modules(cairo REQUIRED cairo)
include_directories(${cairo_INCLUDE_DIRS})

pkg_check_modules(pixman REQUIRED pixman-1)
include_directories(${pixman_INCLUDE_DIRS})

pkg_check_modules(pangocairo REQUIRED pangocairo)
include_directories(${pangocairo_INCLUDE_DIRS})

find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)

pkg_check_modules(webp REQUIRED libwebp)
include_directories(${webp_INCLUDE_DIRS})

add_library(ttk window.c ${PROT_SOURCES} shared/xalloc.c shared/os-compatibility.c shared/config-parser.c shared/frame.c shared/file-util.c shared/cairo-util.c shared/image-loader.c)
target_link_libraries(ttk ${wayland-client_LIBRARIES} ${cairo_LIBRARIES} ${pixman_LIBRARIES} -lm ${wayland-cursor_LIBRARIES} ${xkbcommon_LIBRARIES} ${pangocairo_LIBRARIES} PNG::PNG ${JPEG_LIBRARIES} ${webp_LIBRARIES})

## examples

add_executable(image image.c)
target_link_libraries(image ttk)

add_executable(terminal terminal.c shared/option-parser.c)
target_link_libraries(terminal ttk -lutil)

add_executable(editor editor.c shared/option-parser.c)
target_link_libraries(editor ttk)

add_executable(clickdot clickdot.c)
target_link_libraries(clickdot ttk)

find_package(OpenGL COMPONENTS EGL)
pkg_check_modules(wayland-egl REQUIRED wayland-egl)
include_directories(${wayland-egl_INCLUDE_DIRS})

add_executable(simple-egl simple-egl.c)
target_link_libraries(simple-egl ttk OpenGL::EGL ${wayland-egl_LIBRARIES})

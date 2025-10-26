cmake_minimum_required(VERSION 3.16)

project(gstwlrsrc CXX)


include(ExternalProject)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-base-1.0 gstreamer-allocators-1.0)
pkg_check_modules(GLIB REQUIRED glib-2.0 gobject-2.0)
pkg_check_modules(WAYLAND REQUIRED wayland-client gbm)

ExternalProject_Add(wayland_protocols
    GIT_REPOSITORY https://gitlab.freedesktop.org/wayland/wayland-protocols.git
    GIT_TAG main
    GIT_SHALLOW TRUE 
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    PREFIX ${CMAKE_BINARY_DIR}/wayland_protocols
)

# 2. wlr-screencopy-unstable-v1.xml (wlroots)
ExternalProject_Add(wlroots_protocols
    GIT_REPOSITORY https://gitlab.freedesktop.org/wlroots/wlroots.git
    GIT_TAG master
    GIT_SHALLOW TRUE 
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    PREFIX ${CMAKE_BINARY_DIR}/wlroots_protocols
)

add_custom_command(
    OUTPUT linux-dmabuf-unstable-v1-client-protocol.c linux-dmabuf-unstable-v1-client-protocol.h
    COMMAND wayland-scanner public-code ${CMAKE_BINARY_DIR}/wayland_protocols/src/wayland_protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml ${CMAKE_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.c
    COMMAND wayland-scanner client-header  ${CMAKE_BINARY_DIR}/wayland_protocols/src/wayland_protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml ${CMAKE_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.h
    DEPENDS  ${CMAKE_BINARY_DIR}/wayland_protocols/src/wayland_protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml
)

add_custom_command(
    OUTPUT wlr-screencopy-unstable-v1-client-protocol.c wlr-screencopy-unstable-v1-client-protocol.h
    COMMAND wayland-scanner public-code ${CMAKE_BINARY_DIR}/wlroots_protocols/src/wlroots_protocols/protocol/wlr-screencopy-unstable-v1.xml ${CMAKE_BINARY_DIR}/wlr-screencopy-unstable-v1-client-protocol.c
    COMMAND wayland-scanner client-header ${CMAKE_BINARY_DIR}/wlroots_protocols/src/wlroots_protocols/protocol/wlr-screencopy-unstable-v1.xml ${CMAKE_BINARY_DIR}/wlr-screencopy-unstable-v1-client-protocol.h
    DEPENDS ${CMAKE_BINARY_DIR}/wlroots_protocols/src/wlroots_protocols/protocol/wlr-screencopy-unstable-v1.xml
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.o
    COMMAND ${CMAKE_C_COMPILER} -c
            ${CMAKE_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.c
            -o ${CMAKE_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.o
            ${WAYLAND_CFLAGS}
    DEPENDS ${CMAKE_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.c
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/wlr-screencopy-unstable-v1-client-protocol.o
    COMMAND ${CMAKE_C_COMPILER} -c
            ${CMAKE_BINARY_DIR}/wlr-screencopy-unstable-v1-client-protocol.c
            -o ${CMAKE_BINARY_DIR}/wlr-screencopy-unstable-v1-client-protocol.o
            ${WAYLAND_CFLAGS}
    DEPENDS ${CMAKE_BINARY_DIR}/wlr-screencopy-unstable-v1-client-protocol.c
)

add_custom_target(gen_wayland_objs ALL
    DEPENDS
        ${CMAKE_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.o
        ${CMAKE_BINARY_DIR}/wlr-screencopy-unstable-v1-client-protocol.o
)

add_library(gstwlrsrc SHARED
    wlrsrc.cpp
    ${CMAKE_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.o
    ${CMAKE_BINARY_DIR}/wlr-screencopy-unstable-v1-client-protocol.o
)

add_dependencies(gstwlrsrc gen_wayland_objs)

target_include_directories(gstwlrsrc PRIVATE
    ${GST_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
    ${WAYLAND_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}
)

target_link_libraries(gstwlrsrc
    ${GST_LIBRARIES}
    ${GLIB_LIBRARIES}
    ${WAYLAND_LIBRARIES}
)

set_target_properties(gstwlrsrc PROPERTIES PREFIX "lib" SUFFIX ".so")

install(TARGETS gstwlrsrc
    LIBRARY DESTINATION lib/gstreamer-1.0
    ARCHIVE DESTINATION lib/gstreamer-1.0
    RUNTIME DESTINATION bin)
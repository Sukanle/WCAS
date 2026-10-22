cmake_minimum_required(VERSION 3.16)
project(
    wcas
    VERSION 0.1.0
    HOMEPAGE_URL "https://github.com/sukanle/WCAS"
    LANGUAGES CXX)

# Apache-2.0. The full text lives in LICENSE at the repository root.
set(WCAS_LICENSE "Apache-2.0")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/ABIX)

include(GNUInstallDirs)
include(CTest)
include(ExternalProject)
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc CONFIG)
find_package(Boost REQUIRED COMPONENTS filesystem system program_options CONFIG)
find_package(fmt REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-composite)
pkg_check_modules(XTST IMPORTED_TARGET xtst x11)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)
# Optional EGL/GBM backend: the only path that decodes NVIDIA framebuffer- compressed DMA-BUFs (the compositor uses the
# same stack). Absent => the backend compiles to a stub and capture stays on Vulkan.
pkg_check_modules(EGL IMPORTED_TARGET egl)
pkg_check_modules(GLESV2 IMPORTED_TARGET glesv2)
pkg_check_modules(GBM IMPORTED_TARGET gbm)

find_program(MESON_EXECUTABLE meson REQUIRED)
find_program(NINJA_EXECUTABLE ninja REQUIRED)
pkg_check_modules(DRMTAP_LIBDRM REQUIRED IMPORTED_TARGET "libdrm>=2.4.95")
find_package(Threads REQUIRED)

# The proxy translates raw wire messages, so it needs each message's wire opcode/signature. libwayland's and waylandpp's
# `wl_interface` tables are client-side tables (the version-gated `release` request shares an opcode with its
# neighbour), so the metadata is generated from the protocol XML instead.
find_program(PYTHON3_EXECUTABLE NAMES python3 REQUIRED)
find_file(
    WAYLAND_CORE_XML
    NAMES wayland.xml
    PATHS /usr/share/wayland /usr/local/share/wayland /usr/share/wayland-protocols/../../wayland
    DOC "Path to the core wayland.xml protocol definition")
find_path(
    WAYLAND_PROTOCOLS_DIR
    NAMES stable/xdg-shell/xdg-shell.xml
    PATHS /usr/share/wayland-protocols /usr/local/share/wayland-protocols
    DOC "Path to the wayland-protocols XML tree")

if(WAYLAND_CORE_XML AND WAYLAND_PROTOCOLS_DIR)
    set(WCAS_WIRE_PROTOCOLS
        stable/xdg-shell/xdg-shell.xml
        stable/viewporter/viewporter.xml
        stable/presentation-time/presentation-time.xml
        stable/tablet/tablet-v2.xml
        stable/linux-dmabuf/linux-dmabuf-v1.xml
        staging/fractional-scale/fractional-scale-v1.xml
        staging/cursor-shape/cursor-shape-v1.xml
        staging/single-pixel-buffer/single-pixel-buffer-v1.xml
        staging/xdg-activation/xdg-activation-v1.xml
        unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
        unstable/xdg-output/xdg-output-unstable-v1.xml
        unstable/text-input/text-input-unstable-v3.xml
        unstable/idle-inhibit/idle-inhibit-unstable-v1.xml
        unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
        unstable/relative-pointer/relative-pointer-unstable-v1.xml
        unstable/pointer-gestures/pointer-gestures-unstable-v1.xml
        unstable/input-timestamps/input-timestamps-unstable-v1.xml)
    # wayland.xml is core, and its metadata is generated from the server header instead (see below); feeding it here
    # would add a second, incomplete definition of every wl_* interface.
    set(WCAS_WIRE_XMLS "")
    foreach(_protocol IN LISTS WCAS_WIRE_PROTOCOLS)
        list(APPEND WCAS_WIRE_XMLS "${WAYLAND_PROTOCOLS_DIR}/${_protocol}")
    endforeach()

    # Core `wl_*` messages: wayland.xml does not spell out every wire argument (wayland-scanner fills some in), so the
    # scanner-generated server header is the authoritative parameter list.
    find_path(
        WAYLAND_CORE_INCLUDE_DIR
        NAMES wayland-server-protocol.h
        PATHS /usr/include /usr/local/include
        DOC "Directory holding wayland-server-protocol.h")
    set(WCAS_WIRE_META_H "${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas/proxy/wire_meta.h")
    set(WCAS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(WCAS_EXT_WIRE_META_CPP "${WCAS_GENERATED_DIR}/wire_meta.cpp")
    add_custom_command(
        OUTPUT "${WCAS_EXT_WIRE_META_CPP}"
        COMMAND "${PYTHON3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/codegen/generate_wire_meta.py"
                "${WCAS_GENERATED_DIR}" ${WCAS_WIRE_XMLS}
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/codegen/generate_wire_meta.py" "${WCAS_WIRE_META_H}" ${WCAS_WIRE_XMLS}
        COMMENT "Generating Wayland extension wire metadata"
        VERBATIM)
    set(WCAS_WIRE_META_SOURCES "${WCAS_EXT_WIRE_META_CPP}")

    if(WAYLAND_CORE_INCLUDE_DIR)
        set(WCAS_CORE_WIRE_META_CPP "${WCAS_GENERATED_DIR}/core_wire_meta.cpp")
        add_custom_command(
            OUTPUT "${WCAS_CORE_WIRE_META_CPP}"
            COMMAND
                "${PYTHON3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/codegen/generate_core_wire_meta.py"
                "${WAYLAND_CORE_XML}" "${WAYLAND_CORE_INCLUDE_DIR}/wayland-server-protocol.h"
                "${WCAS_CORE_WIRE_META_CPP}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/codegen/generate_core_wire_meta.py" "${WCAS_WIRE_META_H}"
                    "${WAYLAND_CORE_XML}" "${WAYLAND_CORE_INCLUDE_DIR}/wayland-server-protocol.h"
            COMMENT "Generating Wayland core wire metadata"
            VERBATIM)
        list(APPEND WCAS_WIRE_META_SOURCES "${WCAS_CORE_WIRE_META_CPP}")
    else()
        message(FATAL_ERROR "wayland-server-protocol.h not found; core wire metadata cannot be generated")
    endif()
else()
    message(FATAL_ERROR "Wayland protocol XML not found; install the wayland and wayland-protocols "
                        "development files to generate the proxy's wire metadata")
endif()

set(DRMTAP_HELPER
    "AUTO"
    CACHE STRING "Build the confined libdrmtap helper: AUTO, ON, or OFF")
set_property(CACHE DRMTAP_HELPER PROPERTY STRINGS AUTO ON OFF)
set(DRMTAP_EGL
    "AUTO"
    CACHE STRING "Build the libdrmtap EGL backend: AUTO, ON, or OFF")
set_property(CACHE DRMTAP_EGL PROPERTY STRINGS AUTO ON OFF)

function(wcas_meson_feature out_var value)
    string(TOUPPER "${value}" _value)
    if(_value STREQUAL "AUTO")
        set(${out_var}
            auto
            PARENT_SCOPE)
    elseif(_value STREQUAL "ON")
        set(${out_var}
            enabled
            PARENT_SCOPE)
    elseif(_value STREQUAL "OFF")
        set(${out_var}
            disabled
            PARENT_SCOPE)
    else()
        message(FATAL_ERROR "${out_var} must be AUTO, ON, or OFF (got '${value}')")
    endif()
endfunction()

wcas_meson_feature(DRMTAP_HELPER_FEATURE "${DRMTAP_HELPER}")
wcas_meson_feature(DRMTAP_EGL_FEATURE "${DRMTAP_EGL}")

set(DRMTAP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdrmtap")
set(DRMTAP_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/libdrmtap-build")
set(DRMTAP_VERSION "0.5.5")
set(DRMTAP_LIBRARY "${DRMTAP_BUILD_DIR}/libdrmtap.so.${DRMTAP_VERSION}")

# libdrmtap is Meson-owned. Keep its source tree untouched and use an imported target so WCAS consumers can link it like
# any other CMake library.
ExternalProject_Add(
    wcas-libdrmtap
    SOURCE_DIR "${DRMTAP_SOURCE_DIR}"
    BINARY_DIR "${DRMTAP_BUILD_DIR}"
    CONFIGURE_COMMAND
        "${MESON_EXECUTABLE}" setup "<BINARY_DIR>" "<SOURCE_DIR>" --backend=ninja
        --prefix=${CMAKE_CURRENT_BINARY_DIR}/libdrmtap-prefix -Dhelper=${DRMTAP_HELPER_FEATURE}
        -Degl=${DRMTAP_EGL_FEATURE}
    BUILD_COMMAND "${MESON_EXECUTABLE}" compile -C "<BINARY_DIR>"
    INSTALL_COMMAND "${MESON_EXECUTABLE}" install -C "<BINARY_DIR>"
    BUILD_BYPRODUCTS "${DRMTAP_LIBRARY}"
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
    USES_TERMINAL_INSTALL TRUE)

add_library(libdrmtap::drmtap SHARED IMPORTED GLOBAL)
set_target_properties(libdrmtap::drmtap PROPERTIES IMPORTED_LOCATION "${DRMTAP_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES
                                                                                         "${DRMTAP_SOURCE_DIR}/include")
add_dependencies(libdrmtap::drmtap wcas-libdrmtap)

add_library(
    wcas SHARED
    src/skl/wcas/error.cpp
    src/skl/wcas/drm.cpp
    src/skl/wcas/x11_capture.cpp
    src/skl/wcas/dmabuf_probe.cpp
    src/skl/wcas/xwayland.cpp
    src/skl/wcas/proxy/conn.cpp
    src/skl/wcas/proxy/input.cpp
    src/skl/wcas/proxy/interfaces.cpp
    src/skl/wcas/proxy/keymap.cpp
    src/skl/wcas/proxy/keysym.cpp
    src/skl/wcas/proxy/reactor.cpp
    src/skl/wcas/proxy/capture_slot.cpp
    src/skl/wcas/proxy/capture_scheduler.cpp
    src/skl/wcas/proxy/vulkan_dmabuf.cpp
    src/skl/wcas/proxy/egl_dmabuf.cpp
    ${WCAS_WIRE_META_SOURCES}
    src/skl/wcas/harness/session.cpp
    src/skl/wcas/harness/computer_use.cpp
    src/skl/wcas/harness/controller.cpp
    src/skl/wcas/harness/process_host.cpp
    src/skl/wcas/harness/focus_backend.cpp
    src/skl/wcas/harness/launch_strategy.cpp
    src/skl/wcas/harness/x11_input.cpp
    src/skl/wcas/harness/health_monitor.cpp
    src/skl/wcas/harness/target_selector.cpp
    src/skl/wcas/harness/capture_source.cpp
    src/skl/wcas/harness/window_source.cpp
    src/skl/wcas/harness/frame_bus.cpp
    src/skl/wcas/harness/controlled_app.cpp)
add_library(wcas::wcas ALIAS wcas)
target_compile_definitions(wcas PRIVATE WCAS_BUILDING_LIBRARY=1)
target_include_directories(wcas PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                       $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/ABIX>
                                       $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(
    wcas PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas/proxy
                 ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(
    wcas
    PUBLIC ${OpenCV_LIBS} fmt::fmt
    PRIVATE Boost::filesystem
            Boost::system
            PkgConfig::WAYLAND
            PkgConfig::XKBCOMMON
            PkgConfig::XCB
            PkgConfig::VULKAN
            PkgConfig::LIBURING)
target_link_libraries(wcas PRIVATE libdrmtap::drmtap)
target_compile_definitions(wcas PRIVATE WCAS_HAVE_DRMTAP=1)
if(XTST_FOUND)
    target_compile_definitions(wcas PRIVATE WCAS_HAVE_XTEST=1)
    target_link_libraries(wcas PRIVATE PkgConfig::XTST)
endif()
if(EGL_FOUND
   AND GLESV2_FOUND
   AND GBM_FOUND)
    target_compile_definitions(wcas PRIVATE WCAS_HAVE_EGL=1)
    target_link_libraries(wcas PRIVATE PkgConfig::EGL PkgConfig::GLESV2 PkgConfig::GBM)
endif()
set_property(
    TARGET wcas
    APPEND
    PROPERTY BUILD_RPATH "${DRMTAP_BUILD_DIR}")

# cmdly is the line editor behind the interactive console of the test harness.
# Only its library is added: the submodule's top-level CMakeLists rewrites the
# build-tree compile_commands.json, and its tests need googletest, so BUILD_TESTING
# is shadowed to OFF for the duration of the add_subdirectory.
set(_wcas_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
add_subdirectory(third_party/cmdly/libcmdly cmdly EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${_wcas_build_testing}")
unset(_wcas_build_testing)

add_executable(wcas-example main.cpp)
target_link_libraries(wcas-example PRIVATE wcas::wcas)
add_executable(proton-inject proton-inject.cpp)
target_link_libraries(proton-inject PRIVATE wcas::wcas Boost::program_options cmdly_static)
if(BUILD_TESTING)
    add_executable(wcas-unit-tests tests/unit.cpp)
    target_link_libraries(wcas-unit-tests PRIVATE wcas::wcas)
    target_include_directories(wcas-unit-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas)
    target_compile_options(wcas-unit-tests PRIVATE -UNDEBUG)
    add_test(NAME wcas-unit-tests COMMAND wcas-unit-tests)
    add_executable(wcas-proxy-integration tests/proxy_integration.cpp)
    target_link_libraries(wcas-proxy-integration PRIVATE wcas::wcas)
    target_include_directories(wcas-proxy-integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas)
    target_compile_options(wcas-proxy-integration PRIVATE -UNDEBUG)
    add_test(NAME wcas-proxy-integration COMMAND wcas-proxy-integration)
    add_executable(wcas-input-injection tests/input_injection.cpp)
    target_link_libraries(wcas-input-injection PRIVATE wcas::wcas PkgConfig::XKBCOMMON)
    target_include_directories(wcas-input-injection PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas)
    target_compile_options(wcas-input-injection PRIVATE -UNDEBUG)
    add_test(NAME wcas-input-injection COMMAND wcas-input-injection)
    add_executable(wcas-dmabuf-capture tests/dmabuf_capture.cpp)
    target_link_libraries(wcas-dmabuf-capture PRIVATE wcas::wcas)
    target_include_directories(wcas-dmabuf-capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas)
    target_compile_options(wcas-dmabuf-capture PRIVATE -UNDEBUG)
    add_test(NAME wcas-dmabuf-capture COMMAND wcas-dmabuf-capture)
    add_executable(wcas-capture-scheduler tests/capture_scheduler.cpp)
    target_link_libraries(wcas-capture-scheduler PRIVATE wcas::wcas)
    target_include_directories(wcas-capture-scheduler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas)
    target_compile_options(wcas-capture-scheduler PRIVATE -UNDEBUG)
    add_test(NAME wcas-capture-scheduler COMMAND wcas-capture-scheduler)
    add_executable(wcas-capture-pipeline tests/capture_pipeline.cpp)
    target_link_libraries(wcas-capture-pipeline PRIVATE wcas::wcas)
    target_include_directories(wcas-capture-pipeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas)
    target_compile_options(wcas-capture-pipeline PRIVATE -UNDEBUG)
    add_test(NAME wcas-capture-pipeline COMMAND wcas-capture-pipeline)
    add_executable(wcas-window-geometry tests/window_geometry.cpp)
    target_link_libraries(wcas-window-geometry PRIVATE wcas::wcas)
    target_include_directories(wcas-window-geometry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas)
    target_compile_options(wcas-window-geometry PRIVATE -UNDEBUG)
    add_test(NAME wcas-window-geometry COMMAND wcas-window-geometry)
    add_executable(wcas-kms-capture tests/kms_capture.cpp)
    target_link_libraries(wcas-kms-capture PRIVATE wcas::wcas)
    target_include_directories(wcas-kms-capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas)
    target_compile_options(wcas-kms-capture PRIVATE -UNDEBUG)
    add_test(NAME wcas-kms-capture COMMAND wcas-kms-capture)
    add_executable(wcas-dmabuf-probe tests/dmabuf_probe.cpp)
    target_link_libraries(wcas-dmabuf-probe PRIVATE wcas::wcas)
    target_include_directories(wcas-dmabuf-probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas)
    target_compile_options(wcas-dmabuf-probe PRIVATE -UNDEBUG)
    add_test(NAME wcas-dmabuf-probe COMMAND wcas-dmabuf-probe)
    add_test(NAME wcas-cli-smoke COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.sh
                                         $<TARGET_FILE:proton-inject>)
    add_executable(wcas-x11-capture tests/x11_capture.cpp)
    target_link_libraries(wcas-x11-capture PRIVATE wcas::wcas)
    target_include_directories(wcas-x11-capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas)
    target_compile_options(wcas-x11-capture PRIVATE -UNDEBUG)
    add_test(NAME wcas-x11-capture COMMAND wcas-x11-capture)
    add_executable(wcas-xwayland-bridge tests/xwayland_bridge.cpp)
    target_link_libraries(wcas-xwayland-bridge PRIVATE wcas::wcas)
    target_include_directories(wcas-xwayland-bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas)
    target_compile_options(wcas-xwayland-bridge PRIVATE -UNDEBUG)
    add_test(NAME wcas-xwayland-bridge COMMAND wcas-xwayland-bridge)
    add_executable(wcas-xwayland-fallback tests/xwayland_fallback.cpp)
    target_link_libraries(wcas-xwayland-fallback PRIVATE wcas::wcas PkgConfig::WAYLAND)
    target_include_directories(wcas-xwayland-fallback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skl/wcas)
    target_compile_options(wcas-xwayland-fallback PRIVATE -UNDEBUG)
    add_test(NAME wcas-xwayland-fallback COMMAND wcas-xwayland-fallback)
    if(GBM_FOUND)
        target_compile_definitions(wcas-dmabuf-capture PRIVATE WCAS_HAVE_GBM=1)
        target_link_libraries(wcas-dmabuf-capture PRIVATE PkgConfig::GBM)
    endif()
endif()
install(TARGETS wcas LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/skl DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# controller.h exposes ABIX interface declarations, so consumers need ABIX's
# header-only Interface/model/mics tree as well.
install(DIRECTORY third_party/ABIX/ABIX third_party/ABIX/mics DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
    DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/libdrmtap-prefix/lib/"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    FILES_MATCHING
    PATTERN "libdrmtap.so*")
install(FILES "${DRMTAP_SOURCE_DIR}/include/drmtap.h" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

file(CREATE_LINK ${CMAKE_BINARY_DIR}/compile_commands.json ${CMAKE_BINARY_DIR}/../compile_commands.json SYMBOLIC)

cmake_minimum_required(VERSION 3.16)
project(
    WCAS
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

include(GNUInstallDirs)
include(CTest)
include(ExternalProject)
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc CONFIG)
find_package(Boost REQUIRED COMPONENTS filesystem system program_options CONFIG)
find_package(fmt REQUIRED)
# ABIX is vendored so the runtime headers track the pinned submodule. 0.2.3's runtime-only mode builds just the
# header-only ABIX::runtime target (plus MICS); it skips AMC, DLLs, tests and install rules, so no LLVM/Clang or
# separate ABIX installation is needed. Packagers can opt into an installed package with -DWCAS_USE_SYSTEM_ABIX=ON.
option(WCAS_USE_SYSTEM_ABIX "Use an installed ABIX CMake package instead of third_party/ABIX" OFF)
if(WCAS_USE_SYSTEM_ABIX)
    find_package(ABIX 0.2.3 CONFIG REQUIRED COMPONENTS runtime)
else()
    set(ABIX_BUILD_RUNTIME_ONLY ON)
    set(ABIX_INSTALL OFF)
    set(ABIX_INSTALL_BUNDLED_MICS OFF)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/ABIX ${CMAKE_CURRENT_BINARY_DIR}/third_party/ABIX
                     EXCLUDE_FROM_ALL)
endif()
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
    set(WCAS_WIRE_META_H "${CMAKE_CURRENT_SOURCE_DIR}/include/SKL/WCAS/Private/Proxy/WireMeta.h")
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
    WCAS SHARED
    src/SKL/WCAS/Error.cpp
    src/SKL/WCAS/DRM.cpp
    src/SKL/WCAS/DMABUF.cpp
    src/SKL/WCAS/Private/X11Capture.cpp
    src/SKL/WCAS/Private/XWayland.cpp
    src/SKL/WCAS/Private/Proxy/Conn.cpp
    src/SKL/WCAS/Private/Proxy/Input.cpp
    src/SKL/WCAS/Private/Proxy/Interfaces.cpp
    src/SKL/WCAS/Private/Proxy/Keymap.cpp
    src/SKL/WCAS/Private/Proxy/Keysym.cpp
    src/SKL/WCAS/Private/Proxy/Reactor.cpp
    src/SKL/WCAS/Private/Proxy/CaptureSlot.cpp
    src/SKL/WCAS/Private/Proxy/CaptureScheduler.cpp
    src/SKL/WCAS/Private/Proxy/DMABUF/Vulkan.cpp
    src/SKL/WCAS/Private/Proxy/DMABUF/EGL.cpp
    ${WCAS_WIRE_META_SOURCES}
    src/SKL/WCAS/Private/Harness/Session.cpp
    src/SKL/WCAS/Private/Harness/ComputerUse.cpp
    src/SKL/WCAS/Private/Harness/Controller.cpp
    src/SKL/WCAS/Private/Harness/ProcessHost.cpp
    src/SKL/WCAS/Private/Harness/FocusBackend.cpp
    src/SKL/WCAS/Private/Harness/LaunchStrategy.cpp
    src/SKL/WCAS/Private/Harness/X11Input.cpp
    src/SKL/WCAS/Private/Harness/HealthMonitor.cpp
    src/SKL/WCAS/Private/Harness/TargetSelector.cpp
    src/SKL/WCAS/Private/Harness/CaptureSource.cpp
    src/SKL/WCAS/Private/Harness/WindowSource.cpp
    src/SKL/WCAS/Private/Harness/FrameBus.cpp
    src/SKL/WCAS/Private/Harness/ControlledApp.cpp)
add_library(WCAS::WCAS ALIAS WCAS)
target_compile_definitions(WCAS PRIVATE WCAS_BUILDING_LIBRARY=1)
target_include_directories(WCAS PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                       $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(WCAS PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(
    WCAS
    PUBLIC ${OpenCV_LIBS} fmt::fmt ABIX::runtime
    PRIVATE Boost::filesystem
            Boost::system
            PkgConfig::WAYLAND
            PkgConfig::XKBCOMMON
            PkgConfig::XCB
            PkgConfig::VULKAN
            PkgConfig::LIBURING)
target_link_libraries(WCAS PRIVATE libdrmtap::drmtap)
target_compile_definitions(WCAS PRIVATE WCAS_HAVE_DRMTAP=1)
if(XTST_FOUND)
    target_compile_definitions(WCAS PRIVATE WCAS_HAVE_XTEST=1)
    target_link_libraries(WCAS PRIVATE PkgConfig::XTST)
endif()
if(EGL_FOUND
   AND GLESV2_FOUND
   AND GBM_FOUND)
    target_compile_definitions(WCAS PRIVATE WCAS_HAVE_EGL=1)
    target_link_libraries(WCAS PRIVATE PkgConfig::EGL PkgConfig::GLESV2 PkgConfig::GBM)
endif()
set_property(
    TARGET WCAS
    APPEND
    PROPERTY BUILD_RPATH "${DRMTAP_BUILD_DIR}")

# cmdly is the line editor behind the interactive console of the test harness. Only its library is added: the
# submodule's top-level CMakeLists rewrites the build-tree compile_commands.json, and its tests need googletest, so
# BUILD_TESTING is shadowed to OFF for the duration of the add_subdirectory.
set(_wcas_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
add_subdirectory(third_party/cmdly/libcmdly cmdly EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${_wcas_build_testing}")
unset(_wcas_build_testing)

add_executable(wcas-example main.cpp)
target_link_libraries(wcas-example PRIVATE WCAS::WCAS)
add_executable(proton-inject proton-inject.cpp)
target_link_libraries(proton-inject PRIVATE WCAS::WCAS Boost::program_options cmdly_static)

if(BUILD_TESTING)
    # 辅助函数：注册一个测试可执行文件并加入 CTest。 用法：wcas_add_test(<名称> <源文件> [LIBS <额外链接库>...]) <名称>     → target 名 = wcas-<名称>，测试名同样
    # <源文件>   → 该测试的主源文件路径 LIBS ...   → 除 WCAS::WCAS 之外需要额外链接的库
    function(wcas_add_test name source)
        cmake_parse_arguments(TEST "" "" "LIBS" ${ARGN})

        set(target "wcas-${name}")
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE WCAS::WCAS ${TEST_LIBS})
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_options(${target} PRIVATE -UNDEBUG)
        add_test(NAME ${target} COMMAND ${target})
    endfunction()

    # ---- 常规单元/接口测试 ----
    wcas_add_test(unit-tests tests/unit.cpp)
    wcas_add_test(controller-interface tests/controller_interface.cpp)
    wcas_add_test(capture-interface tests/capture_interface.cpp)
    wcas_add_test(window-interface tests/window_interface.cpp)
    wcas_add_test(frame-bus tests/frame_bus.cpp)
    wcas_add_test(frame-pump tests/frame_pump.cpp)
    wcas_add_test(dmabuf-decoder-interface tests/dmabuf_decoder.cpp)
    wcas_add_test(process-host tests/process_host.cpp)
    wcas_add_test(focus-backend tests/focus_backend.cpp)
    wcas_add_test(health-monitor tests/health_monitor.cpp)
    wcas_add_test(target-selector tests/target_selector.cpp)
    wcas_add_test(transport-interface tests/transport_interface.cpp)
    wcas_add_test(x11-input tests/x11_input.cpp)
    wcas_add_test(launch-strategy tests/launch_strategy.cpp)
    wcas_add_test(capture-runtime tests/capture_runtime.cpp)
    wcas_add_test(runtime-bindings tests/runtime_bindings.cpp)
    wcas_add_test(proxy-integration tests/proxy_integration.cpp)
    wcas_add_test(dmabuf-capture tests/dmabuf_capture.cpp)
    wcas_add_test(capture-scheduler tests/capture_scheduler.cpp)
    wcas_add_test(capture-pipeline tests/capture_pipeline.cpp)
    wcas_add_test(window-geometry tests/window_geometry.cpp)
    wcas_add_test(kms-capture tests/kms_capture.cpp)
    wcas_add_test(dmabuf-probe tests/dmabuf_probe.cpp)
    wcas_add_test(x11-capture tests/x11_capture.cpp)
    wcas_add_test(xwayland-bridge tests/xwayland_bridge.cpp)

    # ---- 额外链接库的测试 ----
    wcas_add_test(input-injection tests/input_injection.cpp LIBS PkgConfig::XKBCOMMON)
    wcas_add_test(xwayland-fallback tests/xwayland_fallback.cpp LIBS PkgConfig::WAYLAND)

    # ---- 脚本型冒烟测试（保留原样）----
    add_test(NAME wcas-cli-smoke COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.sh $<TARGET_FILE:proton-inject>)

    # ---- 条件编译/链接（针对已存在的 target 二次追加）----
    if(GBM_FOUND)
        target_compile_definitions(wcas-dmabuf-capture PRIVATE WCAS_HAVE_GBM=1)
        target_link_libraries(wcas-dmabuf-capture PRIVATE PkgConfig::GBM)
    endif()
endif()
install(TARGETS WCAS LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/SKL DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# WCAS public headers include SKL/ABIX/...; ABIX itself is not installed into this prefix (the vendored submodule is
# built runtime-only for WCAS). Consumers must provide ABIX themselves, e.g. find_package(ABIX 0.2.3 CONFIG REQUIRED).
install(
    DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/libdrmtap-prefix/lib/"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    FILES_MATCHING
    PATTERN "libdrmtap.so*")
install(FILES "${DRMTAP_SOURCE_DIR}/include/drmtap.h" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

file(CREATE_LINK ${CMAKE_BINARY_DIR}/compile_commands.json ${CMAKE_BINARY_DIR}/../compile_commands.json SYMBOLIC)

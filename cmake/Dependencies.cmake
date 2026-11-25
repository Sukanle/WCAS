# Dependency bootstrap for WCAS.
#
# Every dependency is first looked up with find_package(). When no package is
# installed, FetchContent clones the repository over SSH (or the repository
# override provided on the command line) into a named checkout under
# dependencies/ and adds the source to this build. No dependency is expected to
# live under third_party/.
#
# dependencies/ only holds source checkouts, mirroring AMC's layout:
#
#   dependencies/
#   ├── ABIX/
#   ├── EPCD/
#   ├── cmdly/
#   ├── libdrmtap/
#   ├── tomlplusplus/   (only when toml++ is not installed)
#   └── README.md
#
# FetchContent build/subbuild trees live in the CMake binary directory
# (build/<config>/_deps), never in dependencies/. dependencies/ is generated,
# git-ignored content (only its README is tracked) so checkouts never enter the
# WCAS history.

set(WCAS_DEPENDENCIES_DIR "${PROJECT_SOURCE_DIR}/dependencies"
    CACHE PATH "Directory for CMake-fetched dependency checkouts")

# Older WCAS revisions pointed FetchContent's base directory at dependencies/,
# producing flat abix-src/abix-build/abix-subbuild siblings. Migrate that stale
# cache entry so build trees move into the binary directory.
if(FETCHCONTENT_BASE_DIR STREQUAL WCAS_DEPENDENCIES_DIR)
    unset(FETCHCONTENT_BASE_DIR CACHE)
endif()
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps"
    CACHE PATH "Base directory for FetchContent build and subbuild trees" FORCE)

include(FetchContent)

# Repositories are cache variables so CI/offline builds can override them
# (for example with a local checkout or an HTTPS mirror).
set(WCAS_ABIX_REPOSITORY "git@github.com:Sukanle/ABIX.git" CACHE STRING "ABIX Git repository")
set(WCAS_ABIX_TAG "main" CACHE STRING "ABIX Git tag or branch")
set(WCAS_EPCD_REPOSITORY "git@github.com:Sukanle/EPCD.git" CACHE STRING "EPCD Git repository")
set(WCAS_EPCD_TAG "main" CACHE STRING "EPCD Git tag or branch")
set(WCAS_CMDLY_REPOSITORY "git@github.com:Sukanle/cmdly.git" CACHE STRING "cmdly Git repository")
set(WCAS_CMDLY_TAG "main" CACHE STRING "cmdly Git tag or branch")
set(WCAS_LIBDRMTAP_REPOSITORY "git@github.com:fxd0h/libdrmtap.git" CACHE STRING "libdrmtap Git repository")
set(WCAS_LIBDRMTAP_TAG "main" CACHE STRING "libdrmtap Git tag or branch")

set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# toml++: header-only profile configuration parser used by proton-inject.
# ---------------------------------------------------------------------------
find_package(tomlplusplus CONFIG QUIET)
if(NOT tomlplusplus_FOUND)
    set(_wcas_tomlplusplus_SOURCE_DIR "${WCAS_DEPENDENCIES_DIR}/tomlplusplus")
    set(_wcas_tomlplusplus_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/tomlplusplus-build")
    if(EXISTS "${_wcas_tomlplusplus_SOURCE_DIR}/CMakeLists.txt")
        message(STATUS "toml++ not installed; using existing checkout at ${_wcas_tomlplusplus_SOURCE_DIR}")
        add_subdirectory("${_wcas_tomlplusplus_SOURCE_DIR}" "${_wcas_tomlplusplus_BINARY_DIR}")
    else()
        message(STATUS "toml++ not installed; fetching tomlplusplus into ${_wcas_tomlplusplus_SOURCE_DIR}")
        fetchcontent_declare(
            tomlplusplus
            GIT_REPOSITORY "git@github.com:marzer/tomlplusplus.git"
            GIT_TAG "v3.4.0"
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
            SOURCE_DIR "${_wcas_tomlplusplus_SOURCE_DIR}"
            BINARY_DIR "${_wcas_tomlplusplus_BINARY_DIR}")
        fetchcontent_makeavailable(tomlplusplus)
    endif()
    unset(_wcas_tomlplusplus_SOURCE_DIR)
    unset(_wcas_tomlplusplus_BINARY_DIR)
endif()
if(NOT TARGET tomlplusplus::tomlplusplus)
    message(FATAL_ERROR "tomlplusplus::tomlplusplus is unavailable")
endif()

# ---------------------------------------------------------------------------
# ABIX: header-only runtime (plus MICS); prefer an installed package, otherwise
# clone the repository and build it runtime-only.
# ---------------------------------------------------------------------------
find_package(ABIX 0.2.3 CONFIG QUIET COMPONENTS runtime)
if(NOT ABIX_FOUND AND NOT TARGET ABIX::runtime)
    set(ABIX_BUILD_RUNTIME_ONLY ON CACHE BOOL "Build only the header-only ABIX runtime" FORCE)
    set(ABIX_INSTALL OFF CACHE BOOL "Do not generate ABIX install rules" FORCE)
    set(ABIX_INSTALL_BUNDLED_MICS OFF CACHE BOOL "Do not install bundled MICS" FORCE)
    set(_wcas_abix_SOURCE_DIR "${WCAS_DEPENDENCIES_DIR}/ABIX")
    set(_wcas_abix_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/abix-build")
    if(EXISTS "${_wcas_abix_SOURCE_DIR}/CMakeLists.txt")
        message(STATUS "ABIX not installed; using existing checkout at ${_wcas_abix_SOURCE_DIR}")
        add_subdirectory("${_wcas_abix_SOURCE_DIR}" "${_wcas_abix_BINARY_DIR}")
    else()
        message(
            STATUS
                "ABIX not installed; fetching ${WCAS_ABIX_REPOSITORY} (${WCAS_ABIX_TAG}) into ${_wcas_abix_SOURCE_DIR}")
        fetchcontent_declare(
            abix
            GIT_REPOSITORY "${WCAS_ABIX_REPOSITORY}"
            GIT_TAG "${WCAS_ABIX_TAG}"
            GIT_SHALLOW TRUE
            GIT_SUBMODULES_RECURSE TRUE
            GIT_PROGRESS TRUE
            SOURCE_DIR "${_wcas_abix_SOURCE_DIR}"
            BINARY_DIR "${_wcas_abix_BINARY_DIR}")
        fetchcontent_makeavailable(abix)
    endif()
    unset(_wcas_abix_SOURCE_DIR)
    unset(_wcas_abix_BINARY_DIR)
endif()
if(NOT TARGET ABIX::runtime)
    message(FATAL_ERROR "ABIX::runtime is unavailable; install ABIX or check ${WCAS_ABIX_REPOSITORY}")
endif()

# ---------------------------------------------------------------------------
# EPCD: error/condition domain package. Its Interface component depends on
# ABIX; it reuses the ABIX target created above, so it does not clone ABIX
# again.
# ---------------------------------------------------------------------------
find_package(EPCD 0.1 CONFIG QUIET COMPONENTS Core Interface)
if(NOT EPCD_FOUND)
    set(_wcas_epcd_SOURCE_DIR "${WCAS_DEPENDENCIES_DIR}/EPCD")
    set(_wcas_epcd_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/epcd-build")
    if(EXISTS "${_wcas_epcd_SOURCE_DIR}/CMakeLists.txt")
        message(STATUS "EPCD not installed; using existing checkout at ${_wcas_epcd_SOURCE_DIR}")
        add_subdirectory("${_wcas_epcd_SOURCE_DIR}" "${_wcas_epcd_BINARY_DIR}")
    else()
        message(
            STATUS
                "EPCD not installed; fetching ${WCAS_EPCD_REPOSITORY} (${WCAS_EPCD_TAG}) into ${_wcas_epcd_SOURCE_DIR}")
        fetchcontent_declare(
            epcd
            GIT_REPOSITORY "${WCAS_EPCD_REPOSITORY}"
            GIT_TAG "${WCAS_EPCD_TAG}"
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
            SOURCE_DIR "${_wcas_epcd_SOURCE_DIR}"
            BINARY_DIR "${_wcas_epcd_BINARY_DIR}")
        fetchcontent_makeavailable(epcd)
    endif()
    unset(_wcas_epcd_SOURCE_DIR)
    unset(_wcas_epcd_BINARY_DIR)
endif()
if(NOT TARGET EPCD::Core)
    message(FATAL_ERROR "EPCD::Core is unavailable; install EPCD or check ${WCAS_EPCD_REPOSITORY}")
endif()
set(WCAS_EPCD_TARGETS EPCD::Core)
if(TARGET EPCD::Interface)
    list(APPEND WCAS_EPCD_TARGETS EPCD::Interface)
endif()

# ---------------------------------------------------------------------------
# cmdly: line editor. No installed CMake package is currently shipped, so this
# falls back to a source clone and adds only libcmdly (not its examples/tests).
# ---------------------------------------------------------------------------
find_package(cmdly CONFIG QUIET)
set(WCAS_CMDLY_TARGET "")
if(TARGET cmdly_static)
    set(WCAS_CMDLY_TARGET cmdly_static)
elseif(TARGET Cmdly::cmdly_static)
    set(WCAS_CMDLY_TARGET Cmdly::cmdly_static)
else()
    set(_wcas_cmdly_SOURCE_DIR "${WCAS_DEPENDENCIES_DIR}/cmdly")
    set(_wcas_cmdly_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/cmdly-build")
    set(_wcas_cmdly_build_testing "${BUILD_TESTING}")
    set(BUILD_TESTING OFF)
    if(EXISTS "${_wcas_cmdly_SOURCE_DIR}/libcmdly/CMakeLists.txt")
        message(STATUS "cmdly not installed; using existing checkout at ${_wcas_cmdly_SOURCE_DIR}")
        add_subdirectory("${_wcas_cmdly_SOURCE_DIR}/libcmdly" "${_wcas_cmdly_BINARY_DIR}")
    else()
        message(
            STATUS
                "cmdly not installed; fetching ${WCAS_CMDLY_REPOSITORY} (${WCAS_CMDLY_TAG}) into ${_wcas_cmdly_SOURCE_DIR}"
        )
        fetchcontent_declare(
            cmdly
            GIT_REPOSITORY "${WCAS_CMDLY_REPOSITORY}"
            GIT_TAG "${WCAS_CMDLY_TAG}"
            GIT_SHALLOW FALSE
            GIT_PROGRESS TRUE
            SOURCE_DIR "${_wcas_cmdly_SOURCE_DIR}"
            BINARY_DIR "${_wcas_cmdly_BINARY_DIR}"
            SOURCE_SUBDIR libcmdly)
        fetchcontent_makeavailable(cmdly)
    endif()
    set(BUILD_TESTING "${_wcas_cmdly_build_testing}")
    unset(_wcas_cmdly_build_testing)
    unset(_wcas_cmdly_SOURCE_DIR)
    unset(_wcas_cmdly_BINARY_DIR)
    if(TARGET cmdly_static)
        set(WCAS_CMDLY_TARGET cmdly_static)
    else()
        message(FATAL_ERROR "cmdly was fetched but cmdly_static was not created")
    endif()
endif()

# ---------------------------------------------------------------------------
# libdrmtap: Meson-owned library. Find an installed copy first; otherwise clone
# the source and let the ExternalProject block in CMakeLists.txt build it.
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
find_package(libdrmtap QUIET MODULE)
if(NOT libdrmtap_FOUND)
    set(_wcas_libdrmtap_SOURCE_DIR "${WCAS_DEPENDENCIES_DIR}/libdrmtap")
    if(EXISTS "${_wcas_libdrmtap_SOURCE_DIR}/meson.build")
        message(STATUS "libdrmtap not installed; using existing checkout at ${_wcas_libdrmtap_SOURCE_DIR}")
    else()
        message(
            STATUS
                "libdrmtap not installed; fetching ${WCAS_LIBDRMTAP_REPOSITORY} (${WCAS_LIBDRMTAP_TAG}) into ${_wcas_libdrmtap_SOURCE_DIR}"
        )
        fetchcontent_declare(
            libdrmtap
            GIT_REPOSITORY "${WCAS_LIBDRMTAP_REPOSITORY}"
            GIT_TAG "${WCAS_LIBDRMTAP_TAG}"
            GIT_SHALLOW FALSE
            GIT_PROGRESS TRUE
            SOURCE_DIR "${_wcas_libdrmtap_SOURCE_DIR}"
            BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/libdrmtap-build")
        fetchcontent_getproperties(libdrmtap)
        if(NOT libdrmtap_POPULATED)
            fetchcontent_populate(libdrmtap)
        endif()
    endif()
    set(DRMTAP_SOURCE_DIR "${_wcas_libdrmtap_SOURCE_DIR}")
    set(WCAS_DRMTAP_FETCHED ON)
    unset(_wcas_libdrmtap_SOURCE_DIR)
else()
    set(WCAS_DRMTAP_FETCHED OFF)
endif()

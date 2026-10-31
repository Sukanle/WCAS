# Dependency bootstrap for WCAS.
#
# Every dependency is first looked up with find_package(). When no package is
# installed, FetchContent clones the repository over SSH (or the repository
# override provided on the command line) and adds the source to this build.
# No dependency is expected to live under third_party/.

include(FetchContent)

# Repositories are cache variables so CI/offline builds can override them
# (for example with a local checkout or an HTTPS mirror).
set(WCAS_ABIX_REPOSITORY "git@github.com:Sukanle/ABIX.git" CACHE STRING "ABIX Git repository")
set(WCAS_ABIX_TAG "v0.2.3" CACHE STRING "ABIX Git tag")
set(WCAS_EPCD_REPOSITORY "git@github.com:Sukanle/EPCD.git" CACHE STRING "EPCD Git repository")
set(WCAS_EPCD_TAG "main" CACHE STRING "EPCD Git tag or branch")
set(WCAS_CMDLY_REPOSITORY "git@github.com:Sukanle/cmdly.git" CACHE STRING "cmdly Git repository")
set(WCAS_CMDLY_TAG "f3cf7baae7201430d5eb7abc913faaef6d5ab6df" CACHE STRING "cmdly Git commit")
set(WCAS_LIBDRMTAP_REPOSITORY "git@github.com:fxd0h/libdrmtap.git" CACHE STRING "libdrmtap Git repository")
set(WCAS_LIBDRMTAP_TAG "892fbe0d1b1083e3344a49e31672046a2ebebc24" CACHE STRING "libdrmtap Git commit")

set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# ABIX: header-only runtime (plus MICS); prefer an installed package, otherwise
# clone the repository and build it runtime-only.
# ---------------------------------------------------------------------------
find_package(ABIX 0.2.3 CONFIG QUIET COMPONENTS runtime)
if(NOT ABIX_FOUND AND NOT TARGET ABIX::runtime)
    message(STATUS "ABIX not installed; fetching ${WCAS_ABIX_REPOSITORY} (${WCAS_ABIX_TAG})")
    set(ABIX_BUILD_RUNTIME_ONLY ON CACHE BOOL "Build only the header-only ABIX runtime" FORCE)
    set(ABIX_INSTALL OFF CACHE BOOL "Do not generate ABIX install rules" FORCE)
    set(ABIX_INSTALL_BUNDLED_MICS OFF CACHE BOOL "Do not install bundled MICS" FORCE)
    FetchContent_Declare(
        abix
        GIT_REPOSITORY "${WCAS_ABIX_REPOSITORY}"
        GIT_TAG "${WCAS_ABIX_TAG}"
        GIT_SHALLOW TRUE
        GIT_SUBMODULES_RECURSE TRUE
        GIT_PROGRESS TRUE)
    FetchContent_MakeAvailable(abix)
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
    message(STATUS "EPCD not installed; fetching ${WCAS_EPCD_REPOSITORY} (${WCAS_EPCD_TAG})")
    FetchContent_Declare(
        epcd
        GIT_REPOSITORY "${WCAS_EPCD_REPOSITORY}"
        GIT_TAG "${WCAS_EPCD_TAG}"
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE)
    FetchContent_MakeAvailable(epcd)
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
    message(STATUS "cmdly not installed; fetching ${WCAS_CMDLY_REPOSITORY} (${WCAS_CMDLY_TAG})")
    FetchContent_Declare(
        cmdly
        GIT_REPOSITORY "${WCAS_CMDLY_REPOSITORY}"
        GIT_TAG "${WCAS_CMDLY_TAG}"
        GIT_SHALLOW FALSE
        GIT_PROGRESS TRUE
        SOURCE_SUBDIR libcmdly)
    set(_wcas_cmdly_build_testing "${BUILD_TESTING}")
    set(BUILD_TESTING OFF)
    FetchContent_MakeAvailable(cmdly)
    set(BUILD_TESTING "${_wcas_cmdly_build_testing}")
    unset(_wcas_cmdly_build_testing)
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
    message(STATUS "libdrmtap not installed; fetching ${WCAS_LIBDRMTAP_REPOSITORY} (${WCAS_LIBDRMTAP_TAG})")
    FetchContent_Declare(
        libdrmtap
        GIT_REPOSITORY "${WCAS_LIBDRMTAP_REPOSITORY}"
        GIT_TAG "${WCAS_LIBDRMTAP_TAG}"
        GIT_SHALLOW FALSE
        GIT_PROGRESS TRUE)
    FetchContent_GetProperties(libdrmtap)
    if(NOT libdrmtap_POPULATED)
        FetchContent_Populate(libdrmtap)
    endif()
    set(DRMTAP_SOURCE_DIR "${libdrmtap_SOURCE_DIR}")
    set(WCAS_DRMTAP_FETCHED ON)
else()
    set(WCAS_DRMTAP_FETCHED OFF)
endif()

# Find an installed libdrmtap (Meson-installed library/header) and expose it as
# the imported target used by WCAS. The in-tree fallback is handled by
# cmake/Dependencies.cmake and the ExternalProject block in CMakeLists.txt.
find_path(LIBDRMTAP_INCLUDE_DIR NAMES drmtap.h)
find_library(LIBDRMTAP_LIBRARY NAMES drmtap)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(libdrmtap DEFAULT_MSG LIBDRMTAP_INCLUDE_DIR LIBDRMTAP_LIBRARY)

if(libdrmtap_FOUND AND NOT TARGET libdrmtap::drmtap)
    add_library(libdrmtap::drmtap UNKNOWN IMPORTED)
    set_target_properties(
        libdrmtap::drmtap
        PROPERTIES IMPORTED_LOCATION "${LIBDRMTAP_LIBRARY}"
                   INTERFACE_INCLUDE_DIRECTORIES "${LIBDRMTAP_INCLUDE_DIR}")
endif()

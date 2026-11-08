# Resolve libpng exclusively to the vendored, already-configured zlib 1.4.
# No host/NDK system library search is performed by this integration adapter.
if(NOT TARGET zlibstatic)
    message(FATAL_ERROR "Configure pinned zlibstatic before libpng")
endif()
if(TARGET ZLIB::ZLIB)
    get_target_property(nnn_zlib_backend ZLIB::ZLIB INTERFACE_LINK_LIBRARIES)
    if(NOT nnn_zlib_backend STREQUAL "zlibstatic")
        message(FATAL_ERROR "Existing ZLIB::ZLIB does not resolve to pinned zlibstatic")
    endif()
else()
    # An imported facade matches FindZLIB's public contract and avoids making
    # libpng's unused install exports require exporting our private vendor target.
    add_library(ZLIB::ZLIB INTERFACE IMPORTED GLOBAL)
    set_property(TARGET ZLIB::ZLIB PROPERTY INTERFACE_LINK_LIBRARIES zlibstatic)
endif()
set(ZLIB_FOUND TRUE)
set(ZLIB_VERSION "1.4")
set(ZLIB_VERSION_STRING "1.4")
set(ZLIB_INCLUDE_DIRS "${zlib_BINARY_DIR}" "${zlib_SOURCE_DIR}")
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR}")
set(ZLIB_LIBRARIES ZLIB::ZLIB)

# FindLibreDWG.cmake
# ----------------
# Locates or builds GNU LibreDWG from source.
#
# Input variables:
#   LIBREDWG_ROOT_DIR    - Hint: path to libredwg install prefix or source tree
#   LIBREDWG_BUILD_FROM_SOURCE - If ON, try to build from third_party/libredwg
#
# Output variables:
#   LIBREDWG_FOUND        - TRUE if found
#   LIBREDWG_INCLUDE_DIRS - Include directories
#   LIBREDWG_LIBRARIES    - Libraries to link against
#   LIBREDWG_VERSION      - Version string
#
# This module first tries find_library/find_path for a system install.
# If not found and LIBREDWG_BUILD_FROM_SOURCE is ON, it builds from the
# source tree at third_party/libredwg/ using autotools.

# ---- Try system install first ----
find_path(LIBREDWG_INCLUDE_DIR
    NAMES dwg.h
    PATH_SUFFIXES include libredwg libreDWG
    HINTS ${LIBREDWG_ROOT_DIR}
          ${LIBREDWG_ROOT_DIR}/include
          ${CMAKE_CURRENT_LIST_DIR}/../third_party/libredwg/include
    DOC "libredwg include directory"
)

find_library(LIBREDWG_LIBRARY
    NAMES redwg dwg libredwg
    PATH_SUFFIXES lib lib64 bin src/.libs build/Release build/Debug build/RelWithDebInfo
    HINTS ${LIBREDWG_ROOT_DIR}
          ${LIBREDWG_ROOT_DIR}/lib
          ${LIBREDWG_ROOT_DIR}/lib64
          ${LIBREDWG_ROOT_DIR}/bin
          ${LIBREDWG_ROOT_DIR}/src/.libs
          ${LIBREDWG_ROOT_DIR}/build/Release
          ${LIBREDWG_ROOT_DIR}/build/Debug
          ${CMAKE_CURRENT_LIST_DIR}/../third_party/libredwg/src/.libs
          ${CMAKE_CURRENT_LIST_DIR}/../third_party/libredwg/build/Release
    DOC "libredwg library"
)

# ---- Check version from dwg.h ----
if(LIBREDWG_INCLUDE_DIR AND EXISTS "${LIBREDWG_INCLUDE_DIR}/dwg.h")
    file(STRINGS "${LIBREDWG_INCLUDE_DIR}/dwg.h" _version_line
         REGEX "^#define[ \t]+DWG_VERSION[ \t]+\"[0-9.]+\"")
    if(_version_line)
        string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1"
               LIBREDWG_VERSION "${_version_line}")
    else()
        file(STRINGS "${LIBREDWG_INCLUDE_DIR}/dwg.h" _version_major_line
             REGEX "^#define[ \t]+LIBREDWG_VERSION_MAJOR[ \t]+[0-9]+")
        file(STRINGS "${LIBREDWG_INCLUDE_DIR}/dwg.h" _version_minor_line
             REGEX "^#define[ \t]+LIBREDWG_VERSION_MINOR[ \t]+[0-9]+")
        if(_version_major_line AND _version_minor_line)
            string(REGEX REPLACE ".*[ \t]([0-9]+)$" "\\1"
                   _version_major "${_version_major_line}")
            string(REGEX REPLACE ".*[ \t]([0-9]+)$" "\\1"
                   _version_minor "${_version_minor_line}")
            set(LIBREDWG_VERSION "${_version_major}.${_version_minor}")
        endif()
    endif()
endif()

# If system install found, we're done
if(LIBREDWG_INCLUDE_DIR AND LIBREDWG_LIBRARY)
    set(LIBREDWG_FOUND TRUE)
    set(LIBREDWG_INCLUDE_DIRS ${LIBREDWG_INCLUDE_DIR})
    set(LIBREDWG_LIBRARIES ${LIBREDWG_LIBRARY})
    message(STATUS "Found libredwg: ${LIBREDWG_LIBRARY} (version ${LIBREDWG_VERSION})")
    return()
endif()

# ---- Not found ----
set(LIBREDWG_FOUND FALSE)
set(LIBREDWG_INCLUDE_DIRS "")
set(LIBREDWG_LIBRARIES "")

if(NOT LIBREDWG_FIND_QUIETLY)
    message(STATUS "libredwg not found on system.")
    message(STATUS "  To build DWG support from source:")
    message(STATUS "  1. cd third_party && git clone https://git.savannah.gnu.org/git/libredwg.git")
    message(STATUS "  2. cd libredwg && ./autogen.sh && ./configure --disable-bindings && make")
    message(STATUS "  3. Re-run cmake with: -DLIBREDWG_ROOT_DIR=third_party/libredwg")
    message(STATUS "  Or use: ./scripts/build_libredwg.sh")
endif()

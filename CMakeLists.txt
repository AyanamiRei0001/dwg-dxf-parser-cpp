cmake_minimum_required(VERSION 3.16)
project(cad-parser VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- CMake module path ----
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---- Options ----
option(CAD_USE_LIBREDWG_API "Use libredwg C API for DWG parsing (instead of CLI subprocess)" ON)
option(CAD_USE_LIBREDWG_CLI  "Fallback to libredwg CLI (dwgread) when API unavailable" ON)

# ---- Library sources ----
set(CAD_PARSER_SOURCES
    src/common_types.cpp
    src/dxf_parser.cpp
    src/dwg_parser.cpp
    src/svg_exporter.cpp
)

# ---- libredwg integration ----
set(CAD_DWG_BACKEND "none")  # will be set to "api" or "cli"

if(CAD_USE_LIBREDWG_API)
    find_package(LibreDWG QUIET)

    if(LIBREDWG_FOUND)
        message(STATUS "✅ DWG backend: libredwg C API (${LIBREDWG_VERSION})")
        set(CAD_DWG_BACKEND "api")

        # Add DWG API implementation source
        list(APPEND CAD_PARSER_SOURCES src/dwg_api_impl.cpp)
    else()
        message(STATUS "⚠ libredwg C API not found, falling back to CLI")
    endif()
endif()

if(CAD_DWG_BACKEND STREQUAL "none" AND CAD_USE_LIBREDWG_CLI)
    message(STATUS "🔧 DWG backend: libredwg CLI (dwgread subprocess)")
    set(CAD_DWG_BACKEND "cli")
endif()

if(CAD_DWG_BACKEND STREQUAL "none")
    message(STATUS "⚠ DWG support disabled — install libredwg for DWG parsing")
endif()

# ---- Library target ----
add_library(cad_parser STATIC ${CAD_PARSER_SOURCES})

target_include_directories(cad_parser PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# libredwg headers and link
if(CAD_DWG_BACKEND STREQUAL "api")
    target_include_directories(cad_parser PRIVATE ${LIBREDWG_INCLUDE_DIRS})
    target_link_libraries(cad_parser PUBLIC ${LIBREDWG_LIBRARIES})
    target_compile_definitions(cad_parser PRIVATE CAD_HAS_LIBREDWG_API=1)
elseif(CAD_DWG_BACKEND STREQUAL "cli")
    target_compile_definitions(cad_parser PRIVATE CAD_HAS_LIBREDWG_CLI=1)
endif()

target_compile_options(cad_parser PRIVATE -Wall -Wextra -pedantic)

# ---- CLI demo tool ----
add_executable(cad-parser-cli src/main.cpp)
target_link_libraries(cad-parser-cli PRIVATE cad_parser)

# ---- Install ----
install(TARGETS cad_parser cad-parser-cli
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/ DESTINATION include)

# ---- Qt Viewer (optional) ----
option(BUILD_QT_VIEWER "Build Qt5 Widgets CAD viewer" ON)

if(BUILD_QT_VIEWER)
    find_package(Qt5 COMPONENTS Widgets QUIET)
    if(Qt5_FOUND)
        message(STATUS "🖥  Qt viewer: ENABLED (Qt${Qt5_VERSION})")

        set(QT_VIEWER_SOURCES
            src/qt_viewer/main.cpp
            src/qt_viewer/CadViewer.cpp
        )

        add_executable(cad-viewer ${QT_VIEWER_SOURCES})
        target_link_libraries(cad-viewer PRIVATE cad_parser Qt5::Widgets)
        target_include_directories(cad-viewer PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/qt_viewer
        )
        target_compile_options(cad-viewer PRIVATE -Wall -Wextra)

        # Qt MOC needs the header
        set_target_properties(cad-viewer PROPERTIES
            AUTOMOC ON
            WIN32_EXECUTABLE_ON_WIN32 TRUE
        )

        install(TARGETS cad-viewer RUNTIME DESTINATION bin)
    else()
        message(STATUS "⚠ Qt viewer: DISABLED (Qt5 not found). Install qt5-default or qtbase5-dev")
    endif()
endif()

# ---- Tests (optional) ----
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# ---- Print configuration summary ----
message(STATUS "")
message(STATUS "cad-parser configuration summary:")
message(STATUS "  C++ Standard:  17")
message(STATUS "  DWG backend:   ${CAD_DWG_BACKEND}")
message(STATUS "  Qt viewer:     ${Qt5_FOUND}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")

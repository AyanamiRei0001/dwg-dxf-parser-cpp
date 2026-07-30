cmake_minimum_required(VERSION 3.16)
project(cad-parser VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- CMake module path ----
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---- Options ----
option(CAD_USE_LIBREDWG_API "Use libredwg C API for DWG parsing (instead of CLI subprocess)" ON)
option(CAD_USE_LIBREDWG_CLI  "Use libredwg CLI (dwgread) when the C API is not selected" ON)

function(cad_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -pedantic)
    endif()
endfunction()

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

# GCC 8 provides C++17 filesystem in a separate compatibility library. Newer
# GCC, Clang, and MSVC link it as part of the standard library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(cad_parser PUBLIC stdc++fs)
endif()

# libredwg headers and link
if(CAD_DWG_BACKEND STREQUAL "api")
    target_include_directories(cad_parser PRIVATE ${LIBREDWG_INCLUDE_DIRS})
    target_link_libraries(cad_parser PUBLIC ${LIBREDWG_LIBRARIES})
    target_compile_definitions(cad_parser PRIVATE CAD_HAS_LIBREDWG_API=1)
elseif(CAD_DWG_BACKEND STREQUAL "cli")
    target_compile_definitions(cad_parser PRIVATE CAD_HAS_LIBREDWG_CLI=1)
endif()

cad_enable_warnings(cad_parser)

# ---- CLI demo tool ----
add_executable(cad-parser-cli src/main.cpp)
target_link_libraries(cad-parser-cli PRIVATE cad_parser)
target_include_directories(cad-parser-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
cad_enable_warnings(cad-parser-cli)

# ---- Install ----
install(TARGETS cad_parser cad-parser-cli
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/ DESTINATION include)

# ---- Qt Viewer (optional) ----
option(BUILD_QT_VIEWER "Build Qt5 Widgets CAD viewer" ON)
set(CAD_QT_VIEWER_STATUS "disabled")

if(BUILD_QT_VIEWER)
    # Search common Qt installation directories on Windows
    if(WIN32)
        set(_qt_hints
            "D:/Qt"
            "C:/Qt"
            "$ENV{LOCALAPPDATA}/Qt"
            "$ENV{APPDATA}/Qt"
        )
        foreach(_hint ${_qt_hints})
            file(GLOB _qt_dirs "${_hint}/*/*")
            foreach(_dir ${_qt_dirs})
                if(IS_DIRECTORY "${_dir}")
                    list(APPEND CMAKE_PREFIX_PATH "${_dir}")
                endif()
            endforeach()
        endforeach()
    endif()

    find_package(Qt5 COMPONENTS Widgets QUIET)
    if(Qt5_FOUND)
        message(STATUS "🖥  Qt viewer: ENABLED (Qt${Qt5_VERSION})")
        set(CAD_QT_VIEWER_STATUS "Qt${Qt5_VERSION}")
    endif()

    if(Qt5_FOUND)
        set(QT_VIEWER_SOURCES
            src/qt_viewer/main.cpp
            src/qt_viewer/CadViewer.cpp
        )

        add_executable(cad-viewer ${QT_VIEWER_SOURCES})
        target_link_libraries(cad-viewer PRIVATE cad_parser Qt5::Widgets)
        target_include_directories(cad-viewer PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/qt_viewer
        )
        cad_enable_warnings(cad-viewer)

        # Qt MOC needs the header
        set_target_properties(cad-viewer PROPERTIES AUTOMOC ON)
        if(WIN32)
            set_target_properties(cad-viewer PROPERTIES WIN32_EXECUTABLE TRUE)
        endif()
        install(TARGETS cad-viewer RUNTIME DESTINATION bin)
    else()
        message(STATUS "⚠ Qt viewer: DISABLED (Qt5 Widgets not found)")
        if(WIN32)
            message(STATUS "  Install Qt5 for Windows: pip install aqtinstall && aqt install-qt windows desktop 5.15.2 win64_msvc2019_64 -O D:/Qt")
        endif()
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
message(STATUS "  Host platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  DWG backend:   ${CAD_DWG_BACKEND}")
message(STATUS "  Qt viewer:     ${CAD_QT_VIEWER_STATUS}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")

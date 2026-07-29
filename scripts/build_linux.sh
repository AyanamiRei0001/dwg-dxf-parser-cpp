#!/usr/bin/env bash
# Build the bundled LibreDWG C API, then build and test cad-parser on Linux.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "${SCRIPT_DIR}")"

BUILD_DIR="${PROJECT_DIR}/build"
CONFIGURATION="Release"
GENERATOR=""
LIBREDWG_ROOT=""
QT_PREFIX=""
ENABLE_QT_VIEWER=0
SKIP_TESTS=0
SKIP_SUBMODULES=0
SKIP_LIBREDWG=0
CLEAN=0

usage() {
    cat <<'EOF'
Usage: ./scripts/build_linux.sh [options]

Options:
  --build-dir PATH       Build directory (default: ./build)
  --configuration NAME   Debug, Release, RelWithDebInfo, or MinSizeRel
  --generator NAME       CMake generator to use
  --libredwg-root PATH   Use an existing LibreDWG installation
  --qt-prefix PATH       Qt5 CMake prefix; implies --enable-qt-viewer
  --enable-qt-viewer     Build the Qt5 viewer
  --skip-tests           Do not build or run tests
  --skip-submodules      Do not initialize git submodules
  --skip-libredwg        Disable the LibreDWG C API and use the CLI fallback
  --clean                Remove the selected build directory first
  -h, --help             Show this help text
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command '$1' was not found in PATH"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || die "--build-dir requires a value"
            BUILD_DIR="$2"
            shift 2
            ;;
        --configuration)
            [[ $# -ge 2 ]] || die "--configuration requires a value"
            CONFIGURATION="$2"
            shift 2
            ;;
        --generator)
            [[ $# -ge 2 ]] || die "--generator requires a value"
            GENERATOR="$2"
            shift 2
            ;;
        --libredwg-root)
            [[ $# -ge 2 ]] || die "--libredwg-root requires a value"
            LIBREDWG_ROOT="$2"
            shift 2
            ;;
        --qt-prefix)
            [[ $# -ge 2 ]] || die "--qt-prefix requires a value"
            QT_PREFIX="$2"
            ENABLE_QT_VIEWER=1
            shift 2
            ;;
        --enable-qt-viewer)
            ENABLE_QT_VIEWER=1
            shift
            ;;
        --skip-tests)
            SKIP_TESTS=1
            shift
            ;;
        --skip-submodules)
            SKIP_SUBMODULES=1
            shift
            ;;
        --skip-libredwg)
            SKIP_LIBREDWG=1
            shift
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option '$1'"
            ;;
    esac
done

case "${CONFIGURATION}" in
    Debug|Release|RelWithDebInfo|MinSizeRel) ;;
    *) die "unsupported configuration '${CONFIGURATION}'" ;;
esac

if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${PROJECT_DIR}/${BUILD_DIR}"
fi

require_command cmake
if [[ "${SKIP_SUBMODULES}" -eq 0 && -f "${PROJECT_DIR}/.gitmodules" ]]; then
    require_command git
fi

if [[ "${CLEAN}" -eq 1 ]]; then
    rm -rf "${BUILD_DIR}"
fi

if [[ "${SKIP_SUBMODULES}" -eq 0 && -f "${PROJECT_DIR}/.gitmodules" ]]; then
    printf '[1/5] Initializing git submodules...\n'
    git -C "${PROJECT_DIR}" submodule update --init --recursive --depth 1
fi

main_cmake_args=(
    -S "${PROJECT_DIR}"
    -B "${BUILD_DIR}"
    "-DCMAKE_BUILD_TYPE=${CONFIGURATION}"
    "-DBUILD_TESTS=$([[ "${SKIP_TESTS}" -eq 1 ]] && printf OFF || printf ON)"
    "-DBUILD_QT_VIEWER=$([[ "${ENABLE_QT_VIEWER}" -eq 1 ]] && printf ON || printf OFF)"
    -DCAD_USE_LIBREDWG_CLI=ON
)

if [[ -n "${GENERATOR}" ]]; then
    main_cmake_args+=(-G "${GENERATOR}")
fi

if [[ -n "${QT_PREFIX}" ]]; then
    main_cmake_args+=("-DCMAKE_PREFIX_PATH=${QT_PREFIX}")
fi

if [[ "${SKIP_LIBREDWG}" -eq 1 ]]; then
    printf '[2/5] Using the DWG CLI fallback (LibreDWG API disabled)...\n'
    main_cmake_args+=(-DCAD_USE_LIBREDWG_API=OFF)
elif [[ -n "${LIBREDWG_ROOT}" ]]; then
    [[ -d "${LIBREDWG_ROOT}" ]] || die "LibreDWG root '${LIBREDWG_ROOT}' does not exist"
    printf '[2/5] Using the supplied LibreDWG installation...\n'
    main_cmake_args+=(-DCAD_USE_LIBREDWG_API=ON "-DLIBREDWG_ROOT_DIR=${LIBREDWG_ROOT}")
else
    libredwg_source="${PROJECT_DIR}/third_party/libredwg"
    libredwg_include="${libredwg_source}/include"
    [[ -f "${libredwg_include}/dwg.h" ]] || die "LibreDWG source is missing; run without --skip-submodules or use --skip-libredwg"

    libredwg_build="${BUILD_DIR}/third_party/libredwg"
    libredwg_cmake_args=(
        -S "${libredwg_source}"
        -B "${libredwg_build}"
        "-DCMAKE_BUILD_TYPE=${CONFIGURATION}"
        -DBUILD_SHARED_LIBS=OFF
        -DLIBREDWG_LIBONLY=ON
        -DLIBREDWG_DISABLE_WRITE=ON
        -DLIBREDWG_DISABLE_JSON=ON
        -DDISABLE_WERROR=ON
        -DENABLE_LTO=OFF
    )
    if [[ -n "${GENERATOR}" ]]; then
        libredwg_cmake_args+=(-G "${GENERATOR}")
    fi

    printf '[2/5] Configuring bundled LibreDWG...\n'
    cmake "${libredwg_cmake_args[@]}"
    printf '[3/5] Building bundled LibreDWG...\n'
    cmake --build "${libredwg_build}" --parallel

    libredwg_library="$(find "${libredwg_build}" -type f \( -name 'libredwg.a' -o -name 'libredwg.so' \) -print -quit)"
    [[ -n "${libredwg_library}" ]] || die "could not find the built LibreDWG library under '${libredwg_build}'"

    main_cmake_args+=(
        -DCAD_USE_LIBREDWG_API=ON
        "-DLIBREDWG_INCLUDE_DIR=${libredwg_include}"
        "-DLIBREDWG_LIBRARY=${libredwg_library}"
    )
fi

printf '[4/5] Configuring and building cad-parser...\n'
cmake "${main_cmake_args[@]}"
cmake --build "${BUILD_DIR}" --parallel

if [[ "${ENABLE_QT_VIEWER}" -eq 1 && ! -x "${BUILD_DIR}/cad-viewer" ]]; then
    die "Qt viewer was requested but was not built; provide --qt-prefix for a Qt5 installation"
fi

if [[ "${SKIP_TESTS}" -eq 0 ]]; then
    printf '[5/5] Running tests...\n'
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

printf 'Build complete: %s\n' "${BUILD_DIR}"
printf 'CLI: %s\n' "${BUILD_DIR}/cad-parser-cli"

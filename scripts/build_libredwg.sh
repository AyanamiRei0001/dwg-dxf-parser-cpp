#!/usr/bin/env bash
# =============================================================================
# build_libredwg.sh — Clone and build GNU LibreDWG as a static library
# =============================================================================
#
# This script fetches libredwg from source, configures and builds it
# as a static library that can be linked into cad-parser.
#
# Usage:
#   ./scripts/build_libredwg.sh              # Build in third_party/libredwg
#   ./scripts/build_libredwg.sh /opt/libredwg  # Build in custom location
#
# After building, configure cad-parser with:
#   cmake .. -DLIBREDWG_ROOT_DIR=third_party/libredwg
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${1:-${PROJECT_DIR}/third_party/libredwg}"

LIBREDWG_REPO="https://git.savannah.gnu.org/git/libredwg.git"

echo "=== GNU LibreDWG Build Helper ==="
echo "Project dir: ${PROJECT_DIR}"
echo "Target dir:  ${BUILD_DIR}"
echo ""

# ---- Clone ----
if [ ! -d "${BUILD_DIR}" ]; then
    echo "[1/4] Cloning libredwg from ${LIBREDWG_REPO} ..."
    mkdir -p "$(dirname "${BUILD_DIR}")"
    git clone --depth 1 "${LIBREDWG_REPO}" "${BUILD_DIR}"
else
    echo "[1/4] Using existing source at ${BUILD_DIR}"
fi

cd "${BUILD_DIR}"

# ---- Autogen (if needed) ----
if [ ! -f "./configure" ]; then
    echo "[2/4] Running autogen.sh ..."
    if [ -f "./autogen.sh" ]; then
        ./autogen.sh
    else
        # For release tarballs that include configure
        echo "  (no autogen.sh, assuming configure exists or using autoreconf)"
        autoreconf -fi || true
    fi
fi

# ---- Configure (static, no bindings to reduce deps) ----
echo "[3/4] Configuring (static library, no python bindings) ..."
./configure \
    --prefix="${BUILD_DIR}/_install" \
    --disable-bindings \
    --disable-shared \
    --enable-static \
    --with-pic \
    CFLAGS="-fPIC -O2" \
    2>&1 | tail -5

# ---- Build ----
echo "[4/4] Building ..."
make -j"$(nproc)" 2>&1 | tail -5

echo ""
echo "=== Done! ==="
echo ""
echo "Library:  ${BUILD_DIR}/src/.libs/libredwg.a"
echo "Headers:  ${BUILD_DIR}/include/"
echo ""
echo "Now configure cad-parser with:"
echo "  mkdir -p build && cd build"
echo "  cmake .. -DLIBREDWG_ROOT_DIR=${BUILD_DIR}"
echo "  make -j\$(nproc)"

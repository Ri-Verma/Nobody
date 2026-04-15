#!/usr/bin/env bash
# =============================================================================
#  build.sh  —  Build osint-ai (activates venv automatically)
#  Usage:  ./build.sh [debug|release] [clean]
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/venv"
BUILD_TYPE="${1:-Release}"
CLEAN="${2:-}"
BUILD_DIR="$SCRIPT_DIR/build"
BUILD_JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; RESET='\033[0m'; BOLD='\033[1m'

# Normalise build type
case "${BUILD_TYPE,,}" in
    debug)   BUILD_TYPE="Debug" ;;
    release) BUILD_TYPE="Release" ;;
    *)       BUILD_TYPE="Release" ;;
esac

# Check venv
if [ ! -f "$VENV_DIR/.activated" ]; then
    echo -e "${RED}ERROR: Virtual environment not found.${RESET}"
    echo "Please run:  bash setup_venv.sh"
    exit 1
fi

# Activate venv environment variables (inline — no sourcing needed)
export CMAKE_PREFIX_PATH="$VENV_DIR${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PKG_CONFIG_PATH="$VENV_DIR/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PATH="$VENV_DIR/bin:$PATH"

echo -e "${BOLD}${CYAN}[build]${RESET} Build type: $BUILD_TYPE | Jobs: $BUILD_JOBS"

# Clean if requested
if [[ "$CLEAN" == "clean" ]]; then
    echo -e "${CYAN}[build]${RESET} Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

# Configure
echo -e "${CYAN}[build]${RESET} Configuring with CMake..."
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_PREFIX_PATH="$VENV_DIR" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -Wno-dev

# Build
echo -e "${CYAN}[build]${RESET} Compiling ($BUILD_JOBS parallel jobs)..."
cmake --build "$BUILD_DIR" -j"$BUILD_JOBS"

echo ""
echo -e "${GREEN}[build] ✓ Build successful!${RESET}"
echo -e "${GREEN}[build]   Executable: $BUILD_DIR/osint_ai${RESET}"
echo ""
echo "  Run:  $BUILD_DIR/osint_ai"
echo "  Help: $BUILD_DIR/osint_ai --help"
echo ""
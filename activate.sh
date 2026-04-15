#!/usr/bin/env bash
# =============================================================================
#  activate.sh  —  Activate the osint-ai C++ virtual environment
#  Usage:  source activate.sh
#  Equiv.:  source venv/bin/activate  (Python)
# =============================================================================

# Guard: must be sourced, not executed
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo "ERROR: This script must be sourced, not executed."
    echo "Usage: source activate.sh"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/venv"

# Verify venv exists
if [ ! -f "$VENV_DIR/.activated" ]; then
    echo "ERROR: Virtual environment not found."
    echo "Please run:  bash setup_venv.sh"
    return 1
fi

# ── Save original values for deactivation ────────────────────────────────────
export _OSINT_ORIG_CMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:-}"
export _OSINT_ORIG_PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}"
export _OSINT_ORIG_PATH="$PATH"
export _OSINT_ORIG_PS1="${PS1:-}"

# ── Set venv-specific environment variables ───────────────────────────────────
export OSINT_AI_VENV=1
export OSINT_AI_VENV_DIR="$VENV_DIR"

# CMake prefix so find_package() resolves to venv first
export CMAKE_PREFIX_PATH="$VENV_DIR${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"

# pkg-config looks in venv/lib/pkgconfig
export PKG_CONFIG_PATH="$VENV_DIR/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# binaries installed into venv/bin take priority
export PATH="$VENV_DIR/bin:$PATH"

# C/C++ compiler include search path
export CPATH="$VENV_DIR/include${CPATH:+:$CPATH}"
export LIBRARY_PATH="$VENV_DIR/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
export LD_LIBRARY_PATH="$VENV_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# macOS (DYLD instead of LD)
if [[ "$(uname)" == "Darwin" ]]; then
    export DYLD_LIBRARY_PATH="$VENV_DIR/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
fi

# ── Prompt decoration (like Python venv) ──────────────────────────────────────
export PS1="(osint-ai-venv) ${PS1:-}"

# ── Deactivate function ───────────────────────────────────────────────────────
deactivate_osint_venv() {
    export CMAKE_PREFIX_PATH="$_OSINT_ORIG_CMAKE_PREFIX_PATH"
    export PKG_CONFIG_PATH="$_OSINT_ORIG_PKG_CONFIG_PATH"
    export PATH="$_OSINT_ORIG_PATH"
    export PS1="$_OSINT_ORIG_PS1"
    unset CPATH LIBRARY_PATH LD_LIBRARY_PATH
    unset OSINT_AI_VENV OSINT_AI_VENV_DIR
    unset _OSINT_ORIG_CMAKE_PREFIX_PATH _OSINT_ORIG_PKG_CONFIG_PATH
    unset _OSINT_ORIG_PATH _OSINT_ORIG_PS1
    unset -f deactivate_osint_venv
    echo "osint-ai venv deactivated."
}

echo ""
echo "  ✓ osint-ai C++ virtual environment activated"
echo "    Venv : $VENV_DIR"
echo "    Deps : nlohmann/json · fmt · spdlog · libcurl"
echo ""
echo "  Run './build.sh' to compile the project."
echo "  Run 'deactivate_osint_venv' to exit the venv."
echo ""
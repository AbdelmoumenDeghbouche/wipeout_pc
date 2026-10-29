#!/usr/bin/env bash
set -euo pipefail
here=$(dirname -- "$0")
source "$here/scripts/environment.sh"
wipeout_oc_anchor_config
cd -- "$here"
if [[ ${1:-} == --software ]]; then
    shift
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then
        for candidate in /run/opengl-driver/share/vulkan/icd.d/lvp_icd*.json /usr/share/vulkan/icd.d/lvp_icd*.json; do
            if [[ -f $candidate ]]; then export VK_DRIVER_FILES=$candidate; break; fi
        done
    fi
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then echo 'Lavapipe not found; set VK_DRIVER_FILES.' >&2; exit 1; fi
    export VK_LOADER_LAYERS_DISABLE='~implicit~'
fi
# WIPEOUT_OC_PREBUILT=1 (packaged builds, the AppImage): bin/gow3-probe and its GPU library are installed
# next to this script; nothing is built and no nix-shell is needed.
# WIPEOUT_OC_DATA_DIR: writable directory for generated files (out/), saves (user/) and wipeout_oc.ini;
# by default this directory. The preparation itself is in run.py.
data=${WIPEOUT_OC_DATA_DIR:-.}
if [[ -z ${WIPEOUT_OC_PREBUILT:-} && -z ${WIPEOUT_OC_IN_NIX_SHELL:-} ]] && ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3; } && command -v nix-shell >/dev/null; then
    args=''; if (( $# )); then args=$(printf '%q ' "$@"); fi
    exec env WIPEOUT_OC_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash run.sh $args"
fi
if [[ -z ${PYTHON:-} ]]; then
    PYTHON=$(command -v python3 || true)
    if [[ -z $PYTHON ]]; then
        for candidate in /nix/store/*-python3-*/bin/python3; do
            if [[ -x $candidate ]]; then PYTHON=$candidate; break; fi
        done
    fi
fi
if [[ -z ${PYTHON:-} ]]; then echo 'Install Python 3 or set PYTHON.' >&2; exit 1; fi
# FSR 4: faster post passes next to the downloaded ones (incremental; tools/fsr4_optimize.sh).
if [[ -z ${WIPEOUT_OC_PREBUILT:-} && -d fsr4_shaders ]] && command -v spirv-cross >/dev/null; then
    bash tools/fsr4_optimize.sh || echo 'FSR 4: optimized post passes not built' >&2
fi
if [[ -z ${WIPEOUT_OC_PREBUILT:-} ]]; then bash build.sh; fi
export WIPEOUT_OC_DATA_DIR=$data
exec "$PYTHON" run.py "$@"

#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
target=${1:-gpu/third_party/fsr-vulkan}
environment_patch=$PWD/gpu/patches/fsr-vulkan/0004-port-environment-compatibility.patch
for patch in "$PWD"/gpu/patches/fsr-vulkan/*.patch; do
    # 0004 updates lines added by 0001, so its reverse check proves that predecessor is present.
    if [[ ${patch##*/} == 0001-* ]] && git -C "$target" apply --reverse --check "$environment_patch" 2>/dev/null; then
        continue
    fi
    if ! git -C "$target" apply --reverse --check "$patch" 2>/dev/null; then
        git -C "$target" apply "$patch"
    fi
done

#!/usr/bin/env bash
# Builds gow3_dlss.dll (gpu/dlss_bridge, the only code that uses the NVIDIA DLSS SDK) with
# MSVC next to out/gow3-probe.exe; package.sh ships it. NVIDIA's nvngx_dlss.dll is copied too when
# the SDK checkout has it (for local tests); it is never packaged: the launcher downloads it.
# Needs Visual Studio 2022 or 2026 (Build Tools) and a checkout of https://github.com/NVIDIA/DLSS:
#   DLSS_SDK_ROOT=F:/sdk/DLSS bash packaging/windows/build_dlss.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
sdk=${DLSS_SDK_ROOT:?Set DLSS_SDK_ROOT to a checkout of github.com/NVIDIA/DLSS}
cmake=${CMAKE_EXE:-cmake}
# Only the Vulkan headers: MSYS2's C headers must not reach MSVC.
rm -rf out/vkinclude && mkdir -p out/vkinclude
cp -r /c/msys64/clang64/include/vulkan /c/msys64/clang64/include/vk_video out/vkinclude/
# The newest Visual Studio with the C++ tools (2022 or 2026); DLSS_GENERATOR overrides it.
vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
generator=${DLSS_GENERATOR:-Visual Studio 17 2022}
if [[ -z ${DLSS_GENERATOR:-} && -x $vswhere ]] && "$vswhere" -latest -products '*' \
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion | grep -q '^18\.'; then
    generator='Visual Studio 18 2026'
fi
rm -rf out/dlss-bridge
"$cmake" -S gpu/dlss_bridge -B out/dlss-bridge -G "$generator" -A x64 \
    -DDLSS_SDK_ROOT="$(cygpath -m "$sdk")" -DVULKAN_INCLUDE="$(cygpath -m "$PWD/out/vkinclude")"
"$cmake" --build out/dlss-bridge --config Release
cp out/dlss-bridge/Release/gow3_dlss.dll out/
cp "$sdk/LICENSE.txt" out/NVIDIA-DLSS-LICENSE.txt
runtime=$sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll
[[ -f $runtime ]] && cp "$runtime" out/
echo "DLSS bridge ready: out/gow3_dlss.dll"

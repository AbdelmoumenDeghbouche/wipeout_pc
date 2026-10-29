#!/usr/bin/env bash
# Builds dist/wipeout-x86_64.AppImage: the port's current build (run build.sh first),
# its scripts and the launcher, bundled with their Nix closure (nix-appimage: the AppImage mounts
# its /nix/store with user namespaces, available on SteamOS and most desktops).
# Running it opens the launcher; `--play` starts the game with the launcher's saved settings.
# Data (generated files, saves, gow3.ini): ~/.local/share/gow3 (WIPEOUT_OC_DATA_DIR).
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/../scripts/environment.sh"
cd -- "$(dirname -- "$0")/.."
[[ -f out/gow3-probe && -f out/gpu/libgow3gpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
root=$PWD
package_fsr411=false
if [[ ${WIPEOUT_OC_PACKAGE_FSR411:-} == 1 ]]; then package_fsr411=true; fi
# The libraries' store paths (RUNPATH entries and their closures come along).
{
    echo '['
    for elf in out/gow3-probe out/gpu/libgow3gpu.so; do
        readelf -d "$elf" | sed -n 's/.*\[\(.*\)\]/\1/p' | tr ':' '\n'
    done | grep -o '^/nix/store/[^/]*' | sort -u | grep -v -- '-nix-shell$' | sed 's/.*/  "&"/'
    echo ']'
} > packaging/runtime-paths.nix
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
(cd "$work" && nix bundle --impure --bundler github:ralismark/nix-appimage \
    --expr "import $root/packaging { packageFsr411 = $package_fsr411; }")
mkdir -p dist
install -m755 "$work/gow3.AppImage" dist/wipeout-x86_64.AppImage
ls -lh dist/wipeout-x86_64.AppImage

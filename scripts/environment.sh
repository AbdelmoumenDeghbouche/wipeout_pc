# Source before reading port options. Presence, including an empty value, selects the new name.
wipeout_oc_normalize_environment() {
    local name current marker
    local -a derived_names=()
    marker=${WIPEOUT_OC_DERIVED-${GOW3_DERIVED-}}
    unset WIPEOUT_OC_DERIVED GOW3_DERIVED
    IFS=',' read -r -a derived_names <<< "$marker"
    for name in "${derived_names[@]}"; do
        if [[ $name =~ ^(GOW3_|WIPEOUT_OC_)[A-Za-z0-9_]+$ ]]; then
            unset "$name"
        fi
    done
    while IFS= read -r name; do
        current="WIPEOUT_OC_${name#GOW3_}"
        if [[ ! -v $current ]]; then
            printf -v "$current" '%s' "${!name}"
            export "$current"
        fi
        unset "$name"
    done < <(compgen -A variable GOW3_)
}
wipeout_oc_normalize_environment
unset -f wipeout_oc_normalize_environment

# Linux wrappers must capture an explicit caller path before entering the port directory.
wipeout_oc_anchor_config() {
    if [[ -n ${WIPEOUT_OC_CONFIG-} && $WIPEOUT_OC_CONFIG != /* ]]; then
        export WIPEOUT_OC_CONFIG=$PWD/$WIPEOUT_OC_CONFIG
    fi
}

# Packaged Linux entry points reuse an existing directory in place; no saves are moved.
wipeout_oc_select_data_directory() {
    if [[ ! -v WIPEOUT_OC_DATA_DIR ]]; then
        local base=${XDG_DATA_HOME:-$HOME/.local/share}
        WIPEOUT_OC_DATA_DIR=$base/wipeout_oc
        if [[ ! -d $WIPEOUT_OC_DATA_DIR && -d $base/gow3 ]]; then
            WIPEOUT_OC_DATA_DIR=$base/gow3
        fi
    fi
    export WIPEOUT_OC_DATA_DIR
}

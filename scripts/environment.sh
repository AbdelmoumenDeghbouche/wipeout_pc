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

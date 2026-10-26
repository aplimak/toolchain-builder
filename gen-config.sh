#!/usr/bin/env bash
set -euo pipefail

if [ $# = 0 ]; then
    echo "usage: $0 board" >&2
    exit 1
fi

board=$1
config_index="$(realpath "configs/$board.config" 2>/dev/null || true)"
if [ -z "$config_index" ] || [ ! -f "$config_index" ]; then
    echo "board not found: $board" >&2
    exit 1
fi

mkdir -p .build
tmp_config="$(realpath ".build/$board")"

declare -A final_val=() has_app=()
declare -a has_app_order=() # first-seen order of += vars
declare -a out_lines=() out_var=()

out_lines=("BR2_TOOLCHAIN_BUILDROOT_VENDOR=\"$board\"")
out_var=(BR2_TOOLCHAIN_BUILDROOT_VENDOR)
final_val[BR2_TOOLCHAIN_BUILDROOT_VENDOR]="$board"

# Tracks files currently being processed (cycle detection).
declare -A in_progress=()

# Expand a path/glob spec and process every match.
# Resolves relative to $base first; if that finds nothing and $base != configs,
# falls back to configs/. Absolute paths are used as-is.
expand_and_process() {
    local spec="$1" base="$2"
    local -a matches=()
    local g

    shopt -s nullglob
    if [[ "$spec" == /* ]]; then
        for g in $spec; do matches+=("$g"); done
    else
        for g in "$base"/$spec; do matches+=("$g"); done
        if [ ${#matches[@]} -eq 0 ] && [ "$base" != "configs" ]; then
            for g in configs/$spec; do matches+=("$g"); done
        fi
    fi
    shopt -u nullglob

    if [ ${#matches[@]} -eq 0 ]; then
        echo "error: no files match '$spec' (relative to $base)" >&2
        exit 1
    fi
    for g in "${matches[@]}"; do
        process_file "$g"
    done
}

process_file() {
    local file="$1" real dir
    real="$(realpath "$file" 2>/dev/null || true)"
    if [ -z "$real" ] || [ ! -f "$real" ]; then
        echo "error: config file not found: $file" >&2
        exit 1
    fi
    if [ -n "${in_progress[$real]:-}" ]; then
        echo "error: include cycle detected at $real" >&2
        exit 1
    fi
    if [[ "$real" == *.config || "$real" == *.cfg ]]; then
        echo "error: $real looks like an index, not a fragment" >&2
        exit 1
    fi

    in_progress[$real]=1
    dir="$(dirname "$real")"

    local line v x prev spec
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"

        # ---- @FILE / @glob : include directive -------------------------
        if [[ "$line" =~ ^@(.+)$ ]]; then
            spec="${BASH_REMATCH[1]}"
            spec="${spec#"${spec%%[![:space:]]*}"}" # ltrim
            spec="${spec%"${spec##*[![:space:]]}"}" # rtrim
            expand_and_process "$spec" "$dir"
            continue
        fi

        # ---- VAR+=value : accumulate, never buffered -------------------
        if [[ "$line" =~ ^([A-Za-z0-9_]+)\+=(.*)$ ]]; then
            v="${BASH_REMATCH[1]}"
            x="${BASH_REMATCH[2]%\"}"
            x="${x#\"}"
            if [ -z "${has_app[$v]:-}" ]; then
                has_app_order+=("$v")
            fi
            has_app[$v]=1
            prev="${final_val[$v]:-}"
            final_val[$v]="${prev:+$prev }$x"
            continue
        fi

        out_lines+=("$line")

        # ---- VAR=value : remember last value, may be suppressed --------
        if [[ "$line" =~ ^([A-Za-z0-9_]+)=(.*)$ ]]; then
            v="${BASH_REMATCH[1]}"
            x="${BASH_REMATCH[2]%\"}"
            x="${x#\"}"
            out_var+=("$v")
            final_val[$v]="$x"
        else
            out_var+=("") # comment / blank / free-form
        fi
    done <"$real"

    unset 'in_progress[$real]'
}

read_index() {
    local idx="$1" real dir spec line
    real="$(realpath "$idx" 2>/dev/null || true)"
    if [ -z "$real" ] || [ ! -f "$real" ]; then
        echo "error: index file not found: $idx" >&2
        exit 1
    fi
    if [ -n "${in_progress[$real]:-}" ]; then
        echo "error: include cycle detected at $real" >&2
        exit 1
    fi
    if [[ "$real" == *.fragment ]]; then
        echo "error: $real looks like a fragment, not a config" >&2
        exit 1
    fi

    in_progress[$real]=1
    dir="$(dirname "$real")"

    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [ -z "$line" ] && continue
        [[ "$line" == \#* ]] && continue

        # @FILE / @glob : include another index file
        if [[ "$line" == @* ]]; then
            spec="${line#@}"
            spec="${spec#"${spec%%[![:space:]]*}"}"
            spec="${spec%"${spec##*[![:space:]]}"}"
            local -a m=() g
            shopt -s nullglob
            if [[ "$spec" == /* ]]; then
                for g in $spec; do m+=("$g"); done
            else
                for g in "$dir"/$spec; do m+=("$g"); done
                if [ ${#m[@]} -eq 0 ] && [ "$dir" != "$(realpath configs)" ]; then
                    for g in configs/$spec; do m+=("$g"); done
                fi
            fi
            shopt -u nullglob
            if [ ${#m[@]} -eq 0 ]; then
                echo "error: no index files match '$spec' (from $real)" >&2
                exit 1
            fi
            for g in "${m[@]}"; do read_index "$g"; done
            continue
        fi

        expand_and_process "$line" "configs"
    done <"$real"

    unset 'in_progress[$real]'
}

# Always load common.fragment first (as the original did).
process_file "configs/core/common.fragment"

read_index "$config_index"

{
    for i in "${!out_lines[@]}"; do
        v="${out_var[$i]}"
        if [ -z "$v" ] || [ -z "${has_app[$v]:-}" ]; then
            printf '%s\n' "${out_lines[$i]}"
        fi
    done

    for v in "${has_app_order[@]}"; do
        printf '%s="%s"\n' "$v" "${final_val[$v]}"
    done
} >"$tmp_config"

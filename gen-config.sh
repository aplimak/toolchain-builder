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
configs_root="$(realpath configs 2>/dev/null || true)"
[ -n "$configs_root" ] || {
    echo "error: cannot resolve configs/" >&2
    exit 1
}

declare -A final_val=() has_app=()
declare -a has_app_order=() # first-seen order of += vars
declare -a out_lines=() out_var=()

out_lines=("BR2_TOOLCHAIN_BUILDROOT_VENDOR=\"$board\"")
out_var=(BR2_TOOLCHAIN_BUILDROOT_VENDOR)
final_val[BR2_TOOLCHAIN_BUILDROOT_VENDOR]="$board"

declare -A in_progress=()   # realpath -> 1 while on the include stack
declare -a include_stack=() # ancestry: entries are "file:line"

die() {
    # die CALLER MSG
    # CALLER: "file:line" for a specific site, "" for top-level
    local caller="$1" msg="$2" i
    if [ -n "$caller" ]; then
        printf 'error: %s: %s\n' "$caller" "$msg" >&2
    else
        printf 'error: %s\n' "$msg" >&2
    fi
    for ((i = ${#include_stack[@]} - 1; i >= 0; i--)); do
        echo "  included from ${include_stack[$i]}" >&2
    done
    exit 1
}

# resolve_glob SPEC BASE OUT_NAMEREF
# Expands SPEC relative to BASE. Returns 0 if >=1 match.
resolve_glob() {
    local spec="$1" base="$2"
    local -n _rg_out="$3"
    _rg_out=()
    local g
    shopt -s nullglob
    if [[ "$spec" == /* ]]; then
        for g in $spec; do _rg_out+=("$g"); done
    else
        for g in "$base"/$spec; do _rg_out+=("$g"); done
    fi
    shopt -u nullglob
    [ ${#_rg_out[@]} -gt 0 ]
}

expand_and_process() {
    # expand_and_process SPEC BASE CALLER
    local spec="$1" base="$2" caller="${3:-}"
    local -a matches=()
    resolve_glob "$spec" "$base" matches ||
        die "$caller" "no files match '$spec' (base: $base)"
    local g
    for g in "${matches[@]}"; do
        include_stack+=("$caller")
        process_file "$g" "$caller"
        unset 'include_stack[-1]'
    done
}

process_file() {
    # process_file FILE [CALLER]
    local file="$1" caller="${2:-}"
    local real dir
    real="$(realpath "$file" 2>/dev/null || true)"
    if [ -z "$real" ] || [ ! -e "$real" ]; then
        die "$caller" "config file not found: $file"
    elif [ -d "$real" ]; then
        die "$caller" "expected a file, got a directory: $real"
    elif [ ! -f "$real" ]; then
        die "$caller" "not a regular file: $real"
    fi
    [ -z "${in_progress[$real]:-}" ] ||
        die "$caller" "include cycle detected at $real"
    if [[ "$real" == *.config || "$real" == *.cfg ]]; then
        die "$caller" "$real looks like an index, not a fragment"
    fi
    in_progress[$real]=1
    dir="$(dirname "$real")"

    local line v x prev spec lineno=0
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno + 1))
        line="${line%$'\r'}"

        # @FILE / @glob
        if [[ "$line" =~ ^@(.+)$ ]]; then
            spec="${BASH_REMATCH[1]}"
            spec="${spec#"${spec%%[![:space:]]*}"}"
            spec="${spec%"${spec##*[![:space:]]}"}"
            [ -n "$spec" ] || die "$real:$lineno" "empty @ include"
            expand_and_process "$spec" "$dir" "$real:$lineno"
            continue
        fi

        # VAR+=value : accumulate, never buffered
        if [[ "$line" =~ ^([A-Za-z_][A-Za-z0-9_]*)\+=(.*)$ ]]; then
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

        # Blank line or comment (#..., including "# BR2_FOO is not set")
        if [[ "$line" =~ ^[[:space:]]*$ ]] || [[ "$line" == \#* ]]; then
            out_lines+=("$line")
            out_var+=("")
            continue
        fi

        # VAR=value : buffered verbatim; may be suppressed at emit time
        if [[ "$line" =~ ^([A-Za-z_][A-Za-z0-9_]*)=(.*)$ ]]; then
            v="${BASH_REMATCH[1]}"
            x="${BASH_REMATCH[2]%\"}"
            x="${x#\"}"
            out_lines+=("$line")
            out_var+=("$v")
            final_val[$v]="$x"
            continue
        fi

        # Anything else is malformed.
        die "$real:$lineno" "invalid fragment line: '$line' (expected VAR=value, VAR+=value, @include, blank, or #comment)"
    done <"$real"

    unset 'in_progress[$real]'
}

read_index() {
    # read_index FILE [CALLER]
    local idx="$1" caller="${2:-}"
    local real dir spec line lineno=0
    real="$(realpath "$idx" 2>/dev/null || true)"
    if [ -z "$real" ] || [ ! -e "$real" ]; then
        die "$caller" "index file not found: $idx"
    elif [ -d "$real" ]; then
        die "$caller" "expected a file, got a directory: $real"
    elif [ ! -f "$real" ]; then
        die "$caller" "not a regular file: $real"
    fi
    [ -z "${in_progress[$real]:-}" ] ||
        die "$caller" "include cycle detected at $real"
    if [[ "$real" == *.fragment ]]; then
        die "$caller" "$real looks like a fragment, not an index"
    fi
    in_progress[$real]=1
    dir="$(dirname "$real")"

    local -a m=()
    local g
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno + 1))
        line="${line%$'\r'}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [ -z "$line" ] && continue
        [[ "$line" == \#* ]] && continue

        # @FILE / @glob : include another index file
        if [[ "$line" =~ ^@(.+)$ ]]; then
            spec="${BASH_REMATCH[1]}"
            spec="${spec#"${spec%%[![:space:]]*}"}"
            spec="${spec%"${spec##*[![:space:]]}"}"
            [ -n "$spec" ] || die "$real:$lineno" "empty @ include"
            m=()
            resolve_glob "$spec" "$dir" m ||
                die "$real:$lineno" "no index files match '$spec'"
            for g in "${m[@]}"; do
                include_stack+=("$real:$lineno")
                read_index "$g" "$real:$lineno"
                unset 'include_stack[-1]'
            done
            continue
        fi

        # Guard against the mix-up: VAR=value in an index file.
        if [[ "$line" =~ ^[A-Za-z_][A-Za-z0-9_]*\+?= ]]; then
            die "$real:$lineno" "'$line' is a fragment assignment, not a fragment path"
        fi

        expand_and_process "$line" "$dir" "$real:$lineno"
    done <"$real"

    unset 'in_progress[$real]'
}

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
        printf '%s="%s"\n' "$v" "${final_val[$v]:-}"
    done
} >"$tmp_config"

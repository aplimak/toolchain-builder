#!/usr/bin/env bash
set -eu

if [ $# = 0 ]; then
    echo "usage: $0 board"
    exit 1
fi

board=$1
config_index="$(realpath "configs/$board.config")"

if [ ! -f "$config_index" ]; then
    echo "board not found: $board"
    exit 1
fi

mkdir -p .build
tmp_config="$(realpath ".build/$board")"

fragments=(configs/common.fragment)
while read -r cfg; do
    [ -n "$cfg" ] && fragments+=("configs/$cfg")
done <"$config_index"

# final value for any var touched by '=' (last wins) or '+=' (accumulate)
declare -A final_val=() has_app=()

# buffered output, with a parallel array of "which var owns this line" ("" = free-form line)
declare -a out_lines=() out_var=()

out_lines=("BR2_TOOLCHAIN_BUILDROOT_VENDOR=\"$board\"")
out_var=(BR2_TOOLCHAIN_BUILDROOT_VENDOR)
final_val[BR2_TOOLCHAIN_BUILDROOT_VENDOR]="$board"

for f in "${fragments[@]}"; do
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"

        # VAR+=value: accumulate, never buffer
        if [[ "$line" =~ ^([A-Za-z0-9_]+)\+=(.*)$ ]]; then
            v="${BASH_REMATCH[1]}"
            x="${BASH_REMATCH[2]%\"}"
            x="${x#\"}"
            has_app[$v]=1
            final_val[$v]="${final_val[$v]:+${final_val[$v]} }$x"
            continue
        fi

        out_lines+=("$line")

        # VAR=value: remember last value; may be suppressed at emit time
        if [[ "$line" =~ ^([A-Za-z0-9_]+)=(.*)$ ]]; then
            v="${BASH_REMATCH[1]}"
            x="${BASH_REMATCH[2]%\"}"
            x="${x#\"}"
            out_var+=("$v")
            final_val[$v]="$x"
        else
            out_var+=("") # comment / blank / anything else
        fi
    done <"$f"
done

{
    for i in "${!out_lines[@]}"; do
        v="${out_var[$i]}"
        # keep the line if it isn't an assignment to a var that ever used '+='
        if [ -z "$v" ] || [ -z "${has_app[$v]+x}" ]; then
            printf '%s\n' "${out_lines[$i]}"
        fi
    done

    for v in "${!has_app[@]}"; do
        printf '%s="%s"\n' "$v" "${final_val[$v]}"
    done
} >"$tmp_config"

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
tmmpconfig_path="$(realpath ".build/$board")"
echo BR2_TOOLCHAIN_BUILDROOT_VENDOR=\""$board"\" >"$tmmpconfig_path"

cd configs
cat common.fragment >>"$tmmpconfig_path"
while read -r config; do
    cat "$config" >>"$tmmpconfig_path"
done <"$config_index"

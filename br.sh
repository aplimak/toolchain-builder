#! /usr/bin/env bash

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

shift

if [ ! -d "output/$board" ]; then
    make --no-print-directory "$board"
fi

out_path=$(realpath "output/$board")

make --no-print-directory -C "$out_path" "$@"

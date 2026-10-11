#! /usr/bin/env bash

set -eu

if [ $# = 0 ]; then
    echo "usage: $0 config [make args]..."
    exit 1
fi

config=$1
shift
if [ ! -f "configs/$config"_defconfig ]; then
    echo "config not found: $config"
    exit 1
fi

if [ ! -d "output/$config" ]; then
    make "$config"_defconfig
fi

out_path=$(realpath "output/$config")

make -C buildroot O="$out_path" "$@"

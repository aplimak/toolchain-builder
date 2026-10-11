#! /usr/bin/env bash

set -eu

for config in configs/*_defconfig; do
    config=$(basename "$config")
    config=${config//_defconfig/}
    make "$config"_defconfig
    make -C buildroot O="$(realpath "output/$config")" olddefconfig
    make -C buildroot O="$(realpath "output/$config")" savedefconfig
done

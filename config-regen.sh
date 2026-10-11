#! /usr/bin/env bash

set -eu

for config in configs/*_defconfig; do
    config=$(basename "$config")
    config=${config//_defconfig/}
    ./br.sh "$config" olddefconfig
    ./br.sh "$config" savedefconfig
done

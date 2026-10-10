#!/usr/bin/env bash

set -e

cd "$1"

if [ ! -f bin/ldd ]; then
    ln -sf /lib/libc.so bin/ldd
fi

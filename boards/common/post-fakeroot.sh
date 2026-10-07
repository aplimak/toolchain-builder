#!/usr/bin/env bash

set -e

cd "$1"

if [ -L etc/resolv.conf ]; then
    rm -f etc/resolv.conf
fi

cat >etc/resolv.conf <<EOF
nameserver 1.1.1.1
nameserver 8.8.8.8
EOF

if [ -L etc/dropbear ] || [ ! -d etc/dropbear ]; then
    rm -f etc/dropbear
    mkdir etc/dropbear
    chmod 700 etc/dropbear
fi

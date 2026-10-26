#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>
#
# Checks meson.build the two ways a Meson project takes uPCIe: installed, found
# through upcie.pc, and fetched by a wrap-git of this checkout's HEAD. Each
# builds tests/meson/consumer.c as a consumer; the wrapped build must not build
# uPCIe's own example and tests, must install uPCIe's headers and upcie.pc by
# default, and must install nothing with skip-install=true. Run from the
# repository's top.
set -eu

top=$PWD
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

consumer() {
	mkdir -p "$tmp/$1"
	cp tests/meson/meson.build tests/meson/consumer.c "$tmp/$1/"
}

meson setup --prefix="$tmp/usr" "$tmp/b" > /dev/null
meson install -C "$tmp/b" > /dev/null
pc=$(find "$tmp/usr" -name upcie.pc)
test -n "$pc"
test -f "$tmp/usr/include/upcie/upcie.h"
test -f "$tmp/usr/include/upcie/nvme/nvme_qpair.h"
if grep -q '^Libs:.*-l' "$pc"; then echo "upcie.pc links a library" >&2; exit 1; fi
consumer installed
PKG_CONFIG_PATH=$(dirname "$pc") meson setup --wrap-mode=nofallback "$tmp/c1" "$tmp/installed" > /dev/null
ninja -C "$tmp/c1" > /dev/null
test -x "$tmp/c1/consumer"

consumer wrapped
mkdir -p "$tmp/wrapped/subprojects"
cat > "$tmp/wrapped/subprojects/upcie.wrap" << WRAP
[wrap-git]
url = $top
revision = $(git rev-parse HEAD)
depth = 1

[provide]
dependency_names = upcie
WRAP
PKG_CONFIG_PATH= meson setup --wrap-mode=forcefallback "$tmp/c2" "$tmp/wrapped" > /dev/null
ninja -C "$tmp/c2" > /dev/null
test -x "$tmp/c2/consumer"
if [ -n "$(find "$tmp/c2/subprojects" -name 'test_*' -o -name upcie_nvme_driver)" ]; then
	echo "a wrapped uPCIe built its own example or tests" >&2
	exit 1
fi
DESTDIR="$tmp/c2-dest" meson install -C "$tmp/c2" > /dev/null
for f in upcie/upcie.h upcie/nvme/nvme_qpair.h upcie.pc; do
	if [ -z "$(find "$tmp/c2-dest" -path "*/$f")" ]; then
		echo "a wrapped uPCIe did not install $f" >&2
		exit 1
	fi
done

meson configure -Dupcie:skip-install=true "$tmp/c2" > /dev/null
DESTDIR="$tmp/c2-noinst" meson install -C "$tmp/c2" > /dev/null
if [ -n "$(find "$tmp/c2-noinst" -path '*upcie*' 2> /dev/null)" ]; then
	echo "a wrapped uPCIe installed with skip-install=true" >&2
	exit 1
fi
echo "meson: installed and wrapped consumers built"

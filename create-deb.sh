#!/bin/sh
set -eu

DESTDIR="$(pwd)/deb" ninja -C build install

mkdir -p deb/DEBIAN
cp debian-control deb/DEBIAN/control

dpkg-deb --build deb evolution-decsync.deb

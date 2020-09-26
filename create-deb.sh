#!/bin/sh
set -eu

mkdir -p deb/DEBIAN
cp debian-control deb/DEBIAN/control

mkdir -p deb/usr/lib/evolution-data-server/addressbook-backends
cp build/src/backends/addressbook/libebookbackenddecsync.so deb/usr/lib/evolution-data-server/addressbook-backends/libebookbackenddecsync.so
mkdir -p deb/usr/lib/evolution-data-server/calendar-backends
cp build/src/backends/calendar/libecalbackenddecsync.so deb/usr/lib/evolution-data-server/calendar-backends/libecalbackenddecsync.so

mkdir -p deb/usr/lib/evolution/modules
cp build/src/modules/book-config/module-book-config-decsync.so deb/usr/lib/evolution/modules/module-book-config-decsync.so
cp build/src/modules/cal-config/module-cal-config-decsync.so deb/usr/lib/evolution/modules/module-cal-config-decsync.so

mkdir -p deb/usr/share/evolution-decsync/sources
cp src/e-source/decsync.source deb/usr/share/evolution-decsync/sources/decsync.source

dpkg-deb --build deb

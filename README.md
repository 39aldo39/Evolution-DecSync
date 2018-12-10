DecSync for Evolution
=====================

DecSync for Evolution is an [Evolution](https://wiki.gnome.org/Apps/Evolution) plugin which synchronizes contacts and calendars using [DecSync](https://github.com/39aldo39/DecSync). To start synchronizing, all you have to do is synchronize the DecSync directory (by default `~/.local/share/decsync`), using for example [Syncthing](https://syncthing.net).

## Build from source

Install dependencies:

### Ubuntu

```
sudo apt install \
	build-essential \
	meson \
	ninja-build \
	valac \
	pkg-config \
	libgee-0.8-dev \
	libjson-glib-dev \
	libedata-book1.2-dev \
	libedata-cal1.2-dev \
	evolution-dev
```

### Arch Linux

```
sudo pacman -S \
	cmake \
	meson \
	ninja \
	vala \
	libgee \
	json-glib \
	evolution-data-server \
	evolution
```

### Build

```
git clone --recursive https://github.com/39aldo39/Evolution-DecSync
cd ./Evolution-DecSync
meson builddir
sudo ninja -C builddir install
# DecSync is now a backend in Evolution for contacts and calendars
```

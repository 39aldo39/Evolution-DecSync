DecSync for Evolution
=====================

DecSync for Evolution is an [Evolution](https://wiki.gnome.org/Apps/Evolution) plugin which synchronizes contacts and calendars using [DecSync](https://github.com/39aldo39/DecSync). To start synchronizing, all you have to do is synchronize the DecSync directory (by default `~/.local/share/decsync`), using for example [Syncthing](https://syncthing.net).

Installation
------------

### Debian/Ubuntu
For Debian/Ubuntu, there is a `.deb` package available at the [releases page](https://github.com/39aldo39/Evolution-DecSync/releases).

### Arch Linux
For Arch Linux, there is a [AUR package](https://aur.archlinux.org/packages/evolution-decsync) available, created by vasket.

Build from source
-----------------

Install dependencies:

### Debian/Ubuntu

```
sudo apt install \
	build-essential \
	git \
	meson \
	ninja-build \
	valac \
	pkg-config \
	libgee-0.8-dev \
	libjson-glib-dev \
	libebook1.2-dev \
	libedata-book1.2-dev \
	libedata-cal2.0-dev \
	evolution-dev
```

### Fedora
```
sudo dnf install \
	git \
	gcc \
	meson \
	vala \
	libgee-devel \
	json-glib-devel \
	evolution-data-server-devel \
	evolution-devel
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
meson build
sudo ninja -C build install
```

### Further steps

Once building is complete, enable syncing with Evolution by:

1. Restart your computer
2. Open Evolution
3. Click `Edit` → `Accounts`
4. In the window that opens, you can use `Add` on the right-hand side to add an `Address Book` or `Calendar`.
5. For type, select `DecSync`; eventually check `Mark as default address book/calendar`. If applicable, select the location of your DecSync folder.
6. A new address book will appear. Right-click it and select `Refresh` to instantly show your contacts/events.

All done!


Donations
---------

### PayPal
[![](https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif)](https://www.paypal.com/cgi-bin/webscr?cmd=_s-xclick&hosted_button_id=4V96AFD3S4TPJ)

### Bitcoin
[`1JWYoV2MZyu8LYYHCur9jUJgGqE98m566z`](bitcoin:1JWYoV2MZyu8LYYHCur9jUJgGqE98m566z)

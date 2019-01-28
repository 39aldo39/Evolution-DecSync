DecSync for Evolution
=====================

DecSync for Evolution is an [Evolution](https://wiki.gnome.org/Apps/Evolution) plugin which synchronizes contacts and calendars using [DecSync](https://github.com/39aldo39/DecSync). To start synchronizing, all you have to do is synchronize the DecSync directory (by default `~/.local/share/decsync`), using for example [Syncthing](https://syncthing.net).

Build from source
-----------------

Install dependencies:

### Ubuntu

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
	libedata-cal1.2-dev \
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
# Restart the computer
# DecSync is now a backend in Evolution for contacts and calendars

### Further steps

Once building is complete, enable syncing with evolution by:

1. Restart your computer
2. Open Evolution
3. Click `edit` -> `accounts`
4. In the window that opens, you can use `add` on the right-hand side to add an `Address Book`.
5. For type, select `DecSync`; eventually check `use as default address book`. If applicable, select the location of your DecSync folder.
6. A new address book will appear. Right-click it and select 'Refresh' to instantly show your contacts.

All done!


Donations
---------

### PayPal
[![](https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif)](https://www.paypal.com/cgi-bin/webscr?cmd=_s-xclick&hosted_button_id=4V96AFD3S4TPJ)

### Bitcoin
[`1JWYoV2MZyu8LYYHCur9jUJgGqE98m566z`](bitcoin:1JWYoV2MZyu8LYYHCur9jUJgGqE98m566z)

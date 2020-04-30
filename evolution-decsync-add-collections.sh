#!/bin/sh
set -eu

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}"
source_dir="$config_dir/evolution/sources"
decsync_dir="${DECSYNC_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/decsync}"
app_id="$(hostname)-Evolution-$(shuf -i 00000-99999 -n1)"
interval=5

OPTIND=1

while getopts "i:" opt; do
  case "$opt" in
    i) interval="$OPTARG";;
    *) exit 1;;
  esac
done

# Add main DecSync source file, if necessary
decsync_source="[Data Source]
DisplayName=DecSync
Enabled=true
Parent=
"
if ! [ -f "$source_dir/decsync.source" ]; then
  echo "$decsync_source" > "$source_dir/decsync.source"
fi

# Add the source file of a calendar, if necessary
add_calendar() {
  collection=$1
  name=$collection

  collection_source="[Data Source]
DisplayName=$name
Enabled=true
Parent=decsync

[DecSync Backend]
DecsyncDir=$decsync_dir
Collection=$collection
AppId=$app_id

[Refresh]
Enabled=true
IntervalMinutes=$interval

[Calendar]
BackendName=decsync
Selected=true

[Offline]
StaySynchronized=true

[Alarms]
IncludeMe=true

[Conflict Search]
IncludeMe=true
"
  if ! [ -f "$source_dir/decsync-calendar-$collection.source" ]; then
    echo "$collection_source" > "$source_dir/decsync-calendar-$collection.source"
    echo "Calendar $collection added"
  else
    echo "Calendar $collection already exists"
  fi
}

# Add the source file of a addressbook, if necessary
add_addressbook() {
  collection=$1
  name=$collection

  collection_source="[Data Source]
DisplayName=$name
Enabled=true
Parent=decsync

[DecSync Backend]
DecsyncDir=$decsync_dir
Collection=$collection
AppId=$app_id

[Refresh]
Enabled=true
IntervalMinutes=$interval

[Contacts Backend]
IncludeMe=true

[Address Book]
BackendName=decsync

[Autocomplete]
IncludeMe=true
"
  if ! [ -f "$source_dir/decsync-addressbook-$collection.source" ]; then
    echo "$collection_source" > "$source_dir/decsync-addressbook-$collection.source"
    echo "Addressbook $collection added"
  else
    echo "Addressbook $collection already exists"
  fi
}

# Add calendar collections
for collection in "$decsync_dir/calendars/"*; do
  collection=$(basename "$collection")
  if [ "$collection" = "*" ]; then
    echo "No calendars found in $decsync_dir"
    break
  fi;
  add_calendar "$collection"
done

# Add addressbook collections
for collection in "$decsync_dir/contacts/"*; do
  collection=$(basename "$collection")
  if [ "$collection" = "*" ]; then
    echo "No addressbooks found in $decsync_dir"
    break
  fi;
  add_addressbook "$collection"
done

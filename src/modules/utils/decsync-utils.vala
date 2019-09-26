/**
 * Evolution-DecSync - decsync-utils.vala
 *
 * Copyright (C) 2018 Aldo Gunsing
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

public string checkDecsyncInfoWrapper(string decsyncDir)
{
	try {
		checkDecsyncInfo(decsyncDir);
		return "";
	} catch (DecsyncError e) {
		return e.message;
	}
}

public string getOwnAppId(bool random = false)
{
	int? id = null;
	if (random) {
		id = Random.int_range(0, 100000);
	}
	return getAppId("Evolution", id);
}

public string[] listDecsyncCollectionsWrapper(string decsyncDir, string syncType)
{
	try {
		return listDecsyncCollections(decsyncDir, syncType).to_array();
	} catch (GLib.Error e) {
		return {};
	}
}

public string? getInfo(string decsyncDir, string syncType, string collection, string key, string? fallback)
{
	try {
		var dir = getDecsyncSubdir(decsyncDir, syncType, collection);
		var jsonDeleted = Decsync.getStoredStaticValue(dir, {"info"}, stringToNode("deleted"));
		var deleted = jsonDeleted != null && jsonDeleted.get_boolean();
		if (deleted) {
			return null;
		}
		var jsonValue = Decsync.getStoredStaticValue(dir, {"info"}, stringToNode(key));
		string? value = null;
		if (jsonValue != null) {
			value = jsonValue.get_string();
		}
		return value ?? fallback;
	} catch (DecsyncError e) {
		return fallback;
	}
}

public string createCollection(string decsyncDir, string syncType, string name)
{
	string? collection = null;
	while (collection == null || File.new_for_path(getDecsyncSubdir(decsyncDir, syncType, collection)).query_exists()) {
		collection = "colID%05d".printf(Random.int_range(0, 100000));
	}
	setInfoEntry(decsyncDir, syncType, collection, "name", name);
	return collection;
}

public void setInfoEntry(string decsyncDir, string syncType, string collection, string keyString, string valueString)
{
	var listeners = new Gee.ArrayList<OnEntryUpdateListener>();
	var key = stringToNode(keyString);
	Json.Node value = stringToNode(valueString);
	try {
		new Decsync<Unit>(getDecsyncSubdir(decsyncDir, syncType, collection), getOwnAppId(), listeners).setEntry({"info"}, key, value);
	} catch (DecsyncError e) {
		Log.e("Could not write info\n" + e.message);
	}
}

public void setDeleteEntry(string decsyncDir, string syncType, string collection, bool deleted)
{
	var listeners = new Gee.ArrayList<OnEntryUpdateListener>();
	var key = stringToNode("deleted");
	Json.Node value = boolToNode(deleted);
	try {
		new Decsync<Unit>(getDecsyncSubdir(decsyncDir, syncType, collection), getOwnAppId(), listeners).setEntry({"info"}, key, value);
	} catch (DecsyncError e) {
		Log.e("Could not write info\n" + e.message);
	}
}

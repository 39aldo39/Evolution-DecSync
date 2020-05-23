/**
 * Evolution-DecSync - backend-decsync-utils.vala
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

public class Extra {

	public void *backend;

	public Extra(void *backend)
	{
		this.backend = backend;
	}
}

public delegate void DeleteCollectionFunc(Extra extra);
public delegate void UpdateNameFunc(Extra extra, string name);
public delegate void UpdateColorFunc(Extra extra, string color);
public delegate void UpdateResourceFunc(string uid, string resource, Extra extra);
public delegate void RemoveResourceFunc(string uid, Extra extra);

class InfoListener : OnSubfileEntryUpdateListener<Extra> {

	private Gee.List<string> m_subfile;
	private DeleteCollectionFunc m_deleteCollection;
	private UpdateNameFunc m_updateName;
	private UpdateColorFunc? m_updateColor;

	public InfoListener(owned DeleteCollectionFunc deleteCollection, owned UpdateNameFunc updateName, owned UpdateColorFunc? updateColor)
	{
		this.m_subfile = toList({"info"});
		this.m_deleteCollection = (owned)deleteCollection;
		this.m_updateName = (owned)updateName;
		this.m_updateColor = (owned)updateColor;
	}

	public override Gee.List<string> subfile()
	{
		return m_subfile;
	}

	public override void onSubfileEntryUpdate(Decsync.Entry entry, Extra extra)
	{
		var info = entry.key.get_string();
		if (info == null) {
			Log.w("Invalid info key " + Json.to_string(entry.key, false));
			return;
		}

		if (info == "deleted") {
			var deleted = entry.value.get_boolean();
			if (deleted) {
				m_deleteCollection(extra);
			}
		} else if (info == "name") {
			var name = entry.value.get_string();
			if (name == null) {
				Log.w("Invalid name " + Json.to_string(entry.value, false));
			}
			m_updateName(extra, name);
		} else if (m_updateColor != null && info == "color") {
			var color = entry.value.get_string();
			if (color == null) {
				Log.w("Invalid color " + Json.to_string(entry.value, false));
				return;
			}
			m_updateColor(extra, color);
		} else {
			Log.w("Unknown info key " + info);
		}
	}
}

class ResourcesListener : OnSubdirEntryUpdateListener<Extra> {

	private Gee.List<string> m_subdir;
	private UpdateResourceFunc m_updateResource;
	private RemoveResourceFunc m_removeResource;

	public ResourcesListener(owned UpdateResourceFunc updateResource, owned RemoveResourceFunc removeResource)
	{
		this.m_subdir = toList({"resources"});
		this.m_updateResource = (owned)updateResource;
		this.m_removeResource = (owned)removeResource;
	}

	public override Gee.List<string> subdir()
	{
		return m_subdir;
	}

	public override void onSubdirEntryUpdate(Gee.List<string> path, Decsync.Entry entry, Extra extra)
	{
		if (path.size != 1) {
			Log.w("Invalid path " + string.joinv("/", path.to_array()));
			return;
		}
		var uid = path[0];
		if (!entry.key.is_null()) {
			Log.w("Invalid entry key " + Json.to_string(entry.key, false));
			return;
		}
		string? resource = null;
		if (!entry.value.is_null()) {
			resource = entry.value.get_string();
			if (resource == null) {
				Log.w("Invalid entry value " + Json.to_string(entry.value, false));
				return;
			}
		}

		if (resource == null) {
			m_removeResource(uid, extra);
		} else {
			m_updateResource(uid, resource, extra);
		}
	}
}

public bool getDecsync(out Decsync<Extra> decsync,
    string decsyncDir, string syncType, string collection, string ownAppId,
    owned DeleteCollectionFunc deleteCollection, owned UpdateNameFunc updateName, owned UpdateColorFunc? updateColor,
    owned UpdateResourceFunc updateResource, owned RemoveResourceFunc removeResource)
{
	var listeners = new Gee.ArrayList<OnEntryUpdateListener>();
	listeners.add(new InfoListener((owned)deleteCollection, (owned)updateName, (owned)updateColor));
	listeners.add(new ResourcesListener((owned)updateResource, (owned)removeResource));
	try {
		decsync = new Decsync<Extra>(getDecsyncSubdir(decsyncDir, syncType, collection), ownAppId, listeners);
		return true;
	} catch (DecsyncError e) {
		return false;
	}
}

public void writeUpdate(Decsync decsync, string uid, string? resource)
{
	string[] path = {"resources", uid};
	var key = new Json.Node(Json.NodeType.NULL);
	var value = stringToNode(resource);
	decsync.setEntry(path, key, value);
}

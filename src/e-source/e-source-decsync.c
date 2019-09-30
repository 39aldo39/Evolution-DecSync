/**
 * Evolution-DecSync - e-source-decsync.c
 *
 * Copyright (C) 2018 Aldo Gunsing
 *
 * This library is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "evolution-decsync-config.h"

#include "e-source-decsync.h"

struct _ESourceDecsyncPrivate {
	gchar *decsync_dir;
	gchar *collection;
	gchar *appid;
};

enum {
	PROP_0,
	PROP_DECSYNC_DIR,
	PROP_COLLECTION,
	PROP_APPID
};

G_DEFINE_TYPE_WITH_CODE (
	ESourceDecsync,
	e_source_decsync,
	E_TYPE_SOURCE_EXTENSION,
	G_ADD_PRIVATE (ESourceDecsync))

static void
source_decsync_set_property (GObject *object,
                          guint property_id,
                          const GValue *value,
                          GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_DECSYNC_DIR:
			e_source_decsync_set_decsync_dir (
				E_SOURCE_DECSYNC (object),
				g_value_get_string (value));
			return;

		case PROP_COLLECTION:
			e_source_decsync_set_collection (
				E_SOURCE_DECSYNC (object),
				g_value_get_string (value));
			return;

		case PROP_APPID:
			e_source_decsync_set_appid (
				E_SOURCE_DECSYNC (object),
				g_value_get_string (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
source_decsync_get_property (GObject *object,
                          guint property_id,
                          GValue *value,
                          GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_DECSYNC_DIR:
			g_value_take_string (
				value,
				e_source_decsync_dup_decsync_dir (
				E_SOURCE_DECSYNC (object)));
			return;

		case PROP_COLLECTION:
			g_value_take_string (
				value,
				e_source_decsync_dup_collection (
				E_SOURCE_DECSYNC (object)));
			return;

		case PROP_APPID:
			g_value_take_string (
				value,
				e_source_decsync_dup_appid (
				E_SOURCE_DECSYNC (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
source_decsync_finalize (GObject *object)
{
	ESourceDecsyncPrivate *priv;

	priv = E_SOURCE_DECSYNC (object)->priv;

	g_free (priv->decsync_dir);
	g_free (priv->collection);
	g_free (priv->appid);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_source_decsync_parent_class)->finalize (object);
}

static void
e_source_decsync_class_init (ESourceDecsyncClass *class)
{
	GObjectClass *object_class;
	ESourceExtensionClass *extension_class;

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = source_decsync_set_property;
	object_class->get_property = source_decsync_get_property;
	object_class->finalize = source_decsync_finalize;

	extension_class = E_SOURCE_EXTENSION_CLASS (class);
	extension_class->name = E_SOURCE_EXTENSION_DECSYNC_BACKEND;

	g_object_class_install_property (
		object_class,
		PROP_DECSYNC_DIR,
		g_param_spec_string (
			"decsync-dir",
			"Decsync Dir",
			"DecSync directory",
			"",
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT |
			E_SOURCE_PARAM_SETTING));

	g_object_class_install_property (
		object_class,
		PROP_COLLECTION,
		g_param_spec_string (
			"collection",
			"Collection",
			"Collection",
			"",
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT |
			E_SOURCE_PARAM_SETTING));

	g_object_class_install_property (
		object_class,
		PROP_APPID,
		g_param_spec_string (
			"app-id",
			"AppId",
			"AppId",
			"",
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT |
			E_SOURCE_PARAM_SETTING));
}

static void
e_source_decsync_init (ESourceDecsync *extension)
{
	extension->priv = e_source_decsync_get_instance_private (extension);
}

const gchar *
e_source_decsync_get_decsync_dir (ESourceDecsync *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_DECSYNC (extension), NULL);

	return extension->priv->decsync_dir;
}

gchar *
e_source_decsync_dup_decsync_dir (ESourceDecsync *extension)
{
	const gchar *protected;
	gchar *duplicate;

	g_return_val_if_fail (E_IS_SOURCE_DECSYNC (extension), NULL);

	e_source_extension_property_lock (E_SOURCE_EXTENSION (extension));

	protected = e_source_decsync_get_decsync_dir (extension);
	duplicate = g_strdup (protected);

	e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));

	return duplicate;
}

void
e_source_decsync_set_decsync_dir (ESourceDecsync *extension, const gchar *decsync_dir)
{
	g_return_if_fail (E_IS_SOURCE_DECSYNC (extension));

	e_source_extension_property_lock (E_SOURCE_EXTENSION (extension));

	if (g_strcmp0 (extension->priv->decsync_dir, decsync_dir) == 0) {
		e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));
		return;
	}

	g_free (extension->priv->decsync_dir);
	extension->priv->decsync_dir = g_strdup (decsync_dir);

	e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));

	g_object_notify (G_OBJECT (extension), "decsync-dir");
}

const gchar *
e_source_decsync_get_collection (ESourceDecsync *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_DECSYNC (extension), NULL);

	return extension->priv->collection;
}

gchar *
e_source_decsync_dup_collection (ESourceDecsync *extension)
{
	const gchar *protected;
	gchar *duplicate;

	g_return_val_if_fail (E_IS_SOURCE_DECSYNC (extension), NULL);

	e_source_extension_property_lock (E_SOURCE_EXTENSION (extension));

	protected = e_source_decsync_get_collection (extension);
	duplicate = g_strdup (protected);

	e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));

	return duplicate;
}

void
e_source_decsync_set_collection (ESourceDecsync *extension, const gchar *collection)
{
	g_return_if_fail (E_IS_SOURCE_DECSYNC (extension));

	e_source_extension_property_lock (E_SOURCE_EXTENSION (extension));

	if (g_strcmp0 (extension->priv->collection, collection) == 0) {
		e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));
		return;
	}

	g_free (extension->priv->collection);
	extension->priv->collection = g_strdup (collection);

	e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));

	g_object_notify (G_OBJECT (extension), "decsync-dir");
}

const gchar *
e_source_decsync_get_appid (ESourceDecsync *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_DECSYNC (extension), NULL);

	return extension->priv->appid;
}

gchar *
e_source_decsync_dup_appid (ESourceDecsync *extension)
{
	const gchar *protected;
	gchar *duplicate;

	g_return_val_if_fail (E_IS_SOURCE_DECSYNC (extension), NULL);

	e_source_extension_property_lock (E_SOURCE_EXTENSION (extension));

	protected = e_source_decsync_get_appid (extension);
	duplicate = g_strdup (protected);

	e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));

	return duplicate;
}

void
e_source_decsync_set_appid (ESourceDecsync *extension, const gchar *appid)
{
	g_return_if_fail (E_IS_SOURCE_DECSYNC (extension));

	e_source_extension_property_lock (E_SOURCE_EXTENSION (extension));

	if (g_strcmp0 (extension->priv->appid, appid) == 0) {
		e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));
		return;
	}

	g_free (extension->priv->appid);
	extension->priv->appid = g_strdup (appid);

	e_source_extension_property_unlock (E_SOURCE_EXTENSION (extension));

	g_object_notify (G_OBJECT (extension), "app-id");
}

/**
 * Evolution-DecSync - decsync.c
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

#include "decsync.h"
#include <decsync-utils.h>
#include <libdecsync.h>

typedef struct _Context Context;

struct _Context {
	ESourceConfigBackend *backend;		/* not referenced */
	ESource *scratch_source;		/* not referenced */
	gchar *orig_color;
	const gchar *sync_type;		/* not referenced */
	const gchar *sync_type_title;		/* not referenced */
	GtkFileChooser *decsync_dir_chooser;
	GtkComboBoxText *collection_combo_box;
	GtkButton *collection_rename_button;
	GtkButton *collection_delete_button;
};

static void
config_decsync_context_free (Context *context)
{
	g_free (context->orig_color);
	g_object_unref (context->decsync_dir_chooser);
	g_object_unref (context->collection_combo_box);
	g_object_unref (context->collection_rename_button);
	g_object_unref (context->collection_delete_button);

	g_slice_free (Context, context);
}

static void
config_decsync_update_color (Context *context)
{
	ESourceExtension *extension;
	const gchar *extension_name, *decsync_dir, *collection;
	gchar *color;

	extension_name = E_SOURCE_EXTENSION_CALENDAR;
	if (!e_source_has_extension (context->scratch_source, extension_name))
		return;

	extension_name = E_SOURCE_EXTENSION_DECSYNC_BACKEND;
	extension = e_source_get_extension (context->scratch_source, extension_name);
	decsync_dir = e_source_decsync_get_decsync_dir (E_SOURCE_DECSYNC (extension));
	collection = e_source_decsync_get_collection (E_SOURCE_DECSYNC (extension));

	if (decsync_dir != NULL && *decsync_dir != '\0' && collection != NULL && *collection != '\0')
		color = getInfo (decsync_dir, context->sync_type, collection, "color", context->orig_color);
	else
		color = g_strdup (context->orig_color);

	extension_name = E_SOURCE_EXTENSION_CALENDAR;
	extension = e_source_get_extension (context->scratch_source, extension_name);
	e_source_selectable_set_color (E_SOURCE_SELECTABLE (extension), color);
	g_free (color);
}

static void
config_decsync_update_combo_box (Context *context)
{
	ESourceExtension *extension;
	const gchar *extension_name, *decsync_dir, *collection;
	gchar **collections;
	gchar *name;
	int ii, length;

	extension_name = E_SOURCE_EXTENSION_DECSYNC_BACKEND;
	extension = e_source_get_extension (context->scratch_source, extension_name);
	decsync_dir = e_source_decsync_get_decsync_dir (E_SOURCE_DECSYNC (extension));

	gtk_combo_box_text_remove_all (context->collection_combo_box);

	if (decsync_dir != NULL && *decsync_dir != '\0') {
		collections = listDecsyncCollectionsWrapper (decsync_dir, context->sync_type, &length);
		for (ii = 0; ii < length; ii++) {
			name = getInfo (decsync_dir, context->sync_type, collections[ii], "name", collections[ii]);
			if (name != NULL && *name != '\0')
				gtk_combo_box_text_append (context->collection_combo_box, collections[ii], name);
			g_free (name);
			g_free (collections[ii]);
		}
		gtk_combo_box_text_append (context->collection_combo_box, "", _("New..."));
	}

	collection = e_source_decsync_get_collection (E_SOURCE_DECSYNC (extension));
	if (collection != NULL && *collection != '\0')
		gtk_combo_box_set_active_id (GTK_COMBO_BOX (context->collection_combo_box), collection);

	config_decsync_update_color (context);
}

static void
config_decsync_dir_set_cb (GtkFileChooserButton *button, Context *context)
{
	ESourceExtension *extension;
	const gchar *extension_name;
	gchar *decsync_dir;
	GFile *dir;

	extension_name = E_SOURCE_EXTENSION_DECSYNC_BACKEND;
	extension = e_source_get_extension (context->scratch_source, extension_name);
	dir = gtk_file_chooser_get_file (context->decsync_dir_chooser);
	decsync_dir = g_file_get_path (dir);
	e_source_decsync_set_decsync_dir (E_SOURCE_DECSYNC (extension), decsync_dir);
	e_source_decsync_set_collection (E_SOURCE_DECSYNC (extension), NULL);

	config_decsync_update_combo_box (context);

	g_free (decsync_dir);
}

static void
config_decsync_collection_set_cb (GtkComboBox *combo_box, Context *context)
{
	ESourceConfig *config;
	ESourceExtension *extension;
	const gchar *extension_name, *id, *dir, *name;
	gchar *collection, *title;
	GtkWidget *dialog, *container, *widget;
	gpointer parent;

	config = e_source_config_backend_get_config (context->backend);
	extension_name = E_SOURCE_EXTENSION_DECSYNC_BACKEND;
	extension = e_source_get_extension (context->scratch_source, extension_name);

	id = gtk_combo_box_get_active_id (combo_box);
	if (id == NULL) {
		gtk_widget_set_sensitive (GTK_WIDGET (context->collection_rename_button), FALSE);
		gtk_widget_set_sensitive (GTK_WIDGET (context->collection_delete_button), FALSE);
		return;
	} else {
		gtk_widget_set_sensitive (GTK_WIDGET (context->collection_rename_button), TRUE);
		gtk_widget_set_sensitive (GTK_WIDGET (context->collection_delete_button), TRUE);
	}

	if (*id == '\0') {
		parent = gtk_widget_get_toplevel (GTK_WIDGET (config));
		parent = gtk_widget_is_toplevel (parent) ? parent : NULL;
		title = g_strdup_printf (_("Name for new %s"), context->sync_type_title);
		dialog = gtk_dialog_new_with_buttons (title, parent, GTK_DIALOG_DESTROY_WITH_PARENT,
			_("_Cancel"), GTK_RESPONSE_REJECT,
			_("_OK"), GTK_RESPONSE_ACCEPT,
			NULL);
		g_free (title);

		widget = gtk_entry_new ();

		container = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
		gtk_container_add (GTK_CONTAINER (container), widget);
		gtk_widget_show_all (dialog);

		if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
			name = gtk_entry_get_text (GTK_ENTRY (widget));
			if (name != NULL && *name != '\0') {
				dir = e_source_decsync_get_decsync_dir (E_SOURCE_DECSYNC (extension));
				collection = createCollection (dir, context->sync_type, name);
				e_source_decsync_set_collection (E_SOURCE_DECSYNC (extension), collection);
				g_free (collection);
			}
		}
		gtk_widget_destroy (dialog);
		config_decsync_update_combo_box (context);
	} else {
		e_source_decsync_set_collection (E_SOURCE_DECSYNC (extension), id);
		config_decsync_update_color (context);
	}
}

static void
config_decsync_collection_rename_cb (GtkButton *button, Context *context)
{
	ESourceConfig *config;
	ESourceExtension *extension;
	const gchar *extension_name, *dir, *collection, *name_old, *name;
	gchar *title;
	GtkWidget *dialog, *container, *widget;
	gpointer parent;
	gint position;

	config = e_source_config_backend_get_config (context->backend);
	extension_name = E_SOURCE_EXTENSION_DECSYNC_BACKEND;
	extension = e_source_get_extension (context->scratch_source, extension_name);

	dir = e_source_decsync_get_decsync_dir (E_SOURCE_DECSYNC (extension));
	collection = e_source_decsync_get_collection (E_SOURCE_DECSYNC (extension));

	parent = gtk_widget_get_toplevel (GTK_WIDGET (config));
	parent = gtk_widget_is_toplevel (parent) ? parent : NULL;

	title = g_strdup_printf (_("New name for %s"), context->sync_type_title);
	dialog = gtk_dialog_new_with_buttons (title, parent, GTK_DIALOG_DESTROY_WITH_PARENT,
		_("_Cancel"), GTK_RESPONSE_REJECT,
		_("_OK"), GTK_RESPONSE_ACCEPT,
		NULL);
	g_free (title);

	widget = gtk_entry_new ();
	name_old = gtk_combo_box_text_get_active_text (context->collection_combo_box);
	position = gtk_combo_box_get_active (GTK_COMBO_BOX (context->collection_combo_box));
	gtk_entry_set_text (GTK_ENTRY (widget), name_old);

	container = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
	gtk_container_add (GTK_CONTAINER (container), widget);
	gtk_widget_show_all (dialog);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
		name = gtk_entry_get_text (GTK_ENTRY (widget));
		if (name != NULL && *name != '\0' && g_strcmp0 (name, name_old)) {
			dir = e_source_decsync_get_decsync_dir (E_SOURCE_DECSYNC (extension));
			setInfoEntry (dir, context->sync_type, collection, "name", name);
			gtk_combo_box_text_remove (context->collection_combo_box, position);
			gtk_combo_box_text_insert (context->collection_combo_box, position, collection, name);
			gtk_combo_box_set_active_id (GTK_COMBO_BOX (context->collection_combo_box), collection);
		}
	}
	gtk_widget_destroy (dialog);
}

static void
config_decsync_collection_delete_cb (GtkButton *button, Context *context)
{
	ESourceConfig *config;
	ESourceExtension *extension;
	const gchar *extension_name, *dir, *collection, *name;
	gchar *title;
	GtkWidget *dialog;
	gpointer parent;
	gint position;

	config = e_source_config_backend_get_config (context->backend);
	extension_name = E_SOURCE_EXTENSION_DECSYNC_BACKEND;
	extension = e_source_get_extension (context->scratch_source, extension_name);

	dir = e_source_decsync_get_decsync_dir (E_SOURCE_DECSYNC (extension));
	collection = e_source_decsync_get_collection (E_SOURCE_DECSYNC (extension));
	name = gtk_combo_box_text_get_active_text (context->collection_combo_box);

	parent = gtk_widget_get_toplevel (GTK_WIDGET (config));
	parent = gtk_widget_is_toplevel (parent) ? parent : NULL;

	dialog = gtk_message_dialog_new (parent,
		GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_QUESTION,
		GTK_BUTTONS_YES_NO,
		_("Are you sure you want to delete the %s '%s'?"),
		context->sync_type_title,
		name);
	title = g_strdup_printf (_("Delete %s"), context->sync_type_title);
	gtk_window_set_title (GTK_WINDOW (dialog), title);
	g_free (title);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_YES) {
		setDeleteEntry (dir, context->sync_type, collection, TRUE);
		position = gtk_combo_box_get_active (GTK_COMBO_BOX (context->collection_combo_box));
		gtk_combo_box_text_remove (context->collection_combo_box, position);
	}
	gtk_widget_destroy (dialog);
}

void
config_decsync_insert_widgets (const gchar *sync_type, const gchar *sync_type_title, ESourceConfigBackend *backend, ESource *scratch_source)
{
	ESourceConfig *config;
	ESourceExtension *extension;
	GtkWidget *widget, *container;
	Context *context;
	const gchar *extension_name, *uid;
	gchar *decsync_dir, *title;

	uid = e_source_get_uid (scratch_source);
	config = e_source_config_backend_get_config (backend);
	context = g_slice_new (Context);

	context->backend = backend;
	context->scratch_source = scratch_source;
	context->sync_type = sync_type;
	context->sync_type_title = sync_type_title;

	extension_name = E_SOURCE_EXTENSION_CALENDAR;
	if (e_source_has_extension (scratch_source, extension_name)) {
		extension = e_source_get_extension (scratch_source, extension_name);
		context->orig_color = e_source_selectable_dup_color (E_SOURCE_SELECTABLE (extension));
	} else {
		context->orig_color = NULL;
	}

	g_object_set_data_full (
		G_OBJECT (backend), uid, context,
		(GDestroyNotify) config_decsync_context_free);

	widget = gtk_file_chooser_button_new (
		_("Select Directory"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
	gtk_file_chooser_set_create_folders (GTK_FILE_CHOOSER (widget), TRUE);
	gtk_file_chooser_set_show_hidden (GTK_FILE_CHOOSER (widget), TRUE);

	extension_name = E_SOURCE_EXTENSION_DECSYNC_BACKEND;
	extension = e_source_get_extension (scratch_source, extension_name);
	decsync_dir = e_source_decsync_dup_decsync_dir (E_SOURCE_DECSYNC (extension));
	if (decsync_dir == NULL || *decsync_dir == '\0') {
		decsync_dir = getDefaultDecsyncBaseDir();
		e_source_decsync_set_decsync_dir (E_SOURCE_DECSYNC (extension), decsync_dir);
	}
	gtk_file_chooser_set_file (GTK_FILE_CHOOSER (widget), g_file_new_for_path (decsync_dir), NULL);
	g_free (decsync_dir);

	e_source_config_insert_widget (
		config, scratch_source, _("Directory:"), widget);
	context->decsync_dir_chooser = GTK_FILE_CHOOSER (g_object_ref (widget));
	gtk_widget_show (widget);

	g_signal_connect (
		widget, "file-set",
		G_CALLBACK (config_decsync_dir_set_cb),
		context);

	container = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

	widget = gtk_combo_box_text_new ();
	gtk_box_pack_start (GTK_BOX (container), widget, TRUE, TRUE, 0);
	context->collection_combo_box = GTK_COMBO_BOX_TEXT (g_object_ref (widget));
	gtk_widget_show (widget);

	g_signal_connect (
		widget, "changed",
		G_CALLBACK (config_decsync_collection_set_cb),
		context);

	widget = gtk_button_new_with_label (_("Rename"));
	gtk_widget_set_sensitive (widget, FALSE);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	context->collection_rename_button = GTK_BUTTON (g_object_ref (widget));
	gtk_widget_show (widget);

	g_signal_connect (
		widget, "clicked",
		G_CALLBACK (config_decsync_collection_rename_cb),
		context);

	widget = gtk_button_new_with_label (_("Delete"));
	gtk_widget_set_sensitive (widget, FALSE);
	gtk_box_pack_start (GTK_BOX (container), widget, FALSE, FALSE, 0);
	context->collection_delete_button = GTK_BUTTON (g_object_ref (widget));
	gtk_widget_show (widget);

	g_signal_connect (
		widget, "clicked",
		G_CALLBACK (config_decsync_collection_delete_cb),
		context);

	title = g_strdup_printf (_("%s:"), sync_type_title);
	e_source_config_insert_widget (
		config, scratch_source, title, GTK_WIDGET (container));
	gtk_widget_show (GTK_WIDGET (container));
	g_free (title);

	config_decsync_update_combo_box (context);

	e_source_config_add_refresh_interval (config, scratch_source);
}

gboolean
config_decsync_check_complete (ESourceConfigBackend *backend, ESource *scratch_source)
{
	ESourceDecsync *extension;
	Context *context;
	const gchar *uid, *extension_name, *dir, *collection;

	uid = e_source_get_uid (scratch_source);
	context = g_object_get_data (G_OBJECT (backend), uid);

	/* This function might get called before we install a
	 * context for this ESource, in which case just return. */
	if (context == NULL)
		return FALSE;

	extension_name = E_SOURCE_EXTENSION_DECSYNC_BACKEND;
	extension = e_source_get_extension (scratch_source, extension_name);

	dir = e_source_decsync_get_decsync_dir (extension);
	if (dir == NULL)
		return FALSE;

	collection = e_source_decsync_get_collection (extension);
	if (collection == NULL)
		return FALSE;

	return *dir != '\0' && *collection != '\0';
}

void
config_decsync_commit_changes (ESourceConfigBackend *backend, ESource *scratch_source)
{
	ESourceExtension *extension;
	Context *context;
	const gchar *uid, *extension_name, *decsync_dir, *collection, *old_appid, *new_color;
	gchar *new_appid, *old_color;

	uid = e_source_get_uid (scratch_source);
	context = g_object_get_data (G_OBJECT (backend), uid);

	extension_name = E_SOURCE_EXTENSION_DECSYNC_BACKEND;
	extension = e_source_get_extension (scratch_source, extension_name);
	decsync_dir = e_source_decsync_get_decsync_dir (E_SOURCE_DECSYNC (extension));
	collection = e_source_decsync_get_collection (E_SOURCE_DECSYNC (extension));

	old_appid = e_source_decsync_get_appid (E_SOURCE_DECSYNC (extension));
	if (old_appid == NULL || *old_appid == '\0') {
		new_appid = getOwnAppId (true);
		e_source_decsync_set_appid (E_SOURCE_DECSYNC (extension), new_appid);
		g_free (new_appid);
	}

	extension_name = E_SOURCE_EXTENSION_CALENDAR;
	if (e_source_has_extension (scratch_source, extension_name)) {
		extension = e_source_get_extension (scratch_source, extension_name);
		new_color = e_source_selectable_get_color (E_SOURCE_SELECTABLE (extension));
		old_color = getInfo (decsync_dir, context->sync_type, collection, "color", NULL);

		if (g_strcmp0 (new_color, old_color))
			setInfoEntry (decsync_dir, context->sync_type, collection, "color", new_color);

		g_free (old_color);
	}
}

void
config_decsync_add_source_file ()
{
	const gchar *to_dir;
	GFile *from_file, *to_file;

	from_file = g_file_new_build_filename (E_SOURCE_DIR, "decsync.source", NULL);

	to_dir = e_server_side_source_get_user_dir ();
	to_file = g_file_new_build_filename (to_dir, "decsync.source", NULL);

	g_file_copy (from_file, to_file, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL);

	g_object_unref (from_file);
	g_object_unref (to_file);
}

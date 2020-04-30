/* Evolution calendar - iCalendar decsync backend.
 * Based on the iCalendar file backend.
 *
 * Copyright (C) 1993 Free Software Foundation, Inc.
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
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
 *
 * Authors: Federico Mena-Quintero <federico@ximian.com>
 *          Rodrigo Moya <rodrigo@ximian.com>
 *          Jan Brittenson <bson@gnu.ai.mit.edu>
 */

#include "evolution-decsync-config.h"

#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>

#include <libedataserver/libedataserver.h>
#include <e-source/e-source-decsync.h>
#include "backend-decsync-utils.h"

#include "e-cal-backend-decsync-events.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

#define EC_ERROR(_code) e_client_error_create (_code, NULL)
#define EC_ERROR_EX(_code, _msg) e_client_error_create (_code, _msg)
#define EC_ERROR_NO_URI() e_client_error_create (E_CLIENT_ERROR_OTHER_ERROR, _("Cannot get URI"))
#define ECC_ERROR(_code) e_cal_client_error_create (_code, NULL)

#define ECAL_REVISION_X_PROP  "X-EVOLUTION-DATA-REVISION"

/* Placeholder for each component and its recurrences */
typedef struct {
	ECalComponent *full_object;
	GHashTable *recurrences;
	GList *recurrences_list;
} ECalBackendDecsyncObject;

/* Private part of the ECalBackendDecsync structure */
struct _ECalBackendDecsyncPrivate {
	/* path where the calendar data is stored */
	gchar *path;

	/* Filename in the dir */
	gchar *file_name;
	gboolean is_dirty;
	guint dirty_idle_id;

	/* locked in high-level functions to ensure data is consistent
	 * in idle and CORBA thread(s?); because high-level functions
	 * may call other high-level functions the mutex must allow
	 * recursive locking
	 */
	GRecMutex idle_save_rmutex;

	/* Toplevel VCALENDAR component */
	ICalComponent *vcalendar;

	/* All the objects in the calendar, hashed by UID.  The
	 * hash key *is* the uid returned by cal_component_get_uid(); it is not
	 * copied, so don't free it when you remove an object from the hash
	 * table. Each item in the hash table is a ECalBackendDecsyncObject.
	 */
	GHashTable *comp_uid_hash;

	EIntervalTree *interval_tree;

	GList *comp;

	/* guards refresh members */
	GMutex refresh_lock;
	/* set to TRUE to indicate thread should stop */
	gboolean refresh_thread_stop;
	/* condition for refreshing, not NULL when thread exists */
	GCond *refresh_cond;
	/* cond to know the refresh thread gone */
	GCond *refresh_gone_cond;
	/* increased when backend saves the file */
	guint refresh_skip;

	GFileMonitor *refresh_monitor;

	/* Just an incremental number to ensure uniqueness across revisions */
	guint revision_counter;

	Decsync *decsync;

	/* Only for ETimezoneCache::get_timezone() call */
	GHashTable *cached_timezones; /* gchar *tzid -> ICalTimezone * */
};

#define d(x)

static void bump_revision (ECalBackendDecsync *cbfile);

static void	e_cal_backend_decsync_timezone_cache_init
					(ETimezoneCacheInterface *iface);

static ETimezoneCacheInterface *parent_timezone_cache_interface;

static gboolean	ecal_backend_decsync_refresh_start (ECalBackendDecsync *cbfile);
static void	e_cal_backend_decsync_initable_init
						(GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (
	ECalBackendDecsync,
	e_cal_backend_decsync,
	E_TYPE_CAL_BACKEND_SYNC,
	G_ADD_PRIVATE (ECalBackendDecsync)
	G_IMPLEMENT_INTERFACE (
		E_TYPE_TIMEZONE_CACHE,
		e_cal_backend_decsync_timezone_cache_init)
	G_IMPLEMENT_INTERFACE (
		G_TYPE_INITABLE,
		e_cal_backend_decsync_initable_init))

/* g_hash_table_foreach() callback to destroy a ECalBackendDecsyncObject */
static void
free_object_data (gpointer data)
{
	ECalBackendDecsyncObject *obj_data = data;

	if (obj_data->full_object)
		g_object_unref (obj_data->full_object);
	g_hash_table_destroy (obj_data->recurrences);
	g_list_free (obj_data->recurrences_list);

	g_free (obj_data);
}

/* Saves the calendar data */
static gboolean
save_file_when_idle (gpointer user_data)
{
	ECalBackendDecsyncPrivate *priv;
	GError *e = NULL;
	GFile *file, *backup_file;
	GFileOutputStream *stream;
	gboolean succeeded;
	gchar *tmp, *backup_uristr;
	gchar *buf;
	ECalBackendDecsync *cbfile = user_data;
	gboolean writable;

	priv = cbfile->priv;
	g_return_val_if_fail (priv->path != NULL, FALSE);
	g_return_val_if_fail (priv->vcalendar != NULL, FALSE);

	writable = e_cal_backend_get_writable (E_CAL_BACKEND (cbfile));

	g_rec_mutex_lock (&priv->idle_save_rmutex);
	if (!priv->is_dirty || !writable) {
		priv->dirty_idle_id = 0;
		priv->is_dirty = FALSE;
		g_rec_mutex_unlock (&priv->idle_save_rmutex);
		return FALSE;
	}

	file = g_file_new_for_path (priv->path);
	if (!file)
		goto error_malformed_uri;

	/* save calendar to backup file */
	tmp = g_file_get_uri (file);
	if (!tmp) {
		g_object_unref (file);
		goto error_malformed_uri;
	}

	backup_uristr = g_strconcat (tmp, "~", NULL);
	backup_file = g_file_new_for_uri (backup_uristr);

	g_free (tmp);
	g_free (backup_uristr);

	if (!backup_file) {
		g_object_unref (file);
		goto error_malformed_uri;
	}

	priv->refresh_skip++;
	stream = g_file_replace (backup_file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &e);
	if (!stream || e) {
		if (stream)
			g_object_unref (stream);

		g_object_unref (file);
		g_object_unref (backup_file);
		priv->refresh_skip--;
		goto error;
	}

	buf = i_cal_component_as_ical_string (priv->vcalendar);
	succeeded = g_output_stream_write_all (G_OUTPUT_STREAM (stream), buf, strlen (buf) * sizeof (gchar), NULL, NULL, &e);
	g_free (buf);

	if (!succeeded || e) {
		g_object_unref (stream);
		g_object_unref (file);
		g_object_unref (backup_file);
		goto error;
	}

	succeeded = g_output_stream_close (G_OUTPUT_STREAM (stream), NULL, &e);
	g_object_unref (stream);

	if (!succeeded || e) {
		g_object_unref (file);
		g_object_unref (backup_file);
		goto error;
	}

	/* now copy the temporary file to the real file */
	g_file_move (backup_file, file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &e);

	g_object_unref (file);
	g_object_unref (backup_file);
	if (e)
		goto error;

	priv->is_dirty = FALSE;
	priv->dirty_idle_id = 0;

	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	return FALSE;

 error_malformed_uri:
	g_rec_mutex_unlock (&priv->idle_save_rmutex);
	e_cal_backend_notify_error (E_CAL_BACKEND (cbfile),
				  _("Cannot save calendar data: Malformed URI."));
	return FALSE;

 error:
	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	if (e) {
		gchar *msg = g_strdup_printf ("%s: %s", _("Cannot save calendar data"), e->message);

		e_cal_backend_notify_error (E_CAL_BACKEND (cbfile), msg);
		g_free (msg);
		g_error_free (e);
	} else
		e_cal_backend_notify_error (E_CAL_BACKEND (cbfile), _("Cannot save calendar data"));

	return FALSE;
}

static void
save (ECalBackendDecsync *cbfile,
      gboolean do_bump_revision)
{
	ECalBackendDecsyncPrivate *priv;

	if (do_bump_revision)
		bump_revision (cbfile);

	priv = cbfile->priv;

	g_rec_mutex_lock (&priv->idle_save_rmutex);
	priv->is_dirty = TRUE;

	if (!priv->dirty_idle_id)
		priv->dirty_idle_id = g_idle_add ((GSourceFunc) save_file_when_idle, cbfile);

	g_rec_mutex_unlock (&priv->idle_save_rmutex);
}

static void
free_calendar_components (GHashTable *comp_uid_hash,
                          ICalComponent *top_icomp)
{
	if (comp_uid_hash)
		g_hash_table_destroy (comp_uid_hash);

	if (top_icomp)
		g_object_unref (top_icomp);
}

static void
free_calendar_data (ECalBackendDecsync *cbfile)
{
	ECalBackendDecsyncPrivate *priv;

	priv = cbfile->priv;

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	e_intervaltree_destroy (priv->interval_tree);
	priv->interval_tree = NULL;

	free_calendar_components (priv->comp_uid_hash, priv->vcalendar);
	priv->comp_uid_hash = NULL;
	priv->vcalendar = NULL;

	g_list_free (priv->comp);
	priv->comp = NULL;

	g_rec_mutex_unlock (&priv->idle_save_rmutex);
}

/* Dispose handler for the decsync backend */
static void
e_cal_backend_decsync_dispose (GObject *object)
{
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncPrivate *priv;
	ESource *source;

	cbfile = E_CAL_BACKEND_DECSYNC (object);
	priv = cbfile->priv;

	/* Save if necessary */
	if (priv->is_dirty)
		save_file_when_idle (cbfile);

	free_calendar_data (cbfile);

	source = e_backend_get_source (E_BACKEND (cbfile));
	if (source)
		g_signal_handlers_disconnect_matched (source, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, cbfile);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_cal_backend_decsync_parent_class)->dispose (object);
}

/* Finalize handler for the decsync backend */
static void
e_cal_backend_decsync_finalize (GObject *object)
{
	ECalBackendDecsyncPrivate *priv;

	priv = E_CAL_BACKEND_DECSYNC (object)->priv;

	/* Clean up */

	if (priv->dirty_idle_id)
		g_source_remove (priv->dirty_idle_id);

	g_mutex_clear (&priv->refresh_lock);

	g_rec_mutex_clear (&priv->idle_save_rmutex);
	g_hash_table_destroy (priv->cached_timezones);

	g_free (priv->path);
	g_free (priv->file_name);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_cal_backend_decsync_parent_class)->finalize (object);
}

/* Looks up an component by its UID on the backend's component hash table
 * and returns TRUE if any event (regardless whether it is the master or a child)
 * with that UID exists */
static gboolean
uid_in_use (ECalBackendDecsync *cbfile,
            const gchar *uid)
{
	ECalBackendDecsyncPrivate *priv;
	ECalBackendDecsyncObject *obj_data;

	priv = cbfile->priv;

	obj_data = g_hash_table_lookup (priv->comp_uid_hash, uid);
	return obj_data != NULL;
}

static ICalProperty *
get_revision_property (ECalBackendDecsync *cbfile)
{
	if (!cbfile->priv->vcalendar)
		return NULL;

	return e_cal_util_component_find_x_property (cbfile->priv->vcalendar, ECAL_REVISION_X_PROP);
}

static gchar *
make_revision_string (ECalBackendDecsync *cbfile)
{
	GTimeVal timeval;
	gchar   *datestr;
	gchar   *revision;

	g_get_current_time (&timeval);

	datestr = g_time_val_to_iso8601 (&timeval);
	revision = g_strdup_printf ("%s(%d)", datestr, cbfile->priv->revision_counter++);

	g_free (datestr);
	return revision;
}

static ICalProperty *
ensure_revision (ECalBackendDecsync *cbfile)
{
	ICalProperty *prop;

	if (cbfile->priv->vcalendar == NULL)
		return NULL;

	prop = get_revision_property (cbfile);

	if (!prop) {
		gchar *revision = make_revision_string (cbfile);

		e_cal_util_component_set_x_property (cbfile->priv->vcalendar, ECAL_REVISION_X_PROP, revision);

		g_free (revision);

		prop = get_revision_property (cbfile);
		g_warn_if_fail (prop != NULL);
	}

	return prop;
}

static void
bump_revision (ECalBackendDecsync *cbfile)
{
	/* Update the revision string */
	ICalProperty *prop = ensure_revision (cbfile);
	gchar *revision = make_revision_string (cbfile);

	i_cal_property_set_x (prop, revision);

	e_cal_backend_notify_property_changed (E_CAL_BACKEND (cbfile),
					      E_CAL_BACKEND_PROPERTY_REVISION,
					      revision);

	g_object_unref (prop);
	g_free (revision);
}

/* Calendar backend methods */

/* Get_email_address handler for the decsync backend */
static gchar *
e_cal_backend_decsync_get_backend_property (ECalBackend *backend,
                                         const gchar *prop_name)
{
	g_return_val_if_fail (prop_name != NULL, FALSE);

	if (g_str_equal (prop_name, CLIENT_BACKEND_PROPERTY_CAPABILITIES)) {
		return g_strjoin (
			",",
			E_CAL_STATIC_CAPABILITY_NO_EMAIL_ALARMS,
			E_CAL_STATIC_CAPABILITY_NO_THISANDPRIOR,
			E_CAL_STATIC_CAPABILITY_DELEGATE_SUPPORTED,
			E_CAL_STATIC_CAPABILITY_REMOVE_ONLY_THIS,
			E_CAL_STATIC_CAPABILITY_BULK_ADDS,
			E_CAL_STATIC_CAPABILITY_BULK_MODIFIES,
			E_CAL_STATIC_CAPABILITY_BULK_REMOVES,
			E_CAL_STATIC_CAPABILITY_ALARM_DESCRIPTION,
			E_CAL_STATIC_CAPABILITY_TASK_CAN_RECUR,
			E_CAL_STATIC_CAPABILITY_COMPONENT_COLOR,
			E_CAL_STATIC_CAPABILITY_REFRESH_SUPPORTED,
			NULL);

	} else if (g_str_equal (prop_name, E_CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS) ||
		   g_str_equal (prop_name, E_CAL_BACKEND_PROPERTY_ALARM_EMAIL_ADDRESS)) {
		/* A decsync backend has no particular email address associated
		 * with it (although that would be a useful feature some day).
		 */
		return NULL;

	} else if (g_str_equal (prop_name, E_CAL_BACKEND_PROPERTY_DEFAULT_OBJECT)) {
		ECalComponent *comp;
		gchar *prop_value;

		comp = e_cal_component_new ();

		switch (e_cal_backend_get_kind (E_CAL_BACKEND (backend))) {
		case I_CAL_VEVENT_COMPONENT:
			e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_EVENT);
			break;
		default:
			g_object_unref (comp);
			return NULL;
		}

		prop_value = e_cal_component_get_as_string (comp);

		g_object_unref (comp);

		return prop_value;

	} else if (g_str_equal (prop_name, E_CAL_BACKEND_PROPERTY_REVISION)) {
		ICalProperty *prop;
		gchar *revision = NULL;

		/* This returns NULL if backend lacks a vcalendar. */
		prop = ensure_revision (E_CAL_BACKEND_DECSYNC (backend));
		if (prop) {
			revision = g_strdup (i_cal_property_get_x (prop));
			g_object_unref (prop);
		}

		return revision;
	}

	/* Chain up to parent's method. */
	return E_CAL_BACKEND_CLASS (e_cal_backend_decsync_parent_class)->
		impl_get_backend_property (backend, prop_name);
}

typedef struct _ResolveTzidData {
	ICalComponent *vcalendar;
	GHashTable *zones; /* gchar *tzid -> ICalTimezone * */
} ResolveTzidData;

static void
resolve_tzid_data_init (ResolveTzidData *rtd,
                        ICalComponent *vcalendar)
{
	if (rtd) {
		rtd->vcalendar = vcalendar;
		rtd->zones = NULL;
	}
}

/* Clears the content, not the structure */
static void
resolve_tzid_data_clear (ResolveTzidData *rtd)
{
	if (rtd && rtd->zones)
		g_hash_table_destroy (rtd->zones);
}

/* function to resolve timezones */
static ICalTimezone *
resolve_tzid_cb (const gchar *tzid,
                 gpointer user_data,
                 GCancellable *cancellable,
                 GError **error)
{
	ResolveTzidData *rtd = user_data;
	ICalTimezone *zone;

	if (!tzid || !tzid[0])
		return NULL;
	else if (!strcmp (tzid, "UTC"))
		return i_cal_timezone_get_utc_timezone ();

	if (rtd->zones) {
		zone = g_hash_table_lookup (rtd->zones, tzid);
		if (zone)
			return zone;
	}

	zone = i_cal_timezone_get_builtin_timezone_from_tzid (tzid);
	if (zone)
		g_object_ref (zone);
	else if (rtd->vcalendar)
		zone = i_cal_component_get_timezone (rtd->vcalendar, tzid);

	if (zone) {
		if (!rtd->zones)
			rtd->zones = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

		g_hash_table_insert (rtd->zones, g_strdup (tzid), zone);
	}

	return zone;
}

/* Checks if the specified component has a duplicated UID and if so changes it.
 * UIDs may be shared between components if there is at most one component
 * without RECURRENCE-ID (master) and all others have different RECURRENCE-ID
 * values.
 */
static void
check_dup_uid (ECalBackendDecsync *cbfile,
               ECalComponent *comp)
{
	ECalBackendDecsyncPrivate *priv;
	ECalBackendDecsyncObject *obj_data;
	const gchar *uid;
	gchar *new_uid = NULL;
	gchar *rid = NULL;

	priv = cbfile->priv;

	uid = e_cal_component_get_uid (comp);

	if (!uid) {
		g_warning ("Checking for duplicate uid, the component does not have a valid UID skipping it\n");
		return;
	}

	obj_data = g_hash_table_lookup (priv->comp_uid_hash, uid);
	if (!obj_data)
		return; /* Everything is fine */

	rid = e_cal_component_get_recurid_as_string (comp);
	if (rid && *rid) {
		/* new component has rid, must not be the same as in other detached recurrence */
		if (!g_hash_table_lookup (obj_data->recurrences, rid))
			goto done;
	} else {
		/* new component has no rid, must not clash with existing master */
		if (!obj_data->full_object)
			goto done;
	}

	d (
		g_message (G_STRLOC ": Got object with duplicated UID `%s' and rid `%s', changing it...",
		uid,
		rid ? rid : ""));

	new_uid = e_util_generate_uid ();
	e_cal_component_set_uid (comp, new_uid);

	/* FIXME: I think we need to reset the SEQUENCE property and reset the
	 * CREATED/DTSTAMP/LAST-MODIFIED.
	 */

	save (cbfile, FALSE);

 done:
	g_free (rid);
	g_free (new_uid);
}

static time_t
get_rid_as_time_t (ECalComponent *comp)
{
	ECalComponentRange *range;
	ECalComponentDateTime *dt;
	time_t tmt = (time_t) -1;

	range = e_cal_component_get_recurid (comp);
	if (!range)
		return tmt;

	dt = e_cal_component_range_get_datetime (range);
	if (!dt) {
		e_cal_component_range_free (range);
		return tmt;
	}

	tmt = i_cal_time_as_timet (e_cal_component_datetime_get_value (dt));

	e_cal_component_range_free (range);

	return tmt;
}

/* Adds component to the interval tree
 */
static void
add_component_to_intervaltree (ECalBackendDecsync *cbfile,
                               ECalComponent *comp)
{
	time_t time_start = -1, time_end = -1;
	ECalBackendDecsyncPrivate *priv;
	ResolveTzidData rtd;

	g_return_if_fail (cbfile != NULL);
	g_return_if_fail (comp != NULL);

	priv = cbfile->priv;

	resolve_tzid_data_init (&rtd, cbfile->priv->vcalendar);

	e_cal_util_get_component_occur_times (
		comp, &time_start, &time_end,
		resolve_tzid_cb, &rtd, i_cal_timezone_get_utc_timezone (),
		e_cal_backend_get_kind (E_CAL_BACKEND (cbfile)));

	resolve_tzid_data_clear (&rtd);

	if (time_end != -1 && time_start > time_end) {
		gchar *str = e_cal_component_get_as_string (comp);
		g_print ("Bogus component %s\n", str);
		g_free (str);
	} else {
		g_rec_mutex_lock (&priv->idle_save_rmutex);
		e_intervaltree_insert (priv->interval_tree, time_start, time_end, comp);
		g_rec_mutex_unlock (&priv->idle_save_rmutex);
	}
}

static gboolean
remove_component_from_intervaltree (ECalBackendDecsync *cbfile,
                                    ECalComponent *comp)
{
	const gchar *uid;
	gchar *rid;
	gboolean res;
	ECalBackendDecsyncPrivate *priv;

	g_return_val_if_fail (cbfile != NULL, FALSE);
	g_return_val_if_fail (comp != NULL, FALSE);

	priv = cbfile->priv;

	uid = e_cal_component_get_uid (comp);
	rid = e_cal_component_get_recurid_as_string (comp);

	g_rec_mutex_lock (&priv->idle_save_rmutex);
	res = e_intervaltree_remove (priv->interval_tree, uid, rid);
	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	g_free (rid);

	return res;
}

/* Tries to add an ICalComponent to the decsync backend.  We only store the objects
 * of the types we support; all others just remain in the toplevel component so
 * that we don't lose them.
 *
 * The caller is responsible for ensuring that the component has a UID and that
 * the UID is not in use already.
 */
static void
add_component (ECalBackendDecsync *cbfile,
               ECalComponent *comp,
               gboolean add_to_toplevel)
{
	ECalBackendDecsyncPrivate *priv;
	ECalBackendDecsyncObject *obj_data;
	const gchar *uid;

	priv = cbfile->priv;

	uid = e_cal_component_get_uid (comp);

	if (!uid) {
		g_warning ("The component does not have a valid UID skipping it\n");
		return;
	}

	obj_data = g_hash_table_lookup (priv->comp_uid_hash, uid);
	if (e_cal_component_is_instance (comp)) {
		gchar *rid;

		rid = e_cal_component_get_recurid_as_string (comp);
		if (obj_data) {
			if (g_hash_table_lookup (obj_data->recurrences, rid)) {
				g_warning (G_STRLOC ": Tried to add an already existing recurrence");
				g_free (rid);
				return;
			}
		} else {
			obj_data = g_new0 (ECalBackendDecsyncObject, 1);
			obj_data->full_object = NULL;
			obj_data->recurrences = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
			g_hash_table_insert (priv->comp_uid_hash, g_strdup (uid), obj_data);
		}

		g_hash_table_insert (obj_data->recurrences, rid, comp);
		obj_data->recurrences_list = g_list_append (obj_data->recurrences_list, comp);
	} else {
		if (obj_data) {
			if (obj_data->full_object) {
				g_warning (G_STRLOC ": Tried to add an already existing object");
				return;
			}

			obj_data->full_object = comp;
		} else {
			obj_data = g_new0 (ECalBackendDecsyncObject, 1);
			obj_data->full_object = comp;
			obj_data->recurrences = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

			g_hash_table_insert (priv->comp_uid_hash, g_strdup (uid), obj_data);
		}
	}

	add_component_to_intervaltree (cbfile, comp);

	priv->comp = g_list_prepend (priv->comp, comp);

	/* Put the object in the toplevel component if required */

	if (add_to_toplevel) {
		ICalComponent *icomp;

		icomp = e_cal_component_get_icalcomponent (comp);
		g_return_if_fail (icomp != NULL);

		i_cal_component_add_component (priv->vcalendar, icomp);
	}
}

/* g_hash_table_foreach_remove() callback to remove recurrences from the calendar */
static gboolean
remove_recurrence_cb (gpointer key,
                      gpointer value,
                      gpointer data)
{
	ICalComponent *icomp;
	ECalBackendDecsyncPrivate *priv;
	ECalComponent *comp = value;
	ECalBackendDecsync *cbfile = data;

	priv = cbfile->priv;

	/* remove the recurrence from the top-level calendar */
	icomp = e_cal_component_get_icalcomponent (comp);
	g_return_val_if_fail (icomp != NULL, FALSE);

	icomp = g_object_ref (icomp);

	if (!remove_component_from_intervaltree (cbfile, comp)) {
		g_message (G_STRLOC " Could not remove component from interval tree!");
	}
	i_cal_component_remove_component (priv->vcalendar, icomp);

	g_object_unref (icomp);

	/* remove it from our mapping */
	priv->comp = g_list_remove (priv->comp, comp);

	return TRUE;
}

/* Removes a component from the backend's hash and lists.  Does not perform
 * notification on the clients.  Also removes the component from the toplevel
 * ICalComponent.
 */
static void
remove_component (ECalBackendDecsync *cbfile,
                  const gchar *uid,
                  ECalBackendDecsyncObject *obj_data)
{
	ECalBackendDecsyncPrivate *priv;
	ICalComponent *icomp;
	GList *l;

	priv = cbfile->priv;

	/* Remove the ICalComponent from the toplevel */
	if (obj_data->full_object) {
		icomp = e_cal_component_get_icalcomponent (obj_data->full_object);
		g_return_if_fail (icomp != NULL);

		i_cal_component_remove_component (priv->vcalendar, icomp);

		/* Remove it from our mapping */
		l = g_list_find (priv->comp, obj_data->full_object);
		g_return_if_fail (l != NULL);
		priv->comp = g_list_delete_link (priv->comp, l);

		if (!remove_component_from_intervaltree (cbfile, obj_data->full_object)) {
			g_message (G_STRLOC " Could not remove component from interval tree!");
		}
	}

	/* remove the recurrences also */
	g_hash_table_foreach_remove (obj_data->recurrences, (GHRFunc) remove_recurrence_cb, cbfile);

	g_hash_table_remove (priv->comp_uid_hash, uid);

	save (cbfile, TRUE);
}

/* Scans the toplevel VCALENDAR component and stores the objects it finds */
static void
scan_vcalendar (ECalBackendDecsync *cbfile)
{
	ECalBackendDecsyncPrivate *priv;
	ICalCompIter *iter;
	ICalComponent *icomp;

	priv = cbfile->priv;
	g_return_if_fail (priv->vcalendar != NULL);
	g_return_if_fail (priv->comp_uid_hash != NULL);

	iter = i_cal_component_begin_component (priv->vcalendar, I_CAL_ANY_COMPONENT);
	icomp = iter ? i_cal_comp_iter_deref (iter) : NULL;
	while (icomp) {
		ICalComponentKind kind;
		ECalComponent *comp;

		kind = i_cal_component_isa (icomp);

		if (kind == I_CAL_VEVENT_COMPONENT) {
			comp = e_cal_component_new ();

			if (e_cal_component_set_icalcomponent (comp, icomp)) {
				/* Thus it's not freed while being used in the 'comp' */
				g_object_ref (icomp);

				check_dup_uid (cbfile, comp);

				add_component (cbfile, comp, FALSE);
			} else {
				g_object_unref (comp);
			}
		}

		g_object_unref (icomp);
		icomp = i_cal_comp_iter_next (iter);
	}

	g_clear_object (&iter);
}

static gchar *
uri_to_path (ECalBackend *backend)
{
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncPrivate *priv;
	const gchar *cache_dir;
	gchar *filename = NULL;

	cbfile = E_CAL_BACKEND_DECSYNC (backend);
	priv = cbfile->priv;

	cache_dir = e_cal_backend_get_cache_dir (backend);

	filename = g_build_filename (cache_dir, priv->file_name, NULL);

	if (filename != NULL && *filename == '\0') {
		g_free (filename);
		filename = NULL;
	}

	return filename;
}

static void
cal_backend_decsync_take_icomp (ECalBackendDecsync *cbfile,
                                ICalComponent *icomp)
{
	ICalProperty *prop;

	g_warn_if_fail (cbfile->priv->vcalendar == NULL);
	cbfile->priv->vcalendar = icomp;

	prop = ensure_revision (cbfile);

	e_cal_backend_notify_property_changed (
		E_CAL_BACKEND (cbfile),
		E_CAL_BACKEND_PROPERTY_REVISION,
		i_cal_property_get_x (prop));

	g_clear_object (&prop);
}

/* Parses an open iCalendar file and loads it into the backend */
static void
open_cal (ECalBackendDecsync *cbfile,
          const gchar *uristr,
          GError **perror)
{
	ECalBackendDecsyncPrivate *priv;
	ICalComponent *icomp;

	priv = cbfile->priv;

	icomp = e_cal_util_parse_ics_file (uristr);
	if (!icomp) {
		g_propagate_error (perror, e_client_error_create_fmt (E_CLIENT_ERROR_OTHER_ERROR, _("Cannot parse ISC file “%s”"), uristr));
		return;
	}

	/* FIXME: should we try to demangle XROOT components and
	 * individual components as well?
	 */

	if (i_cal_component_isa (icomp) != I_CAL_VCALENDAR_COMPONENT) {
		g_object_unref (icomp);

		g_propagate_error (perror, e_client_error_create_fmt (E_CLIENT_ERROR_OTHER_ERROR, _("File “%s” is not a VCALENDAR component"), uristr));
		return;
	}

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	cal_backend_decsync_take_icomp (cbfile, icomp);
	priv->path = uri_to_path (E_CAL_BACKEND (cbfile));

	priv->comp_uid_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, free_object_data);
	priv->interval_tree = e_intervaltree_new ();
	scan_vcalendar (cbfile);

	g_rec_mutex_unlock (&priv->idle_save_rmutex);
}

typedef struct
{
	ECalBackend *backend;
	GHashTable *old_uid_hash;
	GHashTable *new_uid_hash;
}
BackendDeltaContext;

static void
notify_removals_cb (gpointer key,
                    gpointer value,
                    gpointer data)
{
	BackendDeltaContext *context = data;
	const gchar *uid = key;
	ECalBackendDecsyncObject *old_obj_data = value;

	if (!g_hash_table_lookup (context->new_uid_hash, uid)) {
		ECalComponentId *id;

		/* Object was removed */

		if (!old_obj_data->full_object)
			return;

		id = e_cal_component_get_id (old_obj_data->full_object);

		e_cal_backend_notify_component_removed (context->backend, id, old_obj_data->full_object, NULL);

		e_cal_component_id_free (id);
	}
}

static void
notify_adds_modifies_cb (gpointer key,
                         gpointer value,
                         gpointer data)
{
	BackendDeltaContext *context = data;
	const gchar *uid = key;
	ECalBackendDecsyncObject *new_obj_data = value;
	ECalBackendDecsyncObject *old_obj_data;

	old_obj_data = g_hash_table_lookup (context->old_uid_hash, uid);

	if (!old_obj_data) {
		/* Object was added */
		if (!new_obj_data->full_object)
			return;

		e_cal_backend_notify_component_created (context->backend, new_obj_data->full_object);
	} else {
		gchar *old_obj_str, *new_obj_str;

		if (!old_obj_data->full_object || !new_obj_data->full_object)
			return;

		/* There should be better ways to compare an ICalComponent
		 * than serializing and comparing the strings...
		 */
		old_obj_str = e_cal_component_get_as_string (old_obj_data->full_object);
		new_obj_str = e_cal_component_get_as_string (new_obj_data->full_object);
		if (old_obj_str && new_obj_str && strcmp (old_obj_str, new_obj_str) != 0) {
			/* Object was modified */
			e_cal_backend_notify_component_modified (context->backend, old_obj_data->full_object, new_obj_data->full_object);
		}

		g_free (old_obj_str);
		g_free (new_obj_str);
	}
}

static void
notify_changes (ECalBackendDecsync *cbfile,
                GHashTable *old_uid_hash,
                GHashTable *new_uid_hash)
{
	BackendDeltaContext context;

	context.backend = E_CAL_BACKEND (cbfile);
	context.old_uid_hash = old_uid_hash;
	context.new_uid_hash = new_uid_hash;

	g_hash_table_foreach (old_uid_hash, (GHFunc) notify_removals_cb, &context);
	g_hash_table_foreach (new_uid_hash, (GHFunc) notify_adds_modifies_cb, &context);
}

static void
reload_cal (ECalBackendDecsync *cbfile,
            const gchar *uristr,
            GError **perror)
{
	ECalBackendDecsyncPrivate *priv;
	ICalComponent *icomp, *icomp_old;
	GHashTable *comp_uid_hash_old;

	priv = cbfile->priv;

	icomp = e_cal_util_parse_ics_file (uristr);
	if (!icomp) {
		g_propagate_error (perror, e_client_error_create_fmt (E_CLIENT_ERROR_OTHER_ERROR, _("Cannot parse ISC file “%s”"), uristr));
		return;
	}

	/* FIXME: should we try to demangle XROOT components and
	 * individual components as well?
	 */

	if (i_cal_component_isa (icomp) != I_CAL_VCALENDAR_COMPONENT) {
		g_object_unref (icomp);

		g_propagate_error (perror, e_client_error_create_fmt (E_CLIENT_ERROR_OTHER_ERROR, _("File “%s” is not a VCALENDAR component"), uristr));
		return;
	}

	/* Keep old data for comparison - free later */

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	icomp_old = priv->vcalendar;
	priv->vcalendar = NULL;

	comp_uid_hash_old = priv->comp_uid_hash;
	priv->comp_uid_hash = NULL;

	/* Load new calendar */

	free_calendar_data (cbfile);

	cal_backend_decsync_take_icomp (cbfile, icomp);

	priv->comp_uid_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, free_object_data);
	priv->interval_tree = e_intervaltree_new ();
	scan_vcalendar (cbfile);

	priv->path = uri_to_path (E_CAL_BACKEND (cbfile));

	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	/* Compare old and new versions of calendar */

	notify_changes (cbfile, comp_uid_hash_old, priv->comp_uid_hash);

	/* Free old data */

	free_calendar_components (comp_uid_hash_old, icomp_old);
}

static void
create_cal (ECalBackendDecsync *cbfile,
            const gchar *uristr,
            GError **perror)
{
	gchar *dirname;
	ECalBackendDecsyncPrivate *priv;
	ICalComponent *icomp;

	priv = cbfile->priv;

	/* Create the directory to contain the file */
	dirname = g_path_get_dirname (uristr);
	if (g_mkdir_with_parents (dirname, 0700) != 0) {
		g_free (dirname);
		g_propagate_error (perror, ECC_ERROR (E_CAL_CLIENT_ERROR_NO_SUCH_CALENDAR));
		return;
	}

	g_free (dirname);

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	/* Create the new calendar information */
	icomp = e_cal_util_new_top_level ();
	cal_backend_decsync_take_icomp (cbfile, icomp);

	/* Create our internal data */
	priv->comp_uid_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, free_object_data);
	priv->interval_tree = e_intervaltree_new ();

	priv->path = uri_to_path (E_CAL_BACKEND (cbfile));

	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	save (cbfile, TRUE);
}

static gchar *
get_uri_string (ECalBackend *backend)
{
	gchar *str_uri, *full_uri;

	str_uri = uri_to_path (backend);
	full_uri = g_uri_unescape_string (str_uri, "");
	g_free (str_uri);

	return full_uri;
}

/* Open handler for the decsync backend */
static void
e_cal_backend_decsync_open (ECalBackendSync *backend,
                         EDataCal *cal,
                         GCancellable *cancellable,
                         GError **perror)
{
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncPrivate *priv;
	gchar *str_uri;
	gboolean writable = FALSE;
	GError *err = NULL;

	cbfile = E_CAL_BACKEND_DECSYNC (backend);
	priv = cbfile->priv;
	g_rec_mutex_lock (&priv->idle_save_rmutex);

	/* Decsync source is always connected. */
	e_source_set_connection_status (e_backend_get_source (E_BACKEND (backend)),
		E_SOURCE_CONNECTION_STATUS_CONNECTED);

	/* Claim a succesful open if we are already open */
	if (priv->path && priv->comp_uid_hash) {
		/* Success */
		goto done;
	}

	str_uri = get_uri_string (E_CAL_BACKEND (backend));
	if (!str_uri) {
		err = EC_ERROR_NO_URI ();
		goto done;
	}

	writable = TRUE;
	if (g_access (str_uri, R_OK) == 0) {
		open_cal (cbfile, str_uri, &err);
		if (g_access (str_uri, W_OK) != 0)
			writable = FALSE;
	} else {
		create_cal (cbfile, str_uri, &err);
	}

	g_free (str_uri);

	g_idle_add ((GSourceFunc) ecal_backend_decsync_refresh_start, cbfile);

  done:
	g_rec_mutex_unlock (&priv->idle_save_rmutex);
	e_cal_backend_set_writable (E_CAL_BACKEND (backend), writable);
	e_backend_set_online (E_BACKEND (backend), TRUE);

	if (err)
		g_propagate_error (perror, g_error_copy (err));
}

static void
add_detached_recur_to_vcalendar (gpointer key,
                                 gpointer value,
                                 gpointer user_data)
{
	ECalComponent *recurrence = value;
	ICalComponent *vcalendar = user_data;

	i_cal_component_take_component (
		vcalendar,
		i_cal_component_clone (e_cal_component_get_icalcomponent (recurrence)));
}

static void
e_cal_backend_decsync_get_ical (ECalBackendSync *backend,
                             GCancellable *cancellable,
                             const gchar *uid,
                             const gchar *rid,
                             gboolean always_ical,
                             gchar **object,
                             GError **error)
{
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncPrivate *priv;
	ECalBackendDecsyncObject *obj_data;

	cbfile = E_CAL_BACKEND_DECSYNC (backend);
	priv = cbfile->priv;

	if (priv->vcalendar == NULL) {
		g_set_error_literal (
			error, E_CAL_CLIENT_ERROR,
			E_CAL_CLIENT_ERROR_INVALID_OBJECT,
			e_cal_client_error_to_string (
			E_CAL_CLIENT_ERROR_INVALID_OBJECT));
		return;
	}

	g_return_if_fail (uid != NULL);
	g_return_if_fail (priv->comp_uid_hash != NULL);

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	obj_data = g_hash_table_lookup (priv->comp_uid_hash, uid);
	if (!obj_data) {
		g_rec_mutex_unlock (&priv->idle_save_rmutex);
		g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND));
		return;
	}

	if (rid && *rid) {
		ECalComponent *comp;

		comp = g_hash_table_lookup (obj_data->recurrences, rid);
		if (!always_ical && comp) {
			*object = e_cal_component_get_as_string (comp);
		} else {
			ICalComponent *icomp;
			ICalTime *itt;

			if (!obj_data->full_object) {
				g_rec_mutex_unlock (&priv->idle_save_rmutex);
				g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND));
				return;
			}

			itt = i_cal_time_new_from_string (rid);
			icomp = e_cal_util_construct_instance (
				e_cal_component_get_icalcomponent (obj_data->full_object),
				itt);
			g_object_unref (itt);

			if (!icomp) {
				g_rec_mutex_unlock (&priv->idle_save_rmutex);
				g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND));
				return;
			}

			*object = i_cal_component_as_ical_string (icomp);

			g_object_unref (icomp);
		}
	} else {
		if (always_ical || g_hash_table_size (obj_data->recurrences) > 0) {
			ICalComponent *icomp;

			/* if we have detached recurrences, return a VCALENDAR */
			icomp = e_cal_util_new_top_level ();

			/* detached recurrences don't have full_object */
			if (obj_data->full_object)
				i_cal_component_add_component (
					icomp,
					i_cal_component_clone (e_cal_component_get_icalcomponent (obj_data->full_object)));

			/* add all detached recurrences */
			g_hash_table_foreach (obj_data->recurrences, (GHFunc) add_detached_recur_to_vcalendar, icomp);

			*object = i_cal_component_as_ical_string (icomp);

			g_object_unref (icomp);
		} else if (obj_data->full_object)
			*object = e_cal_component_get_as_string (obj_data->full_object);
	}

	g_rec_mutex_unlock (&priv->idle_save_rmutex);
}
/* Get_object_component handler for the decsync backend */
static void
e_cal_backend_decsync_get_object (ECalBackendSync *backend,
                               EDataCal *cal,
                               GCancellable *cancellable,
                               const gchar *uid,
                               const gchar *rid,
                               gchar **object,
                               GError **error)
{
	e_cal_backend_decsync_get_ical (backend, cancellable, uid, rid, FALSE, object, error);
}

/* Add_timezone handler for the decsync backend */
static void
e_cal_backend_decsync_add_timezone (ECalBackendSync *backend,
                                 EDataCal *cal,
                                 GCancellable *cancellable,
                                 const gchar *tzobj,
                                 GError **error)
{
	ETimezoneCache *timezone_cache;
	ICalComponent *tz_comp;

	timezone_cache = E_TIMEZONE_CACHE (backend);

	tz_comp = i_cal_parser_parse_string (tzobj);
	if (!tz_comp) {
		g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_INVALID_OBJECT));
		return;
	}

	if (i_cal_component_isa (tz_comp) == I_CAL_VTIMEZONE_COMPONENT) {
		ICalTimezone *zone;

		zone = i_cal_timezone_new ();
		if (i_cal_timezone_set_component (zone, tz_comp))
			e_timezone_cache_add_timezone (timezone_cache, zone);
		g_object_unref (zone);
	}

	g_object_unref (tz_comp);
}

typedef struct {
	GSList *comps_list;
	gboolean search_needed;
	const gchar *query;
	ECalBackendSExp *obj_sexp;
	ECalBackend *backend;
	EDataCalView *view;
	gboolean as_string;
} MatchObjectData;

static void
match_object_sexp_to_component (gpointer value,
                                gpointer data)
{
	ECalComponent *comp = value;
	MatchObjectData *match_data = data;
	ETimezoneCache *timezone_cache;

	g_return_if_fail (comp != NULL);
	g_return_if_fail (match_data->backend != NULL);

	timezone_cache = E_TIMEZONE_CACHE (match_data->backend);

	if ((!match_data->search_needed) ||
	    (e_cal_backend_sexp_match_comp (match_data->obj_sexp, comp, timezone_cache))) {
		if (match_data->as_string)
			match_data->comps_list = g_slist_prepend (match_data->comps_list, e_cal_component_get_as_string (comp));
		else
			match_data->comps_list = g_slist_prepend (match_data->comps_list, comp);
	}
}

static void
match_recurrence_sexp (gpointer key,
                       gpointer value,
                       gpointer data)
{
	ECalComponent *comp = value;
	MatchObjectData *match_data = data;
	ETimezoneCache *timezone_cache;

	timezone_cache = E_TIMEZONE_CACHE (match_data->backend);

	if ((!match_data->search_needed) ||
	    (e_cal_backend_sexp_match_comp (match_data->obj_sexp, comp, timezone_cache))) {
		if (match_data->as_string)
			match_data->comps_list = g_slist_prepend (match_data->comps_list, e_cal_component_get_as_string (comp));
		else
			match_data->comps_list = g_slist_prepend (match_data->comps_list, comp);
	}
}

static void
match_object_sexp (gpointer key,
                   gpointer value,
                   gpointer data)
{
	ECalBackendDecsyncObject *obj_data = value;
	MatchObjectData *match_data = data;
	ETimezoneCache *timezone_cache;

	timezone_cache = E_TIMEZONE_CACHE (match_data->backend);

	if (obj_data->full_object) {
		if ((!match_data->search_needed) ||
		    (e_cal_backend_sexp_match_comp (match_data->obj_sexp,
						    obj_data->full_object,
						    timezone_cache))) {
			if (match_data->as_string)
				match_data->comps_list = g_slist_prepend (match_data->comps_list, e_cal_component_get_as_string (obj_data->full_object));
			else
				match_data->comps_list = g_slist_prepend (match_data->comps_list, obj_data->full_object);
		}
	}

	/* match also recurrences */
	g_hash_table_foreach (obj_data->recurrences,
			      (GHFunc) match_recurrence_sexp,
			      match_data);
}

/* Get_objects_in_range handler for the decsync backend */
static void
e_cal_backend_decsync_get_object_list (ECalBackendSync *backend,
                                    EDataCal *cal,
                                    GCancellable *cancellable,
                                    const gchar *sexp,
                                    GSList **objects,
                                    GError **perror)
{
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncPrivate *priv;
	MatchObjectData match_data = { 0, };
	time_t occur_start = -1, occur_end = -1;
	gboolean prunning_by_time;
	GList * objs_occuring_in_tw;
	cbfile = E_CAL_BACKEND_DECSYNC (backend);
	priv = cbfile->priv;

	d (g_message (G_STRLOC ": Getting object list (%s)", sexp));

	match_data.search_needed = TRUE;
	match_data.query = sexp;
	match_data.comps_list = NULL;
	match_data.as_string = TRUE;
	match_data.backend = E_CAL_BACKEND (backend);

	if (sexp && !strcmp (sexp, "#t"))
		match_data.search_needed = FALSE;

	match_data.obj_sexp = e_cal_backend_sexp_new (sexp);
	if (!match_data.obj_sexp) {
		g_propagate_error (perror, EC_ERROR (E_CLIENT_ERROR_INVALID_QUERY));
		return;
	}

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	prunning_by_time = e_cal_backend_sexp_evaluate_occur_times (
		match_data.obj_sexp,
		&occur_start,
		&occur_end);

	objs_occuring_in_tw = NULL;

	if (!prunning_by_time) {
		g_hash_table_foreach (priv->comp_uid_hash, (GHFunc) match_object_sexp,
				      &match_data);
	} else {
		objs_occuring_in_tw = e_intervaltree_search (
			priv->interval_tree,
			occur_start, occur_end);

		g_list_foreach (objs_occuring_in_tw, (GFunc) match_object_sexp_to_component,
			       &match_data);
	}

	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	*objects = g_slist_reverse (match_data.comps_list);

	if (objs_occuring_in_tw) {
		g_list_foreach (objs_occuring_in_tw, (GFunc) g_object_unref, NULL);
		g_list_free (objs_occuring_in_tw);
	}

	g_object_unref (match_data.obj_sexp);
}

static void
add_attach_uris (GSList **attachment_uris,
                 ICalComponent *icomp)
{
	ICalProperty *prop;

	g_return_if_fail (attachment_uris != NULL);
	g_return_if_fail (icomp != NULL);

	for (prop = i_cal_component_get_first_property (icomp, I_CAL_ATTACH_PROPERTY);
	     prop;
	     g_object_unref (prop), prop = i_cal_component_get_next_property (icomp, I_CAL_ATTACH_PROPERTY)) {
		ICalAttach *attach = i_cal_property_get_attach (prop);

		if (attach && i_cal_attach_get_is_url (attach)) {
			const gchar *url;

			url = i_cal_attach_get_url (attach);
			if (url) {
				gchar *buf;

				buf = i_cal_value_decode_ical_string (url);

				*attachment_uris = g_slist_prepend (*attachment_uris, g_strdup (buf));

				g_free (buf);
			}
		}

		g_clear_object (&attach);
	}
}

static void
add_detached_recur_attach_uris (gpointer key,
                                gpointer value,
                                gpointer user_data)
{
	ECalComponent *recurrence = value;
	GSList **attachment_uris = user_data;

	add_attach_uris (attachment_uris, e_cal_component_get_icalcomponent (recurrence));
}

/* Gets the list of attachments */
static void
e_cal_backend_decsync_get_attachment_uris (ECalBackendSync *backend,
                                        EDataCal *cal,
                                        GCancellable *cancellable,
                                        const gchar *uid,
                                        const gchar *rid,
                                        GSList **attachment_uris,
                                        GError **error)
{
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncPrivate *priv;
	ECalBackendDecsyncObject *obj_data;

	cbfile = E_CAL_BACKEND_DECSYNC (backend);
	priv = cbfile->priv;

	g_return_if_fail (priv->comp_uid_hash != NULL);

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	obj_data = g_hash_table_lookup (priv->comp_uid_hash, uid);
	if (!obj_data) {
		g_rec_mutex_unlock (&priv->idle_save_rmutex);
		g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND));
		return;
	}

	if (rid && *rid) {
		ECalComponent *comp;

		comp = g_hash_table_lookup (obj_data->recurrences, rid);
		if (comp) {
			add_attach_uris (attachment_uris, e_cal_component_get_icalcomponent (comp));
		} else {
			ICalComponent *icomp;
			ICalTime *itt;

			if (!obj_data->full_object) {
				g_rec_mutex_unlock (&priv->idle_save_rmutex);
				g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND));
				return;
			}

			itt = i_cal_time_new_from_string (rid);
			icomp = e_cal_util_construct_instance (
				e_cal_component_get_icalcomponent (obj_data->full_object),
				itt);
			g_object_unref (itt);
			if (!icomp) {
				g_rec_mutex_unlock (&priv->idle_save_rmutex);
				g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND));
				return;
			}

			add_attach_uris (attachment_uris, icomp);

			g_object_unref (icomp);
		}
	} else {
		if (g_hash_table_size (obj_data->recurrences) > 0) {
			/* detached recurrences don't have full_object */
			if (obj_data->full_object)
				add_attach_uris (attachment_uris, e_cal_component_get_icalcomponent (obj_data->full_object));

			/* add all detached recurrences */
			g_hash_table_foreach (obj_data->recurrences, add_detached_recur_attach_uris, attachment_uris);
		} else if (obj_data->full_object)
			add_attach_uris (attachment_uris, e_cal_component_get_icalcomponent (obj_data->full_object));
	}

	*attachment_uris = g_slist_reverse (*attachment_uris);

	g_rec_mutex_unlock (&priv->idle_save_rmutex);
}

/* get_query handler for the decsync backend */
static void
e_cal_backend_decsync_start_view (ECalBackend *backend,
                               EDataCalView *query)
{
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncPrivate *priv;
	ECalBackendSExp *sexp;
	MatchObjectData match_data = { 0, };
	time_t occur_start = -1, occur_end = -1;
	gboolean prunning_by_time;
	GList * objs_occuring_in_tw;
	cbfile = E_CAL_BACKEND_DECSYNC (backend);
	priv = cbfile->priv;

	sexp = e_data_cal_view_get_sexp (query);

	d (g_message (G_STRLOC ": Starting query (%s)", e_cal_backend_sexp_text (sexp)));

	/* try to match all currently existing objects */
	match_data.search_needed = TRUE;
	match_data.query = e_cal_backend_sexp_text (sexp);
	match_data.comps_list = NULL;
	match_data.as_string = FALSE;
	match_data.backend = backend;
	match_data.obj_sexp = e_data_cal_view_get_sexp (query);
	match_data.view = query;

	if (match_data.query && !strcmp (match_data.query, "#t"))
		match_data.search_needed = FALSE;

	if (!match_data.obj_sexp) {
		GError *error = EC_ERROR (E_CLIENT_ERROR_INVALID_QUERY);
		e_data_cal_view_notify_complete (query, error);
		g_error_free (error);
		return;
	}
	prunning_by_time = e_cal_backend_sexp_evaluate_occur_times (
		match_data.obj_sexp,
		&occur_start,
		&occur_end);

	objs_occuring_in_tw = NULL;

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	if (!prunning_by_time) {
		/* full scan */
		g_hash_table_foreach (priv->comp_uid_hash, (GHFunc) match_object_sexp,
				      &match_data);

		e_debug_log (
			FALSE, E_DEBUG_LOG_DOMAIN_CAL_QUERIES,  "---;%p;QUERY-ITEMS;%s;%s;%d", query,
			e_cal_backend_sexp_text (sexp), G_OBJECT_TYPE_NAME (backend),
			g_hash_table_size (priv->comp_uid_hash));
	} else {
		/* matches objects in new "interval tree" way */
		/* events occuring in time window */
		objs_occuring_in_tw = e_intervaltree_search (priv->interval_tree, occur_start, occur_end);

		g_list_foreach (objs_occuring_in_tw, (GFunc) match_object_sexp_to_component,
			       &match_data);

		e_debug_log (
			FALSE, E_DEBUG_LOG_DOMAIN_CAL_QUERIES,  "---;%p;QUERY-ITEMS;%s;%s;%d", query,
			e_cal_backend_sexp_text (sexp), G_OBJECT_TYPE_NAME (backend),
			g_list_length (objs_occuring_in_tw));
	}

	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	/* notify listeners of all objects */
	if (match_data.comps_list) {
		match_data.comps_list = g_slist_reverse (match_data.comps_list);

		e_data_cal_view_notify_components_added (query, match_data.comps_list);

		/* free memory */
		g_slist_free (match_data.comps_list);
	}

	if (objs_occuring_in_tw) {
		g_list_foreach (objs_occuring_in_tw, (GFunc) g_object_unref, NULL);
		g_list_free (objs_occuring_in_tw);
	}

	e_data_cal_view_notify_complete (query, NULL /* Success */);
}

static gboolean
free_busy_instance (ICalComponent *icomp,
                    ICalTime *instance_start,
                    ICalTime *instance_end,
                    gpointer user_data,
                    GCancellable *cancellable,
                    GError **error)
{
	ICalComponent *vfb = user_data;
	ICalProperty *prop;
	ICalParameter *param;
	ICalPeriod *ipt;
	const gchar *summary, *location;

	if (!i_cal_time_is_date (instance_start))
		i_cal_time_convert_to_zone_inplace (instance_start, i_cal_timezone_get_utc_timezone ());

	if (!i_cal_time_is_date (instance_end))
		i_cal_time_convert_to_zone_inplace (instance_end, i_cal_timezone_get_utc_timezone ());

	ipt = i_cal_period_new_null_period ();
	i_cal_period_set_start (ipt, instance_start);
	i_cal_period_set_end (ipt, instance_end);

        /* add busy information to the vfb component */
	prop = i_cal_property_new (I_CAL_FREEBUSY_PROPERTY);
	i_cal_property_set_freebusy (prop, ipt);
	g_object_unref (ipt);

	param = i_cal_parameter_new_fbtype (I_CAL_FBTYPE_BUSY);
	i_cal_property_take_parameter (prop, param);

	summary = i_cal_component_get_summary (icomp);
	if (summary && *summary)
		i_cal_property_set_parameter_from_string (prop, "X-SUMMARY", summary);
	location = i_cal_component_get_location (icomp);
	if (location && *location)
		i_cal_property_set_parameter_from_string (prop, "X-LOCATION", location);

	i_cal_component_take_property (vfb, prop);

	return TRUE;
}

static ICalComponent *
create_user_free_busy (ECalBackendDecsync *cbfile,
                       const gchar *address,
                       const gchar *cn,
                       time_t start,
                       time_t end,
                       GCancellable *cancellable)
{
	ECalBackendDecsyncPrivate *priv;
	GList *l;
	ICalComponent *vfb;
	ICalTimezone *utc_zone;
	ICalTime *starttt, *endtt;
	ECalBackendSExp *obj_sexp;
	gchar *query, *iso_start, *iso_end;

	priv = cbfile->priv;

	/* create the (unique) VFREEBUSY object that we'll return */
	vfb = i_cal_component_new_vfreebusy ();
	if (address != NULL) {
		ICalProperty *prop;
		ICalParameter *param;

		prop = i_cal_property_new_organizer (address);
		if (prop != NULL && cn != NULL) {
			param = i_cal_parameter_new_cn (cn);
			i_cal_property_add_parameter (prop, param);
		}
		if (prop != NULL)
			i_cal_component_take_property (vfb, prop);
	}
	utc_zone = i_cal_timezone_get_utc_timezone ();

	starttt = i_cal_time_new_from_timet_with_zone (start, FALSE, utc_zone);
	i_cal_component_set_dtstart (vfb, starttt);

	endtt = i_cal_time_new_from_timet_with_zone (end, FALSE, utc_zone);
	i_cal_component_set_dtend (vfb, endtt);

	/* add all objects in the given interval */
	iso_start = isodate_from_time_t (start);
	iso_end = isodate_from_time_t (end);
	query = g_strdup_printf (
		"occur-in-time-range? (make-time \"%s\") (make-time \"%s\")",
		iso_start, iso_end);
	obj_sexp = e_cal_backend_sexp_new (query);
	g_free (query);
	g_free (iso_start);
	g_free (iso_end);

	if (!obj_sexp) {
		g_clear_object (&starttt);
		g_clear_object (&endtt);
		return vfb;
	}

	for (l = priv->comp; l; l = l->next) {
		ECalComponent *comp = l->data;
		ICalComponent *icomp, *vcalendar_comp;
		ICalProperty *prop;
		ResolveTzidData rtd;

		icomp = e_cal_component_get_icalcomponent (comp);
		if (!icomp)
			continue;

		/* If the event is TRANSPARENT, skip it. */
		prop = i_cal_component_get_first_property (icomp, I_CAL_TRANSP_PROPERTY);
		if (prop) {
			ICalPropertyTransp transp_val = i_cal_property_get_transp (prop);

			g_object_unref (prop);

			if (transp_val == I_CAL_TRANSP_TRANSPARENT ||
			    transp_val == I_CAL_TRANSP_TRANSPARENTNOCONFLICT)
				continue;
		}

		if (!e_cal_backend_sexp_match_comp (obj_sexp, comp, E_TIMEZONE_CACHE (cbfile)))
			continue;

		vcalendar_comp = i_cal_component_get_parent (icomp);

		resolve_tzid_data_init (&rtd, vcalendar_comp);

		e_cal_recur_generate_instances_sync (
			e_cal_component_get_icalcomponent (comp), starttt, endtt,
			free_busy_instance,
			vfb,
			resolve_tzid_cb,
			&rtd,
			i_cal_timezone_get_utc_timezone (),
			cancellable, NULL);

		resolve_tzid_data_clear (&rtd);
		g_clear_object (&vcalendar_comp);
	}

	g_clear_object (&starttt);
	g_clear_object (&endtt);
	g_object_unref (obj_sexp);

	return vfb;
}

/* Get_free_busy handler for the decsync backend */
static void
e_cal_backend_decsync_get_free_busy (ECalBackendSync *backend,
                                  EDataCal *cal,
                                  GCancellable *cancellable,
                                  const GSList *users,
                                  time_t start,
                                  time_t end,
                                  GSList **freebusy,
                                  GError **error)
{
	ESourceRegistry *registry;
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncPrivate *priv;
	gchar *address, *name;
	ICalComponent *vfb;
	gchar *calobj;
	const GSList *l;

	cbfile = E_CAL_BACKEND_DECSYNC (backend);
	priv = cbfile->priv;

	if (priv->vcalendar == NULL) {
		g_set_error_literal (
			error, E_CAL_CLIENT_ERROR,
			E_CAL_CLIENT_ERROR_NO_SUCH_CALENDAR,
			e_cal_client_error_to_string (
			E_CAL_CLIENT_ERROR_NO_SUCH_CALENDAR));
		return;
	}

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	*freebusy = NULL;

	registry = e_cal_backend_get_registry (E_CAL_BACKEND (backend));

	if (users == NULL) {
		if (e_cal_backend_mail_account_get_default (registry, &address, &name)) {
			vfb = create_user_free_busy (cbfile, address, name, start, end, cancellable);
			calobj = i_cal_component_as_ical_string (vfb);
			*freebusy = g_slist_append (*freebusy, calobj);
			g_object_unref (vfb);
			g_free (address);
			g_free (name);
		}
	} else {
		for (l = users; l != NULL; l = l->next ) {
			address = l->data;
			if (e_cal_backend_mail_account_is_valid (registry, address, &name)) {
				vfb = create_user_free_busy (cbfile, address, name, start, end, cancellable);
				calobj = i_cal_component_as_ical_string (vfb);
				*freebusy = g_slist_append (*freebusy, calobj);
				g_object_unref (vfb);
				g_free (name);
			}
		}
	}

	g_rec_mutex_unlock (&priv->idle_save_rmutex);
}

static void
sanitize_component (ECalBackendDecsync *cbfile,
                    ECalComponent *comp)
{
	ECalComponentDateTime *dt;
	ICalTimezone *zone;

	/* Check dtstart, dtend and due's timezone, and convert it to local
	 * default timezone if the timezone is not in our builtin timezone
	 * list */
	dt = e_cal_component_get_dtstart (comp);
	if (dt && e_cal_component_datetime_get_value (dt) && e_cal_component_datetime_get_tzid (dt)) {
		zone = e_timezone_cache_get_timezone (E_TIMEZONE_CACHE (cbfile), e_cal_component_datetime_get_tzid (dt));
		if (!zone) {
			e_cal_component_datetime_set_tzid (dt, "UTC");
			e_cal_component_set_dtstart (comp, dt);
		}
	}
	e_cal_component_datetime_free (dt);

	dt = e_cal_component_get_dtend (comp);
	if (dt && e_cal_component_datetime_get_value (dt) && e_cal_component_datetime_get_tzid (dt)) {
		zone = e_timezone_cache_get_timezone (E_TIMEZONE_CACHE (cbfile), e_cal_component_datetime_get_tzid (dt));
		if (!zone) {
			e_cal_component_datetime_set_tzid (dt, "UTC");
			e_cal_component_set_dtend (comp, dt);
		}
	}
	e_cal_component_datetime_free (dt);

	dt = e_cal_component_get_due (comp);
	if (dt && e_cal_component_datetime_get_value (dt) && e_cal_component_datetime_get_tzid (dt)) {
		zone = e_timezone_cache_get_timezone (E_TIMEZONE_CACHE (cbfile), e_cal_component_datetime_get_tzid (dt));
		if (!zone) {
			e_cal_component_datetime_set_tzid (dt, "UTC");
			e_cal_component_set_due (comp, dt);
		}
	}
	e_cal_component_datetime_free (dt);

	e_cal_component_abort_sequence (comp);
}

static void
e_cal_backend_decsync_create_objects_with_decsync (ECalBackendSync *backend,
                                   EDataCal *cal,
                                   GCancellable *cancellable,
                                   const GSList *in_calobjs,
                                   guint32 opflags,
                                   GSList **uids,
                                   GSList **new_components,
                                   GError **error,
                                   gboolean update_decsync)
{
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncPrivate *priv;
	GSList *icomps = NULL;
	const GSList *l;

	cbfile = E_CAL_BACKEND_DECSYNC (backend);
	priv = cbfile->priv;

	if (priv->vcalendar == NULL) {
		g_set_error_literal (
			error, E_CAL_CLIENT_ERROR,
			E_CAL_CLIENT_ERROR_NO_SUCH_CALENDAR,
			e_cal_client_error_to_string (
			E_CAL_CLIENT_ERROR_NO_SUCH_CALENDAR));
		return;
	}

	if (uids)
		*uids = NULL;

	*new_components = NULL;

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	/* First step, parse input strings and do uid verification: may fail */
	for (l = in_calobjs; l; l = l->next) {
		ICalComponent *icomp;
		const gchar *comp_uid;

		/* Parse the icalendar text */
		icomp = i_cal_parser_parse_string ((gchar *) l->data);
		if (!icomp) {
			g_slist_free_full (icomps, g_object_unref);
			g_rec_mutex_unlock (&priv->idle_save_rmutex);
			g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_INVALID_OBJECT));
			return;
		}

		/* Append icalcomponent to icalcomps */
		icomps = g_slist_prepend (icomps, icomp);

		/* Check kind with the parent */
		if (i_cal_component_isa (icomp) != e_cal_backend_get_kind (E_CAL_BACKEND (backend))) {
			g_slist_free_full (icomps, g_object_unref);
			g_rec_mutex_unlock (&priv->idle_save_rmutex);
			g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_INVALID_OBJECT));
			return;
		}

		/* Get the UID */
		comp_uid = i_cal_component_get_uid (icomp);
		if (!comp_uid) {
			gchar *new_uid;

			new_uid = e_util_generate_uid ();
			if (!new_uid) {
				g_slist_free_full (icomps, g_object_unref);
				g_rec_mutex_unlock (&priv->idle_save_rmutex);
				g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_INVALID_OBJECT));
				return;
			}

			i_cal_component_set_uid (icomp, new_uid);
			comp_uid = i_cal_component_get_uid (icomp);

			g_free (new_uid);
		}

		/* check that the object is not in our cache */
		if (uid_in_use (cbfile, comp_uid)) {
			g_slist_free_full (icomps, g_object_unref);
			g_rec_mutex_unlock (&priv->idle_save_rmutex);
			g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_ID_ALREADY_EXISTS));
			return;
		}
	}

	icomps = g_slist_reverse (icomps);

	/* Second step, add the objects */
	for (l = icomps; l; l = l->next) {
		ECalComponent *comp;
		ICalTime *current;
		ICalComponent *icomp = l->data;

		/* Create the cal component */
		comp = e_cal_component_new_from_icalcomponent (icomp);
		if (!comp)
			continue;

		/* Set the created and last modified times on the component, if not there already */
		current = i_cal_time_new_current_with_zone (i_cal_timezone_get_utc_timezone ());

		if (!e_cal_util_component_has_property (icomp, I_CAL_CREATED_PROPERTY)) {
			/* Update both when CREATED is missing, to make sure the LAST-MODIFIED
			   is not before CREATED */
			e_cal_component_set_created (comp, current);
			e_cal_component_set_last_modified (comp, current);
		} else if (!e_cal_util_component_has_property (icomp, I_CAL_LASTMODIFIED_PROPERTY)) {
			e_cal_component_set_last_modified (comp, current);
		}

		g_object_unref (current);

		/* sanitize the component*/
		sanitize_component (cbfile, comp);

		/* Add the object */
		add_component (cbfile, comp, TRUE);

		/* Keep the UID and the modified component to return them later */
		if (uids)
			*uids = g_slist_prepend (*uids, g_strdup (i_cal_component_get_uid (icomp)));

		*new_components = g_slist_prepend (*new_components, e_cal_component_clone (comp));
	}

	g_slist_free (icomps);

	/* Save the file */
	save (cbfile, TRUE);

	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	if (uids)
		*uids = g_slist_reverse (*uids);

	*new_components = g_slist_reverse (*new_components);

	if (update_decsync) {
		for (l = *uids; l; l = l->next) {
			gchar *object;
			e_cal_backend_decsync_get_ical (backend, NULL, l->data, NULL, TRUE, &object, NULL);
			writeUpdate (priv->decsync, l->data, object);
			g_free (object);
		}
	}
}


static void
e_cal_backend_decsync_create_objects (ECalBackendSync *backend,
                                   EDataCal *cal,
                                   GCancellable *cancellable,
                                   const GSList *in_calobjs,
                                   guint32 opflags,
                                   GSList **uids,
                                   GSList **new_components,
                                   GError **error)
{
	e_cal_backend_decsync_create_objects_with_decsync (backend, cal, cancellable, in_calobjs, opflags, uids, new_components, error, TRUE);
}

typedef struct {
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncObject *obj_data;
	const gchar *rid;
	ECalObjModType mod;
} RemoveRecurrenceData;

static gboolean
remove_object_instance_cb (gpointer key,
                           gpointer value,
                           gpointer user_data)
{
	time_t fromtt, instancett;
	ICalTime *itt;
	ECalComponent *instance = value;
	RemoveRecurrenceData *rrdata = user_data;

	itt = i_cal_time_new_from_string (rrdata->rid);
	fromtt = i_cal_time_as_timet (itt);
	g_object_unref (itt);

	instancett = get_rid_as_time_t (instance);

	if (fromtt > 0 && instancett > 0) {
		if ((rrdata->mod == E_CAL_OBJ_MOD_THIS_AND_PRIOR && instancett <= fromtt) ||
		    (rrdata->mod == E_CAL_OBJ_MOD_THIS_AND_FUTURE && instancett >= fromtt)) {
			/* remove the component from our data */
			i_cal_component_remove_component (
				rrdata->cbfile->priv->vcalendar,
				e_cal_component_get_icalcomponent (instance));
			rrdata->cbfile->priv->comp = g_list_remove (rrdata->cbfile->priv->comp, instance);

			rrdata->obj_data->recurrences_list = g_list_remove (rrdata->obj_data->recurrences_list, instance);

			return TRUE;
		}
	}

	return FALSE;
}

static void
e_cal_backend_decsync_modify_objects_with_decsync (ECalBackendSync *backend,
                                   EDataCal *cal,
                                   GCancellable *cancellable,
                                   const GSList *calobjs,
                                   ECalObjModType mod,
                                   guint32 opflags,
                                   GSList **old_components,
                                   GSList **new_components,
                                   GError **error,
                                   gboolean update_decsync)
{
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncPrivate *priv;
	GSList *icomps = NULL;
	const GSList *l;
	ResolveTzidData rtd;

	cbfile = E_CAL_BACKEND_DECSYNC (backend);
	priv = cbfile->priv;

	if (priv->vcalendar == NULL) {
		g_set_error_literal (
			error, E_CAL_CLIENT_ERROR,
			E_CAL_CLIENT_ERROR_NO_SUCH_CALENDAR,
			e_cal_client_error_to_string (
			E_CAL_CLIENT_ERROR_NO_SUCH_CALENDAR));
		return;
	}

	resolve_tzid_data_init (&rtd, priv->vcalendar);

	switch (mod) {
	case E_CAL_OBJ_MOD_THIS:
	case E_CAL_OBJ_MOD_THIS_AND_PRIOR:
	case E_CAL_OBJ_MOD_THIS_AND_FUTURE:
	case E_CAL_OBJ_MOD_ALL:
		break;
	default:
		g_propagate_error (error, EC_ERROR (E_CLIENT_ERROR_NOT_SUPPORTED));
		return;
	}

	if (old_components)
		*old_components = NULL;
	if (new_components)
		*new_components = NULL;

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	/* First step, parse input strings and do uid verification: may fail */
	for (l = calobjs; l; l = l->next) {
		const gchar *comp_uid;
		ICalComponent *icomp;

		/* Parse the iCalendar text */
		icomp = i_cal_parser_parse_string (l->data);
		if (!icomp) {
			g_slist_free_full (icomps, g_object_unref);
			g_rec_mutex_unlock (&priv->idle_save_rmutex);
			g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_INVALID_OBJECT));
			return;
		}

		icomps = g_slist_prepend (icomps, icomp);

		/* Check kind with the parent */
		if (i_cal_component_isa (icomp) != e_cal_backend_get_kind (E_CAL_BACKEND (backend))) {
			g_slist_free_full (icomps, g_object_unref);
			g_rec_mutex_unlock (&priv->idle_save_rmutex);
			g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_INVALID_OBJECT));
			return;
		}

		/* Get the uid */
		comp_uid = i_cal_component_get_uid (icomp);

		/* Get the object from our cache */
		if (!g_hash_table_lookup (priv->comp_uid_hash, comp_uid)) {
			g_slist_free_full (icomps, g_object_unref);
			g_rec_mutex_unlock (&priv->idle_save_rmutex);
			g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND));
			return;
		}
	}

	icomps = g_slist_reverse (icomps);

	/* Second step, update the objects */
	for (l = icomps; l; l = l->next) {
		ICalTime *current;
		RemoveRecurrenceData rrdata;
		GList *detached = NULL;
		gchar *rid = NULL;
		const gchar *comp_uid;
		ICalComponent * icomp = l->data, *split_icomp = NULL;
		ECalComponent *comp, *recurrence;
		ECalBackendDecsyncObject *obj_data;
		gpointer value;

		/* Create the cal component */
		comp = e_cal_component_new_from_icalcomponent (icomp);
		if (!comp)
			continue;

		comp_uid = i_cal_component_get_uid (icomp);
		obj_data = g_hash_table_lookup (priv->comp_uid_hash, comp_uid);

		/* Set the last modified time on the component */
		current = i_cal_time_new_current_with_zone (i_cal_timezone_get_utc_timezone ());
		e_cal_component_set_last_modified (comp, current);
		g_object_unref (current);

		/* sanitize the component*/
		sanitize_component (cbfile, comp);
		rid = e_cal_component_get_recurid_as_string (comp);

		/* handle mod_type */
		switch (mod) {
		case E_CAL_OBJ_MOD_THIS:
			if (!rid || !*rid) {
				if (old_components)
					*old_components = g_slist_prepend (*old_components, obj_data->full_object ? e_cal_component_clone (obj_data->full_object) : NULL);

				/* replace only the full object */
				if (obj_data->full_object) {
					i_cal_component_remove_component (
						priv->vcalendar,
						e_cal_component_get_icalcomponent (obj_data->full_object));
					priv->comp = g_list_remove (priv->comp, obj_data->full_object);

					g_object_unref (obj_data->full_object);
				}

				/* add the new object */
				obj_data->full_object = comp;

				e_cal_recur_ensure_end_dates (comp, TRUE, resolve_tzid_cb, &rtd, cancellable, NULL);

				if (!remove_component_from_intervaltree (cbfile, comp)) {
					g_message (G_STRLOC " Could not remove component from interval tree!");
				}

				add_component_to_intervaltree (cbfile, comp);

				i_cal_component_add_component (
					priv->vcalendar,
					e_cal_component_get_icalcomponent (obj_data->full_object));
				priv->comp = g_list_prepend (priv->comp, obj_data->full_object);
				break;
			}

			if (g_hash_table_lookup_extended (obj_data->recurrences, rid, NULL, &value)) {
				recurrence = value;

				if (old_components)
					*old_components = g_slist_prepend (*old_components, e_cal_component_clone (recurrence));

				/* remove the component from our data */
				i_cal_component_remove_component (
					priv->vcalendar,
					e_cal_component_get_icalcomponent (recurrence));
				priv->comp = g_list_remove (priv->comp, recurrence);
				obj_data->recurrences_list = g_list_remove (obj_data->recurrences_list, recurrence);
				g_hash_table_remove (obj_data->recurrences, rid);
			} else {
				if (old_components)
					*old_components = g_slist_prepend (*old_components, NULL);
			}

			/* add the detached instance */
			g_hash_table_insert (
				obj_data->recurrences,
				g_strdup (rid),
				comp);
			i_cal_component_add_component (
				priv->vcalendar,
				e_cal_component_get_icalcomponent (comp));
			priv->comp = g_list_append (priv->comp, comp);
			obj_data->recurrences_list = g_list_append (obj_data->recurrences_list, comp);
			break;
		case E_CAL_OBJ_MOD_THIS_AND_PRIOR:
		case E_CAL_OBJ_MOD_THIS_AND_FUTURE:
			if (!rid || !*rid)
				goto like_mod_all;

			/* remove the component from our data, temporarily */
			if (obj_data->full_object) {
				if (mod == E_CAL_OBJ_MOD_THIS_AND_FUTURE) {
					ICalTime *itt = i_cal_component_get_recurrenceid (icomp);

					if (e_cal_util_is_first_instance (obj_data->full_object, itt, resolve_tzid_cb, &rtd)) {
						ICalProperty *prop = i_cal_component_get_first_property (icomp, I_CAL_RECURRENCEID_PROPERTY);

						g_clear_object (&itt);

						if (prop) {
							i_cal_component_remove_property (icomp, prop);
							g_object_unref (prop);
						}

						goto like_mod_all;
					}

					g_clear_object (&itt);
				}

				i_cal_component_remove_component (
					priv->vcalendar,
					e_cal_component_get_icalcomponent (obj_data->full_object));
				priv->comp = g_list_remove (priv->comp, obj_data->full_object);
			}

			/* now deal with the detached recurrence */
			if (g_hash_table_lookup_extended (obj_data->recurrences, rid, NULL, &value)) {
				recurrence = value;

				if (old_components)
					*old_components = g_slist_prepend (*old_components, e_cal_component_clone (recurrence));

				/* remove the component from our data */
				i_cal_component_remove_component (
					priv->vcalendar,
					e_cal_component_get_icalcomponent (recurrence));
				priv->comp = g_list_remove (priv->comp, recurrence);
				obj_data->recurrences_list = g_list_remove (obj_data->recurrences_list, recurrence);
				g_hash_table_remove (obj_data->recurrences, rid);
			} else {
				if (*old_components)
					*old_components = g_slist_prepend (*old_components, obj_data->full_object ? e_cal_component_clone (obj_data->full_object) : NULL);
			}

			rrdata.cbfile = cbfile;
			rrdata.obj_data = obj_data;
			rrdata.rid = rid;
			rrdata.mod = mod;
			g_hash_table_foreach_remove (obj_data->recurrences, (GHRFunc) remove_object_instance_cb, &rrdata);

			/* add the modified object to the beginning of the list,
			 * so that it's always before any detached instance we
			 * might have */
			if (obj_data->full_object) {
				ICalTime *rid_struct = i_cal_component_get_recurrenceid (icomp), *master_dtstart;
				ICalComponent *master_icomp = e_cal_component_get_icalcomponent (obj_data->full_object);
				ICalProperty *prop = i_cal_component_get_first_property (icomp, I_CAL_RECURRENCEID_PROPERTY);

				if (prop) {
					i_cal_component_remove_property (icomp, prop);
					g_object_unref (prop);
				}

				master_dtstart = i_cal_component_get_dtstart (master_icomp);
				if (master_dtstart && i_cal_time_get_timezone (master_dtstart) &&
					i_cal_time_get_timezone (master_dtstart) != i_cal_time_get_timezone (rid_struct)) {
					i_cal_time_convert_to_zone_inplace (rid_struct, i_cal_time_get_timezone (master_dtstart));
				}

				split_icomp = e_cal_util_split_at_instance (icomp, rid_struct, master_dtstart);
				if (split_icomp) {
					ECalComponent *prev_comp;

					prev_comp = e_cal_component_clone (obj_data->full_object);

					i_cal_time_convert_to_zone_inplace (rid_struct, i_cal_timezone_get_utc_timezone ());

					e_cal_util_remove_instances (e_cal_component_get_icalcomponent (obj_data->full_object), rid_struct, mod);
					e_cal_recur_ensure_end_dates (obj_data->full_object, TRUE, resolve_tzid_cb, &rtd, cancellable, NULL);

					e_cal_backend_notify_component_modified (E_CAL_BACKEND (backend), prev_comp, obj_data->full_object);

					g_clear_object (&prev_comp);
				}

				i_cal_component_add_component (
					priv->vcalendar,
					e_cal_component_get_icalcomponent (obj_data->full_object));
				priv->comp = g_list_prepend (priv->comp, obj_data->full_object);

				g_clear_object (&rid_struct);
				g_clear_object (&master_dtstart);
			} else {
				ICalTime *rid_struct = i_cal_component_get_recurrenceid (icomp);

				split_icomp = e_cal_util_split_at_instance (icomp, rid_struct, NULL);

				g_object_unref (rid_struct);
			}

			if (split_icomp) {
				gchar *new_uid;

				new_uid = e_util_generate_uid ();
				i_cal_component_set_uid (split_icomp, new_uid);
				g_free (new_uid);

				g_warn_if_fail (e_cal_component_set_icalcomponent (comp, split_icomp));
				e_cal_recur_ensure_end_dates (comp, TRUE, resolve_tzid_cb, &rtd, cancellable, NULL);

				/* sanitize the component */
				sanitize_component (cbfile, comp);

				/* Add the object */
				add_component (cbfile, comp, TRUE);
			}
			break;
		case E_CAL_OBJ_MOD_ALL :
 like_mod_all:
			/* Remove the old version */
			if (old_components)
				*old_components = g_slist_prepend (*old_components, obj_data->full_object ? e_cal_component_clone (obj_data->full_object) : NULL);

			if (obj_data->recurrences_list) {
				/* has detached components, preserve them */
				GList *ll;

				for (ll = obj_data->recurrences_list; ll; ll = ll->next) {
					detached = g_list_prepend (detached, g_object_ref (ll->data));
				}
			}

			remove_component (cbfile, comp_uid, obj_data);

			e_cal_recur_ensure_end_dates (comp, TRUE, resolve_tzid_cb, &rtd, cancellable, NULL);

			/* Add the new object */
			add_component (cbfile, comp, TRUE);

			if (detached) {
				/* it had some detached components, place them back */
				comp_uid = i_cal_component_get_uid (e_cal_component_get_icalcomponent (comp));

				if ((obj_data = g_hash_table_lookup (priv->comp_uid_hash, comp_uid)) != NULL) {
					GList *ll;

					for (ll = detached; ll; ll = ll->next) {
						ECalComponent *c = ll->data;

						g_hash_table_insert (obj_data->recurrences, e_cal_component_get_recurid_as_string (c), c);
						i_cal_component_add_component (priv->vcalendar, e_cal_component_get_icalcomponent (c));
						priv->comp = g_list_append (priv->comp, c);
						obj_data->recurrences_list = g_list_append (obj_data->recurrences_list, c);
					}
				}

				g_list_free (detached);
			}
			break;
		/* coverity[dead_error_begin] */
		case E_CAL_OBJ_MOD_ONLY_THIS:
			/* not reached, keep compiler happy */
			g_warn_if_reached ();
			break;
		}

		g_free (rid);

		if (new_components) {
			*new_components = g_slist_prepend (*new_components, e_cal_component_clone (comp));
		}
	}

	resolve_tzid_data_clear (&rtd);

	g_slist_free (icomps);

	/* All the components were updated, now we save the file */
	save (cbfile, TRUE);

	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	if (old_components)
		*old_components = g_slist_reverse (*old_components);

	if (new_components)
		*new_components = g_slist_reverse (*new_components);

	if (update_decsync) {
		for (l = *new_components; l; l = l->next) {
			const gchar *uid;
			gchar *object;
			uid = i_cal_component_get_uid (e_cal_component_get_icalcomponent (l->data));
			e_cal_backend_decsync_get_ical (backend, NULL, uid, NULL, TRUE, &object, NULL);
			writeUpdate (priv->decsync, uid, object);
			g_free (object);
		}
	}
}

static void
e_cal_backend_decsync_modify_objects (ECalBackendSync *backend,
                                   EDataCal *cal,
                                   GCancellable *cancellable,
                                   const GSList *calobjs,
                                   ECalObjModType mod,
                                   guint32 opflags,
                                   GSList **old_components,
                                   GSList **new_components,
                                   GError **error)
{
	e_cal_backend_decsync_modify_objects_with_decsync (backend, cal, cancellable, calobjs, mod, opflags, old_components, new_components, error, TRUE);
}

/**
 * Remove one and only one instance. The object may be empty
 * afterwards, in which case it will be removed completely.
 *
 * @mod    E_CAL_OBJ_MOD_THIS or E_CAL_OBJ_MOD_ONLY_THIS: the later only
 *         removes the instance, the former also adds an EXDATE if rid is set
 *         TODO: E_CAL_OBJ_MOD_ONLY_THIS
 * @uid    pointer to UID which must remain valid even if the object gets
 *         removed
 * @rid    NULL, "", or non-empty string when manipulating a specific recurrence;
 *         also must remain valid
 * @error  may be NULL if caller is not interested in errors
 * @return modified object or NULL if it got removed
 */
static ECalBackendDecsyncObject *
remove_instance (ECalBackendDecsync *cbfile,
                 ECalBackendDecsyncObject *obj_data,
                 const gchar *uid,
                 const gchar *rid,
                 ECalObjModType mod,
                 ECalComponent **old_comp,
                 ECalComponent **new_comp,
                 GError **error)
{
	ECalComponent *comp;
	ICalTime *current;

	/* only check for non-NULL below, empty string is detected here */
	if (rid && !*rid)
		rid = NULL;

	if (rid) {
		ICalTime *rid_struct;
		gpointer value;

		/* remove recurrence */
		if (g_hash_table_lookup_extended (obj_data->recurrences, rid, NULL, &value)) {
			comp = value;

			/* Removing without parent or not modifying parent?
			 * Report removal to caller. */
			if (old_comp &&
			    (!obj_data->full_object || mod == E_CAL_OBJ_MOD_ONLY_THIS)) {
				*old_comp = e_cal_component_clone (comp);
			}

			/* Reporting parent modification to caller?
			 * Report directly instead of going via caller. */
			if (obj_data->full_object &&
			    mod != E_CAL_OBJ_MOD_ONLY_THIS) {
				/* old object string not provided,
				 * instead rely on the view detecting
				 * whether it contains the id */
				ECalComponentId *id;

				id = e_cal_component_id_new (uid, rid);
				e_cal_backend_notify_component_removed (E_CAL_BACKEND (cbfile), id, NULL, NULL);
				e_cal_component_id_free (id);
			}

			/* remove the component from our data */
			i_cal_component_remove_component (
				cbfile->priv->vcalendar,
				e_cal_component_get_icalcomponent (comp));
			cbfile->priv->comp = g_list_remove (cbfile->priv->comp, comp);
			obj_data->recurrences_list = g_list_remove (obj_data->recurrences_list, comp);
			g_hash_table_remove (obj_data->recurrences, rid);
		} else if (mod == E_CAL_OBJ_MOD_ONLY_THIS) {
			if (error)
				g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND));
			return obj_data;
		} else {
			/* not an error, only add EXDATE */
		}
		/* component empty? */
		if (!obj_data->full_object) {
			if (!obj_data->recurrences_list) {
				/* empty now, remove it */
				remove_component (cbfile, uid, obj_data);
				return NULL;
			} else {
				return obj_data;
			}
		}

		/* avoid modifying parent? */
		if (mod == E_CAL_OBJ_MOD_ONLY_THIS)
			return obj_data;

		/* remove the main component from our data before modifying it */
		i_cal_component_remove_component (
			cbfile->priv->vcalendar,
			e_cal_component_get_icalcomponent (obj_data->full_object));
		cbfile->priv->comp = g_list_remove (cbfile->priv->comp, obj_data->full_object);

		/* add EXDATE or EXRULE to parent, report as update */
		if (old_comp) {
			*old_comp = e_cal_component_clone (obj_data->full_object);
		}

		rid_struct = i_cal_time_new_from_string (rid);
		if (!i_cal_time_get_timezone (rid_struct)) {
			ICalTime *master_dtstart = i_cal_component_get_dtstart (e_cal_component_get_icalcomponent (obj_data->full_object));

			if (master_dtstart && i_cal_time_get_timezone (master_dtstart)) {
				i_cal_time_convert_to_zone_inplace (rid_struct, i_cal_time_get_timezone (master_dtstart));
			}

			i_cal_time_convert_to_zone_inplace (rid_struct, i_cal_timezone_get_utc_timezone ());
		}

		e_cal_util_remove_instances (
			e_cal_component_get_icalcomponent (obj_data->full_object),
			rid_struct, E_CAL_OBJ_MOD_THIS);

		g_clear_object (&rid_struct);

		/* Since we are only removing one instance of recurrence
		 * event, update the last modified time on the component */
		current = i_cal_time_new_current_with_zone (i_cal_timezone_get_utc_timezone ());
		e_cal_component_set_last_modified (obj_data->full_object, current);
		g_object_unref (current);

		/* report update */
		if (new_comp) {
			*new_comp = e_cal_component_clone (obj_data->full_object);
		}

		/* add the modified object to the beginning of the list,
		 * so that it's always before any detached instance we
		 * might have */
		i_cal_component_add_component (
			cbfile->priv->vcalendar,
			e_cal_component_get_icalcomponent (obj_data->full_object));
		cbfile->priv->comp = g_list_prepend (cbfile->priv->comp, obj_data->full_object);
	} else {
		if (!obj_data->full_object) {
			/* Nothing to do, parent doesn't exist. Tell
			 * caller about this? Not an error with
			 * E_CAL_OBJ_MOD_THIS. */
			if (mod == E_CAL_OBJ_MOD_ONLY_THIS && error)
				g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND));
			return obj_data;
		}

		/* remove the main component from our data before deleting it */
		if (!remove_component_from_intervaltree (cbfile, obj_data->full_object)) {
			/* return without changing anything */
			g_message (G_STRLOC " Could not remove component from interval tree!");
			return obj_data;
		}
		i_cal_component_remove_component (
			cbfile->priv->vcalendar,
			e_cal_component_get_icalcomponent (obj_data->full_object));
		cbfile->priv->comp = g_list_remove (cbfile->priv->comp, obj_data->full_object);

		/* remove parent, report as removal */
		if (old_comp) {
			*old_comp = g_object_ref (obj_data->full_object);
		}
		g_object_unref (obj_data->full_object);
		obj_data->full_object = NULL;

		/* component may be empty now, check that */
		if (!obj_data->recurrences_list) {
			remove_component (cbfile, uid, obj_data);
			return NULL;
		}
	}

	/* component still exists in a modified form */
	return obj_data;
}

static ECalComponent *
clone_ecalcomp_from_fileobject (ECalBackendDecsyncObject *obj_data,
                                const gchar *rid)
{
	ECalComponent *comp = obj_data->full_object;

	if (!comp)
		return NULL;

	if (rid) {
		gpointer value;

		if (g_hash_table_lookup_extended (obj_data->recurrences, rid, NULL, &value)) {
			comp = value;
		} else {
			/* FIXME remove this once we delete an instance from master object through
			 * modify request by setting exception */
			comp = obj_data->full_object;
		}
	}

	return comp ? e_cal_component_clone (comp) : NULL;
}

static void
notify_comp_removed_cb (gpointer pecalcomp,
                        gpointer pbackend)
{
	ECalComponent *comp = pecalcomp;
	ECalBackend *backend = pbackend;
	ECalComponentId *id;

	g_return_if_fail (comp != NULL);
	g_return_if_fail (backend != NULL);

	id = e_cal_component_get_id (comp);
	g_return_if_fail (id != NULL);

	e_cal_backend_notify_component_removed (backend, id, comp, NULL);

	e_cal_component_id_free (id);
}

/* Remove_object handler for the decsync backend */
static void
e_cal_backend_decsync_remove_objects_with_decsync (ECalBackendSync *backend,
                                   EDataCal *cal,
                                   GCancellable *cancellable,
                                   const GSList *ids,
                                   ECalObjModType mod,
                                   guint32 opflags,
                                   GSList **old_components,
                                   GSList **new_components,
                                   GError **error,
                                   gboolean update_decsync)
{
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncPrivate *priv;
	const GSList *l;

	cbfile = E_CAL_BACKEND_DECSYNC (backend);
	priv = cbfile->priv;

	if (priv->vcalendar == NULL) {
		g_set_error_literal (
			error, E_CAL_CLIENT_ERROR,
			E_CAL_CLIENT_ERROR_NO_SUCH_CALENDAR,
			e_cal_client_error_to_string (
			E_CAL_CLIENT_ERROR_NO_SUCH_CALENDAR));
		return;
	}

	switch (mod) {
	case E_CAL_OBJ_MOD_THIS:
	case E_CAL_OBJ_MOD_THIS_AND_PRIOR:
	case E_CAL_OBJ_MOD_THIS_AND_FUTURE:
	case E_CAL_OBJ_MOD_ONLY_THIS:
	case E_CAL_OBJ_MOD_ALL:
		break;
	default:
		g_propagate_error (error, EC_ERROR (E_CLIENT_ERROR_NOT_SUPPORTED));
		return;
	}

	*old_components = *new_components = NULL;

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	/* First step, validate the input */
	for (l = ids; l; l = l->next) {
		ECalComponentId *id = l->data;
		/* Make the ID contains a uid */
		if (!id || !e_cal_component_id_get_uid (id)) {
			g_rec_mutex_unlock (&priv->idle_save_rmutex);
			g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND));
			return;
		}
				/* Check that it has a recurrence id if mod is E_CAL_OBJ_MOD_THIS_AND_PRIOR
					 or E_CAL_OBJ_MOD_THIS_AND_FUTURE */
		if ((mod == E_CAL_OBJ_MOD_THIS_AND_PRIOR || mod == E_CAL_OBJ_MOD_THIS_AND_FUTURE) &&
			!e_cal_component_id_get_rid (id)) {
			g_rec_mutex_unlock (&priv->idle_save_rmutex);
			g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND));
			return;
		}
				/* Make sure the uid exists in the local hash table */
		if (!g_hash_table_lookup (priv->comp_uid_hash, e_cal_component_id_get_uid (id))) {
			g_rec_mutex_unlock (&priv->idle_save_rmutex);
			g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND));
			return;
		}
	}

	/* Second step, remove objects from the calendar */
	for (l = ids; l; l = l->next) {
		const gchar *recur_id = NULL;
		ECalComponent *comp;
		RemoveRecurrenceData rrdata;
		ECalBackendDecsyncObject *obj_data;
		ECalComponentId *id = l->data;

		obj_data = g_hash_table_lookup (priv->comp_uid_hash, e_cal_component_id_get_uid (id));
		recur_id = e_cal_component_id_get_rid (id);

		switch (mod) {
		case E_CAL_OBJ_MOD_ALL :
			*old_components = g_slist_prepend (*old_components, clone_ecalcomp_from_fileobject (obj_data, recur_id));
			*new_components = g_slist_prepend (*new_components, NULL);

			if (obj_data->recurrences_list)
				g_list_foreach (obj_data->recurrences_list, notify_comp_removed_cb, cbfile);
			remove_component (cbfile, e_cal_component_id_get_uid (id), obj_data);
			break;
		case E_CAL_OBJ_MOD_ONLY_THIS:
		case E_CAL_OBJ_MOD_THIS: {
			ECalComponent *old_component = NULL;
			ECalComponent *new_component = NULL;

			remove_instance (
				cbfile, obj_data, e_cal_component_id_get_uid (id), recur_id, mod,
				&old_component, &new_component, error);

			*old_components = g_slist_prepend (*old_components, old_component);
			*new_components = g_slist_prepend (*new_components, new_component);
			break;
		}
		case E_CAL_OBJ_MOD_THIS_AND_PRIOR:
		case E_CAL_OBJ_MOD_THIS_AND_FUTURE:
			comp = obj_data->full_object;

			if (comp) {
				ICalTime *rid_struct;

				*old_components = g_slist_prepend (*old_components, e_cal_component_clone (comp));

				/* remove the component from our data, temporarily */
				i_cal_component_remove_component (
					priv->vcalendar,
					e_cal_component_get_icalcomponent (comp));
				priv->comp = g_list_remove (priv->comp, comp);

				rid_struct = i_cal_time_new_from_string (recur_id);
				if (!i_cal_time_get_timezone (rid_struct)) {
					ICalTime *master_dtstart = i_cal_component_get_dtstart (e_cal_component_get_icalcomponent (comp));

					if (master_dtstart && i_cal_time_get_timezone (master_dtstart)) {
						i_cal_time_convert_to_zone_inplace (rid_struct, i_cal_time_get_timezone (master_dtstart));
					}

					i_cal_time_convert_to_zone_inplace (rid_struct, i_cal_timezone_get_utc_timezone ());
				}
				e_cal_util_remove_instances (
					e_cal_component_get_icalcomponent (comp),
					rid_struct, mod);

				g_object_unref (rid_struct);
			} else {
				*old_components = g_slist_prepend (*old_components, NULL);
			}

			/* now remove all detached instances */
			rrdata.cbfile = cbfile;
			rrdata.obj_data = obj_data;
			rrdata.rid = recur_id;
			rrdata.mod = mod;
			g_hash_table_foreach_remove (obj_data->recurrences, (GHRFunc) remove_object_instance_cb, &rrdata);

			/* add the modified object to the beginning of the list,
			 * so that it's always before any detached instance we
			 * might have */
			if (comp)
				priv->comp = g_list_prepend (priv->comp, comp);

			if (obj_data->full_object) {
				*new_components = g_slist_prepend (*new_components, e_cal_component_clone (obj_data->full_object));
			} else {
				*new_components = g_slist_prepend (*new_components, NULL);
			}
			break;
		}
	}

	save (cbfile, TRUE);

	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	*old_components = g_slist_reverse (*old_components);
	*new_components = g_slist_reverse (*new_components);

	if (update_decsync) {
		for (l = *old_components; l; l = l->next) {
			const gchar *uid;
			gchar *object = NULL;
			uid = i_cal_component_get_uid (e_cal_component_get_icalcomponent (l->data));
			e_cal_backend_decsync_get_ical (backend, NULL, uid, NULL, TRUE, &object, NULL);
			writeUpdate (priv->decsync, uid, object);
			g_free (object);
		}
	}
}

static void
e_cal_backend_decsync_remove_objects (ECalBackendSync *backend,
                                   EDataCal *cal,
                                   GCancellable *cancellable,
                                   const GSList *ids,
                                   ECalObjModType mod,
                                   guint32 opflags,
                                   GSList **old_components,
                                   GSList **new_components,
                                   GError **error)
{
	e_cal_backend_decsync_remove_objects_with_decsync (backend, cal, cancellable, ids, mod, opflags, old_components, new_components, error, TRUE);
}

static gboolean
cancel_received_object (ECalBackendDecsync *cbfile,
                        ECalComponent *comp,
                        ECalComponent **old_comp,
                        ECalComponent **new_comp)
{
	ECalBackendDecsyncObject *obj_data;
	ECalBackendDecsyncPrivate *priv;
	gchar *rid;
	const gchar *uid;

	priv = cbfile->priv;

	*old_comp = NULL;
	*new_comp = NULL;

	uid = e_cal_component_get_uid (comp);

	/* Find the old version of the component. */
	obj_data = uid ? g_hash_table_lookup (priv->comp_uid_hash, uid) : NULL;
	if (!obj_data)
		return FALSE;

	/* And remove it */
	rid = e_cal_component_get_recurid_as_string (comp);
	if (rid && *rid) {
		obj_data = remove_instance (
			cbfile, obj_data, uid, rid, E_CAL_OBJ_MOD_THIS,
			old_comp, new_comp, NULL);
		if (obj_data && obj_data->full_object && !*new_comp) {
			*new_comp = e_cal_component_clone (obj_data->full_object);
		}
	} else {
		/* report as removal by keeping *new_component NULL */
		if (obj_data->full_object) {
			*old_comp = e_cal_component_clone (obj_data->full_object);
		}
		remove_component (cbfile, uid, obj_data);
	}

	g_free (rid);

	return TRUE;
}

typedef struct {
	GHashTable *zones;

	gboolean found;
} ECalBackendDecsyncTzidData;

static void
check_tzids (ICalParameter *param,
             gpointer data)
{
	ECalBackendDecsyncTzidData *tzdata = data;
	const gchar *tzid;

	tzid = i_cal_parameter_get_tzid (param);
	if (!tzid || g_hash_table_lookup (tzdata->zones, tzid))
		tzdata->found = FALSE;
}

/* This function is largely duplicated in
 * ../groupwise/e-cal-backend-groupwise.c
 */
static void
fetch_attachments (ECalBackendSync *backend,
                   ECalComponent *comp)
{
	GSList *attach_list;
	GSList *l;
	gchar *dest_url, *dest_file;
	gint fd, fileindex;
	const gchar *uid;

	attach_list = e_cal_component_get_attachments (comp);
	uid = e_cal_component_get_uid (comp);

	for (l = attach_list, fileindex = 0; l; l = l->next, fileindex++) {
		ICalAttach *attach = l->data;
		gchar *sfname;
		gchar *filename;
		GMappedFile *mapped_file;
		GError *error = NULL;

		if (!attach || !i_cal_attach_get_is_url (attach))
			continue;

		sfname = g_filename_from_uri (i_cal_attach_get_url (attach), NULL, NULL);
		if (!sfname)
			continue;

		mapped_file = g_mapped_file_new (sfname, FALSE, &error);
		if (!mapped_file) {
			g_message (
				"DEBUG: could not map %s: %s\n",
				sfname, error ? error->message : "???");
			g_error_free (error);
			g_free (sfname);
			continue;
		}
		filename = g_path_get_basename (sfname);
		dest_file = e_cal_backend_create_cache_filename (E_CAL_BACKEND (backend), uid, filename, fileindex);
		g_free (filename);
		fd = g_open (dest_file, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0600);
		if (fd == -1) {
			/* TODO handle error conditions */
			g_message (
				"DEBUG: could not open %s for writing\n",
				dest_file);
		} else if (write (fd, g_mapped_file_get_contents (mapped_file),
				  g_mapped_file_get_length (mapped_file)) == -1) {
			/* TODO handle error condition */
			g_message ("DEBUG: attachment write failed.\n");
		}

		g_mapped_file_unref (mapped_file);

		if (fd != -1)
			close (fd);
		dest_url = g_filename_to_uri (dest_file, NULL, NULL);
		g_free (dest_file);

		g_object_unref (attach);
		l->data = i_cal_attach_new_from_url (dest_url);

		g_free (dest_url);
		g_free (sfname);
	}

	e_cal_component_set_attachments (comp, attach_list);

	g_slist_free_full (attach_list, g_object_unref);
}

static gint
masters_first_cmp (gconstpointer ptr1,
		   gconstpointer ptr2)
{
	ICalComponent *icomp1 = (ICalComponent *) ptr1;
	ICalComponent *icomp2 = (ICalComponent *) ptr2;
	gboolean has_rid1, has_rid2;

	has_rid1 = (icomp1 && e_cal_util_component_has_property (icomp1, I_CAL_RECURRENCEID_PROPERTY)) ? 1 : 0;
	has_rid2 = (icomp2 && e_cal_util_component_has_property (icomp2, I_CAL_RECURRENCEID_PROPERTY)) ? 1 : 0;

	if (has_rid1 == has_rid2)
		return g_strcmp0 (icomp1 ? i_cal_component_get_uid (icomp1) : NULL,
				  icomp2 ? i_cal_component_get_uid (icomp2) : NULL);

	if (has_rid1)
		return 1;

	return -1;
}

static gint
masters_uid_cmp (gconstpointer ptr1, gconstpointer ptr2)
{
	icalcomponent *icomp1 = (icalcomponent *) ptr1;
	icalcomponent *icomp2 = (icalcomponent *) ptr2;

	return g_strcmp0 (icomp1 ? icalcomponent_get_uid (icomp1) : NULL,
	                  icomp2 ? icalcomponent_get_uid (icomp2) : NULL);
}

static void
e_cal_backend_decsync_receive_objects_with_decsync (ECalBackendSync *backend,
                                                 GCancellable *cancellable,
                                                 const gchar *calobj,
                                                 guint32 opflags,
                                                 gboolean update_decsync,
                                                 GError **error)
{
	ESourceRegistry *registry;
	ECalBackendDecsync *cbfile;
	ECalBackendDecsyncPrivate *priv;
	ECalClientTzlookupICalCompData *lookup_data = NULL;
	ICalComponent *toplevel_comp, *icomp = NULL;
	ICalComponentKind kind;
	ICalPropertyMethod toplevel_method, method;
	ICalComponent *subcomp;
	GSList *comps = NULL, *del_comps = NULL, *link;
	ECalComponent *comp;
	ECalBackendDecsyncTzidData tzdata;
	GError *err = NULL;

	cbfile = E_CAL_BACKEND_DECSYNC (backend);
	priv = cbfile->priv;

	if (priv->vcalendar == NULL) {
		g_set_error_literal (
			error, E_CAL_CLIENT_ERROR,
			E_CAL_CLIENT_ERROR_NO_SUCH_CALENDAR,
			e_cal_client_error_to_string (
			E_CAL_CLIENT_ERROR_NO_SUCH_CALENDAR));
		return;
	}

	/* Pull the component from the string and ensure that it is sane */
	toplevel_comp = i_cal_parser_parse_string (calobj);
	if (!toplevel_comp) {
		g_propagate_error (error, ECC_ERROR (E_CAL_CLIENT_ERROR_INVALID_OBJECT));
		return;
	}

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	registry = e_cal_backend_get_registry (E_CAL_BACKEND (backend));

	kind = i_cal_component_isa (toplevel_comp);
	if (kind != I_CAL_VCALENDAR_COMPONENT) {
		/* If its not a VCALENDAR, make it one to simplify below */
		icomp = toplevel_comp;
		toplevel_comp = e_cal_util_new_top_level ();
		if (i_cal_component_get_method (icomp) == I_CAL_METHOD_CANCEL)
			i_cal_component_set_method (toplevel_comp, I_CAL_METHOD_CANCEL);
		else
			i_cal_component_set_method (toplevel_comp, I_CAL_METHOD_PUBLISH);
		i_cal_component_add_component (toplevel_comp, icomp);
	} else {
		if (!e_cal_util_component_has_property (toplevel_comp, I_CAL_METHOD_PROPERTY))
			i_cal_component_set_method (toplevel_comp, I_CAL_METHOD_PUBLISH);
	}

	toplevel_method = i_cal_component_get_method (toplevel_comp);

	/* Build a list of timezones so we can make sure all the objects have valid info */
	tzdata.zones = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	for (subcomp = i_cal_component_get_first_component (toplevel_comp, I_CAL_VTIMEZONE_COMPONENT);
	     subcomp;
	     g_object_unref (subcomp), subcomp = i_cal_component_get_next_component (toplevel_comp, I_CAL_VTIMEZONE_COMPONENT)) {
		ICalTimezone *zone;

		zone = i_cal_timezone_new ();
		if (i_cal_timezone_set_component (zone, subcomp))
			g_hash_table_insert (tzdata.zones, g_strdup (i_cal_timezone_get_tzid (zone)), NULL);
		g_object_unref (zone);
	}

	/* First we make sure all the components are usuable */
	kind = e_cal_backend_get_kind (E_CAL_BACKEND (backend));

	for (subcomp = i_cal_component_get_first_component (toplevel_comp, I_CAL_ANY_COMPONENT);
	     subcomp;
	     g_object_unref (subcomp), subcomp = i_cal_component_get_next_component (toplevel_comp, I_CAL_ANY_COMPONENT)) {
		ICalComponentKind child_kind = i_cal_component_isa (subcomp);

		if (child_kind != kind) {
			/* remove the component from the toplevel VCALENDAR */
			if (child_kind != I_CAL_VTIMEZONE_COMPONENT)
				del_comps = g_slist_prepend (del_comps, g_object_ref (subcomp));
			continue;
		}

		tzdata.found = TRUE;
		i_cal_component_foreach_tzid (subcomp, check_tzids, &tzdata);

		if (!tzdata.found) {
			err = ECC_ERROR (E_CAL_CLIENT_ERROR_INVALID_OBJECT);
			g_object_unref (subcomp);
			goto error;
		}

		if (!i_cal_component_get_uid (subcomp)) {
			if (toplevel_method == I_CAL_METHOD_PUBLISH) {
				gchar *new_uid = NULL;

				new_uid = e_util_generate_uid ();
				i_cal_component_set_uid (subcomp, new_uid);
				g_free (new_uid);
			} else {
				err = ECC_ERROR (E_CAL_CLIENT_ERROR_INVALID_OBJECT);
				g_object_unref (subcomp);
				goto error;
			}

		}

		comps = g_slist_prepend (comps, g_object_ref (subcomp));
	}

	/* Now we remove the components we don't care about */
	for (link = del_comps; link; link = g_slist_next (link)) {
		subcomp = link->data;

		i_cal_component_remove_component (toplevel_comp, subcomp);
	}

	g_slist_free_full (del_comps, g_object_unref);
	del_comps = NULL;

	lookup_data = e_cal_client_tzlookup_icalcomp_data_new (priv->vcalendar);

        /* check and patch timezones */
	if (!e_cal_client_check_timezones_sync (toplevel_comp,
			       NULL,
			       e_cal_client_tzlookup_icalcomp_cb,
			       lookup_data,
			       NULL,
			       &err)) {
		/*
		 * This makes assumptions about what kind of
		 * errors can occur inside e_cal_check_timezones().
		 * We control it, so that should be safe, but
		 * is the code really identical with the calendar
		 * status?
		 */
		goto error;
	}

	/* Merge the iCalendar components with our existing VCALENDAR,
	 * resolving any conflicting TZIDs. It also frees the toplevel_comp. */
	i_cal_component_merge_component (priv->vcalendar, toplevel_comp);
	g_clear_object (&toplevel_comp);

	/* Now we manipulate the components we care about */
	comps = g_slist_sort (comps, masters_first_cmp);

	for (link = comps; link; link = g_slist_next (link)) {
		ECalComponent *old_component = NULL;
		ECalComponent *new_component = NULL;
		ICalTime *current;
		const gchar *uid;
		gchar *rid;
		ECalBackendDecsyncObject *obj_data;
		gboolean is_declined;

		subcomp = link->data;

		/* Create the cal component */
		comp = e_cal_component_new_from_icalcomponent (g_object_ref (subcomp));
		if (!comp)
			continue;

		/* Set the created and last modified times on the component, if not there already */
		current = i_cal_time_new_current_with_zone (i_cal_timezone_get_utc_timezone ());

		if (!e_cal_util_component_has_property (subcomp, I_CAL_CREATED_PROPERTY)) {
			/* Update both when CREATED is missing, to make sure the LAST-MODIFIED
			   is not before CREATED */
			e_cal_component_set_created (comp, current);
			e_cal_component_set_last_modified (comp, current);
		} else if (!e_cal_util_component_has_property (subcomp, I_CAL_LASTMODIFIED_PROPERTY)) {
			e_cal_component_set_last_modified (comp, current);
		}

		g_clear_object (&current);

		uid = e_cal_component_get_uid (comp);
		rid = e_cal_component_get_recurid_as_string (comp);

		if (e_cal_util_component_has_property (subcomp, I_CAL_METHOD_PROPERTY))
			method = i_cal_component_get_method (subcomp);
		else
			method = toplevel_method;

		switch (method) {
		case I_CAL_METHOD_PUBLISH:
		case I_CAL_METHOD_REQUEST:
		case I_CAL_METHOD_REPLY:
			is_declined = e_cal_backend_user_declined (registry, subcomp);

			/* handle attachments */
			if (!is_declined && e_cal_component_has_attachments (comp))
				fetch_attachments (backend, comp);
			obj_data = g_hash_table_lookup (priv->comp_uid_hash, uid);
			if (obj_data) {

				if (rid) {
					ECalComponent *ignore_comp = NULL;

					remove_instance (
						cbfile, obj_data, uid, rid, E_CAL_OBJ_MOD_THIS,
						&old_component, &ignore_comp, NULL);

					if (ignore_comp)
						g_object_unref (ignore_comp);
				} else {
					if (obj_data->full_object) {
						old_component = e_cal_component_clone (obj_data->full_object);
					}
					remove_component (cbfile, uid, obj_data);
				}

				if (!is_declined)
					add_component (cbfile, comp, FALSE);

				if (!is_declined)
					e_cal_backend_notify_component_modified (E_CAL_BACKEND (backend),
										 old_component, comp);
				else {
					ECalComponentId *id = e_cal_component_get_id (comp);

					e_cal_backend_notify_component_removed (E_CAL_BACKEND (backend),
										id, old_component,
										rid ? comp : NULL);

					e_cal_component_id_free (id);
					g_object_unref (comp);
				}

				if (old_component)
					g_object_unref (old_component);

			} else if (!is_declined) {
				add_component (cbfile, comp, FALSE);

				e_cal_backend_notify_component_created (E_CAL_BACKEND (backend), comp);
			} else {
				g_object_unref (comp);
			}
			g_free (rid);
			break;
		case I_CAL_METHOD_ADD:
			/* FIXME This should be doable once all the recurid stuff is done */
			err = EC_ERROR_EX (E_CLIENT_ERROR_OTHER_ERROR, _("Unsupported method"));
			g_object_unref (comp);
			g_free (rid);
			goto error;
			break;
		case I_CAL_METHOD_COUNTER:
			err = EC_ERROR_EX (E_CLIENT_ERROR_OTHER_ERROR, _("Unsupported method"));
			g_object_unref (comp);
			g_free (rid);
			goto error;
			break;
		case I_CAL_METHOD_DECLINECOUNTER:
			err = EC_ERROR_EX (E_CLIENT_ERROR_OTHER_ERROR, _("Unsupported method"));
			g_object_unref (comp);
			g_free (rid);
			goto error;
			break;
		case I_CAL_METHOD_CANCEL:
			if (cancel_received_object (cbfile, comp, &old_component, &new_component)) {
				ECalComponentId *id;

				id = e_cal_component_get_id (comp);

				e_cal_backend_notify_component_removed (E_CAL_BACKEND (backend),
									id, old_component, new_component);

				/* remove the component from the toplevel VCALENDAR */
				i_cal_component_remove_component (priv->vcalendar, subcomp);
				e_cal_component_id_free (id);

				if (new_component)
					g_object_unref (new_component);
				if (old_component)
					g_object_unref (old_component);
			}
			g_object_unref (comp);
			g_free (rid);
			break;
		default:
			err = EC_ERROR_EX (E_CLIENT_ERROR_OTHER_ERROR, _("Unsupported method"));
			g_object_unref (comp);
			g_free (rid);
			goto error;
		}
	}

	save (cbfile, TRUE);

	if (update_decsync) {
		const gchar *prev_uid = NULL;
		comps = g_slist_sort (comps, masters_uid_cmp);
		for (link = comps; link; link = g_slist_next (link)) {
			const gchar *uid;
			gchar *object = NULL;

			subcomp = link->data;
			uid = i_cal_component_get_uid (subcomp);
			if (g_strcmp0(prev_uid, uid)) {
				e_cal_backend_decsync_get_ical (backend, NULL, uid, NULL, TRUE, &object, NULL);
				writeUpdate (priv->decsync, uid, object);
				g_free (object);
			}
			prev_uid = uid;
		}
	}

 error:
	g_slist_free_full (del_comps, g_object_unref);
	g_slist_free_full (comps, g_object_unref);

	g_hash_table_destroy (tzdata.zones);
	g_rec_mutex_unlock (&priv->idle_save_rmutex);
	e_cal_client_tzlookup_icalcomp_data_free (lookup_data);

	if (err)
		g_propagate_error (error, err);
}

/* Update_objects handler for the decsync backend. */
static void
e_cal_backend_decsync_receive_objects (ECalBackendSync *backend,
                                    EDataCal *cal,
                                    GCancellable *cancellable,
                                    const gchar *calobj,
                                    guint32 opflags,
                                    GError **error)
{
	e_cal_backend_decsync_receive_objects_with_decsync (backend, cancellable, calobj, opflags, TRUE, error);
}

static void
e_cal_backend_decsync_send_objects (ECalBackendSync *backend,
                                 EDataCal *cal,
                                 GCancellable *cancellable,
                                 const gchar *calobj,
                                 guint32 opflags,
                                 GSList **users,
                                 gchar **modified_calobj,
                                 GError **perror)
{
	*users = NULL;
	*modified_calobj = g_strdup (calobj);
}

static void
cal_backend_decsync_constructed (GObject *object)
{
	ECalBackend *backend;
	ESourceRegistry *registry;
	ESource *builtin_source;
	ESource *source;
	ICalComponentKind kind;
	const gchar *user_data_dir;
	const gchar *component_type;
	const gchar *uid;
	gchar *filename;

	user_data_dir = e_get_user_data_dir ();

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_cal_backend_decsync_parent_class)->constructed (object);

	/* Override the cache directory that the parent class just set. */

	backend = E_CAL_BACKEND (object);
	kind = e_cal_backend_get_kind (backend);
	source = e_backend_get_source (E_BACKEND (backend));
	registry = e_cal_backend_get_registry (E_CAL_BACKEND (backend));

	uid = e_source_get_uid (source);
	g_return_if_fail (uid != NULL);

	switch (kind) {
		case I_CAL_VEVENT_COMPONENT:
			component_type = "calendar";
			builtin_source = e_source_registry_ref_builtin_calendar (registry);
			break;
		default:
			g_warn_if_reached ();
			component_type = "calendar";
			builtin_source = e_source_registry_ref_builtin_calendar (registry);
			break;
	}

	/* XXX Backward-compatibility hack:
	 *
	 * The special built-in "Personal" data source UIDs are now named
	 * "system-$COMPONENT" but since the data directories are already
	 * split out by component, we'll continue to use the old "system"
	 * directories for these particular data sources. */
	if (e_source_equal (source, builtin_source))
		uid = "system";

	filename = g_build_filename (user_data_dir, component_type, uid, NULL);
	e_cal_backend_set_cache_dir (backend, filename);
	g_free (filename);

	g_object_unref (builtin_source);
}

static void
cal_backend_decsync_add_cached_timezone (ETimezoneCache *cache,
                                      ICalTimezone *zone)
{
	ECalBackendDecsyncPrivate *priv;
	const gchar *tzid;
	gboolean timezone_added = FALSE;

	priv = E_CAL_BACKEND_DECSYNC (cache)->priv;

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	tzid = i_cal_timezone_get_tzid (zone);
	if (!i_cal_component_get_timezone (priv->vcalendar, tzid)) {
		ICalComponent *tz_comp;

		tz_comp = i_cal_timezone_get_component (zone);

		i_cal_component_take_component (priv->vcalendar, i_cal_component_clone (tz_comp));

		g_clear_object (&tz_comp);

		timezone_added = TRUE;
		save (E_CAL_BACKEND_DECSYNC (cache), TRUE);
	}

	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	/* Emit the signal outside of the mutex. */
	if (timezone_added)
		g_signal_emit_by_name (cache, "timezone-added", zone);
}

static ICalTimezone *
cal_backend_decsync_get_cached_timezone (ETimezoneCache *cache,
                                      const gchar *tzid)
{
	ECalBackendDecsyncPrivate *priv;
	ICalTimezone *zone;

	priv = E_CAL_BACKEND_DECSYNC (cache)->priv;

	g_rec_mutex_lock (&priv->idle_save_rmutex);
	zone = g_hash_table_lookup (priv->cached_timezones, tzid);
	if (!zone) {
		zone = i_cal_component_get_timezone (priv->vcalendar, tzid);
		if (zone)
			g_hash_table_insert (priv->cached_timezones, g_strdup (tzid), zone);
	}
	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	if (zone != NULL)
		return zone;

	/* Chain up and let ECalBackend try to match
	 * the TZID against a built-in ICalTimezone. */
	return parent_timezone_cache_interface->tzcache_get_timezone (cache, tzid);
}

static GList *
cal_backend_decsync_list_cached_timezones (ETimezoneCache *cache)
{
	/* XXX As of 3.7, the only e_timezone_cache_list_timezones()
	 *     call comes from ECalBackendStore, which this backend
	 *     does not use.  So we should never get here.  Emit a
	 *     runtime warning so we know if this changes. */

	g_return_val_if_reached (NULL);
}

/****************************************************************
 *                         DecSync updates                      *
 ****************************************************************/

static void
deleteCal (Extra *extra, void *user_data)
{
	ECalBackend *backend;
	ESource *source;

	backend = E_CAL_BACKEND (extra->backend);
	source = e_backend_get_source (E_BACKEND (backend));
	e_source_remove_sync (source, NULL, NULL);
}

static void
updateColor (Extra *extra, const gchar *color, void *user_data)
{
	ECalBackend *backend;
	ESource *source;
	const gchar *extension_name;
	ESourceExtension *extension;

	backend = E_CAL_BACKEND (extra->backend);
	source = e_backend_get_source (E_BACKEND (backend));

	extension_name = E_SOURCE_EXTENSION_CALENDAR;
	extension = e_source_get_extension (source, extension_name);
	e_source_selectable_set_color (E_SOURCE_SELECTABLE (extension), color);
	e_source_write_sync (source, NULL, NULL);
}

static void
updateEvent (const gchar *uid, const gchar *ical, Extra *extra, void *user_data)
{
	ECalBackendSync *backend;

	backend = E_CAL_BACKEND_SYNC (extra->backend);
	e_cal_backend_decsync_receive_objects_with_decsync (backend, NULL, ical, 0, FALSE, NULL);
}

static void
removeEvent (const gchar *uid, Extra *extra, void *user_data)
{
	ECalBackend *backend;
	ECalComponentId *id;
	GSList *ids;
	GSList *old_components, *new_components;

	backend = E_CAL_BACKEND (extra->backend);

	id = e_cal_component_id_new (uid, NULL);
	ids = g_slist_prepend (NULL, id);
	e_cal_backend_decsync_remove_objects_with_decsync (E_CAL_BACKEND_SYNC (backend), NULL, NULL,
			ids, E_CAL_OBJ_MOD_ALL, 0, &old_components, &new_components, NULL, FALSE);
	if (old_components && new_components) {
		e_cal_backend_notify_component_removed (backend, id, old_components->data, new_components->data);
		g_slist_free (old_components);
		g_slist_free (new_components);
	}
	e_cal_component_id_free (id);
	g_slist_free (ids);
}

static gboolean
getDecsyncFromSource (Decsync **decsync, ESource *source)
{
	ESourceDecsync *decsync_extension;
	const gchar *extension_name, *decsync_dir, *collection, *appid;

	extension_name = E_SOURCE_EXTENSION_DECSYNC_BACKEND;
	decsync_extension = e_source_get_extension (source, extension_name);
	decsync_dir = e_source_decsync_get_decsync_dir (decsync_extension);
	collection = e_source_decsync_get_collection (decsync_extension);
	appid = e_source_decsync_get_appid (decsync_extension);
	return getDecsync(decsync,
		decsync_dir, "calendars", collection, appid,
		deleteCal, NULL, NULL,
		updateColor, NULL, NULL,
		updateEvent, NULL, NULL,
		removeEvent, NULL, NULL);
}

static gboolean
ecal_backend_decsync_refresh_cb (gpointer backend)
{
	ECalBackendDecsync *cbfile;

	cbfile = E_CAL_BACKEND_DECSYNC (backend);
	decsync_executeAllNewEntries (cbfile->priv->decsync, extra_new (cbfile));
	return TRUE;
}

static gboolean
ecal_backend_decsync_refresh_start (ECalBackendDecsync *cbfile)
{
	ESource *source;
	ESourceRefresh *extension;
	const gchar *extension_name;
	guint interval_in_minutes = 0;

	source = e_backend_get_source (E_BACKEND (cbfile));

	extension_name = E_SOURCE_EXTENSION_REFRESH;
	extension = e_source_get_extension (source, extension_name);

	if (e_source_refresh_get_enabled (extension)) {
		interval_in_minutes = e_source_refresh_get_interval_minutes (extension);
		if (interval_in_minutes == 0)
			interval_in_minutes = 30;
	}

	if (interval_in_minutes > 0) {
		e_named_timeout_add_seconds (interval_in_minutes * 60, ecal_backend_decsync_refresh_cb, cbfile);
	}
	return FALSE;
}

static void
ecal_backend_decsync_refresh_sync (ECalBackendSync *backend,
                                 EDataCal *cal,
                                 GCancellable *cancellable,
                                 GError **error)
{
	ecal_backend_decsync_refresh_cb (backend);
}

static gboolean
cal_backend_decsync_initable_init (GInitable *initable,
                                 GCancellable *cancellable,
                                 GError **error)
{
	ESource *source;
	ECalBackendDecsyncPrivate *priv;

	source = e_backend_get_source (E_BACKEND (initable));
	priv = E_CAL_BACKEND_DECSYNC (initable)->priv;

	return getDecsyncFromSource (&priv->decsync, source);
}

static void
e_cal_backend_decsync_class_init (ECalBackendDecsyncClass *class)
{
	GObjectClass *object_class;
	ECalBackendClass *backend_class;
	ECalBackendSyncClass *sync_class;

	object_class = (GObjectClass *) class;
	backend_class = (ECalBackendClass *) class;
	sync_class = (ECalBackendSyncClass *) class;

	object_class->dispose = e_cal_backend_decsync_dispose;
	object_class->finalize = e_cal_backend_decsync_finalize;
	object_class->constructed = cal_backend_decsync_constructed;

	backend_class->impl_get_backend_property = e_cal_backend_decsync_get_backend_property;
	backend_class->impl_start_view = e_cal_backend_decsync_start_view;

	sync_class->open_sync = e_cal_backend_decsync_open;
	sync_class->create_objects_sync = e_cal_backend_decsync_create_objects;
	sync_class->modify_objects_sync = e_cal_backend_decsync_modify_objects;
	sync_class->remove_objects_sync = e_cal_backend_decsync_remove_objects;
	sync_class->receive_objects_sync = e_cal_backend_decsync_receive_objects;
	sync_class->send_objects_sync = e_cal_backend_decsync_send_objects;
	sync_class->get_object_sync = e_cal_backend_decsync_get_object;
	sync_class->get_object_list_sync = e_cal_backend_decsync_get_object_list;
	sync_class->get_attachment_uris_sync = e_cal_backend_decsync_get_attachment_uris;
	sync_class->add_timezone_sync = e_cal_backend_decsync_add_timezone;
	sync_class->get_free_busy_sync = e_cal_backend_decsync_get_free_busy;
	sync_class->refresh_sync = ecal_backend_decsync_refresh_sync;

	/* Register our ESource extension. */
	E_TYPE_SOURCE_DECSYNC;
}

static void
e_cal_backend_decsync_timezone_cache_init (ETimezoneCacheInterface *iface)
{
	parent_timezone_cache_interface = g_type_interface_peek_parent (iface);

	iface->tzcache_add_timezone = cal_backend_decsync_add_cached_timezone;
	iface->tzcache_get_timezone = cal_backend_decsync_get_cached_timezone;
	iface->tzcache_list_timezones = cal_backend_decsync_list_cached_timezones;
}

static void
e_cal_backend_decsync_initable_init (GInitableIface *iface)
{
	iface->init = cal_backend_decsync_initable_init;
}

static void
e_cal_backend_decsync_init (ECalBackendDecsync *cbfile)
{
	cbfile->priv = e_cal_backend_decsync_get_instance_private (cbfile);

	cbfile->priv->file_name = g_strdup ("calendar.ics");

	g_rec_mutex_init (&cbfile->priv->idle_save_rmutex);

	g_mutex_init (&cbfile->priv->refresh_lock);

	cbfile->priv->cached_timezones = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
}

void
e_cal_backend_decsync_set_file_name (ECalBackendDecsync *cbfile,
                                  const gchar *file_name)
{
	ECalBackendDecsyncPrivate *priv;

	g_return_if_fail (cbfile != NULL);
	g_return_if_fail (E_IS_CAL_BACKEND_DECSYNC (cbfile));
	g_return_if_fail (file_name != NULL);

	priv = cbfile->priv;
	g_rec_mutex_lock (&priv->idle_save_rmutex);

	if (priv->file_name)
		g_free (priv->file_name);

	priv->file_name = g_strdup (file_name);

	g_rec_mutex_unlock (&priv->idle_save_rmutex);
}

const gchar *
e_cal_backend_decsync_get_file_name (ECalBackendDecsync *cbfile)
{
	ECalBackendDecsyncPrivate *priv;

	g_return_val_if_fail (cbfile != NULL, NULL);
	g_return_val_if_fail (E_IS_CAL_BACKEND_DECSYNC (cbfile), NULL);

	priv = cbfile->priv;

	return priv->file_name;
}

#ifdef TEST_QUERY_RESULT

static void
test_query_by_scanning_all_objects (ECalBackendDecsync *cbfile,
                                    const gchar *sexp,
                                    GSList **objects)
{
	MatchObjectData match_data;
	ECalBackendDecsyncPrivate *priv;

	priv = cbfile->priv;

	match_data.search_needed = TRUE;
	match_data.query = sexp;
	match_data.comps_list = NULL;
	match_data.as_string = TRUE;
	match_data.backend = E_CAL_BACKEND (cbfile);

	if (sexp && !strcmp (sexp, "#t"))
		match_data.search_needed = FALSE;

	match_data.obj_sexp = e_cal_backend_sexp_new (sexp);
	if (!match_data.obj_sexp)
		return;

	g_rec_mutex_lock (&priv->idle_save_rmutex);

	if (!match_data.obj_sexp)
	{
		g_message (G_STRLOC ": Getting object list (%s)", sexp);
		exit (-1);
	}

	g_hash_table_foreach (priv->comp_uid_hash, (GHFunc) match_object_sexp,
			&match_data);

	g_rec_mutex_unlock (&priv->idle_save_rmutex);

	*objects = g_slist_reverse (match_data.comps_list);

	g_object_unref (match_data.obj_sexp);
}

static void
write_list (GSList *list)
{
	GSList *l;

	for (l = list; l; l = l->next)
	{
		const gchar *str = l->data;
		ECalComponent *comp = e_cal_component_new_from_string (str);
		const gchar *uid;
		uid = e_cal_component_get_uid (comp);
		g_print ("%s\n", uid);
	}
}

static void
get_difference_of_lists (ECalBackendDecsync *cbfile,
                         GSList *smaller,
                         GSList *bigger)
{
	GSList *l, *lsmaller;

	for (l = bigger; l; l = l->next) {
		gchar *str = l->data;
		const gchar *uid;
		ECalComponent *comp = e_cal_component_new_from_string (str);
		gboolean found = FALSE;
		uid = e_cal_component_get_uid (comp);

		for (lsmaller = smaller; lsmaller && !found; lsmaller = lsmaller->next)
		{
			gchar *strsmaller = lsmaller->data;
			const gchar *uidsmaller;
			ECalComponent *compsmaller = e_cal_component_new_from_string (strsmaller);
			uidsmaller = e_cal_component_get_uid (compsmaller);

			found = strcmp (uid, uidsmaller) == 0;

			g_object_unref (compsmaller);
		}

		if (!found)
		{
			time_t time_start, time_end;
			ResolveTzidData rtd;
			printf ("%s IS MISSING\n", uid);

			resolve_tzid_data_init (&rtd, cbfile->priv->vcalendar);

			e_cal_util_get_component_occur_times (
				comp, &time_start, &time_end,
				resolve_tzid_cb, &rtd,
				i_cal_timezone_get_utc_timezone (),
				e_cal_backend_get_kind (E_CAL_BACKEND (cbfile)));

			resolve_tzid_data_clear (&rtd);

			d (printf ("start %s\n", asctime (gmtime (&time_start))));
			d (printf ("end %s\n", asctime (gmtime (&time_end))));
		}

		g_object_unref (comp);
	}
}

static void
test_query (ECalBackendDecsync *cbfile,
            const gchar *query)
{
	GSList *objects = NULL, *all_objects = NULL;

	g_return_if_fail (query != NULL);

	d (g_print ("Query %s\n", query));

	test_query_by_scanning_all_objects (cbfile, query, &all_objects);
	e_cal_backend_decsync_get_object_list (E_CAL_BACKEND_SYNC (cbfile), NULL, NULL, query, &objects, NULL);
	if (objects == NULL)
	{
		g_message (G_STRLOC " failed to get objects\n");
		exit (0);
	}

	if (g_slist_length (objects) < g_slist_length (all_objects) )
	{
		g_print ("ERROR\n");
		get_difference_of_lists (cbfile, objects, all_objects);
		exit (-1);
	}
	else if (g_slist_length (objects) > g_slist_length (all_objects) )
	{
		g_print ("ERROR\n");
		write_list (all_objects);
		get_difference_of_lists (cbfile, all_objects, objects);
		exit (-1);
	}

	g_slist_foreach (objects, (GFunc) g_free, NULL);
	g_slist_free (objects);
	g_slist_foreach (all_objects, (GFunc) g_free, NULL);
	g_slist_free (all_objects);
}

static void
execute_query (ECalBackendDecsync *cbfile,
               const gchar *query)
{
	GSList *objects = NULL;

	g_return_if_fail (query != NULL);

	d (g_print ("Query %s\n", query));
	e_cal_backend_decsync_get_object_list (E_CAL_BACKEND_SYNC (cbfile), NULL, NULL, query, &objects, NULL);
	if (objects == NULL)
	{
		g_message (G_STRLOC " failed to get objects\n");
		exit (0);
	}

	g_slist_foreach (objects, (GFunc) g_free, NULL);
	g_slist_free (objects);
}

static gchar *fname = NULL;
static gboolean only_execute = FALSE;
static gchar *calendar_fname = NULL;

static GOptionEntry entries[] =
{
  { "test-file", 't', 0, G_OPTION_ARG_STRING, &fname, "File with prepared queries", NULL },
  { "only-execute", 'e', 0, G_OPTION_ARG_NONE, &only_execute, "Only execute, do not test query", NULL },
  { "calendar-file", 'c', 0, G_OPTION_ARG_STRING, &calendar_fname, "Path to the calendar.ics file", NULL },
  { NULL }
};

/* Always add at least this many bytes when extending the buffer.  */
#define MIN_CHUNK 64

static gint
private_getline (gchar **lineptr,
                 gsize *n,
                 DECSYNC *stream)
{
	gint nchars_avail;
	gchar *read_pos;

	if (!lineptr || !n || !stream)
		return -1;

	if (!*lineptr) {
		*n = MIN_CHUNK;
		*lineptr = (char *)malloc (*n);
		if (!*lineptr)
			return -1;
	}

	nchars_avail = (gint) *n;
	read_pos = *lineptr;

	for (;;) {
		gint c = getc (stream);

		if (nchars_avail < 2) {
			if (*n > MIN_CHUNK)
				*n *= 2;
			else
				*n += MIN_CHUNK;

			nchars_avail = (gint)(*n + *lineptr - read_pos);
			*lineptr = (char *)realloc (*lineptr, *n);
			if (!*lineptr)
				return -1;
			read_pos = *n - nchars_avail + *lineptr;
		}

		if (ferror (stream) || c == EOF) {
			if (read_pos == *lineptr)
				return -1;
			else
				break;
		}

		*read_pos++ = c;
		nchars_avail--;

		if (c == '\n')
			/* Return the line.  */
			break;
	}

	*read_pos = '\0';

	return (gint)(read_pos - (*lineptr));
}

gint
main (gint argc,
      gchar **argv)
{
	gchar * line = NULL;
	gsize len = 0;
	ECalBackendDecsync * cbfile;
	gint num = 0;
	GError *error = NULL;
	GOptionContext *context;
	DECSYNC * fin = NULL;

	context = g_option_context_new ("- test utility for e-d-s file backend");
	g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
	if (!g_option_context_parse (context, &argc, &argv, &error))
	{
		g_print ("option parsing failed: %s\n", error->message);
		exit (1);
	}

	calendar_fname = g_strdup ("calendar.ics");

	if (!calendar_fname)
	{
		g_message (G_STRLOC " Please, use -c parameter");
		exit (-1);
	}

	cbfile = g_object_new (E_TYPE_CAL_BACKEND_DECSYNC, NULL);
	open_cal (cbfile, calendar_fname, &error);
	if (error != NULL) {
		g_message (G_STRLOC " Could not open calendar %s: %s", calendar_fname, error->message);
		exit (-1);
	}

	if (fname)
	{
		fin = fopen (fname, "r");

		if (!fin)
		{
			g_message (G_STRLOC " Could not open file %s", fname);
			goto err0;
		}
	}
	else
	{
		g_message (G_STRLOC " Reading from stdin");
		fin = stdin;
	}

	while (private_getline (&line, &len, fin) != -1) {
		g_print ("Query %d: %s", num++, line);

		if (only_execute)
			execute_query (cbfile, line);
		else
			test_query (cbfile, line);
	}

	if (line)
		free (line);

	if (fname)
		fclose (fin);

err0:
	g_object_unref (cbfile);

	return 0;
}
#endif

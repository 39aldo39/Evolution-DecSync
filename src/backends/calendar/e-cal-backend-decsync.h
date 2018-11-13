/* Evolution calendar - iCalendar decsync backend.
 * Based on the iCalendar file backend.
 *
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
 */

#ifndef E_CAL_BACKEND_DECSYNC_H
#define E_CAL_BACKEND_DECSYNC_H

#include <libedata-cal/libedata-cal.h>

/* Standard GObject macros */
#define E_TYPE_CAL_BACKEND_DECSYNC \
	(e_cal_backend_decsync_get_type ())
#define E_CAL_BACKEND_DECSYNC(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_CAL_BACKEND_DECSYNC, ECalBackendDecsync))
#define E_CAL_BACKEND_DECSYNC_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_CAL_BACKEND_DECSYNC, ECalBackendDecsyncClass))
#define E_IS_CAL_BACKEND_DECSYNC(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_CAL_BACKEND_DECSYNC))
#define E_IS_CAL_BACKEND_DECSYNC_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_CAL_BACKEND_DECSYNC))
#define E_CAL_BACKEND_DECSYNC_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_CAL_BACKEND_DECSYNC, ECalBackendDecsyncClass))

G_BEGIN_DECLS

typedef struct _ECalBackendDecsync ECalBackendDecsync;
typedef struct _ECalBackendDecsyncClass ECalBackendDecsyncClass;
typedef struct _ECalBackendDecsyncPrivate ECalBackendDecsyncPrivate;

struct _ECalBackendDecsync {
	ECalBackendSync parent;
	ECalBackendDecsyncPrivate *priv;
};

struct _ECalBackendDecsyncClass {
	ECalBackendSyncClass parent_class;
};

GType		e_cal_backend_decsync_get_type	(void);
const gchar *	e_cal_backend_decsync_get_file_name
						(ECalBackendDecsync *cbfile);
void		e_cal_backend_decsync_set_file_name
						(ECalBackendDecsync *cbfile,
						 const gchar *file_name);
void		e_cal_backend_decsync_reload	(ECalBackendDecsync *cbfile,
						 GError **error);

G_END_DECLS

#endif /* E_CAL_BACKEND_DECSYNC_H */

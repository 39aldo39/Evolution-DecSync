/* Evolution calendar - iCalendar file backend
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 * Copyright (C) 2020 Aldo Gunsing
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
 * Authors: Rodrigo Moya <rodrigo@novell.com>
 */

#ifndef E_CAL_BACKEND_DECSYNC_JOURNAL_H
#define E_CAL_BACKEND_DECSYNC_JOURNAL_H

#include "e-cal-backend-decsync.h"

/* Standard GObject macros */
#define E_TYPE_CAL_BACKEND_DECSYNC_JOURNAL \
	(e_cal_backend_decsync_journal_get_type ())
#define E_CAL_BACKEND_DECSYNC_JOURNAL(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_CAL_BACKEND_DECSYNC_JOURNAL, ECalBackendDecsyncJournal))
#define E_CAL_BACKEND_DECSYNC_JOURNAL_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_CAL_BACKEND_DECSYNC_JOURNAL, ECalBackendDecsyncJournalClass))
#define E_IS_CAL_BACKEND_DECSYNC_JOURNAL(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_CAL_BACKEND_DECSYNC_JOURNAL))
#define E_IS_CAL_BACKEND_DECSYNC_JOURNAL_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_CAL_BACKEND_DECSYNC_JOURNAL))
#define E_CAL_BACKEND_DECSYNC_JOURNAL_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_CAL_BACKEND_DECSYNC_JOURNAL, ECalBackendDecsyncJournalClass))

G_BEGIN_DECLS

typedef ECalBackendDecsync ECalBackendDecsyncJournal;
typedef ECalBackendDecsyncClass ECalBackendDecsyncJournalClass;

GType		e_cal_backend_decsync_journal_get_type	(void);

G_END_DECLS

#endif /* E_CAL_BACKEND_DECSYNC_JOURNAL_H */

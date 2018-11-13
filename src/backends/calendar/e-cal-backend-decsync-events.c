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
 *          Rodrigo Moya <rodrigo@ximian.com>
 */

#include "evolution-decsync-config.h"

#include "e-cal-backend-decsync-events.h"

G_DEFINE_TYPE (
	ECalBackendDecsyncEvents,
	e_cal_backend_decsync_events,
	E_TYPE_CAL_BACKEND_DECSYNC)

static void
e_cal_backend_decsync_events_class_init (ECalBackendDecsyncEventsClass *class)
{
}

static void
e_cal_backend_decsync_events_init (ECalBackendDecsyncEvents *cbfile)
{
	e_cal_backend_decsync_set_file_name (
		E_CAL_BACKEND_DECSYNC (cbfile), "calendar.ics");
}


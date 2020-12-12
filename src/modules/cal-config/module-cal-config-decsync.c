/**
 * Evolution-DecSync - module-cal-config-decsync.c
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

#include "evolution-decsync-config.h"
#include <e-source/e-source-decsync.h>

#include <glib/gi18n-lib.h>

#include <libebackend/libebackend.h>

#include <e-util/e-util.h>

#include <modules/utils/decsync.h>

typedef ESourceConfigBackend ECalConfigDecsync;
typedef ESourceConfigBackendClass ECalConfigDecsyncClass;

/* Module Entry Points */
void e_module_load (GTypeModule *type_module);
void e_module_unload (GTypeModule *type_module);

/* Forward Declarations */
GType e_cal_config_decsync_get_type (void);

G_DEFINE_DYNAMIC_TYPE (
	ECalConfigDecsync,
	e_cal_config_decsync,
	E_TYPE_SOURCE_CONFIG_BACKEND)

static void
cal_config_decsync_insert_widgets (ESourceConfigBackend *backend, ESource *scratch_source)
{
	ESourceConfig *config;
	ECalSourceConfig *cal_config;
	ECalClientSourceType source_type;

	config = e_source_config_backend_get_config (backend);

	cal_config = E_CAL_SOURCE_CONFIG (config);
	source_type = e_cal_source_config_get_source_type (cal_config);
	switch (source_type)
	{
		case E_CAL_CLIENT_SOURCE_TYPE_EVENTS:
			config_decsync_insert_widgets ("calendars", _("Calendar"), backend, scratch_source);
			break;
		case E_CAL_CLIENT_SOURCE_TYPE_TASKS:
			config_decsync_insert_widgets ("tasks", _("Task List"), backend, scratch_source);
			break;
		case E_CAL_CLIENT_SOURCE_TYPE_MEMOS:
			config_decsync_insert_widgets ("memos", _("Memo List"), backend, scratch_source);
			break;
		case E_CAL_CLIENT_SOURCE_TYPE_LAST:
			// Do nothing, artificial value
			break;
	}
}

static void
e_cal_config_decsync_class_init (ESourceConfigBackendClass *class)
{
	EExtensionClass *extension_class;

	config_decsync_add_source_file ();

	extension_class = E_EXTENSION_CLASS (class);
	extension_class->extensible_type = E_TYPE_CAL_SOURCE_CONFIG;

	class->parent_uid = "decsync";
	class->backend_name = "decsync";
	class->insert_widgets = cal_config_decsync_insert_widgets;
	class->check_complete = config_decsync_check_complete;
	class->commit_changes = config_decsync_commit_changes;

	E_TYPE_SOURCE_DECSYNC;
}

static void
e_cal_config_decsync_class_finalize (ESourceConfigBackendClass *class)
{
}

static void
e_cal_config_decsync_init (ESourceConfigBackend *backend)
{
}

G_MODULE_EXPORT void
e_module_load (GTypeModule *type_module)
{
	e_cal_config_decsync_register_type (type_module);
}

G_MODULE_EXPORT void
e_module_unload (GTypeModule *type_module)
{
}

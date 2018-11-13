/**
 * Evolution-DecSync - decsync.h
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

#include <decsync-utils.h>

void		config_decsync_insert_widgets (const gchar *decsync_type_dirname, const gchar *decsync_type_title, ESourceConfigBackend *backend, ESource *scratch_source);
gboolean	config_decsync_check_complete (ESourceConfigBackend *backend, ESource *scratch_source);
void		config_decsync_commit_changes (ESourceConfigBackend *backend, ESource *scratch_source);
void		config_decsync_add_source_file ();

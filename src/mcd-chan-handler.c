/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 8 -*- */
/*
 * This file is part of mission-control
 *
 * Copyright (C) 2007 Nokia Corporation. 
 *
 * Contact: Naba Kumar  <naba.kumar@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

/**
 * SECTION:mcd-chan-handler
 * @title: McdChannelHandler
 * @short_description: Channel handler class corresponding to each .chandler file
 * @see_also: 
 * @stability: Unstable
 * @include: mcd-chan-handler.h
 * 
 * FIXME
 */

#include <stdlib.h>
#include <string.h>
#include "mcd-chan-handler.h"
#include <config.h>

#define FILE_SEPARATOR ','
#define CH_FILE_SUFFIX ".chandler"
#define CH_FILE_CH_GROUP "ChannelHandler"

static void
_mcd_channel_handler_free (McdChannelHandler *handler)
{
    g_free((gpointer) handler->bus_name);
    g_free((gpointer) handler->obj_path);
    g_free(handler);
}

static inline void
_mcd_channel_handler_packer(GHashTable *handlers, gchar **string_list,
			    gsize list_length, gchar *bus_name,
			    TelepathyChannelMediaCapability capabilities,
			    gchar *object_path)
{
    gint i;
    McdChannelHandler *handler;
    
    for (i = 0; i < list_length; i++)
    {
	handler = g_new(McdChannelHandler, 1);
	handler->bus_name = bus_name;
	handler->obj_path = object_path;
	handler->capabilities = capabilities;
	g_hash_table_insert(handlers, g_strdup(string_list[i]), handler);
    }
}

/* 
* Read files from configuration file directory.
* This is used for Connection Manager and Channel Handler files.
*/
static void
_mcd_channel_handlers_read_conf_files (GHashTable *handlers,
				       gchar *suffix, gchar *group)
{
    GError *error = NULL;
    GKeyFile *file;
    gchar **string_list;
    gsize len;
    GDir *dir;
    const gchar *filename, *dirname;
    gchar *absolute_filepath;
    gchar *bus_name, *object_path;
    TelepathyChannelMediaCapability capabilities;
    
    dirname = g_getenv ("MC_CHANDLERS_DIR");
    if (dirname == NULL)
	dirname = CHANDLERS_DIR;

    /* Read the configuration file directory */
    if ((dir = g_dir_open(dirname, 0, &error)) == NULL)
    {
	g_error ("Error opening directory %s: %s", dirname, 
		 error->message);
    }
    
    while ((filename = g_dir_read_name(dir)) != NULL)
    {
        /* Skip the file if it doesn't contain the required file suffix */
	if (g_str_has_suffix(filename, suffix))
	{
	    absolute_filepath = g_build_filename(dirname, filename, NULL);
	    
	    file = g_key_file_new();
	    if (!g_key_file_load_from_file
		(file, absolute_filepath, G_KEY_FILE_NONE, &error))
	    {
		g_error ("%s", error->message);
	    }
	    g_key_file_set_list_separator(file, FILE_SEPARATOR);
	    
	    if (!(bus_name = g_key_file_get_string (file, group, 
						    "BusName", &error)))
	    {
		g_error ("%s: %s", absolute_filepath, error->message);
	    }
	    if (!(object_path = g_key_file_get_string(file, group,
						      "ObjectPath", &error)))
	    {
		g_error ("%s: %s", absolute_filepath, error->message);
	    }
	    capabilities = g_key_file_get_integer(file, group, "TypeSpecificCapabilities",
						  &error);
	    if (error)
	    {
		if (error->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND)
		    g_warning ("%s: Error parsing %s: %s",
			       G_STRFUNC, filename, error->message);
		g_error_free(error);
		error = NULL;
		capabilities = 0;
	    }

	    
	    if (!(string_list = g_key_file_get_string_list(file, group, "ChannelType",
							   &len, &error)))
	    {
		g_error ("%s: %s", absolute_filepath, error->message);
	    }
	    
	    _mcd_channel_handler_packer(handlers, string_list, len, bus_name,
				       	capabilities, object_path);
	    
	    g_strfreev(string_list);
	    g_key_file_free(file);
	    g_free(absolute_filepath);
	}
    }
    g_dir_close(dir);
}

GHashTable*
mcd_get_channel_handlers (void)
{
    GHashTable *handlers;
    
    handlers = g_hash_table_new_full(g_str_hash, g_str_equal,
				     g_free,
				     (GDestroyNotify)_mcd_channel_handler_free);
    
    /* Read Channel Handler files */
    _mcd_channel_handlers_read_conf_files (handlers,
					   CH_FILE_SUFFIX, CH_FILE_CH_GROUP);
    return handlers;
}

/*
 * mc-account.c - Telepathy Account D-Bus interface (client side)
 *
 * Copyright (C) 2008 Collabora Ltd. <http://www.collabora.co.uk/>
 * Copyright (C) 2008 Nokia Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <string.h>
#include "mc-account.h"
#include "mc-account-priv.h"
#include "dbus-api.h"
#include "mc-signals-marshal.h"

#include <telepathy-glib/proxy-subclass.h>

#include "_gen/cli-account-body.h"

/**
 * SECTION:mc-account
 * @title: McAccount
 * @short_description: proxy object for the Telepathy Account D-Bus API
 *
 * This module provides a client-side proxy object for the Telepathy
 * Account D-Bus API.
 *
 * Since: FIXME
 */

/**
 * McAccountClass:
 *
 * The class of a #McAccount.
 *
 * Since: FIXME
 */
struct _McAccountClass {
    TpProxyClass parent_class;
    /*<private>*/
    gpointer priv;
};

struct _McAccountProps {
    gchar *display_name;
    gchar *icon;
    guint valid : 1;
    guint enabled : 1;
    guint connect_automatically : 1;
    guint emit_changed : 1;
    guint emit_connection_status_changed : 1;
    gchar *nickname;
    GHashTable *parameters;
    TpConnectionPresenceType auto_presence_type;
    gchar *auto_presence_status;
    gchar *auto_presence_message;
    gchar *connection;
    TpConnectionStatus connection_status;
    TpConnectionStatusReason connection_status_reason;
    TpConnectionPresenceType curr_presence_type;
    gchar *curr_presence_status;
    gchar *curr_presence_message;
    TpConnectionPresenceType req_presence_type;
    gchar *req_presence_status;
    gchar *req_presence_message;
    gchar *normalized_name;
};

/**
 * McAccount:
 *
 * A proxy object for the Telepathy Account D-Bus API. This is a subclass of
 * #TpProxy.
 *
 * Since: FIXME
 */

G_DEFINE_TYPE (McAccount, mc_account, TP_TYPE_PROXY);

enum
{
    PROP_0,
    PROP_OBJECT_PATH,
};

guint _mc_account_signals[LAST_SIGNAL] = { 0 };

static inline gboolean
parse_object_path (McAccount *account)
{
    gchar manager[64], protocol[64], name[256];
    gchar *object_path = account->parent.object_path;
    gint n;

    n = sscanf (object_path, MC_ACCOUNT_DBUS_OBJECT_BASE "%[^/]/%[^/]/%s",
		manager, protocol, name);
    if (n != 3) return FALSE;

    account->manager_name = g_strdup (manager);
    account->protocol_name = g_strdup (protocol);
    account->name = object_path +
       	(sizeof (MC_ACCOUNT_DBUS_OBJECT_BASE) - 1);
    return TRUE;
}

static void
mc_account_init (McAccount *account)
{
    McAccountPrivate *priv;

    priv = account->priv =
       	G_TYPE_INSTANCE_GET_PRIVATE(account, MC_TYPE_ACCOUNT,
				    McAccountPrivate);

    tp_proxy_add_interface_by_id ((TpProxy *)account,
				  MC_IFACE_QUARK_ACCOUNT_INTERFACE_AVATAR);
    tp_proxy_add_interface_by_id ((TpProxy *)account,
				  MC_IFACE_QUARK_ACCOUNT_INTERFACE_COMPAT);
    tp_proxy_add_interface_by_id ((TpProxy *)account,
				  MC_IFACE_QUARK_ACCOUNT_INTERFACE_CONDITIONS);
}

static GObject *
constructor (GType type,
	     guint n_params,
	     GObjectConstructParam *params)
{
    GObjectClass *object_class = (GObjectClass *) mc_account_parent_class;
    McAccount *account;
   
    account =  MC_ACCOUNT (object_class->constructor (type, n_params, params));

    g_return_val_if_fail (account != NULL, NULL);

    if (!parse_object_path (account))
	return NULL;

    return (GObject *) account;
}

static void
account_props_free (McAccountProps *props)
{
    g_free (props->display_name);
    g_free (props->icon);
    g_free (props->nickname);
    if (props->parameters)
	g_hash_table_destroy (props->parameters);
    g_free (props->auto_presence_status);
    g_free (props->auto_presence_message);
    g_free (props->connection);
    g_free (props->curr_presence_status);
    g_free (props->curr_presence_message);
    g_free (props->req_presence_status);
    g_free (props->req_presence_message);
    g_free (props->normalized_name);
    g_free (props);
}

static void
finalize (GObject *object)
{
    McAccount *account = MC_ACCOUNT (object);

    if (account->priv->props)
	account_props_free (account->priv->props);

    if (account->priv->avatar_props)
	_mc_account_avatar_props_free (account->priv->avatar_props);

    if (account->priv->compat_props)
	_mc_account_compat_props_free (account->priv->compat_props);

    if (account->priv->conditions_props)
	_mc_account_conditions_props_free (account->priv->conditions_props);

    g_free (account->manager_name);
    g_free (account->protocol_name);

    G_OBJECT_CLASS (mc_account_parent_class)->finalize (object);
}

static void
mc_account_class_init (McAccountClass *klass)
{
    GType type = MC_TYPE_ACCOUNT;
    GObjectClass *object_class = (GObjectClass *)klass;
    TpProxyClass *proxy_class = (TpProxyClass *)klass;

    g_type_class_add_private (object_class, sizeof (McAccountPrivate));
    object_class->constructor = constructor;
    object_class->finalize = finalize;

    /* the API is stateless, so we can keep the same proxy across restarts */
    proxy_class->must_have_unique_name = FALSE;

    _mc_ext_register_dbus_glib_marshallers ();

    proxy_class->interface = MC_IFACE_QUARK_ACCOUNT;
    tp_proxy_or_subclass_hook_on_interface_add (type, mc_cli_account_add_signals);

    tp_proxy_subclass_add_error_mapping (type, TP_ERROR_PREFIX, TP_ERRORS,
					 TP_TYPE_ERROR);

    _mc_account_signals[PRESENCE_CHANGED] =
	g_signal_new ("presence-changed",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      0,
		      NULL, NULL,
		      mc_signals_marshal_VOID__UINT_UINT_STRING_STRING,
		      G_TYPE_NONE,
		      4, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_STRING,
		      G_TYPE_STRING);

    _mc_account_signals[STRING_CHANGED] =
	g_signal_new ("string-changed",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      0,
		      NULL, NULL,
		      mc_signals_marshal_VOID__UINT_STRING,
		      G_TYPE_NONE,
		      2, G_TYPE_UINT, G_TYPE_STRING);

    _mc_account_signals[CONNECTION_STATUS_CHANGED] =
	g_signal_new ("connection-status-changed",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST,
		      0,
		      NULL, NULL,
		      mc_signals_marshal_VOID__UINT_UINT,
		      G_TYPE_NONE,
		      2, G_TYPE_UINT, G_TYPE_UINT);

    _mc_account_signals[FLAG_CHANGED] =
	g_signal_new ("flag-changed",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      0,
		      NULL, NULL,
		      mc_signals_marshal_VOID__UINT_BOOLEAN,
		      G_TYPE_NONE,
		      2, G_TYPE_UINT, G_TYPE_BOOLEAN);

    _mc_account_signals[PARAMETERS_CHANGED] =
	g_signal_new ("parameters-changed",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      0,
		      NULL, NULL,
		      mc_signals_marshal_VOID__BOXED_BOXED,
		      G_TYPE_NONE,
		      2, G_TYPE_HASH_TABLE, G_TYPE_HASH_TABLE);
}

/**
 * mc_account_new:
 * @dbus: a D-Bus daemon; may not be %NULL
 *
 * <!-- -->
 *
 * Returns: a new NMC 4.x proxy
 *
 * Since: FIXME
 */
McAccount *
mc_account_new (TpDBusDaemon *dbus, const gchar *object_path)
{
    McAccount *account;

    account = g_object_new (MC_TYPE_ACCOUNT,
			    "dbus-daemon", dbus,
			    "bus-name", MC_ACCOUNT_MANAGER_DBUS_SERVICE,
			    "object-path", object_path,
			    NULL);
    return account;
}

static void
update_property (gpointer key, gpointer ht_value, gpointer user_data)
{
    McAccount *account = user_data;
    McAccountProps *props = account->priv->props;
    GValue *value = ht_value;
    const gchar *name = key;
    GValueArray *va;

    if (strcmp (name, "DisplayName") == 0)
    {
	g_free (props->display_name);
	props->display_name = g_value_dup_string (value);
	if (props->emit_changed)
	    g_signal_emit (account, _mc_account_signals[STRING_CHANGED],
			   MC_QUARK_DISPLAY_NAME,
			   MC_QUARK_DISPLAY_NAME,
			   props->display_name);
    }
    else if (strcmp (name, "Icon") == 0)
    {
	g_free (props->icon);
	props->icon = g_value_dup_string (value);
	if (props->emit_changed)
	    g_signal_emit (account, _mc_account_signals[STRING_CHANGED],
			   MC_QUARK_ICON,
			   MC_QUARK_ICON,
			   props->icon);
    }
    else if (strcmp (name, "Valid") == 0)
    {
	props->valid = g_value_get_boolean (value);
	if (props->emit_changed)
	    g_signal_emit (account, _mc_account_signals[FLAG_CHANGED],
			   MC_QUARK_VALID,
			   MC_QUARK_VALID,
			   props->valid);
    }
    else if (strcmp (name, "Enabled") == 0)
    {
	props->enabled = g_value_get_boolean (value);
	if (props->emit_changed)
	    g_signal_emit (account, _mc_account_signals[FLAG_CHANGED],
			   MC_QUARK_ENABLED,
			   MC_QUARK_ENABLED,
			   props->enabled);
    }
    else if (strcmp (name, "Nickname") == 0)
    {
	g_free (props->nickname);
	props->nickname = g_value_dup_string (value);
	if (props->emit_changed)
	    g_signal_emit (account, _mc_account_signals[STRING_CHANGED],
			   MC_QUARK_NICKNAME,
			   MC_QUARK_NICKNAME,
			   props->nickname);
    }
    else if (strcmp (name, "Parameters") == 0)
    {
	GHashTable *old_parameters = props->parameters;

	props->parameters = g_value_get_boxed (value);
	_mc_gvalue_stolen (value);
	if (props->emit_changed)
	    g_signal_emit (account, _mc_account_signals[PARAMETERS_CHANGED],
			   0,
			   old_parameters, props->parameters);
	if (old_parameters)
	    g_hash_table_destroy (old_parameters);
    }
    else if (strcmp (name, "AutomaticPresence") == 0)
    {
	g_free (props->auto_presence_status);
	g_free (props->auto_presence_message);
	va = g_value_get_boxed (value);
	props->auto_presence_type = (gint)g_value_get_uint (va->values);
	props->auto_presence_status = g_value_dup_string (va->values + 1);
	props->auto_presence_message = g_value_dup_string (va->values + 2);
	if (props->emit_changed)
	    g_signal_emit (account, _mc_account_signals[PRESENCE_CHANGED],
			   MC_QUARK_AUTOMATIC_PRESENCE,
			   MC_QUARK_AUTOMATIC_PRESENCE,
			   props->auto_presence_type,
			   props->auto_presence_status,
			   props->auto_presence_message);
    }
    else if (strcmp (name, "ConnectAutomatically") == 0)
    {
	props->connect_automatically = g_value_get_boolean (value);
	if (props->emit_changed)
	    g_signal_emit (account, _mc_account_signals[FLAG_CHANGED],
			   MC_QUARK_CONNECT_AUTOMATICALLY,
			   MC_QUARK_CONNECT_AUTOMATICALLY,
			   props->connect_automatically);
    }
    else if (strcmp (name, "Connection") == 0)
    {
	g_free (props->connection);
	props->connection = g_value_dup_string (value);
    }
    else if (strcmp (name, "ConnectionStatus") == 0)
    {
	props->connection_status = g_value_get_uint (value);
	if (props->emit_changed)
	    props->emit_connection_status_changed = TRUE;
    }
    else if (strcmp (name, "ConnectionStatusReason") == 0)
    {
	props->connection_status_reason = g_value_get_uint (value);
	if (props->emit_changed)
	    props->emit_connection_status_changed = TRUE;
    }
    else if (strcmp (name, "CurrentPresence") == 0)
    {
	g_free (props->curr_presence_status);
	g_free (props->curr_presence_message);
	va = g_value_get_boxed (value);
	props->curr_presence_type = (gint)g_value_get_uint (va->values);
	props->curr_presence_status = g_value_dup_string (va->values + 1);
	props->curr_presence_message = g_value_dup_string (va->values + 2);
	if (props->emit_changed)
	    g_signal_emit (account, _mc_account_signals[PRESENCE_CHANGED],
			   MC_QUARK_CURRENT_PRESENCE,
			   MC_QUARK_CURRENT_PRESENCE,
			   props->curr_presence_type,
			   props->curr_presence_status,
			   props->curr_presence_message);
    }
    else if (strcmp (name, "RequestedPresence") == 0)
    {
	g_free (props->req_presence_status);
	g_free (props->req_presence_message);
	va = g_value_get_boxed (value);
	props->req_presence_type = (gint)g_value_get_uint (va->values);
	props->req_presence_status = g_value_dup_string (va->values + 1);
	props->req_presence_message = g_value_dup_string (va->values + 2);
	if (props->emit_changed)
	    g_signal_emit (account, _mc_account_signals[PRESENCE_CHANGED],
			   MC_QUARK_REQUESTED_PRESENCE,
			   MC_QUARK_REQUESTED_PRESENCE,
			   props->req_presence_type,
			   props->req_presence_status,
			   props->req_presence_message);
    }
    else if (strcmp (name, "NormalizedName") == 0)
    {
	g_free (props->normalized_name);
	props->normalized_name = g_value_dup_string (value);
	if (props->emit_changed)
	    g_signal_emit (account, _mc_account_signals[STRING_CHANGED],
			   MC_QUARK_NORMALIZED_NAME,
			   MC_QUARK_NORMALIZED_NAME,
			   props->normalized_name);
    }
}

static void
create_props (TpProxy *proxy, GHashTable *props)
{
    McAccount *account = MC_ACCOUNT (proxy);
    McAccountPrivate *priv = account->priv;

    priv->props = g_malloc0 (sizeof (McAccountProps));
    g_hash_table_foreach (props, update_property, account);
    priv->props->emit_changed = TRUE;
}
static void 
on_account_property_changed (TpProxy *proxy, GHashTable *props, 
			     gpointer user_data, GObject *weak_object) 
{ 
    McAccount *account = MC_ACCOUNT (proxy); 
    McAccountPrivate *priv = account->priv; 

    /* if the GetAll method hasn't returned yet, we do nothing */
    if (G_UNLIKELY (!priv->props)) return;

    g_hash_table_foreach (props, update_property, account);
    if (priv->props->emit_connection_status_changed)
	g_signal_emit (account,
		       _mc_account_signals[CONNECTION_STATUS_CHANGED], 0,
		       priv->props->connection_status,
		       priv->props->connection_status_reason);
}

void
mc_account_call_when_ready (McAccount *account, McAccountWhenReadyCb callback,
			    gpointer user_data)
{
    McIfaceData iface_data;

    iface_data.id = MC_IFACE_QUARK_ACCOUNT;
    iface_data.props_data_ptr = (gpointer *)&account->priv->props;
    iface_data.create_props = create_props;

    if (_mc_iface_call_when_ready_int ((TpProxy *)account,
				       (McIfaceWhenReadyCb)callback, user_data,
				       &iface_data))
    {
	mc_cli_account_connect_to_account_property_changed (account,
							    on_account_property_changed,
							    NULL, NULL,
							    NULL, NULL);
    }
}

const gchar *
mc_account_get_display_name (McAccount *account)
{
    g_return_val_if_fail (MC_IS_ACCOUNT (account), NULL);
    if (G_UNLIKELY (!account->priv->props)) return NULL;
    return account->priv->props->display_name;
}

const gchar *
mc_account_get_icon (McAccount *account)
{
    g_return_val_if_fail (MC_IS_ACCOUNT (account), NULL);
    if (G_UNLIKELY (!account->priv->props)) return NULL;
    return account->priv->props->icon;
}

gboolean
mc_account_is_valid (McAccount *account)
{
    g_return_val_if_fail (MC_IS_ACCOUNT (account), FALSE);
    if (G_UNLIKELY (!account->priv->props)) return FALSE;
    return account->priv->props->valid;
}

gboolean
mc_account_is_enabled (McAccount *account)
{
    g_return_val_if_fail (MC_IS_ACCOUNT (account), FALSE);
    if (G_UNLIKELY (!account->priv->props)) return FALSE;
    return account->priv->props->enabled;
}

gboolean
mc_account_connects_automatically (McAccount *account)
{
    g_return_val_if_fail (MC_IS_ACCOUNT (account), FALSE);
    if (G_UNLIKELY (!account->priv->props)) return FALSE;
    return account->priv->props->connect_automatically;
}

const gchar *
mc_account_get_nickname (McAccount *account)
{
    g_return_val_if_fail (MC_IS_ACCOUNT (account), NULL);
    if (G_UNLIKELY (!account->priv->props)) return NULL;
    return account->priv->props->nickname;
}

const GHashTable *
mc_account_get_parameters (McAccount *account)
{
    g_return_val_if_fail (MC_IS_ACCOUNT (account), NULL);
    if (G_UNLIKELY (!account->priv->props)) return NULL;
    return account->priv->props->parameters;
}

void
mc_account_get_automatic_presence (McAccount *account,
				   TpConnectionPresenceType *type,
				   const gchar **status,
				   const gchar **message)
{
    McAccountProps *props;

    g_return_if_fail (MC_IS_ACCOUNT (account));
    props = account->priv->props;
    if (G_UNLIKELY (!props))
    {
	*type = TP_CONNECTION_PRESENCE_TYPE_UNSET;
	*status = *message = NULL;
	return;
    }
    *type = props->auto_presence_type;
    *status = props->auto_presence_status;
    *message = props->auto_presence_message;
}

const gchar *
mc_account_get_connection_name (McAccount *account)
{
    g_return_val_if_fail (MC_IS_ACCOUNT (account), NULL);
    if (G_UNLIKELY (!account->priv->props)) return NULL;
    return account->priv->props->connection;
}

TpConnectionStatus
mc_account_get_connection_status (McAccount *account)
{
    g_return_val_if_fail (MC_IS_ACCOUNT (account),
			  TP_CONNECTION_STATUS_DISCONNECTED);
    if (G_UNLIKELY (!account->priv->props))
       	return TP_CONNECTION_STATUS_DISCONNECTED;
    return account->priv->props->connection_status;
}

TpConnectionStatusReason
mc_account_get_connection_status_reason (McAccount *account)
{
    g_return_val_if_fail (MC_IS_ACCOUNT (account),
			  TP_CONNECTION_STATUS_REASON_NONE_SPECIFIED);
    if (G_UNLIKELY (!account->priv->props))
       	return TP_CONNECTION_STATUS_REASON_NONE_SPECIFIED;
    return account->priv->props->connection_status_reason;
}

void
mc_account_get_current_presence (McAccount *account,
				      TpConnectionPresenceType *type,
				      const gchar **status,
				      const gchar **message)
{
    McAccountProps *props;

    g_return_if_fail (MC_IS_ACCOUNT (account));
    props = account->priv->props;
    if (G_UNLIKELY (!props))
    {
	*type = TP_CONNECTION_PRESENCE_TYPE_UNSET;
	*status = *message = NULL;
	return;
    }
    *type = props->curr_presence_type;
    *status = props->curr_presence_status;
    *message = props->curr_presence_message;
}

void
mc_account_get_requested_presence (McAccount *account,
					TpConnectionPresenceType *type,
					const gchar **status,
					const gchar **message)
{
    McAccountProps *props;

    g_return_if_fail (MC_IS_ACCOUNT (account));
    props = account->priv->props;
    if (G_UNLIKELY (!props))
    {
	*type = TP_CONNECTION_PRESENCE_TYPE_UNSET;
	*status = *message = NULL;
	return;
    }
    *type = props->req_presence_type;
    *status = props->req_presence_status;
    *message = props->req_presence_message;
}

const gchar *
mc_account_get_normalized_name (McAccount *account)
{
    g_return_val_if_fail (MC_IS_ACCOUNT (account), NULL);
    if (G_UNLIKELY (!account->priv->props)) return NULL;
    return account->priv->props->normalized_name;
}


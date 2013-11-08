/* Representation of the account manager as presented to plugins. This is
 * deliberately a "smaller" API than McdAccountManager.
 *
 * Copyright © 2010 Nokia Corporation
 * Copyright © 2010-2012 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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
#include "config.h"
#include "mcd-storage.h"

#include "mcd-account.h"
#include "mcd-account-config.h"
#include "mcd-debug.h"
#include "mcd-misc.h"
#include "plugin-loader.h"

#include <errno.h>
#include <string.h>

#include <dbus/dbus.h>
#include <telepathy-glib/telepathy-glib.h>
#include <telepathy-glib/telepathy-glib-dbus.h>

#include "mission-control-plugins/implementation.h"

/* these pseudo-plugins take care of the actual account storage/retrieval */
#include "mcd-account-manager-default.h"

#define MAX_KEY_LENGTH (DBUS_MAXIMUM_NAME_LENGTH + 6)

static GList *stores = NULL;
static void sort_and_cache_plugins (void);

enum {
  PROP_DBUS_DAEMON = 1,
};

struct _McdStorageClass {
    GObjectClass parent;
};

static void plugin_iface_init (McpAccountManagerIface *iface,
    gpointer unused G_GNUC_UNUSED);

G_DEFINE_TYPE_WITH_CODE (McdStorage, mcd_storage,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (MCP_TYPE_ACCOUNT_MANAGER, plugin_iface_init))

typedef struct {
    /* owned string => GVariant
     * e.g. { 'DisplayName': <'Frederick Bloggs'> } */
    GHashTable *attributes;
    /* owned string => owned GVariant
     * e.g. { 'account': <'fred@example.com'>, 'password': <'foo'> } */
    GHashTable *parameters;
    /* owned string => owned string escaped as if for a keyfile
     * e.g. { 'account': 'fred@example.com', 'password': 'foo' }
     * keys of @parameters and @escaped_parameters are disjoint */
    GHashTable *escaped_parameters;

    /* owned storage plugin owning this account */
    McpAccountStorage *storage;
} McdStorageAccount;

static McdStorageAccount *
mcd_storage_account_new (McpAccountStorage *storage)
{
  McdStorageAccount *sa;

  sa = g_slice_new (McdStorageAccount);
  sa->attributes = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) g_variant_unref);
  sa->parameters = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) g_variant_unref);
  sa->escaped_parameters = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, g_free);
  sa->storage = g_object_ref (storage);

  return sa;
}

static void
mcd_storage_account_free (gpointer p)
{
  McdStorageAccount *sa = p;

  g_hash_table_unref (sa->attributes);
  g_hash_table_unref (sa->parameters);
  g_hash_table_unref (sa->escaped_parameters);
  g_object_unref (sa->storage);
  g_slice_free (McdStorageAccount, sa);
}

static void
mcd_storage_init (McdStorage *self)
{
  self->accounts = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, mcd_storage_account_free);
}

static void
storage_finalize (GObject *object)
{
  McdStorage *self = MCD_STORAGE (object);
  GObjectFinalizeFunc finalize =
    G_OBJECT_CLASS (mcd_storage_parent_class)->finalize;

  g_hash_table_unref (self->accounts);
  self->accounts = NULL;

  if (finalize != NULL)
    finalize (object);
}

static void
storage_dispose (GObject *object)
{
  McdStorage *self = MCD_STORAGE (object);
  GObjectFinalizeFunc dispose =
    G_OBJECT_CLASS (mcd_storage_parent_class)->dispose;

  tp_clear_object (&self->dbusd);

  if (dispose != NULL)
    dispose (object);
}

static void
storage_set_property (GObject *obj, guint prop_id,
	      const GValue *val, GParamSpec *pspec)
{
    McdStorage *self = MCD_STORAGE (obj);

    switch (prop_id)
    {
      case PROP_DBUS_DAEMON:
        tp_clear_object (&self->dbusd);
        self->dbusd = TP_DBUS_DAEMON (g_value_dup_object (val));
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
        break;
    }
}

static void
storage_get_property (GObject *obj, guint prop_id,
	      GValue *val, GParamSpec *pspec)
{
    McdStorage *self = MCD_STORAGE (obj);

    switch (prop_id)
    {
      case PROP_DBUS_DAEMON:
        g_value_set_object (val, self->dbusd);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
        break;
    }
}

static void
mcd_storage_class_init (McdStorageClass *cls)
{
  GObjectClass *object_class = (GObjectClass *) cls;
  GParamSpec *spec = g_param_spec_object ("dbus-daemon",
      "DBus daemon",
      "DBus daemon",
      TP_TYPE_DBUS_DAEMON,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  object_class->set_property = storage_set_property;
  object_class->get_property = storage_get_property;
  object_class->dispose = storage_dispose;
  object_class->finalize = storage_finalize;

  g_object_class_install_property (object_class, PROP_DBUS_DAEMON, spec);
}

McdStorage *
mcd_storage_new (TpDBusDaemon *dbus_daemon)
{
  return g_object_new (MCD_TYPE_STORAGE,
      "dbus-daemon", dbus_daemon,
      NULL);
}

static gchar *
mcd_keyfile_escape_variant (GVariant *variant)
{
  GKeyFile *keyfile;
  gchar *ret;

  g_return_val_if_fail (variant != NULL, NULL);

  keyfile = g_key_file_new ();
  mcd_keyfile_set_variant (keyfile, "g", "k", variant);
  ret = g_key_file_get_value (keyfile, "g", "k", NULL);
  g_key_file_free (keyfile);
  return ret;
}

static McdStorageAccount *
lookup_account (McdStorage *self,
    const gchar *account)
{
  return g_hash_table_lookup (self->accounts, account);
}

static gchar *
get_value (const McpAccountManager *ma,
    const gchar *account,
    const gchar *key)
{
  McdStorage *self = MCD_STORAGE (ma);
  McdStorageAccount *sa = lookup_account (self, account);
  GVariant *variant;
  gchar *ret;

  if (sa == NULL)
    return NULL;

  if (g_str_has_prefix (key, "param-"))
    {
      variant = g_hash_table_lookup (sa->parameters, key + 6);

      if (variant != NULL)
        {
          ret = mcd_keyfile_escape_variant (variant);
          g_variant_unref (variant);
          return ret;
        }
      else
        {
          /* OK, we don't have it as a variant. How about the keyfile-escaped
           * version? */
          return g_strdup (g_hash_table_lookup (sa->escaped_parameters,
                key + 6));
        }
    }
  else
    {
      variant = g_hash_table_lookup (sa->attributes, key);

      if (variant != NULL)
        {
          ret = mcd_keyfile_escape_variant (variant);
          g_variant_unref (variant);
          return ret;
        }
      else
        {
          return NULL;
        }
    }
}

static struct {
    const gchar *type;
    const gchar *name;
} known_attributes[] = {
    /* Please keep this sorted by type, then by name. */

    /* Structs */
      { "(uss)", MC_ACCOUNTS_KEY_AUTOMATIC_PRESENCE },

    /* Array of object path */
      { "ao", MC_ACCOUNTS_KEY_SUPERSEDES },

    /* Array of string */
      { "as", MC_ACCOUNTS_KEY_URI_SCHEMES },

    /* Booleans */
      { "b", MC_ACCOUNTS_KEY_ALWAYS_DISPATCH },
      { "b", MC_ACCOUNTS_KEY_CONNECT_AUTOMATICALLY },
      { "b", MC_ACCOUNTS_KEY_ENABLED },
      { "b", MC_ACCOUNTS_KEY_HAS_BEEN_ONLINE },

    /* Strings */
      { "s", MC_ACCOUNTS_KEY_AUTO_PRESENCE_MESSAGE },
      { "s", MC_ACCOUNTS_KEY_AUTO_PRESENCE_STATUS },
      { "s", MC_ACCOUNTS_KEY_AVATAR_MIME },
      { "s", MC_ACCOUNTS_KEY_AVATAR_TOKEN },
      { "s", MC_ACCOUNTS_KEY_DISPLAY_NAME },
      { "s", MC_ACCOUNTS_KEY_ICON },
      { "s", MC_ACCOUNTS_KEY_MANAGER },
      { "s", MC_ACCOUNTS_KEY_NICKNAME },
      { "s", MC_ACCOUNTS_KEY_NORMALIZED_NAME },
      { "s", MC_ACCOUNTS_KEY_PROTOCOL },
      { "s", MC_ACCOUNTS_KEY_SERVICE },

    /* Integers */
      { "u", MC_ACCOUNTS_KEY_AUTO_PRESENCE_TYPE },

      { NULL, NULL }
};

const gchar *
mcd_storage_get_attribute_type (const gchar *attribute)
{
  guint i;

  for (i = 0; known_attributes[i].type != NULL; i++)
    {
      if (!tp_strdiff (attribute, known_attributes[i].name))
        return known_attributes[i].type;
    }

  return NULL;
}

gboolean
mcd_storage_init_value_for_attribute (GValue *value,
    const gchar *attribute)
{
  const gchar *s = mcd_storage_get_attribute_type (attribute);

  if (s == NULL)
    return FALSE;

  switch (s[0])
    {
      case 's':
        g_value_init (value, G_TYPE_STRING);
        return TRUE;

      case 'b':
        g_value_init (value, G_TYPE_BOOLEAN);
        return TRUE;

      case 'u':
        /* this seems wrong but it's how we've always done it */
        g_value_init (value, G_TYPE_INT);
        return TRUE;

      case 'a':
          {
            switch (s[1])
              {
                case 'o':
                  g_value_init (value, TP_ARRAY_TYPE_OBJECT_PATH_LIST);
                  return TRUE;

                case 's':
                  g_value_init (value, G_TYPE_STRV);
                  return TRUE;
              }
          }
        break;

      case '(':
          {
            if (!tp_strdiff (s, "(uss)"))
              {
                g_value_init (value, TP_STRUCT_TYPE_SIMPLE_PRESENCE);
                return TRUE;
              }
          }
        break;
    }

  return FALSE;
}

static gboolean
mcpa_init_value_for_attribute (const McpAccountManager *mcpa,
    GValue *value,
    const gchar *attribute)
{
  return mcd_storage_init_value_for_attribute (value, attribute);
}

static void
mcpa_set_attribute (const McpAccountManager *ma,
    const gchar *account,
    const gchar *attribute,
    GVariant *value,
    McpAttributeFlags flags)
{
  McdStorage *self = MCD_STORAGE (ma);
  McdStorageAccount *sa = lookup_account (self, account);

  g_return_if_fail (sa != NULL);

  if (value != NULL)
    {
      g_hash_table_insert (sa->attributes, g_strdup (attribute),
          g_variant_ref_sink (value));
    }
  else
    {
      g_hash_table_remove (sa->attributes, attribute);
    }
}

static void
mcpa_set_parameter (const McpAccountManager *ma,
    const gchar *account,
    const gchar *parameter,
    GVariant *value,
    McpParameterFlags flags)
{
  McdStorage *self = MCD_STORAGE (ma);
  McdStorageAccount *sa = lookup_account (self, account);

  g_return_if_fail (sa != NULL);

  g_hash_table_remove (sa->parameters, parameter);
  g_hash_table_remove (sa->escaped_parameters, parameter);

  if (value != NULL)
    g_hash_table_insert (sa->parameters, g_strdup (parameter),
        g_variant_ref_sink (value));
}

static void
set_value (const McpAccountManager *ma,
    const gchar *account,
    const gchar *key,
    const gchar *value)
{
  McdStorage *self = MCD_STORAGE (ma);
  McdStorageAccount *sa = lookup_account (self, account);

  g_return_if_fail (sa != NULL);

  if (g_str_has_prefix (key, "param-"))
    {
      g_hash_table_remove (sa->parameters, key + 6);
      g_hash_table_remove (sa->escaped_parameters, key + 6);

      if (value != NULL)
        g_hash_table_insert (sa->escaped_parameters, g_strdup (key + 6),
            g_strdup (value));
    }
  else
    {
      if (value != NULL)
        {
          GValue tmp = G_VALUE_INIT;
          GError *error = NULL;

          if (!mcd_storage_init_value_for_attribute (&tmp, key))
            {
              g_warning ("Not sure what the type of '%s' is, assuming string",
                  key);
              g_value_init (&tmp, G_TYPE_STRING);
            }

          if (mcd_keyfile_unescape_value (value, &tmp, &error))
            {
              g_hash_table_insert (sa->attributes, g_strdup (key),
                  g_variant_ref_sink (dbus_g_value_build_g_variant (&tmp)));
              g_value_unset (&tmp);
            }
          else
            {
              g_warning ("Could not decode attribute '%s':'%s' from plugin: %s",
                  key, value, error->message);
              g_error_free (error);
              g_hash_table_remove (sa->attributes, key);
            }
        }
      else
        {
          g_hash_table_remove (sa->attributes, key);
        }
    }
}

static GStrv
list_keys (const McpAccountManager *ma,
           const gchar * account)
{
  McdStorage *self = MCD_STORAGE (ma);
  GPtrArray *ret = g_ptr_array_new ();
  McdStorageAccount *sa = lookup_account (self, account);

  if (sa != NULL)
    {
      GHashTableIter iter;
      gpointer k;

      g_hash_table_iter_init (&iter, sa->attributes);

      while (g_hash_table_iter_next (&iter, &k, NULL))
        g_ptr_array_add (ret, g_strdup (k));

      g_hash_table_iter_init (&iter, sa->parameters);

      while (g_hash_table_iter_next (&iter, &k, NULL))
        g_ptr_array_add (ret, g_strdup_printf ("param-%s", (gchar *) k));
    }

  g_ptr_array_add (ret, NULL);
  return (GStrv) g_ptr_array_free (ret, FALSE);
}

static gchar *
unique_name (const McpAccountManager *ma,
    const gchar *manager,
    const gchar *protocol,
    const gchar *identification)
{
  McdStorage *self = MCD_STORAGE (ma);
  gchar *esc_manager, *esc_protocol, *esc_base;
  guint i;
  gsize base_len = strlen (TP_ACCOUNT_OBJECT_PATH_BASE);
  DBusGConnection *connection = tp_proxy_get_dbus_connection (self->dbusd);

  esc_manager = tp_escape_as_identifier (manager);
  esc_protocol = g_strdelimit (g_strdup (protocol), "-", '_');
  esc_base = tp_escape_as_identifier (identification);

  for (i = 0; i < G_MAXUINT; i++)
    {
      gchar *path = g_strdup_printf (
          TP_ACCOUNT_OBJECT_PATH_BASE "%s/%s/%s%u",
          esc_manager, esc_protocol, esc_base, i);

      if (!g_hash_table_contains (self->accounts, path + base_len) &&
          dbus_g_connection_lookup_g_object (connection, path) == NULL)
        {
          gchar *ret = g_strdup (path + base_len);

          g_free (path);
          return ret;
        }

      g_free (path);
    }

  return NULL;
}

static void
identify_account_cb (TpProxy *proxy,
    const gchar *identification,
    const GError *error,
    gpointer task,
    GObject *weak_object G_GNUC_UNUSED)
{
  if (error == NULL)
    {
      g_task_return_pointer (task, g_strdup (identification), g_free);
    }
  else if (g_error_matches (error, TP_ERROR, TP_ERROR_NOT_IMPLEMENTED) ||
      g_error_matches (error, DBUS_GERROR, DBUS_GERROR_SERVICE_UNKNOWN))
    {
      g_task_return_pointer (task, g_strdup (g_task_get_task_data (task)),
          g_free);
    }
  else
    {
      g_task_return_error (task, g_error_copy (error));
    }
}

static gchar *
identify_account_finish (McpAccountManager *mcpa,
    GAsyncResult *result,
    GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, mcpa), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
identify_account_async (McpAccountManager *mcpa,
    const gchar *manager,
    const gchar *protocol_name,
    GVariant *parameters,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  McdStorage *self = MCD_STORAGE (mcpa);
  GError *error = NULL;
  TpProtocol *protocol;
  GTask *task;
  GValue value = G_VALUE_INIT;
  const gchar *base;

  task = g_task_new (self, cancellable, callback, user_data);

  /* in case IdentifyAccount fails and we need to make something up */
  if (!g_variant_lookup (parameters, "account", "&s", &base))
    base = "account";

  g_task_set_task_data (task, g_strdup (base), g_free);

  protocol = tp_protocol_new (self->dbusd, manager, protocol_name,
      NULL, &error);

  if (protocol == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  dbus_g_value_parse_g_variant (parameters, &value);

  tp_cli_protocol_call_identify_account (protocol, -1,
      g_value_get_boxed (&value), identify_account_cb, task, g_object_unref,
      NULL);
  g_object_unref (protocol);
  g_value_unset (&value);
}

/* sort in descending order of priority (ie higher prio => earlier in list) */
static gint
account_storage_cmp (gconstpointer a, gconstpointer b)
{
    gint pa = mcp_account_storage_priority (a);
    gint pb = mcp_account_storage_priority (b);

    if (pa > pb) return -1;
    if (pa < pb) return 1;

    return 0;
}

static void
add_storage_plugin (McpAccountStorage *plugin)
{
  stores = g_list_insert_sorted (stores, plugin, account_storage_cmp);
}

static void
sort_and_cache_plugins ()
{
  const GList *p;
  static gboolean plugins_cached = FALSE;

  if (plugins_cached)
    return;

  /* not guaranteed to have been called, but idempotent: */
  _mcd_plugin_loader_init ();

  /* Add compiled-in plugins */
  add_storage_plugin (MCP_ACCOUNT_STORAGE (mcd_account_manager_default_new ()));

  for (p = mcp_list_objects(); p != NULL; p = g_list_next (p))
    {
      if (MCP_IS_ACCOUNT_STORAGE (p->data))
        {
          McpAccountStorage *plugin = g_object_ref (p->data);

          add_storage_plugin (plugin);
        }
    }

  for (p = stores; p != NULL; p = g_list_next (p))
    {
      McpAccountStorage *plugin = p->data;

      DEBUG ("found plugin %s [%s; priority %d]\n%s",
          mcp_account_storage_name (plugin),
          g_type_name (G_TYPE_FROM_INSTANCE (plugin)),
          mcp_account_storage_priority (plugin),
          mcp_account_storage_description (plugin));
    }

    plugins_cached = TRUE;
}

void
mcd_storage_connect_signal (const gchar *signame,
    GCallback func,
    gpointer user_data)
{
  GList *p;

  for (p = stores; p != NULL; p = g_list_next (p))
    {
      McpAccountStorage *plugin = p->data;

      DEBUG ("connecting handler to %s plugin signal %s ",
          mcp_account_storage_name (plugin), signame);
      g_signal_connect (plugin, signame, func, user_data);
    }
}

/*
 * mcd_storage_load:
 * @storage: An object implementing the #McdStorage interface
 *
 * Load the long term account settings storage into our internal cache.
 * Should only really be called during startup, ie before our DBus names
 * have been claimed and other people might be relying on responses from us.
 */
void
mcd_storage_load (McdStorage *self)
{
  McpAccountManager *ma = MCP_ACCOUNT_MANAGER (self);
  GList *store = NULL;

  g_return_if_fail (MCD_IS_STORAGE (self));

  sort_and_cache_plugins ();

  store = g_list_last (stores);

  /* fetch accounts stored in plugins, in reverse priority so higher prio *
   * plugins can overwrite lower prio ones' account data                  */
  while (store != NULL)
    {
      GList *account;
      McpAccountStorage *plugin = store->data;
      GList *stored = mcp_account_storage_list (plugin, ma);
      const gchar *pname = mcp_account_storage_name (plugin);
      const gint prio = mcp_account_storage_priority (plugin);

      DEBUG ("listing from plugin %s [prio: %d]", pname, prio);
      for (account = stored; account != NULL; account = g_list_next (account))
        {
          gchar *name = account->data;

          DEBUG ("fetching %s from plugin %s [prio: %d]", name, pname, prio);
          mcd_storage_add_account_from_plugin (self, plugin, name);
          g_free (name);
        }

      /* already freed the contents, just need to free the list itself */
      g_list_free (stored);
      store = g_list_previous (store);
    }
}

/*
 * mcd_storage_dup_accounts:
 * @storage: An object implementing the #McdStorage interface
 * @n: place for the number of accounts to be written to (or %NULL)
 *
 * Returns: a newly allocated GStrv containing the unique account names,
 * which must be freed by the caller with g_strfreev().
 */
GStrv
mcd_storage_dup_accounts (McdStorage *self,
    gsize *n)
{
  GPtrArray *ret = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer k, v;

  g_hash_table_iter_init (&iter, self->accounts);

  while (g_hash_table_iter_next (&iter, &k, &v))
    {
      McdStorageAccount *sa = v;

      if (g_hash_table_size (sa->attributes) > 0)
        g_ptr_array_add (ret, g_strdup (k));
    }

  g_ptr_array_add (ret, NULL);
  return (GStrv) g_ptr_array_free (ret, FALSE);
}

/*
 * mcd_storage_dup_attributes:
 * @storage: An object implementing the #McdStorage interface
 * @account: unique name of the account
 * @n: place for the number of attributes to be written to (or %NULL)
 *
 * Returns: a newly allocated GStrv containing the names of all the
 * attributes or parameters currently stored for @account. Must be
 * freed by the caller with g_strfreev().
 */
GStrv
mcd_storage_dup_attributes (McdStorage *self,
    const gchar *account,
    gsize *n)
{
  GPtrArray *ret = g_ptr_array_new ();
  McdStorageAccount *sa = lookup_account (self, account);

  if (sa != NULL)
    {
      GHashTableIter iter;
      gpointer k;

      g_hash_table_iter_init (&iter, sa->attributes);

      while (g_hash_table_iter_next (&iter, &k, NULL))
        g_ptr_array_add (ret, g_strdup (k));
    }

  g_ptr_array_add (ret, NULL);
  return (GStrv) g_ptr_array_free (ret, FALSE);
}

/*
 * mcd_storage_get_plugin:
 * @storage: An object implementing the #McdStorage interface
 * @account: unique name of the account
 *
 * Returns: the #McpAccountStorage object which is handling the account,
 * if any (if a new account has not yet been flushed to storage this can
 * be %NULL).
 *
 * Plugins are kept in permanent storage and can never be unloaded, so
 * the returned pointer need not be reffed or unreffed. (Indeed, it's
 * probably safer not to)
 */
McpAccountStorage *
mcd_storage_get_plugin (McdStorage *self,
    const gchar *account)
{
  McdStorageAccount *sa;

  g_return_val_if_fail (MCD_IS_STORAGE (self), NULL);
  g_return_val_if_fail (account != NULL, NULL);

  sa = lookup_account (self, account);
  g_return_val_if_fail (sa != NULL, NULL);

  return sa->storage;
}

/*
 * mcd_storage_dup_string:
 * @storage: An object implementing the #McdStorage interface
 * @account: unique name of the account
 * @attribute: name of the attribute to be retrieved (which must not be a
 *  parameter)
 *
 * Returns: a newly allocated gchar * which must be freed with g_free().
 */
gchar *
mcd_storage_dup_string (McdStorage *self,
    const gchar *account,
    const gchar *attribute)
{
  GValue tmp = G_VALUE_INIT;
  gchar *ret;

  g_return_val_if_fail (MCD_IS_STORAGE (self), NULL);
  g_return_val_if_fail (account != NULL, NULL);
  g_return_val_if_fail (attribute != NULL, NULL);
  g_return_val_if_fail (!g_str_has_prefix (attribute, "param-"), NULL);

  g_value_init (&tmp, G_TYPE_STRING);

  if (!mcd_storage_get_attribute (self, account, attribute, &tmp, NULL))
    return NULL;

  ret = g_value_dup_string (&tmp);
  g_value_unset (&tmp);
  return ret;
}

static gboolean
mcd_storage_coerce_variant_to_value (GVariant *variant,
    GValue *value,
    GError **error)
{
  GValue tmp = G_VALUE_INIT;
  gboolean ret;
  gchar *escaped;

  dbus_g_value_parse_g_variant (variant, &tmp);

  if (G_VALUE_TYPE (&tmp) == G_VALUE_TYPE (value))
    {
      memcpy (value, &tmp, sizeof (tmp));
      return TRUE;
    }

  /* This is really pretty stupid but it'll do for now.
   * FIXME: implement a better similar-type-coercion mechanism than
   * round-tripping through a GKeyFile. */
  g_value_unset (&tmp);
  escaped = mcd_keyfile_escape_variant (variant);
  ret = mcd_keyfile_unescape_value (escaped, value, error);
  g_free (escaped);
  return ret;
}

/*
 * mcd_storage_get_attribute:
 * @storage: An object implementing the #McdStorage interface
 * @account: unique name of the account
 * @attribute: name of the attribute to be retrieved, e.g. 'DisplayName'
 * @value: location to return the value, initialized to the right #GType
 * @error: a place to store any #GError<!-- -->s that occur
 */
gboolean
mcd_storage_get_attribute (McdStorage *self,
    const gchar *account,
    const gchar *attribute,
    GValue *value,
    GError **error)
{
  McdStorageAccount *sa;
  GVariant *variant;

  g_return_val_if_fail (MCD_IS_STORAGE (self), FALSE);
  g_return_val_if_fail (account != NULL, FALSE);
  g_return_val_if_fail (attribute != NULL, FALSE);
  g_return_val_if_fail (!g_str_has_prefix (attribute, "param-"), FALSE);

  sa = lookup_account (self, account);

  if (sa == NULL)
    {
      g_set_error (error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
          "Account %s does not exist", account);
      return FALSE;
    }

  variant = g_hash_table_lookup (sa->attributes, attribute);

  if (variant == NULL)
    {
      g_set_error (error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
          "Setting '%s' not stored by account %s", attribute, account);
      return FALSE;
    }

  return mcd_storage_coerce_variant_to_value (variant, value, error);
}

/*
 * mcd_storage_get_parameter:
 * @storage: An object implementing the #McdStorage interface
 * @account: unique name of the account
 * @parameter: name of the parameter to be retrieved, e.g. 'account'
 * @value: location to return the value, initialized to the right #GType
 * @error: a place to store any #GError<!-- -->s that occur
 */
gboolean
mcd_storage_get_parameter (McdStorage *self,
    const gchar *account,
    const gchar *parameter,
    GValue *value,
    GError **error)
{
  McdStorageAccount *sa;
  const gchar *escaped;
  GVariant *variant;

  g_return_val_if_fail (MCD_IS_STORAGE (self), FALSE);
  g_return_val_if_fail (account != NULL, FALSE);
  g_return_val_if_fail (parameter != NULL, FALSE);

  sa = lookup_account (self, account);

  if (sa == NULL)
    {
      g_set_error (error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
          "Account %s does not exist", account);
      return FALSE;
    }

  variant = g_hash_table_lookup (sa->parameters, parameter);

  if (variant != NULL)
    return mcd_storage_coerce_variant_to_value (variant, value, error);

  /* OK, we don't have it as a variant. How about the keyfile-escaped
   * version? */
  escaped = g_hash_table_lookup (sa->escaped_parameters, parameter);

  if (escaped == NULL)
    {
      g_set_error (error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
          "Parameter '%s' not stored by account %s", parameter, account);
      return FALSE;
    }

  return mcd_keyfile_unescape_value (escaped, value, error);
}

static gboolean
mcpa_unescape_value_from_keyfile (const McpAccountManager *unused G_GNUC_UNUSED,
    const gchar *escaped,
    GValue *value,
    GError **error)
{
  return mcd_keyfile_unescape_value (escaped, value, error);
}

/*
 * @escaped: a keyfile-escaped string
 * @value: a #GValue initialized with a supported #GType
 * @error: used to raise an error if %FALSE is returned
 *
 * Try to interpret @escaped as a value of the type of @value. If we can,
 * write the resulting value into @value and return %TRUE.
 *
 * Returns: %TRUE if @escaped could be interpreted as a value of that type
 */
gboolean
mcd_keyfile_unescape_value (const gchar *escaped,
    GValue *value,
    GError **error)
{
  GKeyFile *keyfile;
  gboolean ret;

  g_return_val_if_fail (escaped != NULL, FALSE);
  g_return_val_if_fail (G_IS_VALUE (value), FALSE);

  keyfile = g_key_file_new ();
  g_key_file_set_value (keyfile, "g", "k", escaped);
  ret = mcd_keyfile_get_value (keyfile, "g", "k", value, error);
  g_key_file_free (keyfile);
  return ret;
}

/*
 * mcd_keyfile_get_value:
 * @keyfile: A #GKeyFile
 * @group: name of a group
 * @key: name of a key
 * @value: location to return the value, initialized to the right #GType
 * @error: a place to store any #GError<!-- -->s that occur
 */
gboolean
mcd_keyfile_get_value (GKeyFile *keyfile,
    const gchar *group,
    const gchar *key,
    GValue *value,
    GError **error)
{
  GType type;
  GVariant *variant = NULL;

  g_return_val_if_fail (keyfile != NULL, FALSE);
  g_return_val_if_fail (group != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (G_IS_VALUE (value), FALSE);

  type = G_VALUE_TYPE (value);

  switch (type)
    {
      case G_TYPE_STRING:
        variant = mcd_keyfile_get_variant (keyfile, group, key,
            G_VARIANT_TYPE_STRING, error);
        break;

      case G_TYPE_INT:
        variant = mcd_keyfile_get_variant (keyfile, group, key,
            G_VARIANT_TYPE_INT32, error);
        break;

      case G_TYPE_INT64:
        variant = mcd_keyfile_get_variant (keyfile, group, key,
            G_VARIANT_TYPE_INT64, error);
        break;

      case G_TYPE_UINT:
        variant = mcd_keyfile_get_variant (keyfile, group, key,
            G_VARIANT_TYPE_UINT32, error);
        break;

      case G_TYPE_UCHAR:
        variant = mcd_keyfile_get_variant (keyfile, group, key,
            G_VARIANT_TYPE_BYTE, error);
        break;

      case G_TYPE_UINT64:
        variant = mcd_keyfile_get_variant (keyfile, group, key,
            G_VARIANT_TYPE_UINT64, error);
        break;

      case G_TYPE_BOOLEAN:
        variant = mcd_keyfile_get_variant (keyfile, group, key,
            G_VARIANT_TYPE_BOOLEAN, error);
        break;

      case G_TYPE_DOUBLE:
        variant = mcd_keyfile_get_variant (keyfile, group, key,
            G_VARIANT_TYPE_DOUBLE, error);
        break;

      default:
        if (type == G_TYPE_STRV)
          {
            variant = mcd_keyfile_get_variant (keyfile, group, key,
                G_VARIANT_TYPE_STRING_ARRAY, error);
          }
        else if (type == DBUS_TYPE_G_OBJECT_PATH)
          {
            variant = mcd_keyfile_get_variant (keyfile, group, key,
                G_VARIANT_TYPE_OBJECT_PATH, error);
          }
        else if (type == TP_ARRAY_TYPE_OBJECT_PATH_LIST)
          {
            variant = mcd_keyfile_get_variant (keyfile, group, key,
                G_VARIANT_TYPE_OBJECT_PATH_ARRAY, error);
          }
        else if (type == TP_STRUCT_TYPE_SIMPLE_PRESENCE)
          {
            variant = mcd_keyfile_get_variant (keyfile, group, key,
                G_VARIANT_TYPE ("(uss)"), error);
          }
        else
          {
            gchar *message =
              g_strdup_printf ("cannot get key %s from group %s: "
                  "unknown type %s",
                  key, group, g_type_name (type));

            g_warning ("%s: %s", G_STRFUNC, message);
            g_set_error (error, MCD_ACCOUNT_ERROR,
                MCD_ACCOUNT_ERROR_GET_PARAMETER,
                "%s", message);
            g_free (message);
          }
    }

  if (variant == NULL)
    return FALSE;

  g_variant_ref_sink (variant);
  g_value_unset (value);
  dbus_g_value_parse_g_variant (variant, value);
  g_assert (G_VALUE_TYPE (value) == type);
  g_variant_unref (variant);
  return TRUE;
}

/*
 * mcd_keyfile_get_variant:
 * @keyfile: A #GKeyFile
 * @group: name of a group
 * @key: name of a key
 * @type: the desired type
 * @error: a place to store any #GError<!-- -->s that occur
 *
 * Returns: a new floating #GVariant
 */
GVariant *
mcd_keyfile_get_variant (GKeyFile *keyfile,
    const gchar *group,
    const gchar *key,
    const GVariantType *type,
    GError **error)
{
  const gchar *type_str = g_variant_type_peek_string (type);
  GVariant *ret = NULL;

  g_return_val_if_fail (keyfile != NULL, NULL);
  g_return_val_if_fail (group != NULL, NULL);
  g_return_val_if_fail (key != NULL, NULL);
  g_return_val_if_fail (g_variant_type_string_scan (type_str, NULL, NULL),
      NULL);

  switch (type_str[0])
    {
      case G_VARIANT_CLASS_STRING:
          {
            gchar *v_string = g_key_file_get_string (keyfile, group,
                key, error);

            if (v_string != NULL)
              ret = g_variant_new_string (v_string);
            /* else error is already set */
          }
        break;

      case G_VARIANT_CLASS_INT32:
          {
            GError *e = NULL;
            gint v_int = g_key_file_get_integer (keyfile, group,
                key, &e);

            if (e != NULL)
              {
                g_propagate_error (error, e);
              }
            else
              {
                ret = g_variant_new_int32 (v_int);
              }
          }
        break;

      case G_VARIANT_CLASS_INT64:
          {
            GError *e = NULL;
            gint64 v_int = g_key_file_get_int64 (keyfile, group,
                key, &e);

            if (e != NULL)
              {
                g_propagate_error (error, e);
              }
            else
              {
                ret = g_variant_new_int64 (v_int);
              }
          }
        break;

      case G_VARIANT_CLASS_UINT32:
          {
            GError *e = NULL;
            guint64 v_uint = g_key_file_get_uint64 (keyfile, group,
                key, &e);

            if (e != NULL)
              {
                g_propagate_error (error, e);
              }
            else if (v_uint > G_MAXUINT32)
              {
                g_set_error (error, MCD_ACCOUNT_ERROR,
                    MCD_ACCOUNT_ERROR_GET_PARAMETER,
                    "Parameter '%s' out of range for an unsigned 32-bit "
                    "integer: %" G_GUINT64_FORMAT, key, v_uint);
              }
            else
              {
                ret = g_variant_new_uint32 (v_uint);
              }
          }
        break;

    case G_VARIANT_CLASS_BYTE:
          {
            GError *e = NULL;
            gint v_int = g_key_file_get_integer (keyfile, group,
                key, &e);

            if (e != NULL)
              {
                g_propagate_error (error, e);
              }
            else if (v_int < 0 || v_int > 0xFF)
              {
                g_set_error (error, MCD_ACCOUNT_ERROR,
                    MCD_ACCOUNT_ERROR_GET_PARAMETER,
                    "Parameter '%s' out of range for an unsigned byte: "
                    "%d", key, v_int);
              }
            else
              {
                ret = g_variant_new_byte (v_int);
              }
          }
        break;

      case G_VARIANT_CLASS_UINT64:
          {
            GError *e = NULL;
            guint64 v_uint = g_key_file_get_uint64 (keyfile, group,
                key, &e);

            if (e != NULL)
              {
                g_propagate_error (error, e);
              }
            else
              {
                ret = g_variant_new_uint64 (v_uint);
              }
          }
        break;

      case G_VARIANT_CLASS_BOOLEAN:
          {
            GError *e = NULL;
            gboolean v_bool = g_key_file_get_boolean (keyfile, group,
                key, &e);

            if (e != NULL)
              {
                g_propagate_error (error, e);
              }
            else
              {
                ret = g_variant_new_boolean (v_bool);
              }
          }
        break;

      case G_VARIANT_CLASS_DOUBLE:
          {
            GError *e = NULL;
            gdouble v_double = g_key_file_get_double (keyfile, group,
                key, &e);

            if (e != NULL)
              {
                g_propagate_error (error, e);
              }
            else
              {
                ret = g_variant_new_double (v_double);
              }
          }
        break;

      default:
        if (g_variant_type_equal (type, G_VARIANT_TYPE_STRING_ARRAY))
          {
            gchar **v = g_key_file_get_string_list (keyfile, group,
                key, NULL, error);

            if (v != NULL)
              {
                ret = g_variant_new_strv ((const gchar **) v, -1);
                g_strfreev (v);
              }
          }
        else if (g_variant_type_equal (type, G_VARIANT_TYPE_OBJECT_PATH))
          {
            gchar *v_string = g_key_file_get_string (keyfile, group,
                key, error);

            if (v_string == NULL)
              {
                /* do nothing, error is already set */
              }
            else if (!tp_dbus_check_valid_object_path (v_string, NULL))
              {
                g_set_error (error, MCD_ACCOUNT_ERROR,
                    MCD_ACCOUNT_ERROR_GET_PARAMETER,
                    "Invalid object path %s", v_string);
              }
            else
              {
                ret = g_variant_new_object_path (v_string);
              }

            g_free (v_string);
          }
        else if (g_variant_type_equal (type, G_VARIANT_TYPE_OBJECT_PATH_ARRAY))
          {
            gchar **v = g_key_file_get_string_list (keyfile, group,
                key, NULL, error);

            if (v != NULL)
              {
                gchar **iter;

                for (iter = v; iter != NULL && *iter != NULL; iter++)
                  {
                    if (!g_variant_is_object_path (*iter))
                      {
                        g_set_error (error, MCD_ACCOUNT_ERROR,
                            MCD_ACCOUNT_ERROR_GET_PARAMETER,
                            "Invalid object path %s stored in keyfile", *iter);
                        g_strfreev (v);
                        v = NULL;
                        break;
                      }
                  }

                ret = g_variant_new_objv ((const gchar **) v, -1);
                g_strfreev (v);
              }
          }
        else if (g_variant_type_equal (type, G_VARIANT_TYPE ("(uss)")))
          {
            gchar **v = g_key_file_get_string_list (keyfile, group,
                key, NULL, error);

            if (v == NULL)
              {
                /* error is already set, do nothing */
              }
            else if (g_strv_length (v) != 3)
              {
                g_set_error (error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
                    "Invalid simple-presence structure stored in keyfile");
              }
            else
              {
                guint64 u;
                gchar *endptr;

                errno = 0;
                u = g_ascii_strtoull (v[0], &endptr, 10);

                if (errno != 0 || *endptr != '\0' || u > G_MAXUINT32)
                  {
                    g_set_error (error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
                        "Invalid presence type stored in keyfile: %s", v[0]);
                  }
                else
                  {
                    /* a syntactically valid simple presence */
                    ret = g_variant_new_parsed ("(%u, %s, %s)",
                        (guint32) u, v[1], v[2]);
                  }
              }

            g_strfreev (v);
          }
        else
          {
            gchar *message =
              g_strdup_printf ("cannot get key %s from group %s: "
                  "unknown type %.*s", key, group,
                  (int) g_variant_type_get_string_length (type),
                  type_str);

            g_warning ("%s: %s", G_STRFUNC, message);
            g_set_error (error, MCD_ACCOUNT_ERROR,
                MCD_ACCOUNT_ERROR_GET_PARAMETER,
                "%s", message);
            g_free (message);
          }
    }

  g_assert (ret == NULL || g_variant_is_of_type (ret, type));
  return ret;
}

/*
 * mcd_storage_get_boolean:
 * @storage: An object implementing the #McdStorage interface
 * @account: unique name of the account
 * @key: name of the attribute to be retrieved
 *
 * Returns: a #gboolean. Unset/unparseable values are returned as %FALSE
 */
gboolean
mcd_storage_get_boolean (McdStorage *self,
    const gchar *account,
    const gchar *attribute)
{
  GValue tmp = G_VALUE_INIT;

  g_return_val_if_fail (MCD_IS_STORAGE (self), FALSE);
  g_return_val_if_fail (account != NULL, FALSE);
  g_return_val_if_fail (attribute != NULL, FALSE);
  g_return_val_if_fail (!g_str_has_prefix (attribute, "param-"), FALSE);

  g_value_init (&tmp, G_TYPE_BOOLEAN);

  if (!mcd_storage_get_attribute (self, account, attribute, &tmp, NULL))
    return FALSE;

  return g_value_get_boolean (&tmp);
}

/*
 * mcd_storage_get_integer:
 * @storage: An object implementing the #McdStorage interface
 * @account: unique name of the account
 * @attribute: name of the attribute to be retrieved
 *
 * Returns: a #gint. Unset or non-numeric values are returned as 0
 */
gint
mcd_storage_get_integer (McdStorage *self,
    const gchar *account,
    const gchar *attribute)
{
  GValue tmp = G_VALUE_INIT;

  g_return_val_if_fail (MCD_IS_STORAGE (self), 0);
  g_return_val_if_fail (account != NULL, 0);
  g_return_val_if_fail (attribute != NULL, 0);
  g_return_val_if_fail (!g_str_has_prefix (attribute, "param-"), 0);

  g_value_init (&tmp, G_TYPE_INT);

  if (!mcd_storage_get_attribute (self, account, attribute, &tmp, NULL))
    return FALSE;

  return g_value_get_int (&tmp);
}

static void
update_storage (McdStorage *self,
    const gchar *account,
    const gchar *key,
    GVariant *variant,
    const gchar *escaped)
{
  McpAccountManager *ma = MCP_ACCOUNT_MANAGER (self);
  gboolean parameter = g_str_has_prefix (key, "param-");
  McdStorageAccount *sa;
  const gchar *pn;

  sa = lookup_account (self, account);
  g_return_if_fail (sa != NULL);

  pn = mcp_account_storage_name (sa->storage);
  if (escaped == NULL)
    {
      DEBUG ("MCP:%s -> delete %s.%s", pn, account, key);
      mcp_account_storage_delete (sa->storage, ma, account, key);
    }
  else if (variant != NULL && !parameter &&
      mcp_account_storage_set_attribute (sa->storage, ma, account, key, variant,
          MCP_ATTRIBUTE_FLAG_NONE))
    {
      DEBUG ("MCP:%s -> store attribute %s.%s", pn, account, key);
    }
  else if (variant != NULL && parameter &&
      mcp_account_storage_set_parameter (sa->storage, ma, account, key + 6,
          variant, MCP_PARAMETER_FLAG_NONE))
    {
      DEBUG ("MCP:%s -> store parameter %s.%s", pn, account, key);
    }
  else
    {
      gboolean done;

      done = mcp_account_storage_set (sa->storage, ma, account, key, escaped);
      DEBUG ("MCP:%s -> %s %s.%s", pn, done ? "store" : "ignore", account, key);
    }
}

/*
 * mcd_storage_set_string:
 * @storage: An object implementing the #McdStorage interface
 * @account: the unique name of an account
 * @key: the name of the attribute
 * @value: the value to be stored (or %NULL to erase it)
 *
 * Copies and stores the supplied @value (or removes it if %NULL) to the
 * internal cache.
 *
 * Returns: a #gboolean indicating whether the cache actually required an
 * update (so that the caller can decide whether to request a commit to
 * long term storage or not). %TRUE indicates the cache was updated and
 * may not be in sync with the store any longer, %FALSE indicates we already
 * held the value supplied.
 */
gboolean
mcd_storage_set_string (McdStorage *self,
    const gchar *account,
    const gchar *attribute,
    const gchar *val)
{
  gboolean updated;

  g_return_val_if_fail (MCD_IS_STORAGE (self), FALSE);
  g_return_val_if_fail (account != NULL, FALSE);
  g_return_val_if_fail (attribute != NULL, FALSE);
  g_return_val_if_fail (!g_str_has_prefix (attribute, "param-"), FALSE);

  if (val == NULL)
    {
      updated = mcd_storage_set_attribute (self, account, attribute, NULL);
    }
  else
    {
      GValue tmp = G_VALUE_INIT;

      g_value_init (&tmp, G_TYPE_STRING);
      g_value_set_string (&tmp, val);
      updated = mcd_storage_set_attribute (self, account, attribute, &tmp);
      g_value_unset (&tmp);
    }

  return updated;
}

/*
 * mcd_storage_set_attribute:
 * @storage: An object implementing the #McdStorage interface
 * @account: the unique name of an account
 * @attribute: the name of the attribute, e.g. "DisplayName"
 * @value: the value to be stored (or %NULL to erase it)
 *
 * Copies and stores the supplied @value (or removes it if %NULL) in the
 * internal cache.
 *
 * Returns: a #gboolean indicating whether the cache actually required an
 * update (so that the caller can decide whether to request a commit to
 * long term storage or not). %TRUE indicates the cache was updated and
 * may not be in sync with the store any longer, %FALSE indicates we already
 * held the value supplied.
 */
gboolean
mcd_storage_set_attribute (McdStorage *self,
    const gchar *account,
    const gchar *attribute,
    const GValue *value)
{
  McdStorageAccount *sa;
  GVariant *old_v;
  GVariant *new_v;
  gboolean updated = FALSE;

  g_return_val_if_fail (MCD_IS_STORAGE (self), FALSE);
  g_return_val_if_fail (account != NULL, FALSE);
  g_return_val_if_fail (attribute != NULL, FALSE);
  g_return_val_if_fail (!g_str_has_prefix (attribute, "param-"), FALSE);

  sa = lookup_account (self, account);
  g_return_val_if_fail (sa != NULL, FALSE);

  if (value != NULL)
    new_v = g_variant_ref_sink (dbus_g_value_build_g_variant (value));
  else
    new_v = NULL;

  old_v = g_hash_table_lookup (sa->attributes, attribute);

  if (!mcd_nullable_variant_equal (old_v, new_v))
    {
      gchar *escaped = NULL;

      /* First put it in the attributes hash table. (Watch out, this might
       * invalidate old_v.) */
      if (new_v == NULL)
        g_hash_table_remove (sa->attributes, attribute);
      else
        g_hash_table_insert (sa->attributes, g_strdup (attribute),
            g_variant_ref (new_v));

      /* OK now we have to escape it in a stupid way for plugins */
      if (value != NULL)
        escaped = mcd_keyfile_escape_value (value);

      update_storage (self, account, attribute, new_v, escaped);
      g_free (escaped);
      updated = TRUE;
    }

  tp_clear_pointer (&new_v, g_variant_unref);
  return updated;
}

/*
 * mcd_storage_set_parameter:
 * @storage: An object implementing the #McdStorage interface
 * @account: the unique name of an account
 * @parameter: the name of the parameter, e.g. "account"
 * @value: the value to be stored (or %NULL to erase it)
 *
 * Copies and stores the supplied @value (or removes it if %NULL) in the
 * internal cache.
 *
 * Returns: a #gboolean indicating whether the cache actually required an
 * update (so that the caller can decide whether to request a commit to
 * long term storage or not). %TRUE indicates the cache was updated and
 * may not be in sync with the store any longer, %FALSE indicates we already
 * held the value supplied.
 */
gboolean
mcd_storage_set_parameter (McdStorage *self,
    const gchar *account,
    const gchar *parameter,
    const GValue *value)
{
  GVariant *old_v;
  GVariant *new_v = NULL;
  const gchar *old_escaped;
  gchar *new_escaped = NULL;
  McdStorageAccount *sa;
  gboolean updated = FALSE;

  g_return_val_if_fail (MCD_IS_STORAGE (self), FALSE);
  g_return_val_if_fail (account != NULL, FALSE);
  g_return_val_if_fail (parameter != NULL, FALSE);

  sa = lookup_account (self, account);
  g_return_val_if_fail (sa != NULL, FALSE);

  if (value != NULL)
    {
      new_escaped = mcd_keyfile_escape_value (value);
      new_v = g_variant_ref_sink (dbus_g_value_build_g_variant (value));
    }

  old_v = g_hash_table_lookup (sa->parameters, parameter);
  old_escaped = g_hash_table_lookup (sa->escaped_parameters, parameter);

  if (old_v != NULL)
    updated = !mcd_nullable_variant_equal (old_v, new_v);
  else if (old_escaped != NULL)
    updated = tp_strdiff (old_escaped, new_escaped);
  else
    updated = (value != NULL);

  if (updated)
    {
      gchar key[MAX_KEY_LENGTH];

      g_hash_table_remove (sa->parameters, parameter);
      g_hash_table_remove (sa->escaped_parameters, parameter);

      if (new_v != NULL)
        g_hash_table_insert (sa->parameters, g_strdup (parameter),
            g_variant_ref (new_v));

      g_snprintf (key, sizeof (key), "param-%s", parameter);
      update_storage (self, account, key, new_v, new_escaped);
      return TRUE;
    }

  g_free (new_escaped);
  tp_clear_pointer (&new_v, g_variant_unref);
  return updated;
}

static gchar *
mcpa_escape_value_for_keyfile (const McpAccountManager *unused G_GNUC_UNUSED,
    const GValue *value)
{
  return mcd_keyfile_escape_value (value);
}

/*
 * @value: a populated #GValue of a supported #GType
 *
 * Escape the contents of @value to go in a #GKeyFile. Return the
 * value that would go in the keyfile.
 *
 * For instance, for a boolean value TRUE this would return "true",
 * and for a string containing one space, it would return "\s".
 */
gchar *
mcd_keyfile_escape_value (const GValue *value)
{
  GVariant *variant;
  gchar *ret;

  g_return_val_if_fail (G_IS_VALUE (value), NULL);

  variant = dbus_g_value_build_g_variant (value);

  if (variant == NULL)
    {
      g_warning ("Unable to convert %s to GVariant",
          G_VALUE_TYPE_NAME (value));
      return NULL;
    }

  g_variant_ref_sink (variant);
  ret = mcd_keyfile_escape_variant (variant);
  g_variant_unref (variant);
  return ret;
}

static gchar *
mcpa_escape_variant_for_keyfile (const McpAccountManager *unused G_GNUC_UNUSED,
    GVariant *variant)
{
  return mcd_keyfile_escape_variant (variant);
}

/*
 * mcd_keyfile_set_value:
 * @keyfile: a keyfile
 * @name: the name of a group
 * @key: the key in the group
 * @value: the value to be stored (or %NULL to erase it)
 *
 * Copies and stores the supplied @value (or removes it if %NULL) to the
 * internal cache.
 *
 * Returns: a #gboolean indicating whether the cache actually required an
 * update (so that the caller can decide whether to request a commit to
 * long term storage or not). %TRUE indicates the cache was updated and
 * may not be in sync with the store any longer, %FALSE indicates we already
 * held the value supplied.
 */
gboolean
mcd_keyfile_set_value (GKeyFile *keyfile,
    const gchar *name,
    const gchar *key,
    const GValue *value)
{
  g_return_val_if_fail (name != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);

  if (value == NULL)
    {
      return mcd_keyfile_set_variant (keyfile, name, key, NULL);
    }
  else
    {
      GVariant *variant;
      gboolean ret;

      variant = dbus_g_value_build_g_variant (value);

      if (variant == NULL)
        {
          g_warning ("Unable to convert %s to GVariant",
              G_VALUE_TYPE_NAME (value));
          return FALSE;
        }

      g_variant_ref_sink (variant);
      ret = mcd_keyfile_set_variant (keyfile, name, key, variant);
      g_variant_unref (variant);
      return ret;
    }
}

/*
 * mcd_keyfile_set_variant:
 * @keyfile: a keyfile
 * @name: the name of a group
 * @key: the key in the group
 * @value: the value to be stored (or %NULL to erase it)
 *
 * Escape @variant and store it in the keyfile.
 *
 * Returns: %TRUE if the keyfile actually changed,
 * so that the caller can decide whether to request a commit to
 * long term storage or not.
 */
gboolean
mcd_keyfile_set_variant (GKeyFile *keyfile,
    const gchar *name,
    const gchar *key,
    GVariant *value)
{
  g_return_val_if_fail (name != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);

  if (value == NULL)
    {
      gchar *old = g_key_file_get_value (keyfile, name, key, NULL);
      gboolean updated = (old != NULL);

      g_free (old);
      g_key_file_remove_key (keyfile, name, key, NULL);
      return updated;
    }
  else
    {
      gboolean updated = FALSE;
      gchar *old = g_key_file_get_value (keyfile, name, key, NULL);
      gchar *new = NULL;
      gchar *buf = NULL;

      switch (g_variant_classify (value))
        {
          case G_VARIANT_CLASS_STRING:
          case G_VARIANT_CLASS_OBJECT_PATH:
          case G_VARIANT_CLASS_SIGNATURE:
            g_key_file_set_string (keyfile, name, key,
                g_variant_get_string (value, NULL));
            break;

          case G_VARIANT_CLASS_UINT16:
            buf = g_strdup_printf ("%u", g_variant_get_uint16 (value));
            break;

          case G_VARIANT_CLASS_UINT32:
            buf = g_strdup_printf ("%u", g_variant_get_uint32 (value));
            break;

          case G_VARIANT_CLASS_INT16:
            buf = g_strdup_printf ("%d", g_variant_get_int16 (value));
            break;

          case G_VARIANT_CLASS_INT32:
            buf = g_strdup_printf ("%d", g_variant_get_int32 (value));
            break;

          case G_VARIANT_CLASS_BOOLEAN:
            g_key_file_set_boolean (keyfile, name, key,
                g_variant_get_boolean (value));
            break;

          case G_VARIANT_CLASS_BYTE:
            buf = g_strdup_printf ("%u", g_variant_get_byte (value));
            break;

          case G_VARIANT_CLASS_UINT64:
            buf = g_strdup_printf ("%" G_GUINT64_FORMAT,
                                   g_variant_get_uint64 (value));
            break;

          case G_VARIANT_CLASS_INT64:
            buf = g_strdup_printf ("%" G_GINT64_FORMAT,
                                   g_variant_get_int64 (value));
            break;

          case G_VARIANT_CLASS_DOUBLE:
            g_key_file_set_double (keyfile, name, key,
                g_variant_get_double (value));
            break;

          case G_VARIANT_CLASS_ARRAY:
            if (g_variant_is_of_type (value, G_VARIANT_TYPE_STRING_ARRAY))
              {
                gsize len;
                const gchar **strings = g_variant_get_strv (value, &len);

                g_key_file_set_string_list (keyfile, name, key, strings, len);
              }
            else if (g_variant_is_of_type (value,
                  G_VARIANT_TYPE_OBJECT_PATH_ARRAY))
              {
                gsize len;
                const gchar **strings = g_variant_get_objv (value, &len);

                g_key_file_set_string_list (keyfile, name, key, strings, len);
              }
            else
              {
                g_warning ("Unexpected array type %s",
                    g_variant_get_type_string (value));
                return FALSE;
              }
            break;

          case G_VARIANT_CLASS_TUPLE:
            if (g_variant_is_of_type (value, G_VARIANT_TYPE ("(uss)")))
              {
                guint32 type;
                /* enough for "4294967296" + \0 */
                gchar printf_buf[11];
                const gchar * strv[4] = { NULL, NULL, NULL, NULL };

                g_variant_get (value, "(u&s&s)",
                    &type,
                    &(strv[1]),
                    &(strv[2]));
                g_snprintf (printf_buf, sizeof (printf_buf), "%u", type);
                strv[0] = printf_buf;

                g_key_file_set_string_list (keyfile, name, key, strv, 3);
              }
            else
              {
                g_warning ("Unexpected struct type %s",
                    g_variant_get_type_string (value));
                return FALSE;
              }
            break;

          default:
              {
                g_warning ("Unexpected variant type %s",
                    g_variant_get_type_string (value));
                return FALSE;
              }
        }

      if (buf != NULL)
        g_key_file_set_string (keyfile, name, key, buf);

      new = g_key_file_get_value (keyfile, name, key, NULL);

      if (tp_strdiff (old, new))
        updated = TRUE;

      g_free (new);
      g_free (buf);
      g_free (old);

      return updated;
    }
}

/*
 * mcd_storage_create_account:
 * @storage: An object implementing the #McdStorage interface
 * @provider: the desired storage provider, or %NULL
 * @manager: the name of the manager
 * @protocol: the name of the protocol
 * @identification: the result of IdentifyAccount
 * @error: a #GError to fill when returning %NULL
 *
 * Create a new account in storage. This should not store any
 * information on the long term storage until mcd_storage_commit() is called.
 *
 * See mcp_account_storage_create().
 *
 * Returns: the unique name to use for the new account, or %NULL on error.
 */
gchar *
mcd_storage_create_account (McdStorage *self,
    const gchar *provider,
    const gchar *manager,
    const gchar *protocol,
    const gchar *identification,
    GError **error)
{
  McpAccountManager *ma = MCP_ACCOUNT_MANAGER (self);
  GList *store;
  gchar *account;

  g_return_val_if_fail (MCD_IS_STORAGE (self), NULL);
  g_return_val_if_fail (!tp_str_empty (manager), NULL);
  g_return_val_if_fail (!tp_str_empty (protocol), NULL);

  /* If a storage provider is specified, use only it or fail */
  if (provider != NULL)
    {
      for (store = stores; store != NULL; store = g_list_next (store))
        {
          McpAccountStorage *plugin = store->data;

          if (!tp_strdiff (mcp_account_storage_provider (plugin), provider))
            {
              account = mcp_account_storage_create (plugin, ma, manager,
                  protocol, identification, error);
              mcd_storage_add_account_from_plugin (self, plugin, account);
              return account;
            }
        }

      g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
          "Storage provider '%s' does not exist", provider);

      return NULL;
    }

  /* No provider specified, let's pick the first plugin able to create this
   * account in priority order.
   */
  for (store = stores; store != NULL; store = g_list_next (store))
    {
      McpAccountStorage *plugin = store->data;

      account = mcp_account_storage_create (plugin, ma, manager, protocol,
          identification, error);

      if (account != NULL)
        {
          mcd_storage_add_account_from_plugin (self, plugin, account);
          return account;
        }

      g_clear_error (error);
    }

  /* This should never happen since the default storage is always able to create
   * an account */
  g_warn_if_reached ();
  g_set_error (error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
      "None of the storage provider are able to create the account");

  return NULL;
}


/*
 * mcd_storage_delete_account:
 * @storage: An object implementing the #McdStorage interface
 * @account: unique name of the account
 *
 * Removes an account's settings from long term storage.
 * This does not handle any of the other logic to do with removing
 * accounts, it merely ensures that no trace of the account remains
 * in long term storage once mcd_storage_commit() has been called.
 */
void
mcd_storage_delete_account (McdStorage *self,
    const gchar *account)
{
  McpAccountManager *ma = MCP_ACCOUNT_MANAGER (self);
  McdStorageAccount *sa;

  g_return_if_fail (MCD_IS_STORAGE (self));
  g_return_if_fail (account != NULL);

  sa = lookup_account (self, account);
  g_return_if_fail (sa != NULL);

  mcp_account_storage_delete (sa->storage, ma, account, NULL);
  g_hash_table_remove (self->accounts, account);
}

/*
 * mcd_storage_commit:
 * @storage: An object implementing the #McdStorage interface
 * @account: the unique name of an account
 *
 * Sync the long term storage (whatever it might be) with the current
 * state of our internal cache.
 */
void
mcd_storage_commit (McdStorage *self, const gchar *account)
{
  McpAccountManager *ma = MCP_ACCOUNT_MANAGER (self);
  GList *store;

  g_return_if_fail (MCD_IS_STORAGE (self));

  if (account != NULL)
    {
      McdStorageAccount *sa = lookup_account (self, account);

      g_return_if_fail (sa != NULL);

      DEBUG ("flushing plugin %s %s to long term storage",
          mcp_account_storage_name (sa->storage), account);
      mcp_account_storage_commit (sa->storage, ma, account);
      return;
    }

  for (store = stores; store != NULL; store = g_list_next (store))
    {
      McpAccountStorage *plugin = store->data;

      DEBUG ("flushing plugin %s to long term storage",
          mcp_account_storage_name (plugin));
      mcp_account_storage_commit (plugin, ma, NULL);
    }
}

/*
 * mcd_storage_set_strv:
 * @storage: An object implementing the #McdStorage interface
 * @account: the unique name of an account
 * @attribute: the name of the attribute
 * @strv: the string vector to be stored (where %NULL is treated as equivalent
 * to an empty vector)
 *
 * Copies and stores the supplied string vector to the internal cache.
 *
 * Returns: a #gboolean indicating whether the cache actually required an
 * update (so that the caller can decide whether to request a commit to
 * long term storage or not). %TRUE indicates the cache was updated and
 * may not be in sync with the store any longer, %FALSE indicates we already
 * held the value supplied.
 */
gboolean
mcd_storage_set_strv (McdStorage *storage,
    const gchar *account,
    const gchar *attribute,
    const gchar * const *strv)
{
  GValue v = G_VALUE_INIT;
  static const gchar * const *empty = { NULL };
  gboolean ret;

  g_return_val_if_fail (MCD_IS_STORAGE (storage), FALSE);
  g_return_val_if_fail (account != NULL, FALSE);
  g_return_val_if_fail (attribute != NULL, FALSE);
  g_return_val_if_fail (!g_str_has_prefix (attribute, "param-"), FALSE);

  g_value_init (&v, G_TYPE_STRV);
  g_value_set_static_boxed (&v, strv == NULL ? empty : strv);
  ret = mcd_storage_set_attribute (storage, account, attribute, &v);
  g_value_unset (&v);
  return ret;
}

void
mcd_storage_ready (McdStorage *self)
{
  GList *store;
  McpAccountManager *ma = MCP_ACCOUNT_MANAGER (self);

  for (store = stores; store != NULL; store = g_list_next (store))
    {
      McpAccountStorage *plugin = store->data;
      const gchar *plugin_name = mcp_account_storage_name (plugin);

      DEBUG ("Unblocking async account ops by %s", plugin_name);
      mcp_account_storage_ready (plugin, ma);
    }
}

static void
plugin_iface_init (McpAccountManagerIface *iface,
    gpointer unused G_GNUC_UNUSED)
{
  DEBUG ();

  iface->get_value = get_value;
  iface->set_value = set_value;
  iface->set_attribute = mcpa_set_attribute;
  iface->set_parameter = mcpa_set_parameter;
  iface->unique_name = unique_name;
  iface->identify_account_async = identify_account_async;
  iface->identify_account_finish = identify_account_finish;
  iface->list_keys = list_keys;
  iface->escape_value_for_keyfile = mcpa_escape_value_for_keyfile;
  iface->escape_variant_for_keyfile = mcpa_escape_variant_for_keyfile;
  iface->unescape_value_from_keyfile = mcpa_unescape_value_from_keyfile;
  iface->init_value_for_attribute = mcpa_init_value_for_attribute;
}

void
mcd_storage_add_account_from_plugin (McdStorage *self,
    McpAccountStorage *plugin,
    const gchar *account)
{
  g_return_if_fail (MCD_IS_STORAGE (self));
  g_return_if_fail (MCP_IS_ACCOUNT_STORAGE (plugin));
  g_return_if_fail (account != NULL);
  g_return_if_fail (!g_hash_table_contains (self->accounts, account));

  g_hash_table_insert (self->accounts, g_strdup (account),
      mcd_storage_account_new (plugin));

  /* This will fill our parameters/attributes tables */
  mcp_account_storage_get (plugin, MCP_ACCOUNT_MANAGER (self), account, NULL);
}

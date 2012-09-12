/*
 * A demonstration plugin that diverts account storage to D-Bus, where the
 * regression tests can manipulate it.
 *
 * Copyright © 2010 Nokia Corporation
 * Copyright © 2010-2012 Collabora Ltd.
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

#include "config.h"
#include "dbus-account-plugin.h"

#define DEBUG(format, ...) g_debug ("%s: " format, G_STRFUNC, ##__VA_ARGS__)

#define TESTDOT "org.freedesktop.Telepathy.MC.Test."
#define TESTSLASH "/org/freedesktop/Telepathy/MC/Test/"

#define TEST_DBUS_ACCOUNT_SERVICE       TESTDOT   "DBusAccountService"
#define TEST_DBUS_ACCOUNT_SERVICE_PATH  TESTSLASH "DBusAccountService"
#define TEST_DBUS_ACCOUNT_SERVICE_IFACE TEST_DBUS_ACCOUNT_SERVICE

#define TEST_DBUS_ACCOUNT_PLUGIN_PATH   TESTSLASH "DBusAccountPlugin"
#define TEST_DBUS_ACCOUNT_PLUGIN_IFACE  TESTDOT   "DBusAccountPlugin"

/* for now, the concepts of parameter/attribute flags are local to this
 * plugin */

typedef enum {
    ATTRIBUTE_FLAG_NONE = 0
} AttributeFlag;

typedef enum {
    PARAMETER_FLAG_NONE = 0,
    PARAMETER_FLAG_SECRET = 1
} ParameterFlag;

typedef struct {
    gchar *path;
    /* string => GVariant */
    GHashTable *attributes;
    /* string => GUINT_TO_POINTER(guint32) */
    GHashTable *attribute_flags;
    /* set of strings */
    GHashTable *uncommitted_attributes;
    /* string => GVariant */
    GHashTable *parameters;
    /* string => string */
    GHashTable *untyped_parameters;
    /* string => GUINT_TO_POINTER(guint32) */
    GHashTable *parameter_flags;
    /* set of strings */
    GHashTable *uncommitted_parameters;
    enum { UNCOMMITTED_CREATION, UNCOMMITTED_DELETION } flags;
} Account;

static void
test_dbus_account_free (gpointer p)
{
  Account *account = p;

  g_hash_table_unref (account->attributes);
  g_hash_table_unref (account->attribute_flags);
  g_hash_table_unref (account->parameters);
  g_hash_table_unref (account->untyped_parameters);
  g_hash_table_unref (account->parameter_flags);

  g_hash_table_unref (account->uncommitted_attributes);
  g_hash_table_unref (account->uncommitted_parameters);

  g_slice_free (Account, account);
}

static void account_storage_iface_init (McpAccountStorageIface *);

struct _TestDBusAccountPlugin {
  GObject parent;

  GDBusConnection *bus;
  GHashTable *accounts;
  gboolean active;
};

struct _TestDBusAccountPluginClass {
  GObjectClass parent_class;
};

G_DEFINE_TYPE_WITH_CODE (TestDBusAccountPlugin, test_dbus_account_plugin,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (MCP_TYPE_ACCOUNT_STORAGE,
        account_storage_iface_init));

typedef struct {
    TestDBusAccountPlugin *self;
    gchar *account_name;
} AsyncData;

static AsyncData *
async_data_new (TestDBusAccountPlugin *self,
    const gchar *account_name)
{
  AsyncData *ret = g_slice_new0 (AsyncData);

  ret->self = g_object_ref (self);
  ret->account_name = g_strdup (account_name);
  return ret;
}

static void
async_data_free (AsyncData *ad)
{
  g_clear_object (&ad->self);
  g_free (ad->account_name);
  g_slice_free (AsyncData, ad);
}

static Account *
lookup_account (TestDBusAccountPlugin *self,
    const gchar *account_name)
{
  return g_hash_table_lookup (self->accounts, account_name);
}

static Account *
ensure_account (TestDBusAccountPlugin *self,
    const gchar *account_name)
{
  Account *account = lookup_account (self, account_name);

  if (account == NULL)
    {
      account = g_slice_new (Account);
      account->path = g_strdup_printf ("%s%s", TP_ACCOUNT_OBJECT_PATH_BASE,
          account_name);

      account->attributes = g_hash_table_new_full (g_str_hash, g_str_equal,
          g_free, (GDestroyNotify) g_variant_unref);
      account->attribute_flags = g_hash_table_new_full (g_str_hash,
          g_str_equal, g_free, NULL);
      account->parameters = g_hash_table_new_full (g_str_hash, g_str_equal,
          g_free, (GDestroyNotify) g_variant_unref);
      account->untyped_parameters = g_hash_table_new_full (g_str_hash,
          g_str_equal, g_free, g_free);
      account->parameter_flags = g_hash_table_new_full (g_str_hash,
          g_str_equal, g_free, NULL);

      account->uncommitted_attributes = g_hash_table_new_full (g_str_hash,
          g_str_equal, g_free, NULL);
      account->uncommitted_parameters = g_hash_table_new_full (g_str_hash,
          g_str_equal, g_free, NULL);

      account->flags = UNCOMMITTED_CREATION;

      g_hash_table_insert (self->accounts, g_strdup (account_name), account);
    }

  account->flags &= ~UNCOMMITTED_DELETION;
  return account;
}

static void
service_appeared_cb (GDBusConnection *bus,
    const gchar *name,
    const gchar *owner,
    gpointer user_data)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (user_data);

  self->active = TRUE;

  /* FIXME: for now, we assume there are no accounts. */

  g_dbus_connection_emit_signal (self->bus, NULL,
      TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
      "Active", NULL, NULL);
}

static void
service_vanished_cb (GDBusConnection *bus,
    const gchar *name,
    gpointer user_data)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (user_data);
  GHashTableIter iter;
  gpointer k, v;

  self->active = FALSE;
  g_hash_table_iter_init (&iter, self->accounts);

  while (g_hash_table_iter_next (&iter, &k, &v))
    {
      Account *account = v;

      if ((account->flags & UNCOMMITTED_DELETION) == 0)
        mcp_account_storage_emit_deleted (MCP_ACCOUNT_STORAGE (self), k);

      g_hash_table_iter_remove (&iter);
    }

  g_dbus_connection_emit_signal (self->bus, NULL,
      TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
      "Inactive", NULL, NULL);
}

static void
test_dbus_account_plugin_init (TestDBusAccountPlugin *self)
{
  GError *error = NULL;

  DEBUG ("called");

  self->accounts = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, test_dbus_account_free);

  self->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);
  g_assert (self->bus != NULL);

  g_bus_watch_name (G_BUS_TYPE_SESSION, TEST_DBUS_ACCOUNT_SERVICE,
      G_BUS_NAME_WATCHER_FLAGS_NONE,
      service_appeared_cb,
      service_vanished_cb,
      g_object_ref (self),
      g_object_unref);
}

static void
test_dbus_account_plugin_class_init (TestDBusAccountPluginClass *cls)
{
  DEBUG ("called");
}

static void
test_dbus_account_plugin_add_account (TestDBusAccountPlugin *self,
    const gchar *account_name,
    GVariant *attributes,
    GVariant *attribute_flags,
    GVariant *parameters,
    GVariant *untyped_parameters,
    GVariant *param_flags)
{
  GVariantIter iter;
  const gchar *k;
  GVariant *v;
  const gchar *s;
  guint32 u;
  Account *account = ensure_account (self, account_name);

  g_variant_iter_init (&iter, attributes);

  while (g_variant_iter_loop (&iter, "{sv}", &k, &v))
    g_hash_table_insert (account->attributes, g_strdup (k),
        g_variant_ref (v));

  g_variant_iter_init (&iter, attribute_flags);

  while (g_variant_iter_loop (&iter, "{su}", &k, &u))
    g_hash_table_insert (account->attribute_flags, g_strdup (k),
        GUINT_TO_POINTER (u));

  g_variant_iter_init (&iter, parameters);

  while (g_variant_iter_loop (&iter, "{sv}", &k, &v))
    g_hash_table_insert (account->parameters, g_strdup (k),
        g_variant_ref (v));

  g_variant_iter_init (&iter, untyped_parameters);

  while (g_variant_iter_loop (&iter, "{ss}", &k, &s))
    g_hash_table_insert (account->untyped_parameters,
        g_strdup (k), g_strdup (s));

  g_variant_iter_init (&iter, param_flags);

  while (g_variant_iter_loop (&iter, "{su}", &k, &u))
    g_hash_table_insert (account->parameter_flags, g_strdup (k),
        GUINT_TO_POINTER (u));
}

static GList *
test_dbus_account_plugin_list (const McpAccountStorage *storage,
    const McpAccountManager *am)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (storage);
  GError *error = NULL;
  GVariant *tuple, *accounts;
  GVariant *attributes, *attribute_flags;
  GVariant *parameters, *untyped_parameters, *param_flags;
  GVariantIter account_iter;
  const gchar *account_name;
  GList *ret = NULL;

  DEBUG ("called");

  g_dbus_connection_emit_signal (self->bus, NULL,
      TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
      "Listing", NULL, NULL);

  /* list is allowed to block */
  tuple = g_dbus_connection_call_sync (self->bus,
      TEST_DBUS_ACCOUNT_SERVICE,
      TEST_DBUS_ACCOUNT_SERVICE_PATH,
      TEST_DBUS_ACCOUNT_SERVICE_IFACE,
      "GetAccounts",
      NULL, /* no parameters */
      G_VARIANT_TYPE ("(a{s(a{sv}a{su}a{sv}a{ss}a{su})})"),
      G_DBUS_CALL_FLAGS_NONE,
      -1,
      NULL, /* no cancellable */
      &error);

  if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
      g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
    {
      /* this regression test isn't using the fake accounts service */
      g_clear_error (&error);
      return NULL;
    }

  self->active = TRUE;

  g_assert_no_error (error);

  g_variant_get (tuple, "(@*)", &accounts);

  g_variant_iter_init (&account_iter, accounts);

  while (g_variant_iter_loop (&account_iter,
        "{s(@a{sv}@a{su}@a{sv}@a{ss}@a{su})}", &account_name,
        &attributes, &attribute_flags,
        &parameters, &untyped_parameters, &param_flags))
    {
      test_dbus_account_plugin_add_account (self, account_name,
          attributes, attribute_flags, parameters, untyped_parameters,
          param_flags);

      ret = g_list_prepend (ret, g_strdup (account_name));
    }

  g_variant_unref (accounts);
  g_variant_unref (tuple);
  return ret;
}

static void
test_dbus_account_plugin_ready (const McpAccountStorage *storage,
    const McpAccountManager *am)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (storage);

  DEBUG ("called");
  g_dbus_connection_emit_signal (self->bus, NULL,
      TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
      "Ready", NULL, NULL);
}

static gchar *
test_dbus_account_plugin_create (const McpAccountStorage *storage,
    const McpAccountManager *am,
    const gchar *manager,
    const gchar *protocol,
    GHashTable *params,
    GError **error)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (storage);
  Account *account;
  gchar *name;

  if (!self->active)
    return FALSE;

  name = mcp_account_manager_get_unique_name ((McpAccountManager *) am,
      manager, protocol, params);
  account = ensure_account (self, name);
  g_dbus_connection_emit_signal (self->bus, NULL,
      TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
      "DeferringCreate", g_variant_new_parsed ("(%o,)", account->path), NULL);
  return name;
}

static gboolean
test_dbus_account_plugin_delete (const McpAccountStorage *storage,
    const McpAccountManager *am,
    const gchar *account_name,
    const gchar *key)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (storage);
  Account *account = lookup_account (self, account_name);

  DEBUG ("called");

  if (account == NULL || !self->active)
    return FALSE;

  if (key == NULL)
    {
      account->flags |= UNCOMMITTED_DELETION;
      g_hash_table_remove_all (account->attributes);
      g_hash_table_remove_all (account->parameters);
      g_hash_table_remove_all (account->untyped_parameters);
      g_hash_table_remove_all (account->attribute_flags);
      g_hash_table_remove_all (account->parameter_flags);

      account->flags &= ~UNCOMMITTED_CREATION;
      g_hash_table_remove_all (account->uncommitted_attributes);
      g_hash_table_remove_all (account->uncommitted_parameters);

      g_dbus_connection_emit_signal (self->bus, NULL,
          TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
          "DeferringDelete", g_variant_new_parsed ("(%o,)", account->path),
          NULL);
    }
  else if (g_str_has_prefix (key, "param-"))
    {
      g_hash_table_remove (account->parameters, key + 6);
      g_hash_table_remove (account->untyped_parameters, key + 6);
      g_hash_table_remove (account->parameter_flags, key + 6);
      g_hash_table_add (account->uncommitted_parameters, g_strdup (key + 6));

      g_dbus_connection_emit_signal (self->bus, NULL,
          TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
          "DeferringDeleteParameter",
          g_variant_new_parsed ("(%o, %s)", account->path, key + 6), NULL);
    }
  else
    {
      g_hash_table_remove (account->attributes, key);
      g_hash_table_remove (account->attribute_flags, key);
      g_hash_table_add (account->uncommitted_attributes, g_strdup (key));

      g_dbus_connection_emit_signal (self->bus, NULL,
          TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
          "DeferringDeleteAttribute",
          g_variant_new_parsed ("(%o, %s)", account->path, key), NULL);
    }

  return TRUE;
}

static gboolean
test_dbus_account_plugin_get (const McpAccountStorage *storage,
    const McpAccountManager *am,
    const gchar *account_name,
    const gchar *key)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (storage);
  Account *account = lookup_account (self, account_name);

  if (!self->active || account == NULL || (account->flags & UNCOMMITTED_DELETION))
    return FALSE;

  if (key == NULL)
    {
      GHashTableIter iter;
      gpointer k, v;

      /* get everything */
      g_dbus_connection_emit_signal (self->bus, NULL,
          TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
          "GetAllKeys",
          g_variant_new_parsed ("(%o,)", account->path), NULL);

      g_hash_table_iter_init (&iter, account->attributes);

      while (g_hash_table_iter_next (&iter, &k, &v))
        {
          gchar *escaped = mcp_account_manager_escape_variant_for_keyfile (am,
              v);

          mcp_account_manager_set_value (am, account_name, k, escaped);
          g_free (escaped);
        }

      g_hash_table_iter_init (&iter, account->untyped_parameters);

      while (g_hash_table_iter_next (&iter, &k, &v))
        {
          gchar *param_foo;
          guint32 flags;

          param_foo = g_strdup_printf ("param-%s", (const gchar *) k);
          mcp_account_manager_set_value (am, account_name, param_foo, v);

          flags = GPOINTER_TO_UINT (g_hash_table_lookup (account->parameter_flags,
                k));

          if (flags & PARAMETER_FLAG_SECRET)
            mcp_account_manager_parameter_make_secret (am, account_name,
                param_foo);

          g_free (param_foo);
        }

      g_hash_table_iter_init (&iter, account->parameters);

      while (g_hash_table_iter_next (&iter, &k, &v))
        {
          gchar *param_foo;
          guint32 flags;
          gchar *escaped = mcp_account_manager_escape_variant_for_keyfile (am,
              v);

          param_foo = g_strdup_printf ("param-%s", (const gchar *) k);
          mcp_account_manager_set_value (am, account_name, param_foo, escaped);
          g_free (escaped);

          flags = GPOINTER_TO_UINT (g_hash_table_lookup (account->parameter_flags,
                k));

          if (flags & PARAMETER_FLAG_SECRET)
            mcp_account_manager_parameter_make_secret (am, account_name,
                param_foo);

          g_free (param_foo);
        }

      return TRUE;
    }

  /* get one parameter */

  if (g_str_has_prefix (key, "param-"))
    {
      GVariant *v = g_hash_table_lookup (account->parameters, key + 6);
      const gchar *s = g_hash_table_lookup (account->untyped_parameters, key + 6);
      guint32 flags = GPOINTER_TO_UINT (
          g_hash_table_lookup (account->parameter_flags, key + 6));

      g_dbus_connection_emit_signal (self->bus, NULL,
          TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
          "GetParameter",
          g_variant_new_parsed ("(%o, %s)", account->path, key + 6), NULL);

      if (flags & PARAMETER_FLAG_SECRET)
        mcp_account_manager_parameter_make_secret (am, account_name, key);

      if (v != NULL)
        {
          gchar *escaped = mcp_account_manager_escape_variant_for_keyfile (am,
              v);

          mcp_account_manager_set_value (am, account_name, key, escaped);
          g_free (escaped);
        }
      else if (s != NULL)
        {
          mcp_account_manager_set_value (am, account_name, key, s);
        }
      else
        {
          return FALSE;
        }
    }
  else
    {
      GVariant *v = g_hash_table_lookup (account->attributes, key);

      g_dbus_connection_emit_signal (self->bus, NULL,
          TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
          "GetAttribute",
          g_variant_new_parsed ("(%o, %s)", account->path, key), NULL);

      if (v != NULL)
        {
          gchar *escaped = mcp_account_manager_escape_variant_for_keyfile (am,
              v);

          mcp_account_manager_set_value (am, account_name, key, escaped);
          g_free (escaped);
        }
      else
        {
          return FALSE;
        }
    }

  return TRUE;
}

static gboolean
test_dbus_account_plugin_set (const McpAccountStorage *storage,
    const McpAccountManager *am,
    const gchar *account_name,
    const gchar *key,
    const gchar *value)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (storage);
  Account *account = lookup_account (self, account_name);

  g_return_val_if_fail (account_name != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);
  /* for deletions, MC would call delete() instead */
  g_return_val_if_fail (value != NULL, FALSE);

  DEBUG ("%s of %s", key, account_name);

  if (!self->active || account == NULL ||
      (account->flags & UNCOMMITTED_DELETION))
    return FALSE;

  if (g_str_has_prefix (key, "param-"))
    {
      guint32 flags = PARAMETER_FLAG_NONE;

      if (mcp_account_manager_parameter_is_secret (am, account_name, key))
        {
          flags |= PARAMETER_FLAG_SECRET;
        }

      g_hash_table_remove (account->parameters, key + 6);
      g_hash_table_insert (account->untyped_parameters, g_strdup (key + 6),
          g_strdup (value));
      g_hash_table_insert (account->parameter_flags, g_strdup (key + 6),
          GUINT_TO_POINTER (flags));
      g_hash_table_add (account->uncommitted_parameters, g_strdup (key + 6));

      g_dbus_connection_emit_signal (self->bus, NULL,
          TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
          "DeferringSetParameterUntyped",
          g_variant_new_parsed ("(%o, %s, %s)", account->path, key, value), NULL);
    }
  else
    {
      GValue gvalue = G_VALUE_INIT;
      GError *error = NULL;
      GVariant *variant;

      if (!mcp_account_manager_init_value_for_attribute (am, &gvalue, key))
        {
          g_warning ("Cannot store unknown attribute %s", key);
          return FALSE;
        }

      if (!mcp_account_manager_unescape_value_from_keyfile (am, value,
            &gvalue, &error))
        {
          g_warning ("MC gave me a attribute it couldn't unescape: %s: %s",
              key, error->message);
          g_clear_error (&error);
          return FALSE;
        }

      variant = g_variant_ref_sink (dbus_g_value_build_g_variant (&gvalue));

      g_hash_table_insert (account->attributes, g_strdup (key),
          g_variant_ref (variant));
      g_hash_table_remove (account->attribute_flags, key);
      g_hash_table_add (account->uncommitted_attributes, g_strdup (key));
      g_value_unset (&gvalue);

      g_dbus_connection_emit_signal (self->bus, NULL,
          TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
          "DeferringSetAttribute",
          g_variant_new_parsed ("(%o, %s, %v)", account->path, key, variant), NULL);
      g_variant_unref (variant);
    }

  return TRUE;
}

static gboolean
test_dbus_account_plugin_commit (const McpAccountStorage *storage,
    const McpAccountManager *am)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (storage);
  GHashTableIter iter;
  gpointer k;

  DEBUG ("called");

  if (!self->active)
    return FALSE;

  g_dbus_connection_emit_signal (self->bus, NULL,
      TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
      "CommittingAll", NULL, NULL);

  g_hash_table_iter_init (&iter, self->accounts);

  while (g_hash_table_iter_next (&iter, &k, NULL))
    {
      if (!mcp_account_storage_commit_one (storage, am, k))
        {
          g_warning ("declined to commit account %s", (const gchar *) k);
        }
    }

  return TRUE;
}

static void
delete_account_cb (GObject *source_object,
    GAsyncResult *res,
    gpointer user_data)
{
  AsyncData *ad = user_data;
  GVariant *tuple;
  GError *error = NULL;

  tuple = g_dbus_connection_call_finish (ad->self->bus, res, &error);

  if (tuple != NULL)
    {
      g_hash_table_remove (ad->self->accounts, ad->account_name);
      g_variant_unref (tuple);
    }
  else
    {
      g_warning ("Unable to delete account %s: %s", ad->account_name,
          error->message);
      g_clear_error (&error);
      /* FIXME: we could roll back the deletion by claiming that
       * the service re-created the account? */
    }

  async_data_free (ad);
}

static void
create_account_cb (GObject *source_object,
    GAsyncResult *res,
    gpointer user_data)
{
  AsyncData *ad = user_data;
  GVariant *tuple;
  GError *error = NULL;

  tuple = g_dbus_connection_call_finish (ad->self->bus, res, &error);

  if (tuple != NULL)
    {
      Account *account = lookup_account (ad->self, ad->account_name);

      if (account != NULL)
        account->flags &= ~UNCOMMITTED_CREATION;

      g_variant_unref (tuple);
    }
  else
    {
      g_warning ("Unable to create account %s: %s", ad->account_name,
          error->message);
      g_clear_error (&error);
      /* FIXME: we could roll back the creation by claiming that
       * the service deleted the account? If we do, we will have
       * to do it in an idle because we might be iterating over
       * all accounts in commit() */
    }

  async_data_free (ad);
}

static void
update_attributes_cb (GObject *source_object,
    GAsyncResult *res,
    gpointer user_data)
{
  AsyncData *ad = user_data;
  GVariant *tuple;
  GError *error = NULL;

  tuple = g_dbus_connection_call_finish (ad->self->bus, res, &error);

  if (tuple != NULL)
    {
      Account *account = lookup_account (ad->self, ad->account_name);

      DEBUG ("Successfully committed attributes of %s", ad->account_name);

      if (account != NULL)
        g_hash_table_remove_all (account->uncommitted_attributes);

      g_variant_unref (tuple);
    }
  else
    {
      g_warning ("Unable to update attributes on %s: %s", ad->account_name,
          error->message);
      g_clear_error (&error);
      /* FIXME: we could roll back the creation by claiming that
       * the service restored the old attributes? */
    }

  async_data_free (ad);
}

static void
update_parameters_cb (GObject *source_object,
    GAsyncResult *res,
    gpointer user_data)
{
  AsyncData *ad = user_data;
  GVariant *tuple;
  GError *error = NULL;

  tuple = g_dbus_connection_call_finish (ad->self->bus, res, &error);

  if (tuple != NULL)
    {
      Account *account = lookup_account (ad->self, ad->account_name);

      DEBUG ("Successfully committed parameters of %s", ad->account_name);

      if (account != NULL)
        g_hash_table_remove_all (account->uncommitted_parameters);

      g_variant_unref (tuple);
    }
  else
    {
      g_warning ("Unable to update parameters on %s: %s", ad->account_name,
          error->message);
      g_clear_error (&error);
      /* FIXME: we could roll back the creation by claiming that
       * the service restored the old parameters? */
    }

  async_data_free (ad);
}

static gboolean
test_dbus_account_plugin_commit_one (const McpAccountStorage *storage,
    const McpAccountManager *am,
    const gchar *account_name)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (storage);
  Account *account = lookup_account (self, account_name);
  GHashTableIter iter;
  gpointer k;
  GVariantBuilder a_sv_builder;
  GVariantBuilder a_ss_builder;
  GVariantBuilder a_su_builder;
  GVariantBuilder as_builder;

  DEBUG ("%s", account_name);

  /* MC does not call @commit_one with parameter %NULL (meaning "all accounts")
   * if we also implement commit(), which, as it happens, we do */
  g_return_val_if_fail (account_name != NULL, FALSE);

  if (!self->active || account == NULL)
    return FALSE;

  g_dbus_connection_emit_signal (self->bus, NULL,
      TEST_DBUS_ACCOUNT_PLUGIN_PATH, TEST_DBUS_ACCOUNT_PLUGIN_IFACE,
      "CommittingOne", g_variant_new_parsed ("(%o,)", account->path), NULL);

  if (account->flags & UNCOMMITTED_DELETION)
    {
      g_dbus_connection_call (self->bus,
          TEST_DBUS_ACCOUNT_SERVICE,
          TEST_DBUS_ACCOUNT_SERVICE_PATH,
          TEST_DBUS_ACCOUNT_SERVICE_IFACE,
          "DeleteAccount",
          g_variant_new_parsed ("(%s,)", account_name),
          G_VARIANT_TYPE_UNIT,
          G_DBUS_CALL_FLAGS_NONE,
          -1,
          NULL, /* no cancellable */
          delete_account_cb,
          async_data_new (self, account_name));

      /* this doesn't mean we succeeded: it means we tried */
      return TRUE;
    }

  if (account->flags & UNCOMMITTED_CREATION)
    {
      g_dbus_connection_call (self->bus,
          TEST_DBUS_ACCOUNT_SERVICE,
          TEST_DBUS_ACCOUNT_SERVICE_PATH,
          TEST_DBUS_ACCOUNT_SERVICE_IFACE,
          "CreateAccount",
          g_variant_new_parsed ("(%s,)", account_name),
          G_VARIANT_TYPE_UNIT,
          G_DBUS_CALL_FLAGS_NONE,
          -1,
          NULL, /* no cancellable */
          create_account_cb,
          async_data_new (self, account_name));
    }

  if (g_hash_table_size (account->uncommitted_attributes) != 0)
    {
      g_variant_builder_init (&a_sv_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_init (&a_su_builder, G_VARIANT_TYPE ("a{su}"));
      g_variant_builder_init (&as_builder, G_VARIANT_TYPE_STRING_ARRAY);
      g_hash_table_iter_init (&iter, account->uncommitted_attributes);

      while (g_hash_table_iter_next (&iter, &k, NULL))
        {
          GVariant *v = g_hash_table_lookup (account->attributes, k);

          DEBUG ("Attribute %s uncommitted, committing it now",
              (const gchar *) k);

          if (v != NULL)
            {
              g_variant_builder_add (&a_sv_builder, "{sv}", k, v);
              g_variant_builder_add (&a_su_builder, "{su}", k,
                  GPOINTER_TO_UINT (g_hash_table_lookup (account->attribute_flags,
                      k)));
            }
          else
            {
              g_variant_builder_add (&as_builder, "s", k);
            }
        }

      g_dbus_connection_call (self->bus,
          TEST_DBUS_ACCOUNT_SERVICE,
          TEST_DBUS_ACCOUNT_SERVICE_PATH,
          TEST_DBUS_ACCOUNT_SERVICE_IFACE,
          "UpdateAttributes",
          g_variant_new_parsed ("(%s, %v, %v, %v)", account_name,
            g_variant_builder_end (&a_sv_builder),
            g_variant_builder_end (&a_su_builder),
            g_variant_builder_end (&as_builder)),
          G_VARIANT_TYPE_UNIT,
          G_DBUS_CALL_FLAGS_NONE,
          -1,
          NULL, /* no cancellable */
          update_attributes_cb,
          async_data_new (self, account_name));
    }
  else
    {
      DEBUG ("no attributes to commit");
    }

  if (g_hash_table_size (account->uncommitted_parameters) != 0)
    {
      g_variant_builder_init (&a_sv_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_init (&a_ss_builder, G_VARIANT_TYPE ("a{ss}"));
      g_variant_builder_init (&a_su_builder, G_VARIANT_TYPE ("a{su}"));
      g_variant_builder_init (&as_builder, G_VARIANT_TYPE_STRING_ARRAY);
      g_hash_table_iter_init (&iter, account->uncommitted_parameters);

      while (g_hash_table_iter_next (&iter, &k, NULL))
        {
          GVariant *v = g_hash_table_lookup (account->parameters, k);
          const gchar *s = g_hash_table_lookup (account->untyped_parameters, k);

          DEBUG ("Parameter %s uncommitted, committing it now",
              (const gchar *) k);

          if (v != NULL)
            {
              g_variant_builder_add (&a_sv_builder, "{sv}", k, v);
              g_variant_builder_add (&a_su_builder, "{su}", k,
                  GPOINTER_TO_UINT (g_hash_table_lookup (account->parameter_flags,
                      k)));
            }
          else if (s != NULL)
            {
              g_variant_builder_add (&a_ss_builder, "{ss}", k, s);
              g_variant_builder_add (&a_su_builder, "{su}", k,
                  GPOINTER_TO_UINT (g_hash_table_lookup (account->parameter_flags,
                      k)));
            }
          else
            {
              g_variant_builder_add (&as_builder, "s", k);
            }
        }

      g_dbus_connection_call (self->bus,
          TEST_DBUS_ACCOUNT_SERVICE,
          TEST_DBUS_ACCOUNT_SERVICE_PATH,
          TEST_DBUS_ACCOUNT_SERVICE_IFACE,
          "UpdateParameters",
          g_variant_new_parsed ("(%s, %v, %v, %v, %v)", account_name,
            g_variant_builder_end (&a_sv_builder),
            g_variant_builder_end (&a_ss_builder),
            g_variant_builder_end (&a_su_builder),
            g_variant_builder_end (&as_builder)),
          G_VARIANT_TYPE_UNIT,
          G_DBUS_CALL_FLAGS_NONE,
          -1,
          NULL, /* no cancellable */
          update_parameters_cb,
          async_data_new (self, account_name));
    }
  else
    {
      DEBUG ("no parameters to commit");
    }

  return TRUE;
}

static void
test_dbus_account_plugin_get_identifier (const McpAccountStorage *storage,
    const gchar *account_name,
    GValue *identifier)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (storage);
  Account *account = lookup_account (self, account_name);

  DEBUG ("%s", account_name);

  if (!self->active || account == NULL || (account->flags & UNCOMMITTED_DELETION))
    return;

  /* Our "library-specific unique identifier" is just the object-path
   * as a string. */
  g_value_init (identifier, G_TYPE_STRING);
  g_value_set_string (identifier, account->path);
}

static GHashTable *
test_dbus_account_plugin_get_additional_info (const McpAccountStorage *storage,
    const gchar *account_name)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (storage);
  Account *account = lookup_account (self, account_name);
  GHashTable *ret;

  DEBUG ("%s", account_name);

  if (!self->active || account == NULL || (account->flags & UNCOMMITTED_DELETION))
    return NULL;

  ret = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) tp_g_value_slice_free);
  g_hash_table_insert (ret, g_strdup ("hello"),
      tp_g_value_slice_new_static_string ("world"));

  return ret;
}

static guint
test_dbus_account_plugin_get_restrictions (const McpAccountStorage *storage,
    const gchar *account_name)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (storage);
  Account *account = lookup_account (self, account_name);

  DEBUG ("%s", account_name);

  if (!self->active || account == NULL || (account->flags & UNCOMMITTED_DELETION))
    return 0;

  /* FIXME: actually enforce this restriction */
  return TP_STORAGE_RESTRICTION_FLAG_CANNOT_SET_SERVICE;
}

static gboolean
test_dbus_account_plugin_owns (McpAccountStorage *storage,
    McpAccountManager *am,
    const gchar *account_name)
{
  TestDBusAccountPlugin *self = TEST_DBUS_ACCOUNT_PLUGIN (storage);
  Account *account = lookup_account (self, account_name);

  DEBUG ("%s", account_name);

  if (!self->active || account == NULL || (account->flags & UNCOMMITTED_DELETION))
    return FALSE;

  return TRUE;
}

static void
account_storage_iface_init (McpAccountStorageIface *iface)
{
  iface->name = "TestDBusAccount";
  iface->desc = "Regression test plugin";
  /* this should be higher priority than the diverted-keyfile one */
  iface->priority = MCP_ACCOUNT_STORAGE_PLUGIN_PRIO_NORMAL + 100;

  iface->get = test_dbus_account_plugin_get;
  iface->set = test_dbus_account_plugin_set;
  iface->list = test_dbus_account_plugin_list;
  iface->ready = test_dbus_account_plugin_ready;
  iface->delete = test_dbus_account_plugin_delete;
  iface->commit = test_dbus_account_plugin_commit;
  iface->commit_one = test_dbus_account_plugin_commit_one;
  iface->get_identifier = test_dbus_account_plugin_get_identifier;
  iface->get_additional_info = test_dbus_account_plugin_get_additional_info;
  iface->get_restrictions = test_dbus_account_plugin_get_restrictions;
  iface->create = test_dbus_account_plugin_create;
  iface->owns = test_dbus_account_plugin_owns;
}

/* ide-application.c
 *
 * Copyright 2014-2019 Christian Hergert <chergert@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "ide-application"

#include "config.h"

#ifdef __linux__
# include <sys/prctl.h>
#endif

#include <glib/gi18n.h>
#include <libpeas/peas-autocleanups.h>
#include <libide-themes.h>

#include "ide-language-defaults.h"

#include "ide-application.h"
#include "ide-application-addin.h"
#include "ide-application-private.h"
#include "ide-gui-global.h"
#include "ide-primary-workspace.h"

G_DEFINE_FINAL_TYPE (IdeApplication, ide_application, ADW_TYPE_APPLICATION)

typedef struct
{
  IdeApplication  *self;
  GFile          **files;
  gint             n_files;
  const gchar     *hint;
} OpenData;

static void
ide_application_add_platform_data (GApplication    *app,
                                   GVariantBuilder *builder)
{
  IdeApplication *self = (IdeApplication *)app;

  g_assert (IDE_IS_APPLICATION (self));
  g_assert (self->argv != NULL);

  G_APPLICATION_CLASS (ide_application_parent_class)->add_platform_data (app, builder);

  g_variant_builder_add (builder,
                         "{sv}",
                         "gnome-builder-version",
                         g_variant_new_string (IDE_VERSION_S));
  g_variant_builder_add (builder,
                         "{sv}",
                         "argv",
                         g_variant_new_strv ((const gchar * const *)self->argv, -1));
}

static gint
ide_application_command_line (GApplication            *app,
                              GApplicationCommandLine *cmdline)
{
  IdeApplication *self = (IdeApplication *)app;

  g_assert (IDE_IS_APPLICATION (self));
  g_assert (G_IS_APPLICATION_COMMAND_LINE (cmdline));

  /* Allow plugins to handle command-line */
  _ide_application_command_line (self, cmdline);

  return G_APPLICATION_CLASS (ide_application_parent_class)->command_line (app, cmdline);
}

static gboolean
ide_application_local_command_line (GApplication   *app,
                                    gchar        ***arguments,
                                    gint           *exit_status)
{
  IdeApplication *self = (IdeApplication *)app;

  g_assert (IDE_IS_APPLICATION (self));
  g_assert (arguments != NULL);
  g_assert (exit_status != NULL);
  g_assert (self->argv == NULL);

  /* Save these for later, to use by cmdline addins */
  self->argv = g_strdupv (*arguments);

  return G_APPLICATION_CLASS (ide_application_parent_class)->local_command_line (app, arguments, exit_status);
}

static void
ide_application_register_keybindings (IdeApplication *self)
{
  g_autoptr(GSettings) settings = NULL;
  g_autofree gchar *name = NULL;

  g_assert (IDE_IS_APPLICATION (self));

  settings = g_settings_new ("org.gnome.builder.editor");
  name = g_settings_get_string (settings, "keybindings");
  self->keybindings = ide_keybindings_new (name);
  g_settings_bind (settings, "keybindings", self->keybindings, "mode", G_SETTINGS_BIND_GET);
}

static void
ide_application_startup (GApplication *app)
{
  IdeApplication *self = (IdeApplication *)app;
  g_autofree gchar *style_path = NULL;
  GtkSourceStyleSchemeManager *styles;
  GtkIconTheme *icon_theme;

  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (IDE_IS_APPLICATION (self));

  /*
   * We require a desktop session that provides a properly working
   * D-Bus environment. Bail if for some reason that is not the case.
   */
  if (g_getenv ("DBUS_SESSION_BUS_ADDRESS") == NULL)
    g_warning ("%s",
               _("GNOME Builder requires a session with D-Bus which was not found. Please set DBUS_SESSION_BUS_ADDRESS. Some features may not be available."));

  G_APPLICATION_CLASS (ide_application_parent_class)->startup (app);

  /* Setup access to private icons dir */
  icon_theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
  gtk_icon_theme_add_search_path (icon_theme, PACKAGE_ICONDIR);

  /* Add custom style locations for gtksourceview schemes */
  styles = gtk_source_style_scheme_manager_get_default ();
  style_path = g_build_filename (g_get_home_dir (), ".local", "share", "gtksourceview-5", "styles", NULL);
  gtk_source_style_scheme_manager_append_search_path (styles, style_path);
  gtk_source_style_scheme_manager_append_search_path (styles, PACKAGE_DATADIR"/gtksourceview-5/styles/");

  /* Load color settings (Night Light, Dark Mode, etc) */
  _ide_application_init_color (self);

  /* And now we can load the rest of our plugins for startup. */
  _ide_application_load_plugins (self);

  /* Load keybindings from plugins and what not */
  ide_application_register_keybindings (self);

  /* Load language defaults into gsettings */
  ide_language_defaults_init_async (NULL, NULL, NULL);
}

static void
ide_application_shutdown (GApplication *app)
{
  IdeApplication *self = (IdeApplication *)app;

  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (IDE_IS_APPLICATION (self));

  _ide_application_unload_addins (self);

  g_clear_pointer (&self->plugin_settings, g_hash_table_unref);
  g_clear_object (&self->addins);
  g_clear_object (&self->settings);
  g_clear_object (&self->keybindings);

  G_APPLICATION_CLASS (ide_application_parent_class)->shutdown (app);
}

static void
ide_application_activate_cb (PeasExtensionSet *set,
                             PeasPluginInfo   *plugin_info,
                             PeasExtension    *exten,
                             gpointer          user_data)
{
  g_assert (PEAS_IS_EXTENSION_SET (set));
  g_assert (plugin_info != NULL);
  g_assert (IDE_IS_APPLICATION_ADDIN (exten));
  g_assert (IDE_IS_APPLICATION (user_data));

  ide_application_addin_activate (IDE_APPLICATION_ADDIN (exten), user_data);
}

static void
ide_application_activate (GApplication *app)
{
  IdeApplication *self = (IdeApplication *)app;
  GtkWindow *window;

  IDE_ENTRY;

  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (IDE_IS_APPLICATION (self));

  if ((window = gtk_application_get_active_window (GTK_APPLICATION (self))))
    gtk_window_present (GTK_WINDOW (window));

  if (self->addins != NULL)
    peas_extension_set_foreach (self->addins,
                                ide_application_activate_cb,
                                self);

  IDE_EXIT;
}

static void
ide_application_open_cb (PeasExtensionSet *set,
                         PeasPluginInfo   *plugin_info,
                         PeasExtension    *exten,
                         gpointer          user_data)
{
  IdeApplicationAddin *app_addin = (IdeApplicationAddin*) exten;
  OpenData *data = user_data;

  g_assert (PEAS_IS_EXTENSION_SET (set));
  g_assert (plugin_info != NULL);
  g_assert (IDE_IS_APPLICATION_ADDIN (app_addin));
  g_assert (data != NULL);
  g_assert (IDE_IS_APPLICATION (data->self));
  g_assert (data->files != NULL);

  ide_application_addin_open (app_addin, data->self, data->files, data->n_files, data->hint);
}

static void
ide_application_open (GApplication  *app,
                      GFile        **files,
                      gint           n_files,
                      const gchar   *hint)
{
  IdeApplication *self = (IdeApplication*)app;
  OpenData data;

  IDE_ENTRY;

  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (IDE_IS_APPLICATION (self));
  g_assert (files);
  g_assert (n_files > 0);
  g_assert (hint);

  data.self = self;
  data.files = files;
  data.n_files = n_files;
  data.hint = hint;

  if (self->addins != NULL)
    peas_extension_set_foreach (self->addins, ide_application_open_cb, &data);

  IDE_EXIT;
}

void
_ide_application_add_resources (IdeApplication *self,
                                const char     *resource_path)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *menu_path = NULL;
  guint merge_id;

  g_assert (IDE_IS_APPLICATION (self));
  g_assert (resource_path != NULL);

  /* We use interned strings for hash table keys */
  resource_path = g_intern_string (resource_path);

  /*
   * If the resource path has a gtk/menus.ui file, we want to auto-load and
   * merge the menus.
   */
  menu_path = g_build_filename (resource_path, "gtk", "menus.ui", NULL);

  if (g_str_has_prefix (menu_path, "resource://"))
    merge_id = ide_menu_manager_add_resource (self->menu_manager, menu_path, &error);
  else
    merge_id = ide_menu_manager_add_filename (self->menu_manager, menu_path, &error);

  if (merge_id != 0)
    g_hash_table_insert (self->menu_merge_ids, (gchar *)resource_path, GUINT_TO_POINTER (merge_id));

  if (error != NULL &&
      !(g_error_matches (error, G_RESOURCE_ERROR, G_RESOURCE_ERROR_NOT_FOUND) ||
        g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT)))
    g_warning ("%s", error->message);
}

static void
ide_application_dispose (GObject *object)
{
  IdeApplication *self = (IdeApplication *)object;

  /* We don't necessarily get startup/shutdown called when we are
   * the remote process, so ensure they get cleared here rather than
   * in ::shutdown.
   */
  g_clear_pointer (&self->started_at, g_date_time_unref);
  g_clear_pointer (&self->workbenches, g_ptr_array_unref);
  g_clear_pointer (&self->plugin_settings, g_hash_table_unref);
  g_clear_pointer (&self->plugin_gresources, g_hash_table_unref);
  g_clear_pointer (&self->argv, g_strfreev);
  g_clear_pointer (&self->menu_merge_ids, g_hash_table_unref);
  g_clear_object (&self->addins);
  g_clear_object (&self->settings);
  g_clear_object (&self->network_monitor);
  g_clear_object (&self->menu_manager);

  G_OBJECT_CLASS (ide_application_parent_class)->dispose (object);
}

static void
ide_application_class_init (IdeApplicationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  object_class->dispose = ide_application_dispose;

  app_class->activate = ide_application_activate;
  app_class->open = ide_application_open;
  app_class->add_platform_data = ide_application_add_platform_data;
  app_class->command_line = ide_application_command_line;
  app_class->local_command_line = ide_application_local_command_line;
  app_class->startup = ide_application_startup;
  app_class->shutdown = ide_application_shutdown;
}

static void
ide_application_init (IdeApplication *self)
{
  self->menu_merge_ids = g_hash_table_new (g_str_hash, g_str_equal);
  self->menu_manager = ide_menu_manager_new ();
  self->started_at = g_date_time_new_now_local ();
  self->workspace_type = IDE_TYPE_PRIMARY_WORKSPACE;
  self->workbenches = g_ptr_array_new_with_free_func (g_object_unref);
  self->settings = g_settings_new ("org.gnome.builder");
  self->plugin_gresources = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                   (GDestroyNotify)g_resource_unref);

  g_application_set_default (G_APPLICATION (self));
  gtk_window_set_default_icon_name (ide_get_application_id ());
  ide_themes_init ();

  /* Ensure our core data is loaded early. */
  _ide_application_add_resources (self, "resource:///org/gnome/libide-sourceview/");
  _ide_application_add_resources (self, "resource:///org/gnome/libide-gui/");
  _ide_application_add_resources (self, "resource:///org/gnome/libide-terminal/");

  /* Make sure our GAction are available */
  _ide_application_init_actions (self);
}

IdeApplication *
_ide_application_new (gboolean standalone)
{
  GApplicationFlags flags = G_APPLICATION_HANDLES_COMMAND_LINE | G_APPLICATION_HANDLES_OPEN;
  IdeApplication *self;

  if (standalone)
    flags |= G_APPLICATION_NON_UNIQUE;

  self = g_object_new (IDE_TYPE_APPLICATION,
                       "application-id", ide_get_application_id (),
                       "flags", flags,
                       "resource-base-path", "/org/gnome/builder",
                       NULL);

  /* Load plugins indicating they support startup features */
  _ide_application_load_plugins_for_startup (self);

  /* Now that early plugins are loaded, we can activate app addins. We'll
   * load additional plugins later after post-early stage startup
   */
  _ide_application_load_addins (self);

  /* Register command-line options, possibly from plugins. */
  _ide_application_add_option_entries (self);

  return g_steal_pointer (&self);
}

static void
ide_application_add_workbench_cb (PeasExtensionSet *set,
                                  PeasPluginInfo   *plugin_info,
                                  PeasExtension    *exten,
                                  gpointer          user_data)
{
  g_assert (PEAS_IS_EXTENSION_SET (set));
  g_assert (plugin_info != NULL);
  g_assert (IDE_IS_APPLICATION_ADDIN (exten));
  g_assert (IDE_IS_WORKBENCH (user_data));

  ide_application_addin_workbench_added (IDE_APPLICATION_ADDIN (exten), user_data);
}

void
ide_application_add_workbench (IdeApplication *self,
                               IdeWorkbench   *workbench)
{
  g_return_if_fail (IDE_IS_APPLICATION (self));
  g_return_if_fail (IDE_IS_WORKBENCH (workbench));

  g_ptr_array_add (self->workbenches, g_object_ref (workbench));

  peas_extension_set_foreach (self->addins,
                              ide_application_add_workbench_cb,
                              workbench);
}

static void
ide_application_remove_workbench_cb (PeasExtensionSet *set,
                                     PeasPluginInfo   *plugin_info,
                                     PeasExtension    *exten,
                                     gpointer          user_data)
{
  g_assert (PEAS_IS_EXTENSION_SET (set));
  g_assert (plugin_info != NULL);
  g_assert (IDE_IS_APPLICATION_ADDIN (exten));
  g_assert (IDE_IS_WORKBENCH (user_data));

  ide_application_addin_workbench_removed (IDE_APPLICATION_ADDIN (exten), user_data);
}

void
ide_application_remove_workbench (IdeApplication *self,
                                  IdeWorkbench   *workbench)
{
  g_return_if_fail (IDE_IS_APPLICATION (self));
  g_return_if_fail (IDE_IS_WORKBENCH (workbench));

  peas_extension_set_foreach (self->addins,
                              ide_application_remove_workbench_cb,
                              workbench);

  g_ptr_array_remove (self->workbenches, workbench);
}

/**
 * ide_application_foreach_workbench:
 * @self: an #IdeApplication
 * @callback: (scope call): a #GFunc callback
 * @user_data: user data for @callback
 *
 * Calls @callback for each of the registered workbenches.
 *
 * Since: 3.32
 */
void
ide_application_foreach_workbench (IdeApplication *self,
                                   GFunc           callback,
                                   gpointer        user_data)
{
  g_return_if_fail (IDE_IS_APPLICATION (self));
  g_return_if_fail (callback != NULL);

  for (guint i = self->workbenches->len; i > 0; i--)
    {
      IdeWorkbench *workbench = g_ptr_array_index (self->workbenches, i - 1);

      callback (workbench, user_data);
    }
}

/**
 * ide_application_set_workspace_type:
 * @self: a #IdeApplication
 *
 * Sets the #GType of an #IdeWorkspace that should be used when creating the
 * next workspace upon handling files from command-line arguments. This is
 * reset after the files are opened and is generally only useful from
 * #IdeApplicationAddin's who need to alter the default workspace.
 *
 * Since: 3.32
 */
void
ide_application_set_workspace_type (IdeApplication *self,
                                    GType           workspace_type)
{
  g_return_if_fail (IDE_IS_APPLICATION (self));
  g_return_if_fail (g_type_is_a (workspace_type, IDE_TYPE_WORKSPACE));

  self->workspace_type = workspace_type;
}

static void
ide_application_network_changed_cb (IdeApplication  *self,
                                    gboolean         available,
                                    GNetworkMonitor *monitor)
{
  g_assert (IDE_IS_APPLICATION (self));
  g_assert (G_IS_NETWORK_MONITOR (monitor));

  self->has_network = !!available;
}

/**
 * ide_application_has_network:
 * @self: (nullable): a #IdeApplication
 *
 * This is a helper that uses an internal #GNetworkMonitor to track if we
 * have access to the network. It works around some issues we've seen in
 * the wild that make determining if we have network access difficult.
 *
 * Returns: %TRUE if we think there is network access.
 *
 * Since: 3.32
 */
gboolean
ide_application_has_network (IdeApplication *self)
{
  g_return_val_if_fail (!self || IDE_IS_APPLICATION (self), FALSE);

  if (self == NULL)
    self = IDE_APPLICATION_DEFAULT;

  if (self->network_monitor == NULL)
    {
      self->network_monitor = g_object_ref (g_network_monitor_get_default ());

      g_signal_connect_object (self->network_monitor,
                               "network-changed",
                               G_CALLBACK (ide_application_network_changed_cb),
                               self,
                               G_CONNECT_SWAPPED);

      self->has_network = g_network_monitor_get_network_available (self->network_monitor);

      /*
       * FIXME: Ignore the network portal initially for now.
       *
       * See https://gitlab.gnome.org/GNOME/glib/merge_requests/227 for more
       * information about when this is fixed.
       *
       * See Also: https://gitlab.gnome.org/GNOME/glib/-/issues/1718
       */
      if (!self->has_network && ide_is_flatpak ())
        self->has_network = TRUE;
    }

  return self->has_network;
}

/**
 * ide_application_get_started_at:
 * @self: a #IdeApplication
 *
 * Gets the time the application was started.
 *
 * Returns: (transfer none): a #GDateTime
 *
 * Since: 3.32
 */
GDateTime *
ide_application_get_started_at (IdeApplication *self)
{
  g_return_val_if_fail (IDE_IS_APPLICATION (self), NULL);

  return self->started_at;
}

/**
 * ide_application_find_workbench_for_file:
 * @self: a #IdeApplication
 * @file: a #GFile
 *
 * Looks for the workbench that is the closest match to @file.
 *
 * If no workbench is the root of @file, then %NULL is returned.
 *
 * Returns: (transfer none) (nullable): an #IdeWorkbench or %NULL
 *
 * Since: 3.32
 */
IdeWorkbench *
ide_application_find_workbench_for_file (IdeApplication *self,
                                         GFile          *file)
{
  g_autofree gchar *suffix = NULL;
  IdeWorkbench *match = NULL;

  g_return_val_if_fail (IDE_IS_MAIN_THREAD (), NULL);
  g_return_val_if_fail (IDE_IS_APPLICATION (self), NULL);

  for (guint i = 0; i < self->workbenches->len; i++)
    {
      IdeWorkbench *workbench = g_ptr_array_index (self->workbenches, 0);
      IdeContext *context = ide_workbench_get_context (workbench);
      g_autoptr(GFile) workdir = ide_context_ref_workdir (context);

      if (!ide_workbench_has_project (workbench))
        continue;

      if (g_file_has_prefix (file, workdir))
        {
          g_autofree gchar *relative = g_file_get_relative_path (workdir, file);

          if (!suffix || strlen (relative) < strlen (suffix))
            {
              match = workbench;
              g_free (suffix);
              suffix = g_steal_pointer (&relative);
            }
        }
    }

  /* TODO: If a file is installed, but was installed by a workspace that
   *       we have open, we want to switch to that file instead of the
   *       installed version. For example, something installed to
   *       /app/include/libpeas-1.0/libpeas/peas-engine.h should really open
   *       libpeas/peas-engine.h from the project. This will require querying
   *       the pipeline/build-system for installed files to reverse-map the
   *       filename.
   */

  return match;
}

void
ide_application_set_command_line_handled (IdeApplication          *self,
                                          GApplicationCommandLine *cmdline,
                                          gboolean                 handled)
{
  g_return_if_fail (IDE_IS_APPLICATION (self));
  g_return_if_fail (G_IS_APPLICATION_COMMAND_LINE (cmdline));

  return g_object_set_data (G_OBJECT (cmdline), "COMMAND_LINE_HANDLED", GINT_TO_POINTER (!!handled));
}

gboolean
ide_application_get_command_line_handled (IdeApplication          *self,
                                          GApplicationCommandLine *cmdline)
{
  g_return_val_if_fail (IDE_IS_APPLICATION (self), FALSE);
  g_return_val_if_fail (G_IS_APPLICATION_COMMAND_LINE (cmdline), FALSE);

  return !!g_object_get_data (G_OBJECT (cmdline), "COMMAND_LINE_HANDLED");
}

/**
 * ide_application_find_addin_by_module_name:
 * @self: a #IdeApplication
 * @module_name: the name of the plugin module
 *
 * Finds a loaded #IdeApplicationAddin within @self that was part of
 * the plugin matching @module_name.
 *
 * Returns: (transfer none) (type IdeApplicationAddin) (nullable): an
 *   #IdeApplicationAddin or %NULL.
 *
 * Since: 3.34
 */
gpointer
ide_application_find_addin_by_module_name (IdeApplication *self,
                                           const gchar    *module_name)
{
  PeasEngine *engine;
  PeasPluginInfo *plugin_info;

  if (self == NULL)
    self = IDE_APPLICATION_DEFAULT;

  g_return_val_if_fail (IDE_IS_APPLICATION (self), NULL);
  g_return_val_if_fail (module_name != NULL, NULL);

  if (self->addins == NULL)
    return NULL;

  engine = peas_engine_get_default ();
  plugin_info = peas_engine_get_plugin_info (engine, module_name);

  if (plugin_info == NULL)
    return NULL;

  return peas_extension_set_get_extension (self->addins, plugin_info);
}

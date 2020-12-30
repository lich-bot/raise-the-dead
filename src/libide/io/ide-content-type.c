/* ide-content-type.c
 *
 * Copyright 2018-2019 Christian Hergert <chergert@redhat.com>
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

#define G_LOG_DOMAIN "ide-glib"

#include "config.h"

#include "../../gconstructor.h"

#include "ide-content-type.h"

static gchar bundled_lookup_table[256];
static GHashTable *bundled;
/* This ensures those files get a proper icon when they end with .md
 * (markdown files).  It can't be fixed in the shared-mime-info db because
 * otherwise they wouldn't get detected as markdown anymore.
 */
static const struct {
  const gchar *searched_prefix;
  const gchar *icon_name;
} bundled_check_by_name_prefix[] = {
  { "README", "text-x-readme-symbolic" },
  { "NEWS", "text-x-changelog-symbolic" },
  { "CHANGELOG", "text-x-changelog-symbolic" },
  { "COPYING", "text-x-copying-symbolic" },
  { "LICENSE", "text-x-copying-symbolic" },
  { "AUTHORS", "text-x-authors-symbolic" },
  { "MAINTAINERS", "text-x-authors-symbolic" },
};

#if defined (G_HAS_CONSTRUCTORS)
# ifdef G_DEFINE_CONSTRUCTOR_NEEDS_PRAGMA
#  pragma G_DEFINE_CONSTRUCTOR_PRAGMA_ARGS(ide_io_init_ctor)
# endif
G_DEFINE_CONSTRUCTOR(ide_io_init_ctor)
#else
# error Your platform/compiler is missing constructor support
#endif

static void
ide_io_init_ctor (void)
{
  bundled = g_hash_table_new (g_str_hash, g_str_equal);

  /*
   * This needs to be updated when we add icons for specific mime-types
   * because of how icon theme loading works (and it wanting to use
   * Adwaita generic icons before our hicolor specific icons).
   */
#define ADD_ICON(t, n, v) g_hash_table_insert (t, (gpointer)n, v ? (gpointer)v : (gpointer)n)
  ADD_ICON (bundled, "application-x-php-symbolic", NULL);
  ADD_ICON (bundled, "text-css-symbolic", NULL);
  ADD_ICON (bundled, "text-html-symbolic", NULL);
  ADD_ICON (bundled, "text-markdown-symbolic", NULL);
  ADD_ICON (bundled, "text-rust-symbolic", NULL);
  ADD_ICON (bundled, "text-sql-symbolic", NULL);
  ADD_ICON (bundled, "text-x-authors-symbolic", NULL);
  ADD_ICON (bundled, "text-x-changelog-symbolic", NULL);
  ADD_ICON (bundled, "text-x-chdr-symbolic", NULL);
  ADD_ICON (bundled, "text-x-copying-symbolic", NULL);
  ADD_ICON (bundled, "text-x-cpp-symbolic", NULL);
  ADD_ICON (bundled, "text-x-csrc-symbolic", NULL);
  ADD_ICON (bundled, "text-x-javascript-symbolic", NULL);
  ADD_ICON (bundled, "text-x-python-symbolic", NULL);
  ADD_ICON (bundled, "text-x-python3-symbolic", "text-x-python-symbolic");
  ADD_ICON (bundled, "text-x-readme-symbolic", NULL);
  ADD_ICON (bundled, "text-x-ruby-symbolic", NULL);
  ADD_ICON (bundled, "text-x-script-symbolic", NULL);
  ADD_ICON (bundled, "text-x-vala-symbolic", NULL);
  ADD_ICON (bundled, "text-xml-symbolic", NULL);
#undef ADD_ICON

  /* Create faster check than doing full string checks */
  for (guint i = 0; i < G_N_ELEMENTS (bundled_check_by_name_prefix); i++)
    bundled_lookup_table[(guint)bundled_check_by_name_prefix[i].searched_prefix[0]] = 1;
}

/**
 * ide_g_content_type_get_symbolic_icon:
 * @content_type: the content-type to lookup
 *
 * This function is similar to g_content_type_get_symbolic_icon() except that
 * it takes our bundled icons into account to ensure that they are taken at a
 * higher priority than the fallbacks from the current icon theme such as
 * Adwaita.
 *
 * In 3.40, this function was modified to add the @filename parameter.
 *
 * Returns: (transfer full) (nullable): A #GIcon or %NULL
 *
 * Since: 3.40
 */
GIcon *
ide_g_content_type_get_symbolic_icon (const gchar *content_type,
                                      const gchar *filename)
{
  g_autoptr(GIcon) icon = NULL;

  g_return_val_if_fail (content_type != NULL, NULL);

  /*
   * Basically just steal the name if we get something that is not generic,
   * because that is the only way we can somewhat ensure that we don't use
   * the Adwaita fallback for generic when what we want is the *exact* match
   * from our hicolor/ bundle.
   */

  icon = g_content_type_get_symbolic_icon (content_type);

  if (G_IS_THEMED_ICON (icon))
    {
      const gchar * const *names;

      if (filename != NULL && bundled_lookup_table [(guint8)filename[0]])
        {
          for (guint j = 0; j < G_N_ELEMENTS (bundled_check_by_name_prefix); j++)
            {
              if (g_str_has_prefix (filename, bundled_check_by_name_prefix[j].searched_prefix))
                return g_icon_new_for_string (bundled_check_by_name_prefix[j].icon_name, NULL);
            }
        }

      names = g_themed_icon_get_names (G_THEMED_ICON (icon));

      if (names != NULL)
        {
          gboolean fallback = FALSE;

          for (guint i = 0; names[i] != NULL; i++)
            {
              const gchar *replace = g_hash_table_lookup (bundled, names[i]);

              if (replace != NULL)
                return g_icon_new_for_string (replace, NULL);

              fallback |= (g_str_equal (names[i], "text-plain") ||
                           g_str_equal (names[i], "application-octet-stream"));
            }

          if (fallback)
            return g_icon_new_for_string ("text-x-generic-symbolic", NULL);
        }
    }

  return g_steal_pointer (&icon);
}

/* gbp-codeshot-editor-page-addin.c
 *
 * Copyright 2023 Christian Hergert <chergert@redhat.com>
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

#define G_LOG_DOMAIN "gbp-codeshot-editor-page-addin"

#include "config.h"

#include <libide-code.h>
#include <libide-editor.h>

#include "gbp-codeshot-editor-page-addin.h"
#include "gbp-codeshot-window.h"

struct _GbpCodeshotEditorPageAddin
{
  GObject        parent_instance;
  IdeEditorPage *page;
  gulong         notify_handler;
};

static void
fill_background (cairo_t              *cr,
                 GtkSourceStyleScheme *scheme)
{
  GtkSourceStyle *style;

  if (scheme != NULL &&
      (style = gtk_source_style_scheme_get_style (scheme, "selection")))
    {
      g_autofree char *background = NULL;
      gboolean background_set = FALSE;
      GdkRGBA rgba;

      g_object_get (style,
                    "background", &background,
                    "background-set", &background_set,
                    NULL);

      if (background_set &&
          background != NULL &&
          gdk_rgba_parse (&rgba, background))
        {
          rgba.alpha = 1;
          gdk_cairo_set_source_rgba (cr, &rgba);
          cairo_fill (cr);
          return;
        }
    }

  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_fill (cr);
}

static void
gbp_codeshot_editor_page_addin_clipboard_action (GbpCodeshotEditorPageAddin *self,
                                                 GVariant                   *param)
{
  g_autoptr(GdkPaintable) paintable = NULL;
  g_autoptr(GskRenderer) renderer = NULL;
  g_autoptr(GtkSnapshot) snapshot = NULL;
  g_autoptr(GskRenderNode) root = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GdkTexture) texture = NULL;
  cairo_surface_t *surface;
  GdkClipboard *clipboard;
  GdkSurface *gdk_surface;
  IdeBuffer *buffer;
  GtkWidget *window;
  cairo_t *cr;
  GtkTextIter begin, end;
  double transform_x, transform_y;
  guint stride;
  int min_width, min_height;
  int nat_width, nat_height;
  int scale_factor;

  IDE_ENTRY;

  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (GBP_IS_CODESHOT_EDITOR_PAGE_ADDIN (self));
  g_assert (IDE_IS_EDITOR_PAGE (self->page));

  buffer = ide_editor_page_get_buffer (self->page);

  if (!gtk_text_buffer_get_selection_bounds (GTK_TEXT_BUFFER (buffer), &begin, &end))
    gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (buffer), &begin, &end);

  window = gbp_codeshot_window_new (buffer, &begin, &end);
  paintable = gtk_widget_paintable_new (window);

  gtk_widget_measure (window,
                      GTK_ORIENTATION_HORIZONTAL,
                      -1,
                      &min_width, &nat_width, NULL, NULL);
  gtk_widget_measure (window,
                      GTK_ORIENTATION_VERTICAL,
                      nat_width,
                      &min_height, &nat_height, NULL, NULL);

  gtk_widget_set_size_request (window, nat_width, nat_height);

  gtk_window_present (GTK_WINDOW (window));

  while (!gtk_widget_get_mapped (window) || g_main_context_pending (NULL))
    g_main_context_iteration (NULL, TRUE);

  gdk_surface = gtk_native_get_surface (GTK_NATIVE (window));
  gtk_native_get_surface_transform (GTK_NATIVE (window), &transform_x, &transform_y);
  scale_factor = gdk_surface_get_scale_factor (gdk_surface);

  snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, snapshot, nat_width, nat_height);
  root = gtk_snapshot_free_to_node (g_steal_pointer (&snapshot));

  gtk_widget_set_opacity (window, 0);

  if (gsk_render_node_get_node_type (root) == GSK_CLIP_NODE)
    {
      GskRenderNode *parent = g_steal_pointer (&root);
      root = gsk_render_node_ref (gsk_clip_node_get_child (parent));
      gsk_render_node_unref (parent);
    }

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                        gdk_surface_get_width (gdk_surface) / (double)scale_factor * 2,
                                        gdk_surface_get_height (gdk_surface) / (double)scale_factor * 2);
  cairo_surface_set_device_scale (surface, 2, 2);

  cr = cairo_create (surface);
  cairo_rectangle (cr, 0, 0,
                   gdk_surface_get_width (gdk_surface),
                   gdk_surface_get_height (gdk_surface));
  fill_background (cr, gtk_source_buffer_get_style_scheme (GTK_SOURCE_BUFFER (buffer)));
  cairo_translate (cr,
                   transform_x / scale_factor,
                   transform_y / scale_factor);
  gsk_render_node_draw (root, cr);
  cairo_surface_flush (surface);
  cairo_destroy (cr);

  stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32,
                                          cairo_image_surface_get_width (surface));
  bytes = g_bytes_new_with_free_func (cairo_image_surface_get_data (surface),
                                      stride * cairo_image_surface_get_height (surface),
                                      (GDestroyNotify) cairo_surface_destroy,
                                      surface);
  texture = gdk_memory_texture_new (cairo_image_surface_get_width (surface),
                                    cairo_image_surface_get_height (surface),
#if G_BYTE_ORDER == G_BIG_ENDIAN
                                    GDK_MEMORY_A8R8G8B8_PREMULTIPLIED,
#else
                                    GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
#endif
                                    bytes, stride);

  clipboard = gtk_widget_get_clipboard (GTK_WIDGET (self->page));
  gdk_clipboard_set_texture (clipboard, texture);

  gtk_window_destroy (GTK_WINDOW (window));

  IDE_EXIT;
}

IDE_DEFINE_ACTION_GROUP (GbpCodeshotEditorPageAddin, gbp_codeshot_editor_page_addin, {
  { "copy-clipboard", gbp_codeshot_editor_page_addin_clipboard_action },
});

static void
notify_has_selection_cb (GbpCodeshotEditorPageAddin *self,
                         GParamSpec                 *pspec,
                         IdeBuffer                  *buffer)
{
  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (GBP_IS_CODESHOT_EDITOR_PAGE_ADDIN (self));
  g_assert (IDE_IS_BUFFER (buffer));

  gbp_codeshot_editor_page_addin_set_action_enabled (self,
                                                     "copy-clipboard",
                                                     gtk_text_buffer_get_has_selection (GTK_TEXT_BUFFER (buffer)));
}

static void
gbp_codeshot_editor_page_addin_load (IdeEditorPageAddin *addin,
                                     IdeEditorPage      *page)
{
  GbpCodeshotEditorPageAddin *self = (GbpCodeshotEditorPageAddin *)addin;
  IdeBuffer *buffer;

  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (GBP_IS_CODESHOT_EDITOR_PAGE_ADDIN (self));
  g_assert (IDE_IS_EDITOR_PAGE (page));

  buffer = ide_editor_page_get_buffer (page);

  self->page = page;
  self->notify_handler =
    g_signal_connect_object (buffer,
                             "notify::has-selection",
                             G_CALLBACK (notify_has_selection_cb),
                             self,
                             G_CONNECT_SWAPPED);

  notify_has_selection_cb (self, NULL, buffer);
}

static void
gbp_codeshot_editor_page_addin_unload (IdeEditorPageAddin *addin,
                                       IdeEditorPage      *page)
{
  GbpCodeshotEditorPageAddin *self = (GbpCodeshotEditorPageAddin *)addin;

  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (GBP_IS_CODESHOT_EDITOR_PAGE_ADDIN (self));
  g_assert (IDE_IS_EDITOR_PAGE (page));

  g_clear_signal_handler (&self->notify_handler, ide_editor_page_get_buffer (page));
  self->page = NULL;
}

static void
editor_page_addin_iface_init (IdeEditorPageAddinInterface *iface)
{
  iface->load = gbp_codeshot_editor_page_addin_load;
  iface->unload = gbp_codeshot_editor_page_addin_unload;
}

G_DEFINE_FINAL_TYPE_WITH_CODE (GbpCodeshotEditorPageAddin, gbp_codeshot_editor_page_addin, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (IDE_TYPE_EDITOR_PAGE_ADDIN, editor_page_addin_iface_init)
                               G_IMPLEMENT_INTERFACE (G_TYPE_ACTION_GROUP, gbp_codeshot_editor_page_addin_init_action_group))

static void
gbp_codeshot_editor_page_addin_class_init (GbpCodeshotEditorPageAddinClass *klass)
{
}

static void
gbp_codeshot_editor_page_addin_init (GbpCodeshotEditorPageAddin *self)
{
}

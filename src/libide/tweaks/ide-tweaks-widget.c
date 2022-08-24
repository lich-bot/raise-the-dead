/* ide-tweaks-widget.c
 *
 * Copyright 2022 Christian Hergert <chergert@redhat.com>
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

#define G_LOG_DOMAIN "ide-tweaks-widget"

#include "config.h"

#include "ide-tweaks-settings.h"
#include "ide-tweaks-widget-private.h"

typedef struct
{
  IdeTweaksWidget *cloned;
} IdeTweaksWidgetPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (IdeTweaksWidget, ide_tweaks_widget, IDE_TYPE_TWEAKS_ITEM)

enum {
  CREATE_FOR_ITEM,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
clone_item_property (IdeTweaksItem *item,
                     IdeTweaksItem *copy,
                     const char    *name)
{
  g_auto(GValue) value = G_VALUE_INIT;

  g_value_init (&value, IDE_TYPE_TWEAKS_ITEM);
  g_object_get_property (G_OBJECT (item), name, &value);

  if (g_value_get_object (&value))
    g_value_set_object (&value, ide_tweaks_item_copy (g_value_get_object (&value)));

  g_object_set_property (G_OBJECT (copy), name, &value);
}

static IdeTweaksItem *
ide_tweaks_widget_copy (IdeTweaksItem *item)
{
  IdeTweaksWidget *self = (IdeTweaksWidget *)item;
  IdeTweaksWidgetPrivate *copy_priv;
  g_autofree GParamSpec **pspecs = NULL;
  IdeTweaksItem *copy;
  guint n_pspecs;

  g_assert (IDE_IS_TWEAKS_WIDGET (self));

  /* Keep a pointer to the cloned widget so we can resolve <signal/>
   * through the original object.
   */
  copy = IDE_TWEAKS_ITEM_CLASS (ide_tweaks_widget_parent_class)->copy (item);
  copy_priv = ide_tweaks_widget_get_instance_private (IDE_TWEAKS_WIDGET (copy));
  g_set_weak_pointer (&copy_priv->cloned, self);

  /* Also keep a copy of any property we find that is an IdeTweaksSettings
   * since those need to be snapshotted.
   */
  pspecs = g_object_class_list_properties (G_OBJECT_GET_CLASS (item), &n_pspecs);
  for (guint i = 0; i < n_pspecs; i++)
    {
      GParamSpec *pspec = pspecs[i];

      if ((pspec->flags & G_PARAM_READWRITE) != G_PARAM_READWRITE ||
          (pspec->flags & G_PARAM_CONSTRUCT_ONLY) != 0)
        continue;

      if (g_type_is_a (pspec->value_type, IDE_TYPE_TWEAKS_SETTINGS) ||
          g_type_is_a (pspec->value_type, IDE_TYPE_TWEAKS_BINDING))
        clone_item_property (item, copy, pspec->name);
    }

  return copy;
}

static void
ide_tweaks_widget_dispose (GObject *object)
{
  IdeTweaksWidget *self = (IdeTweaksWidget *)object;
  IdeTweaksWidgetPrivate *priv = ide_tweaks_widget_get_instance_private (self);

  g_clear_weak_pointer (&priv->cloned);

  G_OBJECT_CLASS (ide_tweaks_widget_parent_class)->dispose (object);
}

static void
ide_tweaks_widget_class_init (IdeTweaksWidgetClass *klass)
{
  IdeTweaksItemClass *item_class = IDE_TWEAKS_ITEM_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  item_class->copy = ide_tweaks_widget_copy;

  object_class->dispose = ide_tweaks_widget_dispose;

  /**
   * IdeTweaksWidget::create-for-item:
   * @self: an #IdeTweaksWidget
   * @item: the original #IdeTweaksItem which might be a clone
   *
   * Creates a new #GtkWidget that can be inserted into the #IdeTweaksWindow
   * representing the item.
   *
   * @item is the original item (which might be a clone) to create the
   * widget for.
   *
   * Only the first signal handler is used.
   *
   * Returns: (transfer full) (nullable): a #GtkWidget or %NULL
   */
  signals [CREATE_FOR_ITEM] =
    g_signal_new ("create-for-item",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (IdeTweaksWidgetClass, create_for_item),
                  g_signal_accumulator_first_wins, NULL,
                  NULL,
                  GTK_TYPE_WIDGET,
                  1,
                  IDE_TYPE_TWEAKS_ITEM);
}

static void
ide_tweaks_widget_init (IdeTweaksWidget *self)
{
}

GtkWidget *
_ide_tweaks_widget_create_for_item (IdeTweaksWidget *self,
                                    IdeTweaksItem   *item)
{
  IdeTweaksWidgetPrivate *priv = ide_tweaks_widget_get_instance_private (self);
  GtkWidget *ret = NULL;

  g_return_val_if_fail (IDE_IS_TWEAKS_WIDGET (self), NULL);
  g_return_val_if_fail (IDE_IS_TWEAKS_WIDGET (item), NULL);

  if (priv->cloned != NULL)
    return _ide_tweaks_widget_create_for_item (priv->cloned, IDE_TWEAKS_ITEM (self));

  g_signal_emit (self, signals [CREATE_FOR_ITEM], 0, item, &ret);

  g_return_val_if_fail (!ret || GTK_IS_WIDGET (ret), NULL);

  return ret;
}

IdeTweaksWidget *
ide_tweaks_widget_new (void)
{
  return g_object_new (IDE_TYPE_TWEAKS_WIDGET, NULL);
}

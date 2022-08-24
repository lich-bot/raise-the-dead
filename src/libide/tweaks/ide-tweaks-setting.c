/* ide-tweaks-setting.c
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

#define G_LOG_DOMAIN "ide-tweaks-setting"

#include "config.h"

#include "ide-tweaks.h"
#include "ide-tweaks-setting.h"

#include "gsettings-mapping.h"

struct _IdeTweaksSetting
{
  IdeTweaksBinding parent_instance;
  const char *schema_id;
  const char *schema_key;
  char *path_suffix;
  GSettings *settings;
  const GVariantType *expected_type;
  gulong changed_handler;
};

enum {
  PROP_0,
  PROP_SCHEMA_ID,
  PROP_SCHEMA_KEY,
  PROP_PATH_SUFFIX,
  N_PROPS
};

G_DEFINE_FINAL_TYPE (IdeTweaksSetting, ide_tweaks_setting, IDE_TYPE_TWEAKS_BINDING)

static GParamSpec *properties [N_PROPS];

static void
ide_tweaks_setting_settings_changed_cb (IdeTweaksSetting *self,
                                        const char       *key,
                                        GSettings        *settings)
{
  g_assert (IDE_IS_TWEAKS_SETTING (self));
  g_assert (key != NULL);
  g_assert (G_IS_SETTINGS (settings));

  if (settings != self->settings)
    return;

  ide_tweaks_binding_changed (IDE_TWEAKS_BINDING (self));
}

static GSettings *
ide_tweaks_setting_acquire (IdeTweaksSetting    *self,
                            const char         **key,
                            const GVariantType **expected_type)
{
  g_assert (IDE_IS_TWEAKS_SETTING (self));

  if (key != NULL)
    *key = self->schema_key;

  if (expected_type != NULL)
    *expected_type = NULL;

  if (self->schema_id == NULL || self->schema_key == NULL)
    return NULL;

  if (self->settings == NULL)
    {
      g_autoptr(GSettingsSchema) schema = NULL;
      g_autoptr(GSettingsSchemaKey) schema_key = NULL;
      g_autofree char *path = NULL;
      g_autofree char *signal_name = NULL;
      g_autoptr(GVariant) value = NULL;
      IdeTweaksItem *root;
      const char *project_id = NULL;

      if ((root = ide_tweaks_item_get_root (IDE_TWEAKS_ITEM (self))) && IDE_IS_TWEAKS (root))
        project_id = ide_tweaks_get_project_id (IDE_TWEAKS (root));

      if (!(path = ide_settings_resolve_schema_path (self->schema_id, project_id, self->path_suffix)))
        return NULL;

      if (!(self->settings = g_settings_new_with_path (self->schema_id, path)))
        return NULL;

      g_object_get (self->settings,
                    "settings-schema", &schema,
                    NULL);
      schema_key = g_settings_schema_get_key (schema, self->schema_key);
      self->expected_type = g_settings_schema_key_get_value_type (schema_key);

      value = g_settings_get_value (self->settings, self->schema_key);
      signal_name = g_strdup_printf ("changed::%s", self->schema_key);

      self->changed_handler =
        g_signal_connect_object (self->settings,
                                 signal_name,
                                 G_CALLBACK (ide_tweaks_setting_settings_changed_cb),
                                 self,
                                 G_CONNECT_SWAPPED);
    }

  if (expected_type != NULL)
    *expected_type = self->expected_type;

  return g_object_ref (self->settings);
}

static void
ide_tweaks_setting_release (IdeTweaksSetting *self)
{
  g_assert (IDE_IS_TWEAKS_SETTING (self));

  g_clear_signal_handler (&self->changed_handler, self->settings);
  g_clear_object (&self->settings);
  self->expected_type = NULL;
}

static gboolean
ide_tweaks_setting_get_value (IdeTweaksBinding *binding,
                              GValue           *value)
{
  IdeTweaksSetting *self = (IdeTweaksSetting *)binding;
  g_autoptr(GSettings) settings = NULL;
  const char *key = NULL;

  g_assert (IDE_IS_TWEAKS_SETTING (self));
  g_assert (G_IS_VALUE (value));

  if ((settings = ide_tweaks_setting_acquire (self, &key, NULL)))
    {
      g_autoptr(GVariant) variant = g_settings_get_value (settings, key);

      if (variant != NULL)
        return g_settings_get_mapping (value, variant, NULL);
    }

  return FALSE;
}

static void
ide_tweaks_setting_set_value (IdeTweaksBinding *binding,
                              const GValue     *value)
{
  IdeTweaksSetting *self = (IdeTweaksSetting *)binding;
  g_autoptr(GSettings) settings = NULL;
  const GVariantType *expected_type = NULL;
  const char *key = NULL;

  g_assert (IDE_IS_TWEAKS_SETTING (self));
  g_assert (G_IS_VALUE (value));

  if ((settings = ide_tweaks_setting_acquire (self, &key, &expected_type)))
    {
      g_autoptr(GVariant) variant = g_settings_set_mapping (value, expected_type, NULL);

      if (variant != NULL)
        g_settings_set_value (settings, key, variant);
    }
}

static void
ide_tweaks_setting_dispose (GObject *object)
{
  IdeTweaksSetting *self = (IdeTweaksSetting *)object;

  ide_tweaks_setting_release (self);
  g_clear_pointer (&self->path_suffix, g_free);

  G_OBJECT_CLASS (ide_tweaks_setting_parent_class)->finalize (object);
}

static void
ide_tweaks_setting_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  IdeTweaksSetting *self = IDE_TWEAKS_SETTING (object);

  switch (prop_id)
    {
    case PROP_SCHEMA_ID:
      g_value_set_static_string (value, ide_tweaks_setting_get_schema_id (self));
      break;

    case PROP_SCHEMA_KEY:
      g_value_set_static_string (value, ide_tweaks_setting_get_schema_key (self));
      break;

    case PROP_PATH_SUFFIX:
      g_value_set_string (value, ide_tweaks_setting_get_path_suffix (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ide_tweaks_setting_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  IdeTweaksSetting *self = IDE_TWEAKS_SETTING (object);

  switch (prop_id)
    {
    case PROP_SCHEMA_ID:
      ide_tweaks_setting_set_schema_id (self, g_value_get_string (value));
      break;

    case PROP_SCHEMA_KEY:
      ide_tweaks_setting_set_schema_key (self, g_value_get_string (value));
      break;

    case PROP_PATH_SUFFIX:
      ide_tweaks_setting_set_path_suffix (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ide_tweaks_setting_class_init (IdeTweaksSettingClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  IdeTweaksBindingClass *tweaks_binding_class = IDE_TWEAKS_BINDING_CLASS (klass);

  object_class->dispose = ide_tweaks_setting_dispose;
  object_class->get_property = ide_tweaks_setting_get_property;
  object_class->set_property = ide_tweaks_setting_set_property;

  tweaks_binding_class->get_value = ide_tweaks_setting_get_value;
  tweaks_binding_class->set_value = ide_tweaks_setting_set_value;

  properties[PROP_SCHEMA_ID] =
    g_param_spec_string ("schema-id", NULL, NULL,
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));

  properties[PROP_SCHEMA_KEY] =
    g_param_spec_string ("schema-key", NULL, NULL,
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));

  properties[PROP_PATH_SUFFIX] =
    g_param_spec_string ("path-suffix", NULL, NULL,
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
ide_tweaks_setting_init (IdeTweaksSetting *self)
{
}

IdeTweaksSetting *
ide_tweaks_setting_new (void)
{
  return g_object_new (IDE_TYPE_TWEAKS_SETTING, NULL);
}

const char *
ide_tweaks_setting_get_schema_id (IdeTweaksSetting *self)
{
  g_return_val_if_fail (IDE_IS_TWEAKS_SETTING (self), NULL);

  return self->schema_id;
}

void
ide_tweaks_setting_set_schema_id (IdeTweaksSetting *self,
                                  const char       *schema_id)
{
  g_return_if_fail (IDE_IS_TWEAKS_SETTING (self));

  schema_id = g_intern_string (schema_id);

  if (schema_id != self->schema_id)
    {
      ide_tweaks_setting_release (self);
      self->schema_id = schema_id;
      g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_SCHEMA_ID]);
    }
}

const char *
ide_tweaks_setting_get_schema_key (IdeTweaksSetting *self)
{
  g_return_val_if_fail (IDE_IS_TWEAKS_SETTING (self), NULL);

  return self->schema_key;
}

void
ide_tweaks_setting_set_schema_key (IdeTweaksSetting *self,
                                   const char       *schema_key)
{
  g_return_if_fail (IDE_IS_TWEAKS_SETTING (self));

  schema_key = g_intern_string (schema_key);

  if (schema_key != self->schema_key)
    {
      ide_tweaks_setting_release (self);
      self->schema_key = schema_key;
      g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_SCHEMA_KEY]);
    }
}

const char *
ide_tweaks_setting_get_path_suffix (IdeTweaksSetting *self)
{
  g_return_val_if_fail (IDE_IS_TWEAKS_SETTING (self), NULL);

  return self->path_suffix;
}

void
ide_tweaks_setting_set_path_suffix (IdeTweaksSetting *self,
                                    const char       *path_suffix)
{
  g_return_if_fail (IDE_IS_TWEAKS_SETTING (self));

  path_suffix = g_intern_string (path_suffix);

  if (ide_set_string (&self->path_suffix, path_suffix))
    {
      ide_tweaks_setting_release (self);
      g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_PATH_SUFFIX]);
    }
}

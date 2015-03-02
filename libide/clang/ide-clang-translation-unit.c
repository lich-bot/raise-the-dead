/* ide-clang-translation-unit.c
 *
 * Copyright (C) 2015 Christian Hergert <christian@hergert.me>
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <clang-c/Index.h>
#include <glib/gi18n.h>

#include "ide-context.h"
#include "ide-clang-translation-unit.h"
#include "ide-diagnostic.h"
#include "ide-file.h"
#include "ide-internal.h"
#include "ide-project.h"
#include "ide-source-location.h"
#include "ide-vcs.h"

typedef struct
{
  CXTranslationUnit  tu;
  gint64             sequence;
  IdeDiagnostics    *diagnostics;
  GFile             *file;
} IdeClangTranslationUnitPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (IdeClangTranslationUnit, ide_clang_translation_unit, IDE_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_FILE,
  PROP_SEQUENCE,
  LAST_PROP
};

static GParamSpec *gParamSpecs [LAST_PROP];

GFile *
ide_clang_translation_unit_get_file (IdeClangTranslationUnit *self)
{
  IdeClangTranslationUnitPrivate *priv = ide_clang_translation_unit_get_instance_private (self);

  g_return_val_if_fail (IDE_IS_CLANG_TRANSLATION_UNIT (self), NULL);

  return priv->file;
}

static void
ide_clang_translation_unit_set_file (IdeClangTranslationUnit *self,
                                     GFile                   *file)
{
  IdeClangTranslationUnitPrivate *priv = ide_clang_translation_unit_get_instance_private (self);

  g_return_if_fail (IDE_IS_CLANG_TRANSLATION_UNIT (self));
  g_return_if_fail (G_IS_FILE (file));

  if (g_set_object (&priv->file, file))
    g_object_notify_by_pspec (G_OBJECT (self), gParamSpecs [PROP_FILE]);
}

IdeClangTranslationUnit *
_ide_clang_translation_unit_new (IdeContext        *context,
                                 CXTranslationUnit  tu,
                                 GFile             *file,
                                 gint64             sequence)
{
  IdeClangTranslationUnitPrivate *priv;
  IdeClangTranslationUnit *ret;

  g_return_val_if_fail (IDE_IS_CONTEXT (context), NULL);
  g_return_val_if_fail (tu, NULL);
  g_return_val_if_fail (!file || G_IS_FILE (file), NULL);

  ret = g_object_new (IDE_TYPE_CLANG_TRANSLATION_UNIT,
                      "file", file,
                      "context", context,
                      NULL);

  priv = ide_clang_translation_unit_get_instance_private (ret);

  priv->tu = tu;
  priv->sequence = sequence;

  return ret;
}

static IdeDiagnosticSeverity
translate_severity (enum CXDiagnosticSeverity severity)
{
  switch (severity)
    {
    case CXDiagnostic_Ignored:
      return IDE_DIAGNOSTIC_IGNORED;

    case CXDiagnostic_Note:
      return IDE_DIAGNOSTIC_NOTE;

    case CXDiagnostic_Warning:
      return IDE_DIAGNOSTIC_WARNING;

    case CXDiagnostic_Error:
      return IDE_DIAGNOSTIC_ERROR;

    case CXDiagnostic_Fatal:
      return IDE_DIAGNOSTIC_FATAL;

    default:
      return 0;
    }
}

static gchar *
get_path (const gchar *workpath,
          const gchar *path)
{
  if (g_str_has_prefix (path, workpath))
    {
      path = path + strlen (workpath);
      while (*path == G_DIR_SEPARATOR)
        path++;

      return g_strdup (path);
    }

  return g_strdup (path);
}

static IdeSourceLocation *
create_location (IdeClangTranslationUnit *self,
                 IdeProject              *project,
                 const gchar             *workpath,
                 CXSourceLocation         cxloc)
{
  IdeSourceLocation *ret = NULL;
  IdeFile *file = NULL;
  CXFile cxfile = NULL;
  g_autofree gchar *path = NULL;
  CXString str;
  unsigned line;
  unsigned column;
  unsigned offset;

  g_return_val_if_fail (self, NULL);

  clang_getFileLocation (cxloc, &cxfile, &line, &column, &offset);

  if (line > 0) line--;
  if (column > 0) column--;

  str = clang_getFileName (cxfile);
  path = get_path (workpath, clang_getCString (str));
  clang_disposeString (str);

  file = ide_project_get_file_for_path (project, path);

  if (!file)
    {
      IdeContext *context;
      GFile *gfile;

      context = ide_object_get_context (IDE_OBJECT (self));
      gfile = g_file_new_for_path (path);

      file = g_object_new (IDE_TYPE_FILE,
                           "context", context,
                           "file", gfile,
                           "path", path,
                           NULL);
    }

  ret = ide_source_location_new (file, line, column, offset);

  return ret;
}

static IdeSourceRange *
create_range (IdeClangTranslationUnit *self,
              IdeProject              *project,
              const gchar             *workpath,
              CXSourceRange            cxrange)
{
  IdeSourceRange *range;
  CXSourceLocation cxbegin;
  CXSourceLocation cxend;
  g_autoptr(IdeSourceLocation) begin = NULL;
  g_autoptr(IdeSourceLocation) end = NULL;

  g_return_val_if_fail (IDE_IS_CLANG_TRANSLATION_UNIT (self), NULL);

  cxbegin = clang_getRangeStart (cxrange);
  cxend = clang_getRangeEnd (cxrange);

  begin = create_location (self, project, workpath, cxbegin);
  end = create_location (self, project, workpath, cxend);

  range = _ide_source_range_new (begin, end);

  return range;
}

static gboolean
cxfile_equal (CXFile  cxfile,
              GFile  *file)
{
  CXString cxstr;
  gchar *path;
  gboolean ret;

  cxstr = clang_getFileName (cxfile);
  path = g_file_get_path (file);

  ret = (0 == g_strcmp0 (clang_getCString (cxstr), path));

  clang_disposeString (cxstr);
  g_free (path);

  return ret;
}

static IdeDiagnostic *
create_diagnostic (IdeClangTranslationUnit *self,
                   IdeProject              *project,
                   const gchar             *workpath,
                   CXDiagnostic            *cxdiag)
{
  IdeClangTranslationUnitPrivate *priv = ide_clang_translation_unit_get_instance_private (self);
  enum CXDiagnosticSeverity cxseverity;
  IdeDiagnosticSeverity severity;
  IdeDiagnostic *diag;
  IdeSourceLocation *loc;
  g_autofree gchar *spelling = NULL;
  CXString cxstr;
  CXSourceLocation cxloc;
  CXFile cxfile = NULL;
  guint num_ranges;
  guint i;

  g_return_val_if_fail (IDE_IS_CLANG_TRANSLATION_UNIT (self), NULL);
  g_return_val_if_fail (cxdiag, NULL);

  cxloc = clang_getDiagnosticLocation (cxdiag);
  clang_getExpansionLocation (cxloc, &cxfile, NULL, NULL, NULL);

  if (cxfile && !cxfile_equal (cxfile, priv->file))
    return NULL;

  cxseverity = clang_getDiagnosticSeverity (cxdiag);
  severity = translate_severity (cxseverity);

  cxstr = clang_getDiagnosticSpelling (cxdiag);
  spelling = g_strdup (clang_getCString (cxstr));
  clang_disposeString (cxstr);

  loc = create_location (self, project, workpath, cxloc);

  diag = _ide_diagnostic_new (severity, spelling, loc);

  num_ranges = clang_getDiagnosticNumRanges (cxdiag);

  for (i = 0; i < num_ranges; i++)
    {
      CXSourceRange cxrange;
      IdeSourceRange *range;

      cxrange = clang_getDiagnosticRange (cxdiag, i);
      range = create_range (self, project, workpath, cxrange);
      _ide_diagnostic_take_range (diag, range);
    }

  return diag;
}

/**
 * ide_clang_translation_unit_get_diagnostics:
 *
 * Retrieves the diagnostics for the translation unit.
 *
 * Returns: (transfer none) (nullable): An #IdeDiagnostics or %NULL.
 */
IdeDiagnostics *
ide_clang_translation_unit_get_diagnostics (IdeClangTranslationUnit *self)
{
  IdeClangTranslationUnitPrivate *priv;

  g_return_val_if_fail (IDE_IS_CLANG_TRANSLATION_UNIT (self), NULL);

  priv = ide_clang_translation_unit_get_instance_private (self);

  if (!priv->diagnostics)
    {
      IdeContext *context;
      IdeProject *project;
      IdeVcs *vcs;
      g_autofree gchar *workpath = NULL;
      GFile *workdir;
      GPtrArray *ar;
      guint count;
      guint i;

      ar = g_ptr_array_new_with_free_func ((GDestroyNotify)ide_diagnostic_unref);
      count = clang_getNumDiagnostics (priv->tu);

      /*
       * Acquire the reader lock for the project since we will need to do
       * a bunch of project tree lookups when creating diagnostics. By doing
       * this outside of the loops, we avoid creating lots of contention on
       * the reader lock, but potentially hold on to the entire lock for a bit
       * longer at a time.
       */
      context = ide_object_get_context (IDE_OBJECT (self));
      project = ide_context_get_project (context);
      vcs = ide_context_get_vcs (context);
      workdir = ide_vcs_get_working_directory (vcs);
      workpath = g_file_get_path (workdir);

      ide_project_reader_lock (project);

      for (i = 0; i < count; i++)
        {
          CXDiagnostic cxdiag;
          IdeDiagnostic *diag;

          cxdiag = clang_getDiagnostic (priv->tu, i);
          diag = create_diagnostic (self, project, workpath, cxdiag);
          if (diag)
            g_ptr_array_add (ar, diag);
          clang_disposeDiagnostic (cxdiag);
        }

      ide_project_reader_unlock (project);

      priv->diagnostics = _ide_diagnostics_new (ar);
    }

  return priv->diagnostics;
}

gint64
ide_clang_translation_unit_get_sequence (IdeClangTranslationUnit *self)
{
  IdeClangTranslationUnitPrivate *priv;

  g_return_val_if_fail (IDE_IS_CLANG_TRANSLATION_UNIT (self), -1);

  priv = ide_clang_translation_unit_get_instance_private (self);

  return priv->sequence;
}

static void
ide_clang_translation_unit_finalize (GObject *object)
{
  IdeClangTranslationUnit *self = (IdeClangTranslationUnit *)object;
  IdeClangTranslationUnitPrivate *priv = ide_clang_translation_unit_get_instance_private (self);

  clang_disposeTranslationUnit (priv->tu);

  G_OBJECT_CLASS (ide_clang_translation_unit_parent_class)->finalize (object);
}

static void
ide_clang_translation_unit_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  IdeClangTranslationUnit *self = IDE_CLANG_TRANSLATION_UNIT (object);

  switch (prop_id)
    {
    case PROP_FILE:
      g_value_set_object (value, ide_clang_translation_unit_get_file (self));
      break;

    case PROP_SEQUENCE:
      g_value_set_int64 (value, ide_clang_translation_unit_get_sequence (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ide_clang_translation_unit_set_property (GObject      *object,
                                         guint         prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  IdeClangTranslationUnit *self = IDE_CLANG_TRANSLATION_UNIT (object);

  switch (prop_id)
    {
    case PROP_FILE:
      ide_clang_translation_unit_set_file (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ide_clang_translation_unit_class_init (IdeClangTranslationUnitClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = ide_clang_translation_unit_finalize;
  object_class->get_property = ide_clang_translation_unit_get_property;
  object_class->set_property = ide_clang_translation_unit_set_property;

  gParamSpecs [PROP_FILE] =
    g_param_spec_object ("file",
                         _("File"),
                         _("The file used to build the translation unit."),
                         G_TYPE_FILE,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_FILE,
                                   gParamSpecs [PROP_FILE]);

  gParamSpecs [PROP_SEQUENCE] =
    g_param_spec_int64 ("sequence",
                        _("Sequence"),
                        _("The sequence number when created."),
                        G_MININT64,
                        G_MAXINT64,
                        0,
                        (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_SEQUENCE, gParamSpecs [PROP_SEQUENCE]);
}

static void
ide_clang_translation_unit_init (IdeClangTranslationUnit *self)
{
}

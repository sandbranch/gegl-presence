/*
 * Shared by the test programs: loads this repository's operations from
 * the build folder, and sets properties from "name=value" strings
 *
 * modules.h
 * Copyright 2026 by David
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PRESENCE_TESTS_MODULES_H
#define PRESENCE_TESTS_MODULES_H

#include <gegl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const gchar *const presence_modules[] =
  { "clarity", "texture", "dehaze", "whites-blacks" };

/* Links the modules of the build folder into a temporary folder of
 * their own and points GEGL_PATH there and at GEGL's own operations only
 * (GEGL loads every module of a folder, and GEGL_PATH replaces GEGL's
 * list, so installed copies are not loaded). Call before gegl_init.
 * Returns the folder, for modules_cleanup. */
static gchar *
modules_setup (const gchar *build)
{
  gchar *dir = g_dir_make_tmp ("presence-modules-XXXXXX", NULL);
  gchar *path;
  guint  i;

  if (! dir)
    return NULL;
  for (i = 0; i < G_N_ELEMENTS (presence_modules); i++)
    {
      gchar *name   = g_strconcat (presence_modules[i], ".so", NULL);
      gchar *from   = g_build_filename (build, name, NULL);
      gchar *target = g_canonicalize_filename (from, NULL);
      gchar *link   = g_build_filename (dir, name, NULL);

      if (symlink (target, link) != 0)
        {
          fprintf (stderr, "cannot link %s\n", target);
          return NULL;
        }
      g_free (name);
      g_free (from);
      g_free (target);
      g_free (link);
    }
  path = g_strconcat (dir, G_SEARCHPATH_SEPARATOR_S, GEGL_PLUGINSDIR, NULL);
  g_setenv ("GEGL_PATH", path, TRUE);
  g_free (path);
  return dir;
}

static void
modules_cleanup (gchar *dir)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (presence_modules); i++)
    {
      gchar *name = g_strconcat (presence_modules[i], ".so", NULL);
      gchar *link = g_build_filename (dir, name, NULL);

      g_unlink (link);
      g_free (link);
      g_free (name);
    }
  g_rmdir (dir);
  g_free (dir);
}

/* name=value, as on the gegl command line: numbers, booleans (true or
 * false), enums by their nick, colors as CSS strings */
static gboolean
node_set_from_string (GeglNode    *node,
                      const gchar *assignment)
{
  const gchar *eq = strchr (assignment, '=');
  const gchar *s;
  gchar       *name;
  GParamSpec  *pspec;
  GValue       value = G_VALUE_INIT;
  gboolean     ok    = TRUE;

  if (! eq)
    return FALSE;
  s     = eq + 1;
  name  = g_strndup (assignment, eq - assignment);
  pspec = gegl_node_find_property (node, name);
  if (! pspec)
    {
      g_free (name);
      return FALSE;
    }
  g_value_init (&value, pspec->value_type);
  if (G_IS_PARAM_SPEC_ENUM (pspec))
    {
      GEnumClass *klass = g_type_class_ref (pspec->value_type);
      GEnumValue *v     = g_enum_get_value_by_nick (klass, s);

      if (v)
        g_value_set_enum (&value, v->value);
      ok = v != NULL;
      g_type_class_unref (klass);
    }
  else if (G_IS_PARAM_SPEC_DOUBLE (pspec))
    g_value_set_double (&value, g_ascii_strtod (s, NULL));
  else if (G_IS_PARAM_SPEC_INT (pspec))
    g_value_set_int (&value, (gint) g_ascii_strtoll (s, NULL, 10));
  else if (G_IS_PARAM_SPEC_BOOLEAN (pspec))
    g_value_set_boolean (&value, strcmp (s, "true") == 0 || strcmp (s, "1") == 0);
  else if (g_type_is_a (pspec->value_type, GEGL_TYPE_COLOR))
    {
      GeglColor *c = gegl_color_new (s);

      g_value_take_object (&value, c);
    }
  else
    ok = FALSE;
  if (ok)
    gegl_node_set_property (node, name, &value);
  g_value_unset (&value);
  g_free (name);
  return ok;
}

#endif

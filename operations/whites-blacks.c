/*
 * Whites and Blacks: end-point tone controls, as a GEGL operation
 *
 * whites-blacks.c
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
 *
 * A tone curve on CIE lightness (L* / 100) that moves the white end
 * (Whites) and the black end (Blacks) and leaves the midtones: the upper
 * half and the lower half each get a cubic that is flat at L = 0.5
 * (the curve is smooth there, to the second derivative) and moves the
 * end by up to 1/6 of the range. With "soft roll-off" an end that would
 * leave the range stays at 1 (or 0) and the tones next to it are raised
 * (or lowered) instead, u + w (u^3 - u^4), so nothing clips. The colour
 * of each pixel is kept (its channels scaled to the new luminance).
 * A point operation: each pixel on its own.
 */

#include <glib/gi18n-lib.h>

#ifdef GEGL_PROPERTIES

enum_start (presence_wb_ends)
  enum_value (PRESENCE_WB_SOFT, "soft", N_("Soft roll-off (no clipping)"))
  enum_value (PRESENCE_WB_MOVE, "move", N_("Move the end points (may clip)"))
enum_end (PresenceWbEnds)

enum_start (presence_wb_range)
  enum_value (PRESENCE_WB_KEEP_HUE, "keep-hue", N_("Keep hue within 0 to 1"))
  enum_value (PRESENCE_WB_CLIP,     "clip",     N_("Clip each channel to 0 to 1"))
  enum_value (PRESENCE_WB_NONE,     "none",     N_("Unbounded (float)"))
enum_end (PresenceWbRange)

property_double (whites, _("Whites"), 0.0)
  description (_("Moves the white end of the tones: positive values "
                 "brighten the lightest tones, negative values darken them "
                 "(white becomes light gray). The midtones stay."))
  value_range (-100.0, 100.0)
  ui_digits (0)

property_double (blacks, _("Blacks"), 0.0)
  description (_("Moves the black end of the tones: positive values lift "
                 "the darkest tones (black becomes dark gray), negative "
                 "values deepen them. The midtones stay."))
  value_range (-100.0, 100.0)
  ui_digits (0)

property_enum (ends, _("Ends"), PresenceWbEnds, presence_wb_ends, PRESENCE_WB_SOFT)
  description (_("Brighter whites and deeper blacks: with a soft roll-off "
                 "white stays white and black stays black and the tones "
                 "next to them move, so nothing clips; or the end points "
                 "move, so the lightest (darkest) tones clip."))

property_enum (output_range, _("Output range"),
               PresenceWbRange, presence_wb_range, PRESENCE_WB_KEEP_HUE)
  description (_("What happens to colors that end up outside 0 to 1: moved "
                 "towards gray of the same luminance (the hue stays), "
                 "clipped per channel, or kept (for high dynamic range "
                 "images)."))

#else

#define GEGL_OP_POINT_FILTER
#define GEGL_OP_NAME     whites_blacks
#define GEGL_OP_C_SOURCE whites-blacks.c

#include "gegl-op.h"
#include "presence-common.h"

typedef struct
{
  PresenceSpace s;
} Data;

/* One half of the curve, in u = distance from L = 0.5 as a fraction of
 * the half range (0 at the midtones, 1 at the end). w in -1 to 1:
 * positive moves the end outwards (brighter whites, deeper blacks),
 * negative inwards. Above u = 1 (colors beyond white or black in float
 * images) the curve goes on straight with its slope at the end. */
static inline gfloat
half_curve (gfloat   u,
            gfloat   w,
            gboolean soft)
{
  gfloat v, slope;

  if (u <= 0.0f || w == 0.0f)
    return u;
  if (w > 0.0f && soft)
    {
      /* the end stays: f(1) = 1, f'(1) = 1 - w */
      gfloat uu = MIN (u, 1.0f), u3 = uu * uu * uu;

      v     = uu + w * (u3 - u3 * uu);
      slope = 1.0f - w;
    }
  else
    {
      /* the end moves by w / 3 of the half range: f(1) = 1 + w / 3,
       * f'(1) = 1 + w, never below 0 for w >= -1 */
      gfloat uu = MIN (u, 1.0f);

      v     = uu + (w / 3.0f) * uu * uu * uu;
      slope = 1.0f + w;
    }
  if (u > 1.0f)
    v += (u - 1.0f) * slope;
  return v;
}

/* the curve on lightness x (L* / 100) */
static inline gfloat
curve (gfloat   x,
       gfloat   whites,
       gfloat   blacks,
       gboolean soft)
{
  if (x > 0.5f)
    return 0.5f + 0.5f * half_curve ((x - 0.5f) * 2.0f, whites, soft);
  /* deeper blacks move the black end outwards: the sign of the slider
   * is turned */
  return 0.5f - 0.5f * half_curve ((0.5f - x) * 2.0f, -blacks, soft);
}

static void
prepare (GeglOperation *operation)
{
  GeglProperties *o     = GEGL_PROPERTIES (operation);
  const Babl     *space = gegl_operation_get_source_space (operation, "input");
  const Babl     *format = babl_format_with_space ("RGBA float", space);
  Data           *d;

  if (! o->user_data)
    o->user_data = g_new0 (Data, 1);
  d = o->user_data;
  presence_space_init (&d->s, space);
  gegl_operation_set_format (operation, "input", format);
  gegl_operation_set_format (operation, "output", format);
}

static void
finalize (GObject *object)
{
  GeglOperation  *operation = GEGL_OPERATION (object);
  GeglProperties *o         = GEGL_PROPERTIES (operation);

  g_clear_pointer (&o->user_data, g_free);
  G_OBJECT_CLASS (gegl_op_parent_class)->finalize (object);
}

static gboolean
process (GeglOperation       *operation,
         void                *in_buf,
         void                *out_buf,
         glong                n_pixels,
         const GeglRectangle *roi,
         gint                 level)
{
  GeglProperties *o    = GEGL_PROPERTIES (operation);
  const Data     *d    = o->user_data;
  const gfloat   *in   = in_buf;
  gfloat         *out  = out_buf;
  gfloat          w    = o->whites / 100.0, b = o->blacks / 100.0;
  gboolean        soft = o->ends == PRESENCE_WB_SOFT;
  glong           i;

  (void) roi;
  (void) level;
  for (i = 0; i < n_pixels; i++, in += 4, out += 4)
    {
      gfloat Y = presence_luminance (&d->s, in);
      gfloat v[3];

      if (! presence_finite3 (in) || ! isfinite (Y))
        {
          memcpy (out, in, 4 * sizeof (gfloat));
          continue;
        }
      {
        gfloat Y2 = presence_L_to_Y (curve (presence_Y_to_L (Y), w, b, soft));

        presence_set_luminance (&d->s, in, Y, Y2, v);
        presence_range (o->output_range, Y2, v);
      }
      out[0] = v[0];
      out[1] = v[1];
      out[2] = v[2];
      out[3] = in[3];
    }
  return TRUE;
}

static gboolean
operation_process (GeglOperation        *operation,
                   GeglOperationContext *context,
                   const gchar          *output_prop,
                   const GeglRectangle  *result,
                   gint                  level)
{
  GeglProperties *o = GEGL_PROPERTIES (operation);

  if (o->whites == 0.0 && o->blacks == 0.0)
    return presence_pass_through (operation, context);
  return GEGL_OPERATION_CLASS (gegl_op_parent_class)->process (operation, context, output_prop,
                                                               result, level);
}

static void
gegl_op_class_init (GeglOpClass *klass)
{
  GObjectClass                  *object_class    = G_OBJECT_CLASS (klass);
  GeglOperationClass            *operation_class = GEGL_OPERATION_CLASS (klass);
  GeglOperationPointFilterClass *point_class     = GEGL_OPERATION_POINT_FILTER_CLASS (klass);

  object_class->finalize     = finalize;
  operation_class->prepare   = prepare;
  operation_class->process   = operation_process;
  point_class->process       = process;

  gegl_operation_class_set_keys (operation_class,
    "name",            "presence:whites-blacks",
    "title",           _("Whites and Blacks"),
    "categories",      "color",
    "description",     _("Moves the white and the black end of the tones, "
                         "with a soft roll-off so that nothing clips, and "
                         "leaves the midtones; colors are kept."),
    "gimp:menu-path",  "<Image>/Colors",
    "gimp:menu-label", _("Whites and Blacks..."),
    NULL);
}

#endif

/*
 * Clarity: midtone local contrast, as a GEGL operation
 *
 * clarity.c
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
 * Raises (or lowers) the contrast of mid-size detail, most in the
 * midtones, without halos at strong edges: a band limited local
 * Laplacian filter on CIE lightness (presence-common.h, README.md).
 * The colour of each pixel is kept (its channels scaled to the new
 * luminance).
 */

#include <glib/gi18n-lib.h>

#ifdef GEGL_PROPERTIES

enum_start (presence_clarity_range)
  enum_value (PRESENCE_CLARITY_KEEP_HUE, "keep-hue", N_("Keep hue within 0 to 1"))
  enum_value (PRESENCE_CLARITY_CLIP,     "clip",     N_("Clip each channel to 0 to 1"))
  enum_value (PRESENCE_CLARITY_NONE,     "none",     N_("Unbounded (float)"))
enum_end (PresenceClarityRange)

property_double (amount, _("Amount"), 0.0)
  description (_("Positive values raise the contrast of mid-size detail, "
                 "most in the midtones; negative values soften it. "
                 "0 leaves the image as it is."))
  value_range (-100.0, 100.0)
  ui_digits (0)

property_double (size, _("Size"), 40.0)
  description (_("The size (radius) in pixels of the detail that is "
                 "changed most. Detail from about a quarter of it to about "
                 "four times it is changed too, less."))
  value_range (4.0, 1000.0)
  ui_range (4.0, 400.0)
  ui_gamma (2.0)
  ui_meta ("unit", "pixel-distance")
  ui_digits (0)

property_enum (output_range, _("Output range"),
               PresenceClarityRange, presence_clarity_range, PRESENCE_CLARITY_KEEP_HUE)
  description (_("What happens to colors that end up outside 0 to 1: moved "
                 "towards gray of the same luminance (the hue stays), "
                 "clipped per channel, or kept (for high dynamic range "
                 "images)."))

#else

#define GEGL_OP_FILTER
#define GEGL_OP_NAME     clarity
#define GEGL_OP_C_SOURCE clarity.c

#include "gegl-op.h"
#include "presence-common.h"

/* the filter's settings: detail below 0.12 in L (12 L*) is enhanced,
 * larger steps are edges and are left; the samples are weighted towards
 * the midtones (a Gaussian of width 0.25 around L = 0.5); the band is
 * about two octaves wide */
#define CLARITY_SIGMA     0.12f
#define CLARITY_MID_WIDTH 0.25f
#define CLARITY_OCTAVES   1.0
/* the gain at +100: the detail of the band about tripled; -100 flattens it */
#define CLARITY_MAX_GAIN  2.0f

static gboolean
process (GeglOperation       *operation,
         GeglBuffer          *input,
         GeglBuffer          *output,
         const GeglRectangle *result,
         gint                 level)
{
  GeglProperties *o    = GEGL_PROPERTIES (operation);
  PresenceLLF     llf  = { 0, };
  gfloat          gain = o->amount >= 0.0 ? CLARITY_MAX_GAIN * o->amount / 100.0
                                          : o->amount / 100.0;

  (void) result;
  (void) level;
  llf.sigma     = CLARITY_SIGMA;
  llf.noise     = 0.0f;
  llf.mid_width = CLARITY_MID_WIDTH;
  return presence_detail_process (operation, input, output, &llf, o->size,
                                  CLARITY_OCTAVES, gain, o->output_range,
                                  "presence:clarity");
}

static gboolean
operation_process (GeglOperation        *operation,
                   GeglOperationContext *context,
                   const gchar          *output_prop,
                   const GeglRectangle  *result,
                   gint                  level)
{
  GeglProperties *o = GEGL_PROPERTIES (operation);

  if (o->amount == 0.0)
    return presence_pass_through (operation, context);
  return GEGL_OPERATION_CLASS (gegl_op_parent_class)->process (operation, context, output_prop,
                                                               result, level);
}

static void
gegl_op_class_init (GeglOpClass *klass)
{
  GeglOperationClass       *operation_class = GEGL_OPERATION_CLASS (klass);
  GeglOperationFilterClass *filter_class    = GEGL_OPERATION_FILTER_CLASS (klass);

  operation_class->prepare                   = presence_prepare_linear;
  operation_class->process                   = operation_process;
  operation_class->get_required_for_output   = presence_get_required_for_output;
  operation_class->get_cached_region         = presence_get_cached_region;
  operation_class->get_invalidated_by_change = presence_get_invalidated_by_change;
  operation_class->threaded                  = FALSE;
  filter_class->process                      = process;

  gegl_operation_class_set_keys (operation_class,
    "name",            "presence:clarity",
    "title",           _("Clarity"),
    "categories",      "enhance",
    "description",     _("Raises or lowers the contrast of mid-size detail, "
                         "most in the midtones, without halos at strong "
                         "edges (a local Laplacian filter on lightness); "
                         "colors are kept."),
    "gimp:menu-path",  "<Image>/Filters/Enhance",
    "gimp:menu-label", _("Clarity..."),
    NULL);
}

#endif

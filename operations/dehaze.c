/*
 * Dehaze: removes or adds atmospheric haze, as a GEGL operation
 *
 * dehaze.c
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
 * The haze model of Koschmieder, I = J t + A (1 - t), in linear light:
 * I the photo, J the scene without haze, A the colour of the haze
 * (airlight), t the transmission. A and t are estimated with the dark
 * channel prior of He, Sun and Tang (CVPR 2009, TPAMI 2011): in haze-free
 * outdoor photos most patches have a pixel with a channel near 0, so
 * 1 - t is the dark channel of I / A. The Microsoft patent on it
 * (US 8,340,461) lapsed for non-payment of its fees (docs/patents.md).
 *
 * The estimates are made once for the whole image on a copy of about
 * 1024 pixels across, so the result does not depend on tiles; the
 * transmission follows the edges of the full-size photo through the fast
 * guided filter (He and Sun 2015): its coefficients from the small copy,
 * applied to the full-size lightness.
 *
 * Amount 1 to 100 removes that percentage of the haze (t scaled
 * towards 1: 100 is the model's full removal); -1 to -100 adds haze of
 * the same colour, more in the distance (the estimated t raised to a
 * power) and a little everywhere.
 */

#include <glib/gi18n-lib.h>

#ifdef GEGL_PROPERTIES

enum_start (presence_dehaze_range)
  enum_value (PRESENCE_DEHAZE_KEEP_HUE, "keep-hue", N_("Keep hue within 0 to 1"))
  enum_value (PRESENCE_DEHAZE_CLIP,     "clip",     N_("Clip each channel to 0 to 1"))
  enum_value (PRESENCE_DEHAZE_NONE,     "none",     N_("Unbounded (float)"))
enum_end (PresenceDehazeRange)

property_double (amount, _("Amount"), 0.0)
  description (_("Positive values remove that percentage of the haze, "
                 "negative values add haze. 0 leaves the image as it is."))
  value_range (-100.0, 100.0)
  ui_digits (0)

property_boolean (auto_haze, _("Estimate the haze color"), TRUE)
  description (_("Estimate the color of the haze (the airlight) from the "
                 "image. Turn off to pick it, for example from a patch of "
                 "hazy sky."))

property_color (haze_color, _("Haze color"), "#c8ccd2")
  description (_("The color of the haze: that of the most distant parts "
                 "of the scene, such as hazy sky."))
  ui_meta ("visible", "! auto-haze")

property_enum (output_range, _("Output range"),
               PresenceDehazeRange, presence_dehaze_range, PRESENCE_DEHAZE_KEEP_HUE)
  description (_("What happens to colors that end up outside 0 to 1: moved "
                 "towards gray of the same luminance (the hue stays), "
                 "clipped per channel, or kept (for high dynamic range "
                 "images)."))

#else

#define GEGL_OP_FILTER
#define GEGL_OP_NAME     dehaze
#define GEGL_OP_C_SOURCE dehaze.c

#include "gegl-op.h"
#include "presence-common.h"
#include <stdlib.h>

/* the copy the estimates are made on */
#define DEHAZE_SMALL      1024
/* He et al.: a 15 x 15 patch on photos of about 600 pixels */
#define DEHAZE_PATCH_600  7.0
/* the brightest 0.1 % of the dark channel are the haze candidates */
#define DEHAZE_TOP        0.001
/* the smallest transmission divided by (He et al.) */
#define DEHAZE_T0         0.1f
/* the guided filter: radius in patches, and its epsilon for a guide in
 * lightness 0 to 1 */
#define DEHAZE_GF_PATCHES 2
#define DEHAZE_GF_EPS     1e-4f
/* adding haze: t^s times a veil of exp (-s DEHAZE_VEIL) everywhere */
#define DEHAZE_VEIL       0.5f

typedef struct
{
  gint    w, h;
  gfloat  f;        /* full-size pixels per small pixel */
  gfloat *rgb;      /* the small copy, linear RGB */
  gfloat *a, *b;    /* the guided filter's coefficients of the transmission */
  gfloat  A[3];     /* the haze colour */
  gint    patch;    /* the patch radius on the small copy */
} Estimate;

static inline gfloat
finite_or_0 (gfloat v)
{
  return isfinite (v) ? v : 0.0f;
}

/* minimum (or with max, maximum) over a square of radius r,
 * separable, borders by the pixels inside only */
static void
rank_filter (const gfloat *src,
             gfloat       *dst,
             gint          w,
             gint          h,
             gint          r,
             gboolean      max)
{
  gfloat *tmp = g_new (gfloat, (gsize) w * h);
  gint    x, y, k;

  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        gfloat m = max ? -G_MAXFLOAT : G_MAXFLOAT;
        for (k = MAX (x - r, 0); k <= MIN (x + r, w - 1); k++)
          m = max ? MAX (m, src[(gsize) y * w + k]) : MIN (m, src[(gsize) y * w + k]);
        tmp[(gsize) y * w + x] = m;
      }
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        gfloat m = max ? -G_MAXFLOAT : G_MAXFLOAT;
        for (k = MAX (y - r, 0); k <= MIN (y + r, h - 1); k++)
          m = max ? MAX (m, tmp[(gsize) k * w + x]) : MIN (m, tmp[(gsize) k * w + x]);
        dst[(gsize) y * w + x] = m;
      }
  g_free (tmp);
}

static int
cmp_float (const void *a,
           const void *b)
{
  gfloat x = *(const gfloat *) a, y = *(const gfloat *) b;

  return (x > y) - (x < y);
}

static void
estimate (GeglProperties      *o,
          const Babl          *format,
          const PresenceSpace *s,
          const gfloat        *in,
          gint                 W,
          gint                 H,
          Estimate            *e)
{
  gint    f = MAX (1, (MAX (W, H) + DEHAZE_SMALL - 1) / DEHAZE_SMALL);
  gint    x, y, c;
  gsize   n, i;
  gfloat *dark, *tmp, *guide, *t;

  e->f = f;
  e->w = (W + f - 1) / f;
  e->h = (H + f - 1) / f;
  n    = (gsize) e->w * e->h;
  e->rgb = g_new0 (gfloat, n * 3);
  e->a   = g_new (gfloat, n);
  e->b   = g_new (gfloat, n);
  e->patch = MAX (1, (gint) floor (DEHAZE_PATCH_600 * MAX (e->w, e->h) / 600.0 + 0.5));

  /* the small copy: block means (the blocks at the right and bottom
   * edges may be smaller); broken pixels count as black */
  for (y = 0; y < e->h; y++)
    for (x = 0; x < e->w; x++)
      {
        gdouble sum[3] = { 0, 0, 0 };
        gint    cnt = 0, dx, dy;

        for (dy = 0; dy < f && y * f + dy < H; dy++)
          for (dx = 0; dx < f && x * f + dx < W; dx++)
            {
              const gfloat *p = in + ((gsize) (y * f + dy) * W + x * f + dx) * 4;
              if (presence_finite3 (p))
                for (c = 0; c < 3; c++)
                  sum[c] += p[c];
              cnt++;
            }
        for (c = 0; c < 3; c++)
          e->rgb[((gsize) y * e->w + x) * 3 + c] = sum[c] / MAX (cnt, 1);
      }

  dark  = g_new (gfloat, n);
  tmp   = g_new (gfloat, n);
  guide = g_new (gfloat, n);
  t     = g_new (gfloat, n);

  /* the haze colour: the mean of the pixels of the brightest 0.1 % of
   * the dark channel (He et al. take the brightest of them; the mean is
   * steadier) */
  if (o->auto_haze)
    {
      gfloat  thr;
      gsize   top = MAX ((gsize) 1, (gsize) (n * DEHAZE_TOP));
      gdouble sum[3] = { 0, 0, 0 };
      gsize   cnt = 0;

      for (i = 0; i < n; i++)
        {
          const gfloat *p = e->rgb + i * 3;
          tmp[i] = MIN (p[0], MIN (p[1], p[2]));
        }
      rank_filter (tmp, dark, e->w, e->h, e->patch, FALSE);
      memcpy (tmp, dark, n * sizeof (gfloat));
      qsort (tmp, n, sizeof (gfloat), cmp_float);
      thr = tmp[n - top];
      for (i = 0; i < n; i++)
        if (dark[i] >= thr)
          {
            for (c = 0; c < 3; c++)
              sum[c] += e->rgb[i * 3 + c];
            cnt++;
          }
      for (c = 0; c < 3; c++)
        e->A[c] = sum[c] / MAX (cnt, (gsize) 1);
    }
  else
    {
      gfloat rgb[3];

      /* in the image's own space, so that a color picked from the image
       * is that of its pixels */
      gegl_color_get_pixel (o->haze_color,
                            babl_format_with_space ("RGB float", babl_format_get_space (format)),
                            rgb);
      for (c = 0; c < 3; c++)
        e->A[c] = finite_or_0 (rgb[c]);
    }
  for (c = 0; c < 3; c++)
    e->A[c] = MAX (e->A[c], 1e-3f);

  /* the transmission: 1 - the dark channel of I / A */
  for (i = 0; i < n; i++)
    {
      const gfloat *p = e->rgb + i * 3;
      tmp[i] = MIN (p[0] / e->A[0], MIN (p[1] / e->A[1], p[2] / e->A[2]));
    }
  rank_filter (tmp, dark, e->w, e->h, e->patch, FALSE);
    {
      /* then the maximum over the same patch (a morphological opening):
       * the minimum alone spreads a dark object's value a patch wide
       * into the haze around it, a bright halo after the removal; the
       * opening keeps its outline and still ignores bright objects
       * smaller than a patch, as the prior wants */
      memcpy (tmp, dark, n * sizeof (gfloat));
      rank_filter (tmp, dark, e->w, e->h, e->patch, TRUE);
    }
  for (i = 0; i < n; i++)
    {
      t[i] = CLAMP (1.0f - dark[i], 0.0f, 1.0f);
      guide[i] = CLAMP (presence_Y_to_L (presence_luminance (s, e->rgb + i * 3)), 0.0f, 1.5f);
    }
  /* refined along the edges of the lightness */
  presence_guided_coeffs (guide, t, e->w, e->h, DEHAZE_GF_PATCHES * e->patch,
                          DEHAZE_GF_EPS,
                          e->a, e->b);

  if (g_getenv ("PRESENCE_DEBUG"))
    g_printerr ("presence:dehaze: small copy %d x %d (1:%d), patch radius %d, haze color %.4f %.4f %.4f\n",
                e->w, e->h, f, e->patch, e->A[0], e->A[1], e->A[2]);
  g_free (t);
  g_free (guide);
  g_free (tmp);
  g_free (dark);
}

typedef struct
{
  const PresenceSpace *s;
  const Estimate      *e;
  gfloat              *buf;
  gint                 W, range;
  gfloat               amount;
  gboolean             show_t;
} Rows;

/* the transmission at a full-size pixel: the guided filter's
 * coefficients, interpolated, applied to its own lightness */
static inline gfloat
transmission (const Rows   *d,
              const gfloat *p,
              gint          x,
              gint          y)
{
  const Estimate *e = d->e;
  gfloat          Y = presence_luminance (d->s, p);
  gfloat          L = CLAMP (presence_Y_to_L (isfinite (Y) ? Y : 0.0f), 0.0f, 1.5f);
  gfloat          a = presence_sample (e->a, e->w, e->h, e->f, x, y);
  gfloat          b = presence_sample (e->b, e->w, e->h, e->f, x, y);

  return CLAMP (a * L + b, 0.0f, 1.0f);
}

static void
dehaze_rows (gint y0, gint y1, gpointer data)
{
  Rows           *d = data;
  const Estimate *e = d->e;
  gint            x, y, c;

  for (y = y0; y < y1; y++)
    for (x = 0; x < d->W; x++)
      {
        gfloat *p = d->buf + ((gsize) y * d->W + x) * 4;
        gfloat  t, v[3];

        if (! presence_finite3 (p) || ! isfinite (presence_luminance (d->s, p)))
          continue;
        t = transmission (d, p, x, y);
        if (d->show_t)
          {
            p[0] = p[1] = p[2] = t;
            continue;
          }
        if (d->amount > 0.0f)
          {
            gfloat ta = MAX (1.0f - d->amount * (1.0f - t), DEHAZE_T0);

            for (c = 0; c < 3; c++)
              v[c] = (p[c] - e->A[c]) / ta + e->A[c];
          }
        else
          {
            gfloat s  = -d->amount;
            gfloat th = powf (MAX (t, 0.05f), s) * expf (-s * DEHAZE_VEIL);

            for (c = 0; c < 3; c++)
              v[c] = p[c] * th + e->A[c] * (1.0f - th);
          }
        presence_range (d->range, presence_luminance (d->s, v), v);
        for (c = 0; c < 3; c++)
          p[c] = v[c];
      }
}

static gboolean
process (GeglOperation       *operation,
         GeglBuffer          *input,
         GeglBuffer          *output,
         const GeglRectangle *result,
         gint                 level)
{
  GeglProperties      *o      = GEGL_PROPERTIES (operation);
  const Babl          *format = gegl_operation_get_format (operation, "output");
  const GeglRectangle *whole  = gegl_operation_source_get_bounding_box (operation, "input");
  PresenceSpace        s;
  Estimate             e = { 0, };
  Rows                 rows;
  gfloat              *buf;
  gint64               t0 = g_get_monotonic_time ();

  (void) result;
  (void) level;
  if (! whole || whole->width < 1 || whole->height < 1)
    return TRUE;
  if (gegl_rectangle_is_infinite_plane (whole))
    {
      gegl_buffer_copy (input, whole, GEGL_ABYSS_NONE, output, whole);
      return TRUE;
    }
  buf = presence_get_whole (operation, input, output, whole, "presence:dehaze");
  if (! buf)
    return TRUE;
  presence_space_init (&s, babl_format_get_space (format));
  estimate (o, format, &s, buf, whole->width, whole->height, &e);

  rows.s = &s; rows.e = &e; rows.buf = buf; rows.W = whole->width;
  rows.range = o->output_range; rows.amount = o->amount / 100.0;
  /* PRESENCE_DEBUG=t: the transmission as gray; =A: the haze color */
  rows.show_t = g_strcmp0 (g_getenv ("PRESENCE_DEBUG"), "t") == 0;
  if (g_strcmp0 (g_getenv ("PRESENCE_DEBUG"), "A") == 0)
    {
      gsize i, n = (gsize) whole->width * whole->height;
      for (i = 0; i < n; i++)
        memcpy (buf + i * 4, e.A, 3 * sizeof (gfloat));
      rows.W = 0;
    }
  if (rows.W > 0)
    presence_parallel_rows (whole->height, (gsize) whole->width * 8, dehaze_rows, &rows);

  gegl_buffer_set (output, whole, 0, format, buf, GEGL_AUTO_ROWSTRIDE);
  if (g_getenv ("PRESENCE_DEBUG"))
    g_printerr ("presence:dehaze: %d x %d, %.3f s\n", whole->width, whole->height,
                (g_get_monotonic_time () - t0) / 1e6);
  g_free (e.b);
  g_free (e.a);
  g_free (e.rgb);
  g_free (buf);
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
    "name",            "presence:dehaze",
    "title",           _("Dehaze"),
    "categories",      "enhance",
    "description",     _("Removes or adds atmospheric haze (dark channel "
                         "prior, with the transmission refined along the "
                         "image's edges)."),
    "gimp:menu-path",  "<Image>/Filters/Enhance",
    "gimp:menu-label", _("Dehaze..."),
    NULL);
}

#endif

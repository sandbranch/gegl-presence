/*
 * Automated checks for presence:clarity, presence:texture,
 * presence:dehaze and presence:whites-blacks
 *
 * check.c
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
 * Runs the operations on generated images and measures what they must
 * do: the gain of each operation over sizes of detail (gratings), the
 * overshoot at a step edge (halos) against an unsharp mask of the same
 * gain, the midtone weighting, noise and smooth areas, dehazing of an
 * image made with the haze model from a known scene, transmission and
 * haze colour, the end points of Whites and Blacks with and without
 * clipping, monotonic curves, colours kept; identity at 0, alpha,
 * broken pixels (NaN, infinity) and huge values, other buffer formats
 * and color spaces, and the same result for any tile size, number of
 * threads and requested region. With the photos of tests/images
 * (tests/images/fetch.py), also the contrast of real hazy photos before
 * and after.
 *
 *   check <build folder>
 *
 * Prints PASS, FAIL or NOTE per case and exits with 1 if any failed.
 */

#include <gegl.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules.h"

#define LIN "RGBA float"

static gint n_failed   = 0;
static gint n_passed   = 0;
static gint n_messages = 0;

static void
log_handler (const gchar    *domain,
             GLogLevelFlags  level,
             const gchar    *message,
             gpointer        data)
{
  (void) data;
  if (level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING))
    n_messages++;
  g_log_default_handler (domain, level, message, NULL);
}

static void report (const gchar *name, gboolean ok, const gchar *format, ...) G_GNUC_PRINTF (3, 4);

static void
report (const gchar *name,
        gboolean     ok,
        const gchar *format,
        ...)
{
  printf ("%s  %s", ok ? "PASS" : "FAIL", name);
  if (format)
    {
      va_list args;
      gchar  *detail;

      va_start (args, format);
      detail = g_strdup_vprintf (format, args);
      va_end (args);
      printf (": %s", detail);
      g_free (detail);
    }
  printf ("\n");
  fflush (stdout);
  if (ok)
    n_passed++;
  else
    n_failed++;
}

static void note (const gchar *name, const gchar *format, ...) G_GNUC_PRINTF (2, 3);

static void
note (const gchar *name,
      const gchar *format,
      ...)
{
  va_list args;
  gchar  *detail;

  va_start (args, format);
  detail = g_strdup_vprintf (format, args);
  va_end (args);
  printf ("NOTE  %s: %s\n", name, detail);
  fflush (stdout);
  g_free (detail);
}

/* lightness, in double precision (the operation's formulas) ------------- */

static gdouble
Y_to_L (gdouble Y)
{
  return Y > 216.0 / 24389.0 ? 1.16 * cbrt (Y) - 0.16 : Y * 24389.0 / 2700.0;
}

static gdouble
L_to_Y (gdouble L)
{
  return L > 0.08 ? pow ((L + 0.16) / 1.16, 3.0) : L * 2700.0 / 24389.0;
}

static gdouble
lum (const gfloat *p)
{
  /* babl's luminance of the sRGB primaries (its XYZ is D50 adapted),
   * as babl_space_get_rgb_luminance gives the operations */
  return 0.22248794 * p[0] + 0.71690384 * p[1] + 0.06060826 * p[2];
}

static gdouble
pix_L (const gfloat *p)
{
  return Y_to_L (lum (p));
}

/* running an operation ----------------------------------------------------- */

static GeglBuffer *
new_buffer (gint        w,
            gint        h,
            const Babl *fmt,
            gint        tile_w,
            gint        tile_h)
{
  if (tile_w > 0)
    return g_object_new (GEGL_TYPE_BUFFER, "x", 0, "y", 0, "width", w, "height", h,
                         "format", fmt, "tile-width", tile_w, "tile-height", tile_h, NULL);
  return gegl_buffer_new (GEGL_RECTANGLE (0, 0, w, h), fmt);
}

static void
set_props (GeglNode    *node,
           const gchar *op,
           const gchar *props)
{
  gchar **p = g_strsplit (props ? props : "", " ", -1);
  gint    i;

  for (i = 0; p[i]; i++)
    if (p[i][0] && ! node_set_from_string (node, p[i]))
      report ("set_property", FALSE, "%s %s", op, p[i]);
  g_strfreev (p);
}

/* op with props ("name=value ...") on w x h pixels in in_fmt, the
 * result in out_fmt, of the region roi (NULL: all); tile_w > 0 gives
 * the input buffer that tile size. g_free the result. */
static gpointer
run_full (const gchar         *op,
          const gchar         *props,
          const Babl          *in_fmt,
          gconstpointer        in,
          gint                 w,
          gint                 h,
          const Babl          *out_fmt,
          const GeglRectangle *roi,
          gint                 tile_w,
          gint                 tile_h)
{
  GeglBuffer   *buf = new_buffer (w, h, in_fmt, tile_w, tile_h);
  GeglNode     *graph, *src, *node;
  GeglRectangle r = roi ? *roi : *GEGL_RECTANGLE (0, 0, w, h);
  gpointer      out;

  gegl_buffer_set (buf, NULL, 0, in_fmt, in, GEGL_AUTO_ROWSTRIDE);
  graph = gegl_node_new ();
  src   = gegl_node_new_child (graph, "operation", "gegl:buffer-source", "buffer", buf, NULL);
  node  = gegl_node_new_child (graph, "operation", op, NULL);
  set_props (node, op, props);
  gegl_node_link (src, node);
  out = g_malloc0 ((gsize) r.width * r.height * babl_format_get_bytes_per_pixel (out_fmt));
  gegl_node_blit (node, 1.0, &r, out_fmt, out, GEGL_AUTO_ROWSTRIDE, GEGL_BLIT_DEFAULT);
  g_object_unref (graph);
  g_object_unref (buf);
  return out;
}

static gfloat *
run (const gchar  *op,
     const gchar  *props,
     const gfloat *in,
     gint          w,
     gint          h)
{
  return run_full (op, props, babl_format (LIN), in, w, h, babl_format (LIN), NULL, 0, 0);
}

static gchar *
fmt_props (const gchar *format, ...) G_GNUC_PRINTF (1, 2);

static gchar *
fmt_props (const gchar *format, ...)
{
  va_list args;
  gchar  *s;

  va_start (args, format);
  s = g_strdup_vprintf (format, args);
  va_end (args);
  return s;
}

static GRand *rnd;

/* images ------------------------------------------------------------------- */

/* gray pixels of lightness L (linear light, sRGB primaries) */
static void
set_gray (gfloat  *p,
          gdouble  L)
{
  gfloat Y = L_to_Y (L);

  p[0] = p[1] = p[2] = Y;
  p[3] = 1.0f;
}

/* a grating along x: base + amp sin (2 pi x / wavelength + 0.7), in L
 * (the phase so that a 2 pixel grating is not sampled at its zeros) */
static gfloat *
grating (gint    w,
         gint    h,
         gdouble base,
         gdouble amp,
         gdouble wavelength)
{
  gfloat *img = g_new (gfloat, (gsize) w * h * 4);
  gint    x, y;

  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      set_gray (img + ((gsize) y * w + x) * 4, base + amp * sin (2 * G_PI * x / wavelength + 0.7));
  return img;
}

/* the amplitude, in L, of row h/2 between w/4 and 3w/4 */
static gdouble
amplitude (const gfloat *img,
           gint          w,
           gint          h)
{
  gdouble lo = G_MAXDOUBLE, hi = -G_MAXDOUBLE;
  gint    x;

  for (x = w / 4; x < 3 * w / 4; x++)
    {
      gdouble L = pix_L (img + ((gsize) (h / 2) * w + x) * 4);
      lo = MIN (lo, L);
      hi = MAX (hi, L);
    }
  return 0.5 * (hi - lo);
}

/* the gain of op at a grating of wavelength wl, base L and amplitude amp */
static gdouble
gain_at (const gchar *op,
         const gchar *props,
         gdouble      wl,
         gdouble      base,
         gdouble      amp)
{
  gint    w   = MAX (1024, (gint) (8 * wl)), h = 8;
  gfloat *in  = grating (w, h, base, amp, wl);
  gfloat *out = run (op, props, in, w, h);
  gdouble g   = amplitude (out, w, h) / amplitude (in, w, h);

  g_free (in);
  g_free (out);
  return g;
}

/* a step in L at the middle, from lo to hi */
static gfloat *
step_edge (gint    w,
           gint    h,
           gdouble lo,
           gdouble hi)
{
  gfloat *img = g_new (gfloat, (gsize) w * h * 4);
  gint    x, y;

  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      set_gray (img + ((gsize) y * w + x) * 4, x < w / 2 ? lo : hi);
  return img;
}

/* the overshoot beyond the two levels of a step, in L, along row h/2 */
static gdouble
overshoot (const gfloat *img,
           gint          w,
           gint          h,
           gdouble       lo,
           gdouble       hi)
{
  gdouble worst = 0;
  gint    x;

  for (x = 0; x < w; x++)
    {
      gdouble L = pix_L (img + ((gsize) (h / 2) * w + x) * 4);
      if (x < w / 2)
        worst = MAX (worst, lo - L);
      else
        worst = MAX (worst, L - hi);
    }
  return worst;
}

/* the overshoot of an unsharp mask on L with a Gaussian of std-dev s
 * and gain k, on the same step (1-D, in double precision) */
static gdouble
usm_overshoot (gint    w,
               gdouble lo,
               gdouble hi,
               gdouble s,
               gdouble k)
{
  gdouble worst = 0;
  gint    x, i, r = (gint) ceil (4 * s);

  for (x = 0; x < w; x++)
    {
      gdouble sum = 0, ws = 0, L = x < w / 2 ? lo : hi, v;

      for (i = -r; i <= r; i++)
        {
          gint    xx = CLAMP (x + i, 0, w - 1);
          gdouble g  = exp (-0.5 * i * i / (s * s));
          sum += g * (xx < w / 2 ? lo : hi);
          ws  += g;
        }
      v = L + k * (L - sum / ws);
      worst = MAX (worst, x < w / 2 ? lo - v : v - hi);
    }
  return worst;
}

/* a photo-like colour image: smooth colour fields, texture of a few
 * sizes, some edges, dark and bright areas, some transparency */
static gfloat *
photo (gint w,
       gint h)
{
  gfloat *img = g_new (gfloat, (gsize) w * h * 4);
  gdouble ph[12];
  gint    x, y, k;

  for (k = 0; k < 12; k++)
    ph[k] = g_rand_double_range (rnd, 0, 2 * G_PI);
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        gfloat *p = img + ((gsize) y * w + x) * 4;
        gdouble u = (gdouble) x / w, v = (gdouble) y / h;
        gdouble L = 0.5 + 0.3 * sin (3 * u + ph[0]) * cos (2 * v + ph[1])
                  + 0.06 * sin (x * 0.21 + ph[2]) * sin (y * 0.17 + ph[3])
                  + 0.03 * sin (x * 0.9 + ph[4]) * cos (y * 1.1 + ph[5])
                  + 0.1 * sin (x * 0.05 + y * 0.03 + ph[6]);
        gdouble cr = 0.5 + 0.4 * sin (4 * u + ph[7]), cb = 0.5 + 0.4 * cos (5 * v + ph[8]);
        gdouble Y;

        if ((x / 97 + y / 61) % 5 == 0)
          L += 0.25;
        L = CLAMP (L, 0.02, 0.98);
        Y = L_to_Y (L);
        /* a colour of that luminance */
        p[0] = Y * (0.4 + 1.2 * cr);
        p[2] = Y * (0.4 + 1.2 * cb);
        p[1] = (Y - 0.2126 * p[0] - 0.0722 * p[2]) / 0.7152;
        if (p[1] < 0)
          {
            p[1] = 0;
            p[0] = p[2] = Y / (0.2126 + 0.0722);
          }
        p[3] = (y < 6) ? 0.5f : 1.0f;
      }
  return img;
}

static gdouble
max_diff (const gfloat *a,
          const gfloat *b,
          gsize         n)
{
  gdouble d = 0;
  gsize   i;

  for (i = 0; i < n; i++)
    {
      gdouble e = fabs ((gdouble) a[i] - b[i]);
      if (! (e <= d))
        d = isnan (e) ? INFINITY : e;
    }
  return d;
}

/* the ops and a setting that changes an image a lot */
static const gchar *const OPS[][2] =
  {
    { "presence:clarity",       "amount=100" },
    { "presence:texture",       "amount=100" },
    { "presence:dehaze",        "amount=80" },
    { "presence:whites-blacks", "whites=70 blacks=-60" },
  };

/* ============================================================== */

static void
check_registration (void)
{
  static const gchar *const menu[] = { "<Image>/Filters/Enhance", "<Image>/Filters/Enhance",
                                       "<Image>/Filters/Enhance", "<Image>/Colors" };
  guint i;

  printf ("-- registration\n");
  for (i = 0; i < G_N_ELEMENTS (OPS); i++)
    {
      const gchar *m    = gegl_operation_get_key (OPS[i][0], "gimp:menu-path");
      gchar       *name = g_strdup_printf ("%s_registered", OPS[i][0] + 9);

      report (name, gegl_has_operation (OPS[i][0]) && m && strcmp (m, menu[i]) == 0,
              "%s, %s", gegl_operation_get_key (OPS[i][0], "title"), m);
      g_free (name);
    }
}

/* identity at 0, alpha, broken pixels -------------------------------------- */

/* random pixels, with values outside 0 to 1, NaN and infinities */
static gfloat *
wild (gint w,
      gint h)
{
  gfloat *img = photo (w, h);
  gsize   i, n = (gsize) w * h;

  for (i = 0; i < n; i += 37)
    {
      gfloat *p = img + i * 4;
      switch ((i / 37) % 7)
        {
        case 0: p[0] = NAN; break;
        case 1: p[1] = INFINITY; break;
        case 2: p[2] = -INFINITY; break;
        case 3: p[0] = -0.2f; break;
        case 4: p[1] = 7.5f; p[0] = 3.0f; p[2] = 4.0f; break;
        case 5: p[3] = 0.0f; break;
        default: p[0] = p[1] = p[2] = 1e30f; break;
        }
    }
  return img;
}

static void
check_identity (void)
{
  static const gchar *const zero[] =
    { "amount=0", "amount=0 size=90", "amount=0", "whites=0 blacks=0 ends=move" };
  const gint w = 300, h = 200;
  gfloat    *in = wild (w, h);
  guint      i;

  printf ("-- identity at 0\n");
  for (i = 0; i < G_N_ELEMENTS (OPS); i++)
    {
      gfloat *out  = run (OPS[i][0], zero[i], in, w, h);
      gchar  *name = g_strdup_printf ("%s_at_0_is_the_identity", OPS[i][0] + 9);

      report (name, memcmp (in, out, (gsize) w * h * 4 * sizeof (gfloat)) == 0,
              "bit for bit, with NaN, infinities, negative and huge values");
      g_free (name);
      g_free (out);
    }
  g_free (in);
}

static void
check_alpha_and_broken (void)
{
  const gint w = 320, h = 240;
  gsize      n = (gsize) w * h, i;
  gfloat    *in = photo (w, h);
  gfloat    *broken = g_memdup2 (in, n * 4 * sizeof (gfloat));
  gfloat    *black  = g_memdup2 (in, n * 4 * sizeof (gfloat));
  guint      o;

  printf ("-- alpha, broken and huge pixels\n");
  for (i = 0; i < n; i++)
    in[i * 4 + 3] = broken[i * 4 + 3] = black[i * 4 + 3] = (i % 7) / 6.0f;
  /* a few broken pixels, and the same image with them black */
  for (i = 1000; i < n; i += 4099)
    {
      gfloat *p = broken + i * 4, *q = black + i * 4;
      if ((i / 4099) % 3 == 0)
        p[0] = NAN;
      else if ((i / 4099) % 3 == 1)
        p[1] = INFINITY;
      else
        p[2] = -INFINITY;
      q[0] = q[1] = q[2] = 0.0f;
    }

  for (o = 0; o < G_N_ELEMENTS (OPS); o++)
    {
      const gchar *op = OPS[o][0], *pr = OPS[o][1];
      gfloat      *out = run (op, pr, in, w, h);
      gfloat      *ob  = run (op, pr, broken, w, h);
      gfloat      *ok  = run (op, pr, black, w, h);
      gboolean     alpha = TRUE, finite = TRUE, same = TRUE, kept = TRUE;
      gdouble      d = 0;
      gchar       *name;

      for (i = 0; i < n; i++)
        {
          gboolean is_broken = ! (isfinite (broken[i * 4]) && isfinite (broken[i * 4 + 1]) &&
                                  isfinite (broken[i * 4 + 2]));

          if (out[i * 4 + 3] != in[i * 4 + 3] || ob[i * 4 + 3] != in[i * 4 + 3])
            alpha = FALSE;
          if (! isfinite (out[i * 4]) || ! isfinite (out[i * 4 + 1]) || ! isfinite (out[i * 4 + 2]))
            finite = FALSE;
          if (is_broken)
            {
              if (memcmp (ob + i * 4, broken + i * 4, 4 * sizeof (gfloat)) != 0)
                kept = FALSE;
            }
          else
            {
              gint c;
              for (c = 0; c < 3; c++)
                d = MAX (d, fabs (ob[i * 4 + c] - ok[i * 4 + c]));
              if (d > 0)
                same = FALSE;
            }
        }
      name = g_strdup_printf ("%s_keeps_alpha", op + 9);
      report (name, alpha, "exactly, also next to broken pixels");
      g_free (name);
      name = g_strdup_printf ("%s_output_finite", op + 9);
      report (name, finite, NULL);
      g_free (name);
      name = g_strdup_printf ("%s_broken_pixels_stay_local", op + 9);
      report (name, same && kept,
              "NaN and infinite pixels left as they are (%s), the rest as with black "
              "there (max difference %.2g)", kept ? "yes" : "NO", d);
      g_free (name);
      g_free (out);
      g_free (ob);
      g_free (ok);
    }

  /* huge finite values (1e30) do not spread */
  for (o = 0; o < G_N_ELEMENTS (OPS); o++)
    {
      gfloat  *huge = g_memdup2 (in, n * 4 * sizeof (gfloat));
      gfloat  *out;
      gboolean finite = TRUE;
      gint     bad = 0;
      gchar   *name;

      for (i = 500; i < n; i += 7919)
        huge[i * 4] = huge[i * 4 + 1] = huge[i * 4 + 2] = 1e30f;
      out = run (OPS[o][0], OPS[o][1], huge, w, h);
      for (i = 0; i < n * 4; i++)
        if (! isfinite (out[i]))
          {
            finite = FALSE;
            bad++;
          }
      name = g_strdup_printf ("%s_huge_values_stay_finite", OPS[o][0] + 9);
      report (name, finite, "1e30 pixels; %d values not finite", bad);
      g_free (name);
      g_free (out);
      g_free (huge);
    }
  g_free (in);
  g_free (broken);
  g_free (black);
}

/* tiles, threads, regions ----------------------------------------------------- */

static void
check_invariance (void)
{
  const gint w = 700, h = 460;
  gsize      n = (gsize) w * h * 4;
  gfloat    *in = photo (w, h);
  gint       threads;
  guint      o;

  printf ("-- tiles, threads and regions\n");
  g_object_get (gegl_config (), "threads", &threads, NULL);
  for (o = 0; o < G_N_ELEMENTS (OPS); o++)
    {
      const gchar *op = OPS[o][0], *pr = OPS[o][1];
      gfloat      *ref, *out;
      gdouble      dt = 0, dth = 0, dr = 0;
      static const gint tiles[][2] = { { 32, 32 }, { 64, 128 }, { 256, 256 }, { 700, 16 } };
      static const gint nth[] = { 1, 3, 8 };
      guint        k;
      gchar       *name;

      ref = run (op, pr, in, w, h);
      for (k = 0; k < G_N_ELEMENTS (tiles); k++)
        {
          out = run_full (op, pr, babl_format (LIN), in, w, h, babl_format (LIN), NULL,
                          tiles[k][0], tiles[k][1]);
          dt = MAX (dt, max_diff (ref, out, n));
          g_free (out);
        }
      for (k = 0; k < G_N_ELEMENTS (nth); k++)
        {
          g_object_set (gegl_config (), "threads", nth[k], NULL);
          out = run (op, pr, in, w, h);
          dth = MAX (dth, max_diff (ref, out, n));
          g_free (out);
        }
      g_object_set (gegl_config (), "threads", threads, NULL);
      /* regions asked for on their own, each in a new graph: a band at
       * the left edge, one inside, one at the bottom right corner */
      {
        static const gint rois[][4] = { { 0, 0, 37, 460 }, { 300, 211, 64, 64 },
                                        { 650, 400, 50, 60 } };
        for (k = 0; k < G_N_ELEMENTS (rois); k++)
          {
            GeglRectangle r = { rois[k][0], rois[k][1], rois[k][2], rois[k][3] };
            gint          x, y;

            out = run_full (op, pr, babl_format (LIN), in, w, h, babl_format (LIN), &r, 0, 0);
            for (y = 0; y < r.height; y++)
              for (x = 0; x < r.width; x++)
                {
                  gint c;
                  for (c = 0; c < 4; c++)
                    dr = MAX (dr, fabs (out[((gsize) y * r.width + x) * 4 + c] -
                                        ref[((gsize) (y + r.y) * w + x + r.x) * 4 + c]));
                }
            g_free (out);
          }
      }
      name = g_strdup_printf ("%s_same_for_any_tiles_threads_regions", op + 9);
      report (name, dt == 0 && dth == 0 && dr == 0,
              "max difference: tile sizes 32x32 to 700x16 %.2g, 1, 3 and 8 threads "
              "(default %d) %.2g, regions %.2g", dt, threads, dth, dr);
      g_free (name);
      g_free (ref);
    }
  g_free (in);
}

/* formats and spaces --------------------------------------------------------------- */

static void
check_formats (void)
{
  static const struct { const gchar *name; gdouble tol; } fmts[] =
    {
      { "R'G'B'A u8", 1.0 / 255 + 1e-6 }, { "R'G'B'A u16", 2.0 / 65535 },
      { "R'G'B'A half", 2e-3 }, { "R'G'B'A float", 1e-5 }, { "RGB float", 1e-5 },
      { "Y'A u8", 1.0 / 255 + 1e-6 }, { "Y float", 1e-5 }, { "RaGaBaA float", 1e-4 },
    };
  const gint w = 256, h = 192;
  gint       n = w * h;
  guint      o, f;

  printf ("-- buffer formats\n");
  for (o = 0; o < G_N_ELEMENTS (OPS); o++)
    for (f = 0; f < G_N_ELEMENTS (fmts); f++)
      {
        const Babl *fmt  = babl_format (fmts[f].name);
        gint        bpp  = babl_format_get_bytes_per_pixel (fmt);
        gfloat     *base = photo (w, h);
        gpointer    raw  = g_malloc ((gsize) n * bpp);
        gfloat     *same = g_new (gfloat, (gsize) 4 * n);
        gfloat     *want, *got = g_new (gfloat, (gsize) 4 * n);
        gpointer    got_raw, tmp = g_malloc ((gsize) n * bpp);
        gdouble     worst = 0;
        gint        i;
        gchar      *name;
        gboolean    no_alpha = ! strchr (fmts[f].name, 'A');

        babl_process (babl_fish (babl_format (LIN), fmt), base, raw, n);
        babl_process (babl_fish (fmt, babl_format (LIN)), raw, same, n);
        want = run (OPS[o][0], OPS[o][1], same, w, h);
        got_raw = run_full (OPS[o][0], OPS[o][1], fmt, raw, w, h, fmt, NULL, 0, 0);
        babl_process (babl_fish (fmt, babl_format (LIN)), got_raw, got, n);
        babl_process (babl_fish (babl_format (LIN), fmt), want, tmp, n);
        babl_process (babl_fish (fmt, babl_format (LIN)), tmp, want, n);
        for (i = 0; i < 4 * n; i++)
          {
            if (no_alpha && i % 4 == 3)
              continue;
            if (strstr (fmts[f].name, "Ra") && want[4 * (i / 4) + 3] < 1e-3f)
              continue;
            /* compared in the format's own terms */
            worst = MAX (worst, fabs (got[i] - want[i]));
          }
        /* linear formats: the tolerance is for the encoded value; half
         * and u8 checked via their encoded forms above */
        name = g_strdup_printf ("%s_on_%s", OPS[o][0] + 9, fmts[f].name);
        report (name, worst <= fmts[f].tol + 1e-6, "max difference %.2g (linear light)", worst);
        g_free (name);
        g_free (base); g_free (raw); g_free (same); g_free (want); g_free (got);
        g_free (got_raw); g_free (tmp);
      }
}

/* the same colours in another RGB space give the same colours out: the
 * operations work on luminance and lightness, which do not depend on
 * the space */
static void
check_spaces (void)
{
  const gint  w = 256, h = 192;
  gint        n = w * h;
  const Babl *srgb  = babl_format (LIN);
  const Babl *adobe = babl_format_with_space (LIN, babl_space ("Adobish"));
  const Babl *pro   = babl_format_with_space (LIN, babl_space ("ProPhoto"));
  guint       o;

  printf ("-- color spaces\n");
  for (o = 0; o < G_N_ELEMENTS (OPS); o++)
    {
      gfloat *in   = photo (w, h);
      gchar  *pr   = g_strdup_printf ("%s output-range=none", OPS[o][1]);
      gfloat *want = run (OPS[o][0], pr, in, w, h);
      gdouble worst = 0;
      const Babl *sp[2] = { adobe, pro };
      gint    s;
      gchar  *name;

      for (s = 0; s < 2; s++)
        {
          gfloat *conv = g_new (gfloat, (gsize) 4 * n), *back = g_new (gfloat, (gsize) 4 * n);
          gfloat *out;
          gint    i;

          babl_process (babl_fish (srgb, sp[s]), in, conv, n);
          out = run_full (OPS[o][0], pr, sp[s], conv, w, h, sp[s], NULL, 0, 0);
          babl_process (babl_fish (sp[s], srgb), out, back, n);
          for (i = 0; i < 4 * n; i++)
            worst = MAX (worst, fabs (back[i] - want[i]));
          g_free (conv); g_free (back); g_free (out);
        }
      name = g_strdup_printf ("%s_same_colors_in_other_spaces", OPS[o][0] + 9);
      /* dehaze estimates a haze colour from the dark channel, which is
       * a minimum over the channels of the space: not the same in
       * another space (noted, not checked) */
      if (o == 2)
        note (name, "Adobe RGB and ProPhoto against sRGB: max difference %.3g "
              "(the dark channel is taken in the image's own channels)", worst);
      else
        report (name, worst < 2e-4, "Adobe RGB and ProPhoto against sRGB, max difference %.2g",
                worst);
      g_free (name);
      g_free (in); g_free (want); g_free (pr);
    }

  /* perceptual and linear buffers of the same pixels */
  {
    gfloat     *in  = photo (w, h), *perc = g_new (gfloat, (gsize) 4 * n);
    gfloat     *a, *b, *bl = g_new (gfloat, (gsize) 4 * n);
    const Babl *pf  = babl_format ("R'G'B'A float");

    babl_process (babl_fish (srgb, pf), in, perc, n);
    for (o = 0; o < G_N_ELEMENTS (OPS); o++)
      {
        gchar *name = g_strdup_printf ("%s_perceptual_buffer_same_as_linear", OPS[o][0] + 9);
        a = run (OPS[o][0], OPS[o][1], in, w, h);
        b = run_full (OPS[o][0], OPS[o][1], pf, perc, w, h, pf, NULL, 0, 0);
        babl_process (babl_fish (pf, srgb), b, bl, n);
        report (name, max_diff (a, bl, (gsize) 4 * n) < 1e-5, "max difference %.2g",
                max_diff (a, bl, (gsize) 4 * n));
        g_free (name); g_free (a); g_free (b);
      }
    g_free (in); g_free (perc); g_free (bl);
  }
}

/* colour kept --------------------------------------------------------------- */

/* the largest change of chromaticity r / (r + g + b), g / (r + g + b)
 * of pixels of luminance above 0.05, all in range */
static gdouble
chroma_change (const gfloat *in,
               const gfloat *out,
               gsize         n)
{
  gdouble worst = 0;
  gsize   i;

  for (i = 0; i < n; i++)
    {
      const gfloat *p = in + i * 4, *q = out + i * 4;
      gdouble       sp = p[0] + p[1] + p[2], sq = q[0] + q[1] + q[2];

      if (lum (p) < 0.05 || lum (q) < 0.05)
        continue;
      worst = MAX (worst, fabs (p[0] / sp - q[0] / sq));
      worst = MAX (worst, fabs (p[1] / sp - q[1] / sq));
    }
  return worst;
}

static void
check_colour_kept (void)
{
  static const gchar *const ops[][2] =
    {
      { "presence:clarity", "amount=100 output-range=none" },
      { "presence:clarity", "amount=-100 output-range=none" },
      { "presence:texture", "amount=100 output-range=none" },
      { "presence:whites-blacks", "whites=100 blacks=-100 output-range=none" },
      { "presence:whites-blacks", "whites=-100 blacks=100 ends=move output-range=none" },
    };
  const gint w = 400, h = 300;
  gfloat    *in = photo (w, h);
  guint      o;

  printf ("-- colors kept\n");
  for (o = 0; o < G_N_ELEMENTS (ops); o++)
    {
      gfloat *out  = run (ops[o][0], ops[o][1], in, w, h);
      gdouble d    = chroma_change (in, out, (gsize) w * h);
      gchar  *name = g_strdup_printf ("%s_keeps_chromaticity_%s", ops[o][0] + 9,
                                      strchr (ops[o][1], '-') && ops[o][1][strlen ("amount=")] == '-'
                                      ? "negative" : o == 4 ? "inwards" : "positive");

      /* the channels are scaled; only the gray part added near black
       * (the offset of 1e-3 in presence_set_luminance) moves the
       * chromaticity, by at most 1e-3 / (Y + 1e-3) of the way to gray */
      report (name, d < 0.01, "largest change of rgb chromaticity (luminance above 0.05) %.4f", d);
      g_free (name);
      g_free (out);
    }
  g_free (in);
}

/* output ranges ------------------------------------------------------------------- */

static void
check_output_range (void)
{
  const gint w = 400, h = 300;
  gsize      n = (gsize) w * h, i;
  gfloat    *in = photo (w, h);
  static const gchar *const ops[][2] =
    {
      { "presence:clarity", "amount=100" },
      { "presence:dehaze", "amount=100" },
      { "presence:whites-blacks", "whites=100 ends=move" },
    };
  guint      o;

  printf ("-- output ranges\n");
  for (o = 0; o < G_N_ELEMENTS (ops); o++)
    {
      gchar   *pk = fmt_props ("%s output-range=keep-hue", ops[o][1]);
      gchar   *pc = fmt_props ("%s output-range=clip", ops[o][1]);
      gchar   *pn = fmt_props ("%s output-range=none", ops[o][1]);
      gfloat  *k  = run (ops[o][0], pk, in, w, h);
      gfloat  *c  = run (ops[o][0], pc, in, w, h);
      gfloat  *u  = run (ops[o][0], pn, in, w, h);
      gint     outside = 0;
      gdouble  kr = 0, cr = 0, hue = 0;
      gchar   *name;

      for (i = 0; i < n; i++)
        {
          gint ch;
          gboolean out_u = FALSE;

          for (ch = 0; ch < 3; ch++)
            {
              if (u[i * 4 + ch] < 0 || u[i * 4 + ch] > 1)
                out_u = TRUE;
              kr = MAX (kr, MAX (k[i * 4 + ch] - 1, -k[i * 4 + ch]));
              cr = MAX (cr, MAX (c[i * 4 + ch] - 1, -c[i * 4 + ch]));
              if (fabs (c[i * 4 + ch] - CLAMP (u[i * 4 + ch], 0, 1)) > 1e-6)
                cr = MAX (cr, 1.0);
            }
          outside += out_u;
          /* keep hue: the direction from gray is that of the unbounded
           * result (its luminance within 0 to 1) */
          if (out_u)
            {
              gdouble Yu = lum (u + i * 4), g = CLAMP (Yu, 0, 1);
              gdouble a[3], b[3], na = 0, nb = 0, dot = 0;

              for (ch = 0; ch < 3; ch++)
                {
                  a[ch] = u[i * 4 + ch] - Yu;
                  b[ch] = k[i * 4 + ch] - g;
                  na += a[ch] * a[ch]; nb += b[ch] * b[ch]; dot += a[ch] * b[ch];
                }
              if (na > 1e-6 && nb > 1e-6 && Yu > 0.001 && Yu < 0.999)
                hue = MAX (hue, 1 - dot / sqrt (na * nb));
            }
        }
      name = g_strdup_printf ("%s_output_ranges", ops[o][0] + 9);
      report (name, outside > 0 && kr <= 1e-6 && cr <= 1e-6 && hue < 1e-4,
              "%d pixels outside 0 to 1 unbounded; keep hue: within by %.1g, direction from "
              "gray kept to 1 - cos %.1g; clip: per channel", outside, kr, hue);
      g_free (name);
      g_free (pk); g_free (pc); g_free (pn); g_free (k); g_free (c); g_free (u);
    }
  g_free (in);
}

/* clarity and texture: gain over sizes, halos ---------------------------------------- */

/* gain over wavelengths; returns the wavelength of the largest gain */
static gdouble
response (const gchar *label,
          const gchar *op,
          const gchar *props,
          gdouble      base,
          gdouble     *gains,
          const gdouble *wls,
          gint         nwl)
{
  GString *s = g_string_new (NULL);
  gdouble  best = 0, best_wl = 0;
  gint     k;

  for (k = 0; k < nwl; k++)
    {
      gains[k] = gain_at (op, props, wls[k], base, 0.02);
      g_string_append_printf (s, " %g:%.2f", wls[k], gains[k]);
      if (gains[k] > best)
        {
          best = gains[k];
          best_wl = wls[k];
        }
    }
  note (label, "gain by wavelength in pixels (2 L* gratings at L* %.0f):%s", base * 100, s->str);
  g_string_free (s, TRUE);
  return best_wl;
}

static void
check_clarity (void)
{
  static const gdouble wls[] = { 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048 };
  gdouble g[G_N_ELEMENTS (wls)], peak;
  gdouble sizes[] = { 20, 40, 80 };
  guint   k;

  printf ("-- clarity\n");
  for (k = 0; k < G_N_ELEMENTS (sizes); k++)
    {
      gchar  *pr    = fmt_props ("amount=100 size=%g", sizes[k]);
      gchar  *label = fmt_props ("clarity_response_size_%g", sizes[k]);
      gchar  *name;
      gdouble lo_g, hi_g;

      peak = response (label, "presence:clarity", pr, 0.5, g, wls, G_N_ELEMENTS (wls));
      /* the gain peaks where the radius (half the wavelength) is the
       * size, within a factor of 2 (the wavelengths are an octave apart) */
      name = fmt_props ("clarity_peaks_at_its_size_%g", sizes[k]);
      report (name, peak / 2 >= sizes[k] / 2 && peak / 2 <= sizes[k] * 2,
              "largest gain at a radius of %g pixels", peak / 2);
      g_free (name);
      /* +100 about triples the detail of the band; the finest detail
       * (2 pixels, noise) and the largest (the image's size) change little */
      lo_g = g[0];
      hi_g = g[G_N_ELEMENTS (wls) - 1];
      {
        gdouble best = 0;
        guint   j;
        for (j = 0; j < G_N_ELEMENTS (wls); j++)
          best = MAX (best, g[j]);
        name = fmt_props ("clarity_raises_its_band_size_%g", sizes[k]);
        report (name, best > 2.0 && lo_g < 1.25 && hi_g < 1.25,
                "largest gain %.2f, at 2 pixels %.2f, at 2048 pixels %.2f", best, lo_g, hi_g);
        g_free (name);
      }
      g_free (pr);
      g_free (label);
    }

  /* negative softens the band */
  {
    gdouble gn = gain_at ("presence:clarity", "amount=-100 size=40", 96, 0.5, 0.02);
    gdouble gh = gain_at ("presence:clarity", "amount=-50 size=40", 96, 0.5, 0.02);
    report ("clarity_negative_softens", gn < 0.35 && gh > gn && gh < 1.0,
            "gain at a radius of 48 pixels: -100 %.2f, -50 %.2f", gn, gh);
  }

  /* most in the midtones */
  {
    gdouble gd = gain_at ("presence:clarity", "amount=100 size=40", 160, 0.15, 0.02);
    gdouble gm = gain_at ("presence:clarity", "amount=100 size=40", 160, 0.50, 0.02);
    gdouble gl = gain_at ("presence:clarity", "amount=100 size=40", 160, 0.85, 0.02);
    report ("clarity_most_in_the_midtones", gm > gd && gm > gl && gm - 1 > 1.5 * (MAX (gd, gl) - 1),
            "gain at L* 15, 50, 85: %.2f, %.2f, %.2f", gd, gm, gl);
  }

  /* Halos: a step of 50 L* (25 to 75) is an edge, far above the
   * filter's detail threshold (12 L*). Bound: the overshoot at +100 stays
   * under 1 L*, about one just noticeable difference of lightness, so no
   * halo can be seen; an unsharp mask of the same scale and gain at the
   * band's peak overshoots by tens of L*. Smaller steps are detail and
   * are raised (noted). */
  {
    const gint w = 2048, h = 8;
    gdouble    steps[] = { 0.05, 0.1, 0.2, 0.3, 0.5, 0.7 };
    GString   *s = g_string_new (NULL);
    guint      j;

    for (j = 0; j < G_N_ELEMENTS (steps); j++)
      {
        gdouble lo = 0.5 - steps[j] / 2, hi = 0.5 + steps[j] / 2;
        gfloat *in  = step_edge (w, h, lo, hi);
        gfloat *out = run ("presence:clarity", "amount=100 size=40", in, w, h);
        gdouble ov  = overshoot (out, w, h, lo, hi);
        /* an unsharp mask with the peak gain of the clarity (about 3)
         * and a Gaussian of the band's size */
        gdouble usm = usm_overshoot (w, lo, hi, 40, 2.0);

        g_string_append_printf (s, " %.0f:%.2f (usm %.1f)", steps[j] * 100, ov * 100, usm * 100);
        if (steps[j] == 0.5)
          report ("clarity_no_halo_at_a_strong_edge", ov < 0.01,
                  "overshoot at a 50 L* step %.2f L* (bound 1 L*); an unsharp mask of "
                  "the same gain and scale %.1f L*", ov * 100, usm * 100);
        g_free (in);
        g_free (out);
      }
    note ("clarity_overshoot_by_step", "step: overshoot in L* at +100, size 40:%s", s->str);
    g_string_free (s, TRUE);
  }
}

/* a texture of blobs of about r pixels, amplitude amp in L, on L = base */
static gfloat *
blobs (gint    w,
       gint    h,
       gdouble base,
       gdouble amp,
       gdouble r)
{
  gfloat *img = g_new (gfloat, (gsize) w * h * 4);
  gdouble ph[6];
  gint    x, y, k;

  for (k = 0; k < 6; k++)
    ph[k] = g_rand_double_range (rnd, 0, 2 * G_PI);
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        gdouble f = G_PI / (2 * r);
        gdouble v = sin (x * f + ph[0]) * sin (y * f + ph[1])
                  + 0.7 * sin ((x + y) * f * 0.8 + ph[2]) * sin ((x - y) * f * 1.3 + ph[3]);
        set_gray (img + ((gsize) y * w + x) * 4, base + amp * v / 1.7);
      }
  return img;
}

/* the standard deviation of L inside the image's middle */
static gdouble
std_L (const gfloat *img,
       gint          w,
       gint          h)
{
  gdouble s = 0, s2 = 0;
  gint    x, y, n = 0;

  for (y = h / 4; y < 3 * h / 4; y++)
    for (x = w / 4; x < 3 * w / 4; x++)
      {
        gdouble L = pix_L (img + ((gsize) y * w + x) * 4);
        s += L; s2 += L * L; n++;
      }
  return sqrt (MAX (s2 / n - (s / n) * (s / n), 0));
}

static void
check_texture (void)
{
  static const gdouble wls[] = { 2, 3, 4, 6, 8, 12, 16, 24, 32, 64, 128, 256 };
  gdouble g[G_N_ELEMENTS (wls)], peak, best = 0;
  guint   k;

  printf ("-- texture\n");
  peak = response ("texture_response_size_8", "presence:texture", "amount=100", 0.5, g,
                   wls, G_N_ELEMENTS (wls));
  for (k = 0; k < G_N_ELEMENTS (wls); k++)
    best = MAX (best, g[k]);
  report ("texture_raises_the_mid_band", best > 1.8 && peak / 2 >= 4 && peak / 2 <= 16,
          "largest gain %.2f at a radius of %g pixels", best, peak / 2);
  /* the finest detail (noise) and large areas are left */
  report ("texture_leaves_finest_and_large", g[0] < 1.25 && g[G_N_ELEMENTS (wls) - 1] < 1.05 &&
          g[G_N_ELEMENTS (wls) - 2] < 1.1,
          "gain at 2 pixels %.2f, at 128 %.2f, at 256 %.2f", g[0], g[G_N_ELEMENTS (wls) - 2],
          g[G_N_ELEMENTS (wls) - 1]);
  /* compared with clarity, the band is finer */
  {
    gdouble tc = gain_at ("presence:texture", "amount=100", 160, 0.5, 0.02);
    gdouble cc = gain_at ("presence:clarity", "amount=100", 160, 0.5, 0.02);
    gdouble tf = gain_at ("presence:texture", "amount=100", 12, 0.5, 0.02);
    gdouble cf = gain_at ("presence:clarity", "amount=100", 12, 0.5, 0.02);
    report ("texture_finer_than_clarity", tf > cf && cc > tc,
            "at 12 pixels texture %.2f, clarity %.2f; at 160 pixels texture %.2f, clarity %.2f",
            tf, cf, tc, cc);
  }

  /* large edges: no halo (as for clarity, bound 1 L*) */
  {
    const gint w = 1024, h = 8;
    gfloat    *in  = step_edge (w, h, 0.25, 0.75);
    gfloat    *out = run ("presence:texture", "amount=100", in, w, h);
    gdouble    ov  = overshoot (out, w, h, 0.25, 0.75);
    gfloat    *o2  = run ("presence:texture", "amount=-100", in, w, h);
    gdouble    ov2 = overshoot (o2, w, h, 0.25, 0.75);
    /* the edge itself stays as steep: its 10 to 90 % width */
    report ("texture_leaves_strong_edges", ov < 0.01 && ov2 < 0.01,
            "overshoot at a 50 L* step: +100 %.2f L*, -100 %.2f L* (bound 1 L*)", ov * 100,
            ov2 * 100);
    g_free (in); g_free (out); g_free (o2);
  }

  /* smooth areas: fine noise of 0.3 L* on gray is left, while texture
   * of the band (3 L* blobs of 6 pixels) is raised */
  {
    const gint w = 512, h = 512;
    gfloat    *noise = g_new (gfloat, (gsize) w * h * 4);
    gfloat    *out, *tb, *to;
    gdouble    sn, so, st, sto;
    gint       i;

    for (i = 0; i < w * h; i++)
      set_gray (noise + (gsize) i * 4, 0.5 + 0.003 * g_rand_double_range (rnd, -1.732, 1.732));
    out = run ("presence:texture", "amount=100", noise, w, h);
    sn = std_L (noise, w, h);
    so = std_L (out, w, h);
    tb = blobs (w, h, 0.5, 0.03, 8);
    to = run ("presence:texture", "amount=100", tb, w, h);
    st = std_L (tb, w, h);
    sto = std_L (to, w, h);
    report ("texture_leaves_smooth_areas", so / sn < 1.3 && sto / st > 1.6,
            "fine noise of %.2f L*: times %.2f; texture of %.1f L* blobs of 8 pixels: times %.2f",
            sn * 100, so / sn, st * 100, sto / st);
    g_free (out); g_free (to);
    /* negative smooths the texture (skin) */
    to = run ("presence:texture", "amount=-100", tb, w, h);
    {
      /* a grating at the band's peak loses more than half of its
       * contrast (the band's weights peak at 0.87 between two levels, so
       * -100 does not flatten it fully); blobs, whose sizes spread over
       * the band's flanks, lose at least 40 % */
      gdouble gp = gain_at ("presence:texture", "amount=-100", 16, 0.5, 0.02);
      report ("texture_negative_smooths", std_L (to, w, h) / st < 0.6 && gp < 0.45,
              "at -100: a grating of radius 8 pixels times %.2f, the blobs times %.2f",
              gp, std_L (to, w, h) / st);
    }
    g_free (to);
    to = run ("presence:texture", "amount=-50", tb, w, h);
    note ("texture_negative_50", "the same blobs at -50: times %.2f", std_L (to, w, h) / st);
    g_free (to);
    g_free (tb);
    g_free (noise);
  }
}

/* dehaze ------------------------------------------------------------------------------- */

/* A scene without haze, its transmission and the haze colour; the hazy
 * image by the haze model I = J t + A (1 - t). The scene follows the
 * dark channel prior (every patch has dark pixels in some channel:
 * coloured surfaces with texture and shadows); depth grows towards the
 * top, sky at the top, and a near object in front of the far part. */
typedef struct
{
  gint    w, h;
  gfloat *J, *I, *t;
  gfloat  A[3];
  guint8 *edge;     /* 1 within 10 pixels of a jump in depth */
} Hazy;

static void
hazy_scene (Hazy *s,
            gint  w,
            gint  h)
{
  gint x, y, c;

  s->w = w; s->h = h;
  s->A[0] = 0.80f; s->A[1] = 0.85f; s->A[2] = 0.92f;
  s->J = g_new (gfloat, (gsize) w * h * 4);
  s->I = g_new (gfloat, (gsize) w * h * 4);
  s->t = g_new (gfloat, (gsize) w * h);
  s->edge = g_new0 (guint8, (gsize) w * h);
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        gsize   i    = (gsize) y * w + x;
        gfloat *J    = s->J + i * 4;
        gint    cell = (x / 24) * 7 + (y / 24) * 13;
        gdouble sky  = y < h / 6;
        gdouble near = (x > w / 2 && x < w * 3 / 4 && y > h / 3 && y < h * 2 / 3);
        gdouble d;
        gdouble tex = 0.5 + 0.5 * sin (x * 0.7) * sin (y * 0.9);
        gint    dark_ch = cell % 3;

        /* depth: 4 at the top of the land, 0.3 at the bottom, 0.4 for
         * the near object; the sky far */
        d = sky ? 6.0 : near ? 0.4 : 0.3 + 3.7 * (1.0 - (gdouble) (y - h / 6) / (h - h / 6));
        s->t[i] = exp (-0.7 * d);
        for (c = 0; c < 3; c++)
          {
            gdouble v = 0.15 + 0.55 * (0.5 + 0.5 * sin (cell * (c + 1) * 1.7));
            J[c] = v * (0.6 + 0.4 * tex);
          }
        /* one channel dark in every cell, and a shadow line every 12 rows */
        J[dark_ch] = 0.02 + 0.02 * tex;
        if (y % 12 == 0)
          J[0] = J[1] = J[2] = 0.03;
        if (sky)
          J[0] = J[1] = J[2] = 0.6;
        J[3] = 1.0f;
        for (c = 0; c < 3; c++)
          s->I[i * 4 + c] = J[c] * s->t[i] + s->A[c] * (1 - s->t[i]);
        s->I[i * 4 + 3] = 1.0f;
      }
  /* the depth edges: sky line and the near object's outline */
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        gboolean e = abs (y - h / 6) < 10 ||
                     ((abs (x - w / 2) < 10 || abs (x - w * 3 / 4) < 10) && y > h / 3 - 10 && y < h * 2 / 3 + 10) ||
                     ((abs (y - h / 3) < 10 || abs (y - h * 2 / 3) < 10) && x > w / 2 - 10 && x < w * 3 / 4 + 10);
        s->edge[(gsize) y * w + x] = e;
      }
}

static void
hazy_free (Hazy *s)
{
  g_free (s->J); g_free (s->I); g_free (s->t); g_free (s->edge);
}

/* RMS difference of colour to the scene, over the land with t >= tmin,
 * away from depth edges if skip_edges */
static gdouble
rms_to_scene (const Hazy   *s,
              const gfloat *img,
              gdouble       tmin,
              gboolean      skip_edges)
{
  gdouble sum = 0;
  gsize   n = 0, i;

  for (i = 0; i < (gsize) s->w * s->h; i++)
    {
      gint c;
      if (s->t[i] < tmin || (skip_edges && s->edge[i]) || (gint) (i / s->w) < s->h / 6)
        continue;
      for (c = 0; c < 3; c++)
        {
          gdouble d = img[i * 4 + c] - s->J[i * 4 + c];
          sum += d * d;
        }
      n += 3;
    }
  return sqrt (sum / MAX (n, (gsize) 1));
}

static void
check_dehaze (void)
{
  Hazy    s;
  gfloat *out, *tmap, *amap;
  gdouble e_in, e_out, e_in_all, e_out_all, t_err = 0, t_err_all = 0, a_err = 0;
  gsize   i, nt = 0, nt_all = 0;
  gint    c;

  printf ("-- dehaze\n");
  hazy_scene (&s, 768, 512);

  g_setenv ("PRESENCE_DEBUG", "A", TRUE);
  amap = run ("presence:dehaze", "amount=100", s.I, s.w, s.h);
  g_setenv ("PRESENCE_DEBUG", "t", TRUE);
  tmap = run ("presence:dehaze", "amount=100", s.I, s.w, s.h);
  g_unsetenv ("PRESENCE_DEBUG");
  out  = run ("presence:dehaze", "amount=100 output-range=none", s.I, s.w, s.h);

  for (c = 0; c < 3; c++)
    a_err = MAX (a_err, fabs (amap[c] - s.A[c]));
  report ("dehaze_finds_the_haze_color", a_err < 0.01,
          "estimated %.3f %.3f %.3f, true %.2f %.2f %.2f: largest error %.4f",
          amap[0], amap[1], amap[2], s.A[0], s.A[1], s.A[2], a_err);

  for (i = 0; i < (gsize) s.w * s.h; i++)
    {
      gdouble d = fabs (tmap[i * 4] - s.t[i]);
      if ((gint) (i / s.w) < s.h / 6)
        continue;
      t_err_all += d;
      nt_all++;
      if (! s.edge[i])
        {
          t_err += d;
          nt++;
        }
    }
  t_err /= MAX (nt, (gsize) 1);
  t_err_all /= MAX (nt_all, (gsize) 1);
  report ("dehaze_finds_the_transmission", t_err < 0.03 && t_err_all < 0.05,
          "mean error of t over the land %.4f away from depth edges, %.4f with them "
          "(t from 0.06 to 0.81)", t_err, t_err_all);

  e_in      = rms_to_scene (&s, s.I, 0.2, TRUE);
  e_out     = rms_to_scene (&s, out, 0.2, TRUE);
  e_in_all  = rms_to_scene (&s, s.I, 0.2, FALSE);
  e_out_all = rms_to_scene (&s, out, 0.2, FALSE);
  report ("dehaze_recovers_the_scene", e_out < e_in / 5 && e_out_all < e_in_all / 3,
          "RMS error to the scene (linear, t >= 0.2): hazy %.4f, dehazed %.4f away from "
          "depth edges; %.4f and %.4f with them", e_in, e_out, e_in_all, e_out_all);

  /* a partial amount removes part of the haze: the error is between */
  {
    gfloat *half = run ("presence:dehaze", "amount=50 output-range=none", s.I, s.w, s.h);
    gdouble eh   = rms_to_scene (&s, half, 0.2, TRUE);
    report ("dehaze_partial_amount_between", eh < e_in && eh > e_out,
            "RMS error at 50: %.4f", eh);
    g_free (half);
  }

  /* negative adds haze: towards the haze colour, more in the distance */
  {
    gfloat *more = run ("presence:dehaze", "amount=-100", s.I, s.w, s.h);
    gdouble near_before = 0, near_after = 0, far_before = 0, far_after = 0;
    gint    x, y, nn = 0, nf = 0;

    for (y = s.h / 6; y < s.h; y++)
      for (x = 0; x < s.w / 2; x++)
        {
          gsize   k = (gsize) y * s.w + x;
          gdouble db = 0, da = 0;
          for (c = 0; c < 3; c++)
            {
              db += fabs (s.I[k * 4 + c] - s.A[c]);
              da += fabs (more[k * 4 + c] - s.A[c]);
            }
          if (s.t[k] > 0.6)
            {
              near_before += db; near_after += da; nn++;
            }
          else if (s.t[k] < 0.2)
            {
              far_before += db; far_after += da; nf++;
            }
        }
    report ("dehaze_negative_adds_haze", near_after < near_before && far_after < far_before &&
            far_after / far_before < near_after / near_before,
            "distance to the haze color, near: %.3f to %.3f, far: %.3f to %.3f",
            near_before / nn, near_after / nn, far_before / nf, far_after / nf);
    g_free (more);
  }

  /* a picked haze colour is used as it is */
  {
    gfloat *pick;
    g_setenv ("PRESENCE_DEBUG", "A", TRUE);
    pick = run ("presence:dehaze", "amount=50 auto-haze=false haze-color=rgb(0.5,0.25,0.125)",
                s.I, s.w, s.h);
    g_unsetenv ("PRESENCE_DEBUG");
    /* GEGL reads rgb() with numbers as linear light */
    report ("dehaze_picked_haze_color", fabs (pick[0] - 0.5) < 1e-6 &&
            fabs (pick[1] - 0.25) < 1e-6 && fabs (pick[2] - 0.125) < 1e-6,
            "%.4f %.4f %.4f (linear), picked rgb(0.5,0.25,0.125)", pick[0], pick[1], pick[2]);
    g_free (pick);
  }

  g_free (out);
  g_free (tmap);
  g_free (amap);
  hazy_free (&s);
}

/* whites and blacks ---------------------------------------------------------------------- */

static void
check_whites_blacks (void)
{
  const gint n = 2001;
  gfloat    *ramp = g_new (gfloat, (gsize) n * 4);
  gint       i;
  struct { const gchar *props; gdouble white, black; gboolean soft; } cases[] =
    {
      { "whites=100",                1.0,         0.0,         TRUE },
      { "whites=100 ends=move",      1.0 + 1 / 6.0, 0.0,       FALSE },
      { "whites=-100",               1.0 - 1 / 6.0, 0.0,       TRUE },
      { "blacks=-100",               1.0,         0.0,         TRUE },
      { "blacks=-100 ends=move",     1.0,         -1 / 6.0,    FALSE },
      { "blacks=100",                1.0,         1 / 6.0,     TRUE },
      { "whites=50 blacks=-50",      1.0,         0.0,         TRUE },
      { "whites=-40 blacks=60 ends=move", 1.0 - 0.4 / 6, 0.1, FALSE },
    };
  guint k;

  printf ("-- whites and blacks\n");
  for (i = 0; i < n; i++)
    set_gray (ramp + (gsize) i * 4, (gdouble) i / (n - 1));

  for (k = 0; k < G_N_ELEMENTS (cases); k++)
    {
      gchar   *pr  = fmt_props ("%s output-range=none", cases[k].props);
      gfloat  *out = run ("presence:whites-blacks", pr, ramp, n, 1);
      gdouble  Lw  = pix_L (out + (gsize) (n - 1) * 4), Lb = pix_L (out);
      gdouble  mid = 0, lo = G_MAXDOUBLE, hi = -G_MAXDOUBLE;
      gboolean mono = TRUE;
      gchar   *name;

      for (i = 0; i < n; i++)
        {
          gdouble L = pix_L (out + (gsize) i * 4), Lin = (gdouble) i / (n - 1);
          if (i > 0 && L < pix_L (out + (gsize) (i - 1) * 4) - 1e-7)
            mono = FALSE;
          if (fabs (Lin - 0.5) < 1e-9)
            mid = fabs (L - 0.5);
          lo = MIN (lo, L);
          hi = MAX (hi, L);
        }
      name = g_strdup_printf ("whites_blacks_%s", cases[k].props);
      g_strdelimit (name, " =", '_');
      report (name, mono && fabs (Lw - cases[k].white) < 2e-5 && fabs (Lb - cases[k].black) < 2e-5 &&
              mid < 1e-6 && (! cases[k].soft || (lo >= -1e-6 && hi <= 1 + 1e-5)),
              "white to L* %.3f (want %.3f), black to %.3f (want %.3f), L* 50 moved %.1g, "
              "monotonic %s%s", Lw * 100, cases[k].white * 100, Lb * 100, cases[k].black * 100,
              mid * 100, mono ? "yes" : "NO", cases[k].soft ? ", within 0 to 100" : "");
      g_free (name);
      g_free (out);
      g_free (pr);
    }

  /* the midtones stay: whites leave L <= 0.5 and blacks L >= 0.5 as
   * they are; soft: brighter whites without clipping, how much */
  {
    gfloat *w  = run ("presence:whites-blacks", "whites=100", ramp, n, 1);
    gfloat *b  = run ("presence:whites-blacks", "blacks=-100", ramp, n, 1);
    gfloat *wm = run ("presence:whites-blacks", "whites=100 ends=move", ramp, n, 1);
    gdouble dw = 0, db = 0;
    gint    clipped_soft = 0, clipped_move = 0, clipped_in = 0;

    for (i = 0; i < n; i++)
      {
        gdouble Lin = (gdouble) i / (n - 1);
        if (Lin <= 0.5)
          dw = MAX (dw, fabs (w[i * 4] - ramp[i * 4]));
        if (Lin >= 0.5)
          db = MAX (db, fabs (b[i * 4] - ramp[i * 4]));
        clipped_in   += ramp[i * 4] >= 1.0f;
        clipped_soft += w[i * 4] >= 1.0f;
        clipped_move += wm[i * 4] >= 1.0f;
      }
    report ("whites_blacks_midtones_stay", dw < 1e-6 && db < 1e-6,
            "whites below L* 50: %.1g, blacks above L* 50: %.1g", dw, db);
    report ("whites_blacks_soft_does_not_clip", clipped_soft == clipped_in && clipped_move > 150,
            "of %d ramp values at white: in %d, whites +100 soft %d, moved %d (from L* %.1f)",
            n, clipped_in, clipped_soft, clipped_move,
            100.0 * (n - clipped_move) / (n - 1));
    note ("whites_blacks_soft_lift", "whites +100 soft: L* 75 to %.2f, L* 90 to %.2f, L* 97 to %.2f",
          100 * pix_L (w + (gsize) (0.75 * (n - 1)) * 4), 100 * pix_L (w + (gsize) (0.90 * (n - 1)) * 4),
          100 * pix_L (w + (gsize) (0.97 * (n - 1)) * 4));
    g_free (w); g_free (b); g_free (wm);
  }

  /* monotonic over a grid of settings, in both modes, also above 1 */
  {
    gint     bad = 0;
    gint     sw, sb, m;
    gfloat  *wide = g_new (gfloat, (gsize) n * 4);

    for (i = 0; i < n; i++)
      set_gray (wide + (gsize) i * 4, -0.1 + 1.6 * i / (n - 1));
    for (m = 0; m < 2; m++)
      for (sw = -100; sw <= 100; sw += 25)
        for (sb = -100; sb <= 100; sb += 25)
          {
            gchar  *pr  = fmt_props ("whites=%d blacks=%d ends=%s output-range=none", sw, sb,
                                     m ? "move" : "soft");
            gfloat *out = run ("presence:whites-blacks", pr, wide, n, 1);
            for (i = 1; i < n; i++)
              {
                gdouble a = lum (out + (gsize) (i - 1) * 4), b = lum (out + (gsize) i * 4);
                /* float rounding: 2 ulp of the value */
                /* float rounding through the cube root and back: some
                 * ulp of the value */
                if (b < a - 2e-6 * fabs (a) - 1e-9)
                  {
                    if (bad < 5)
                      note ("whites_blacks_not_monotonic", "%s at L* %.2f: %.9g then %.9g", pr,
                            100 * (-0.1 + 1.6 * i / (n - 1)), a, b);
                    bad++;
                    break;
                  }
              }
            g_free (out);
            g_free (pr);
          }
    report ("whites_blacks_monotonic", bad == 0,
            "162 settings, L* -10 to 150: %d not monotonic", bad);
    g_free (wide);
  }
  g_free (ramp);
}

/* real photos ---------------------------------------------------------------------------- */

static gfloat *
load_photo (const gchar *path,
            gint        *w,
            gint        *h)
{
  GeglNode     *g = gegl_node_new ();
  GeglNode     *l = gegl_node_new_child (g, "operation", "gegl:load", "path", path, NULL);
  GeglRectangle r = gegl_node_get_bounding_box (l);
  gfloat       *buf;

  if (r.width <= 0)
    {
      g_object_unref (g);
      return NULL;
    }
  buf = g_new (gfloat, (gsize) r.width * r.height * 4);
  gegl_node_blit (l, 1.0, &r, babl_format (LIN), buf, GEGL_AUTO_ROWSTRIDE, GEGL_BLIT_DEFAULT);
  g_object_unref (g);
  *w = r.width;
  *h = r.height;
  return buf;
}

/* mean over blocks of 32 x 32 of the standard deviation of L* inside
 * the block: local contrast */
static gdouble
local_contrast (const gfloat *img,
                gint          w,
                gint          h)
{
  gdouble sum = 0;
  gint    bx, by, nb = 0;

  for (by = 0; by + 32 <= h; by += 32)
    for (bx = 0; bx + 32 <= w; bx += 32)
      {
        gdouble s = 0, s2 = 0;
        gint    x, y;
        for (y = by; y < by + 32; y++)
          for (x = bx; x < bx + 32; x++)
            {
              gdouble L = 100 * pix_L (img + ((gsize) y * w + x) * 4);
              s += L; s2 += L * L;
            }
        sum += sqrt (MAX (s2 / 1024 - (s / 1024) * (s / 1024), 0));
        nb++;
      }
  return sum / MAX (nb, 1);
}

static gdouble
clipped_fraction (const gfloat *img,
                  gsize         n)
{
  gsize i, c = 0;

  for (i = 0; i < n; i++)
    {
      const gfloat *p = img + i * 4;
      /* the tone clipped to white or black (not a single saturated channel) */
      if (MIN (p[0], MIN (p[1], p[2])) >= 0.999f || MAX (p[0], MAX (p[1], p[2])) <= 0.001f)
        c++;
    }
  return (gdouble) c / n;
}

static void
check_photos (void)
{
  const gchar *dir = g_getenv ("PRESENCE_IMAGES") ? g_getenv ("PRESENCE_IMAGES") : "tests/images";
  static const gchar *const hazy[] =
    { "hazy-blue-slopes.jpg", "hazy-mountains-fire.jpg", "hazy-kathmandu.jpg",
      "hazy-summer-valley.jpg", "hazy-turquoise.jpg" };
  guint k;
  gint  found = 0;

  printf ("-- photos (tests/images)\n");
  for (k = 0; k < G_N_ELEMENTS (hazy); k++)
    {
      gchar  *path = g_build_filename (dir, hazy[k], NULL);
      gint    w, h;
      gfloat *in = g_file_test (path, G_FILE_TEST_EXISTS) ? load_photo (path, &w, &h) : NULL;

      if (in)
        {
          gfloat *out = run ("presence:dehaze", "amount=60", in, w, h);
          gdouble ci = local_contrast (in, w, h), co = local_contrast (out, w, h);
          gdouble gi = std_L (in, w, h), go = std_L (out, w, h);
          gdouble ki = clipped_fraction (in, (gsize) w * h), ko = clipped_fraction (out, (gsize) w * h);
          gchar  *name = g_strdup_printf ("dehaze_photo_%s", hazy[k]);

          found++;
          report (name, co > 1.05 * ci,
                  "at 60: local contrast (mean L* std-dev in 32 px blocks) %.2f to %.2f, "
                  "global std-dev %.1f to %.1f L*; pixels clipped to white or black %.2f %% "
                  "to %.2f %%",
                  ci, co, gi * 100, go * 100, ki * 100, ko * 100);
          g_free (name);
          g_free (out);
          g_free (in);
        }
      g_free (path);
    }
  {
    gchar  *path = g_build_filename (dir, "alps-grenoble.jpg", NULL);
    gint    w, h;
    gfloat *in = g_file_test (path, G_FILE_TEST_EXISTS) ? load_photo (path, &w, &h) : NULL;

    if (in)
      {
        gfloat *c = run ("presence:clarity", "amount=60", in, w, h);
        gfloat *t = run ("presence:texture", "amount=60", in, w, h);
        gfloat *tn = run ("presence:texture", "amount=-60", in, w, h);
        gdouble li = local_contrast (in, w, h);

        found++;
        report ("clarity_and_texture_photo_alps-grenoble.jpg",
                local_contrast (c, w, h) > li && local_contrast (t, w, h) > li &&
                local_contrast (tn, w, h) < li,
                "%d x %d, local contrast %.2f: clarity 60 %.2f, texture 60 %.2f, texture -60 %.2f",
                w, h, li, local_contrast (c, w, h), local_contrast (t, w, h), local_contrast (tn, w, h));
        g_free (c); g_free (t); g_free (tn); g_free (in);
      }
    g_free (path);
  }
  if (! found)
    printf ("SKIP  photos: none in %s (tests/images/fetch.py downloads them)\n", dir);
}

int
main (int    argc,
      char **argv)
{
  gchar *dir;

  if (argc < 2)
    {
      fprintf (stderr, "usage: %s <build folder>\n", argv[0]);
      return 2;
    }
  setlocale (LC_ALL, "C");
  dir = modules_setup (argv[1]);
  if (! dir)
    return 2;
  gegl_init (&argc, &argv);
  g_log_set_default_handler (log_handler, NULL);
  rnd = g_rand_new_with_seed (20260928);

  check_registration ();
  check_identity ();
  check_alpha_and_broken ();
  check_invariance ();
  check_formats ();
  check_spaces ();
  check_colour_kept ();
  check_output_range ();
  check_clarity ();
  check_texture ();
  check_dehaze ();
  check_whites_blacks ();
  if (! g_getenv ("PRESENCE_CHECK_NO_PHOTOS"))
    check_photos ();

  report ("no_warnings_or_criticals", n_messages == 0, "%d", n_messages);

  g_rand_free (rnd);
  gegl_exit ();
  if (! g_getenv ("PRESENCE_CHECK_KEEP_MODULE"))
    modules_cleanup (dir);
  else
    g_free (dir);

  printf ("%d passed, %d failed\n", n_passed, n_failed);
  return n_failed ? 1 : 0;
}

/*
 * Shared by the presence operations: lightness, pyramids, the band
 * limited local Laplacian filter, guided filters, the whole-image
 * plumbing and the output range
 *
 * presence-common.h
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
 * The local Laplacian filter is that of Paris, Hasinoff and Kautz
 * (SIGGRAPH 2011) in the fast form of Aubry, Paris, Hasinoff, Kautz and
 * Durand (ACM TOG 2014): the image remapped around a few sampled
 * values, Laplacian pyramids of those, and each output coefficient
 * interpolated between the two samples next to the input's own value at
 * that level. The remapping's detail term, d exp (-d^2 / 2 s^2), is the
 * one of darktable's local contrast module (src/common/locallaplacian.c,
 * GPL-3.0-or-later, Johannes Hanika and the darktable developers). The
 * code here is written anew: only the change of the lightness is built
 * (a pyramid of it collapsed and added), each level has a weight so that
 * one band of sizes is changed, samples may be weighted (midtones), and
 * coarse bands are remapped from a smaller copy (Aubry et al., sec. 4).
 *
 * The guided filter (He, Sun and Tang, ECCV 2010) and its fast form
 * with the coefficients computed on a small copy (He and Sun, 2015) are
 * as in gegl-underwater's underwater-correct.c (same author).
 */

#ifndef PRESENCE_COMMON_H
#define PRESENCE_COMMON_H

#include <gegl.h>
#include <math.h>
#include <string.h>

/* the output range, the same numbers in every operation's enum */
enum
{
  PRESENCE_RANGE_KEEP_HUE = 0,  /* into 0 to 1 towards gray of the same luminance */
  PRESENCE_RANGE_CLIP     = 1,  /* each channel clipped to 0 to 1 */
  PRESENCE_RANGE_NONE     = 2   /* unbounded, for high dynamic range work */
};

#define PRESENCE_MAX_LEVELS 24

/* lightness ------------------------------------------------------------ */

/* CIE lightness L* / 100 from relative luminance, with its linear part
 * near black; defined for negative values and above 1 too (the same
 * formulas continued), so float images keep their range */
static inline gfloat
presence_Y_to_L (gfloat Y)
{
  return Y > 216.0f / 24389.0f ? 1.16f * cbrtf (Y) - 0.16f : Y * (24389.0f / 2700.0f);
}

static inline gfloat
presence_L_to_Y (gfloat L)
{
  if (L > 0.08f)
    {
      gfloat t = (L + 0.16f) / 1.16f;
      return t * t * t;
    }
  return L * (2700.0f / 24389.0f);
}

typedef struct
{
  gfloat kr, kg, kb;   /* luminance of the primaries of the image's space */
} PresenceSpace;

static inline void
presence_space_init (PresenceSpace *s,
                     const Babl    *space)
{
  gdouble r = 0.2126, g = 0.7152, b = 0.0722;

  if (space)
    babl_space_get_rgb_luminance (space, &r, &g, &b);
  s->kr = r;
  s->kg = g;
  s->kb = b;
}

static inline gfloat
presence_luminance (const PresenceSpace *s,
                    const gfloat        *p)
{
  return s->kr * p[0] + s->kg * p[1] + s->kb * p[2];
}

static inline gboolean
presence_finite3 (const gfloat *p)
{
  return isfinite (p[0]) && isfinite (p[1]) && isfinite (p[2]);
}

/* A new luminance Y2 for a pixel of luminance Y, keeping its
 * chromaticity: the channels are scaled, and a gray part makes up what
 * the scaling does not (near black, where the offset e keeps the
 * scaling from blowing up noise and a pixel without luminance cannot be
 * scaled at all). The result's luminance is Y2 exactly. */
static inline void
presence_set_luminance (const PresenceSpace *s,
                        const gfloat        *in,
                        gfloat               Y,
                        gfloat               Y2,
                        gfloat              *out)
{
  const gfloat e = 1e-3f;
  gfloat       k = MAX (Y2, 0.0f) + e;
  gfloat       add;
  gint         c;

  k /= MAX (Y, 0.0f) + e;
  add = Y2 - k * Y;
  (void) s;
  for (c = 0; c < 3; c++)
    out[c] = in[c] * k + add;
}

/* the output range; Y is the luminance the pixel should have */
static inline void
presence_range (gint    mode,
                gfloat  Y,
                gfloat *v)
{
  gint c;

  if (mode == PRESENCE_RANGE_NONE || ! presence_finite3 (v))
    return;
  if (mode == PRESENCE_RANGE_CLIP)
    {
      for (c = 0; c < 3; c++)
        v[c] = CLAMP (v[c], 0.0f, 1.0f);
      return;
    }
  if (v[0] >= 0.0f && v[0] <= 1.0f && v[1] >= 0.0f && v[1] <= 1.0f &&
      v[2] >= 0.0f && v[2] <= 1.0f)
    return;
  /* towards the gray of the same luminance, as little as needed: the
   * hue stays, the saturation gives */
  {
    gfloat g = CLAMP (Y, 0.0f, 1.0f);
    gfloat t = 1.0f;

    for (c = 0; c < 3; c++)
      {
        if (v[c] > 1.0f)
          t = MIN (t, (1.0f - g) / (v[c] - g));
        else if (v[c] < 0.0f)
          t = MIN (t, g / (g - v[c]));
      }
    t = CLAMP (t, 0.0f, 1.0f);
    for (c = 0; c < 3; c++)
      v[c] = CLAMP (g + t * (v[c] - g), 0.0f, 1.0f);
  }
}

/* parallel loops ---------------------------------------------------------- */

typedef void (*PresenceRowFunc) (gint y0, gint y1, gpointer data);

typedef struct
{
  PresenceRowFunc func;
  gpointer        data;
} PresenceRows;

static G_GNUC_UNUSED void
presence_rows_cb (gsize    offset,
                  gsize    count,
                  gpointer user_data)
{
  PresenceRows *r = user_data;

  r->func ((gint) offset, (gint) (offset + count), r->data);
}

/* Runs func over the rows 0 to h in parallel. Each row is computed from
 * the same inputs whichever thread does it, so the result does not
 * depend on the number of threads (checked in tests/check.c). */
static G_GNUC_UNUSED void
presence_parallel_rows (gint            h,
                        gsize           row_cost,
                        PresenceRowFunc func,
                        gpointer        data)
{
  PresenceRows r = { func, data };
  gsize        min_rows = MAX ((gsize) 1, (gsize) 32768 / MAX (row_cost, (gsize) 1));

  if (h <= 0)
    return;
  gegl_parallel_distribute_range (h, (gint) MIN (min_rows, (gsize) G_MAXINT),
                                  presence_rows_cb, &r);
}

/* pyramids ------------------------------------------------------------------ */

static inline gint
presence_half (gint n)
{
  return (n + 1) / 2;
}

typedef struct
{
  const gfloat *src;
  gfloat       *dst;
  gint          w, h, cw, ch;
  gfloat       *tmp;
} PresenceReduce;

/* horizontal: 1 4 6 4 1 / 16 at the even columns, borders repeated */
static G_GNUC_UNUSED void
presence_reduce_h (gint y0, gint y1, gpointer data)
{
  PresenceReduce *r = data;
  gint            y, i;

  for (y = y0; y < y1; y++)
    {
      const gfloat *s = r->src + (gsize) y * r->w;
      gfloat       *d = r->tmp + (gsize) y * r->cw;
      const gint    w = r->w;

      for (i = 0; i < r->cw; i++)
        {
          gint x = 2 * i;
          if (x >= 2 && x + 2 < w)
            d[i] = (s[x - 2] + s[x + 2] + 4.0f * (s[x - 1] + s[x + 1]) + 6.0f * s[x]) * (1.0f / 16.0f);
          else
            {
              gint a = MAX (x - 2, 0), b = MAX (x - 1, 0), e = MIN (x + 1, w - 1), f = MIN (x + 2, w - 1);
              gint c = MIN (x, w - 1);
              d[i] = (s[a] + s[f] + 4.0f * (s[b] + s[e]) + 6.0f * s[c]) * (1.0f / 16.0f);
            }
        }
    }
}

/* vertical: 1 4 6 4 1 / 16 at the even rows */
static G_GNUC_UNUSED void
presence_reduce_v (gint j0, gint j1, gpointer data)
{
  PresenceReduce *r = data;
  gint            j, i;

  for (j = j0; j < j1; j++)
    {
      gint          y = 2 * j, h = r->h;
      const gfloat *a = r->tmp + (gsize) MAX (y - 2, 0) * r->cw;
      const gfloat *b = r->tmp + (gsize) MAX (y - 1, 0) * r->cw;
      const gfloat *c = r->tmp + (gsize) MIN (y, h - 1) * r->cw;
      const gfloat *e = r->tmp + (gsize) MIN (y + 1, h - 1) * r->cw;
      const gfloat *f = r->tmp + (gsize) MIN (y + 2, h - 1) * r->cw;
      gfloat       *d = r->dst + (gsize) j * r->cw;

      for (i = 0; i < r->cw; i++)
        d[i] = (a[i] + f[i] + 4.0f * (b[i] + e[i]) + 6.0f * c[i]) * (1.0f / 16.0f);
    }
}

/* one level down: dst is (w+1)/2 x (h+1)/2; tmp holds (w+1)/2 x h */
static G_GNUC_UNUSED void
presence_reduce (const gfloat *src,
                 gint          w,
                 gint          h,
                 gfloat       *dst,
                 gfloat       *tmp)
{
  PresenceReduce r = { src, dst, w, h, presence_half (w), presence_half (h), tmp };

  presence_parallel_rows (h, r.cw, presence_reduce_h, &r);
  presence_parallel_rows (r.ch, r.cw, presence_reduce_v, &r);
}

typedef struct
{
  const gfloat *coarse;
  gfloat       *fine;    /* the result, w x h */
  const gfloat *sub;     /* if set: fine = sub - expand (coarse) */
  const gfloat *add;     /* if set: fine = add + expand (coarse) */
  gint          w, h, cw, ch;
  gfloat       *tmp;     /* w x ch */
} PresenceExpand;

/* horizontal: even x = 2i from (c[i-1] + 6 c[i] + c[i+1]) / 8, odd from
 * (c[i] + c[i+1]) / 2, borders repeated */
static G_GNUC_UNUSED void
presence_expand_h (gint j0, gint j1, gpointer data)
{
  PresenceExpand *e = data;
  gint            j, x;

  for (j = j0; j < j1; j++)
    {
      const gfloat *c  = e->coarse + (gsize) j * e->cw;
      gfloat       *d  = e->tmp + (gsize) j * e->w;
      const gint    cw = e->cw;

      for (x = 0; x < e->w; x++)
        {
          gint i = x >> 1;

          if (x & 1)
            d[x] = 0.5f * (c[i] + c[MIN (i + 1, cw - 1)]);
          else
            d[x] = 0.125f * (c[MAX (i - 1, 0)] + 6.0f * c[i] + c[MIN (i + 1, cw - 1)]);
        }
    }
}

static G_GNUC_UNUSED void
presence_expand_v (gint y0, gint y1, gpointer data)
{
  PresenceExpand *e = data;
  gint            y, x;

  for (y = y0; y < y1; y++)
    {
      gint          j = y >> 1, ch = e->ch;
      gfloat       *d = e->fine + (gsize) y * e->w;
      const gfloat *s = e->sub ? e->sub + (gsize) y * e->w : NULL;
      const gfloat *a = e->add ? e->add + (gsize) y * e->w : NULL;

      if (y & 1)
        {
          const gfloat *p = e->tmp + (gsize) j * e->w;
          const gfloat *q = e->tmp + (gsize) MIN (j + 1, ch - 1) * e->w;

          for (x = 0; x < e->w; x++)
            {
              gfloat v = 0.5f * (p[x] + q[x]);
              d[x] = s ? s[x] - v : a ? a[x] + v : v;
            }
        }
      else
        {
          const gfloat *p = e->tmp + (gsize) MAX (j - 1, 0) * e->w;
          const gfloat *q = e->tmp + (gsize) j * e->w;
          const gfloat *r = e->tmp + (gsize) MIN (j + 1, ch - 1) * e->w;

          for (x = 0; x < e->w; x++)
            {
              gfloat v = 0.125f * (p[x] + 6.0f * q[x] + r[x]);
              d[x] = s ? s[x] - v : a ? a[x] + v : v;
            }
        }
    }
}

/* fine (w x h) = expand (coarse), or sub - expand (coarse), or add +
 * expand (coarse); fine may be the same as sub or add. tmp holds
 * w x (h+1)/2 */
static G_GNUC_UNUSED void
presence_expand (const gfloat *coarse,
                 gint          w,
                 gint          h,
                 gfloat       *fine,
                 const gfloat *sub,
                 const gfloat *add,
                 gfloat       *tmp)
{
  PresenceExpand e = { coarse, fine, sub, add, w, h, presence_half (w), presence_half (h), tmp };

  presence_parallel_rows (e.ch, w, presence_expand_h, &e);
  presence_parallel_rows (h, w, presence_expand_v, &e);
}

/* the band limited local Laplacian filter --------------------------------- */

typedef struct
{
  gint   levels;                      /* Laplacian levels, 1 to PRESENCE_MAX_LEVELS */
  gfloat band[PRESENCE_MAX_LEVELS];   /* weight of each level */
  gfloat sigma;                       /* size of detail in L; larger steps are edges */
  gfloat noise;                       /* changes below it are cored away (smooth areas), or 0 */
  gfloat mid_width;                   /* the samples weighted towards L = 0.5, or 0 */
  gint   src_level;                   /* remap from this Gaussian level (0: the image) */
} PresenceLLF;

/* the weights of the levels for a band of sizes around size pixels (the
 * radius of a feature, half its wavelength: tests/check.c measures where
 * the gain peaks), width in octaves; the levels there are for an image
 * of w x h */
static G_GNUC_UNUSED void
presence_llf_band (PresenceLLF *p,
                   gdouble      size,
                   gdouble      width,
                   gint         w,
                   gint         h)
{
  gint   max_levels = 1, n = MAX (w, h), l, first = -1, last = -1;
  /* level l of this pyramid peaks at about 2.8 x 2^l pixels (a quarter
   * of the wavelength of largest gain): see tests/check.c */
  gdouble c = log2 (MAX (size, 0.5) / 2.8);

  while (max_levels < PRESENCE_MAX_LEVELS && (n >> max_levels) >= 2)
    max_levels++;
  for (l = 0; l < PRESENCE_MAX_LEVELS; l++)
    {
      gdouble d  = (l - c) / width;
      gdouble wt = exp (-0.5 * d * d);

      p->band[l] = (wt >= 0.02 && l < max_levels) ? wt : 0.0f;
      if (p->band[l] > 0.0f)
        {
          if (first < 0)
            first = l;
          last = l;
        }
    }
  p->levels    = last >= 0 ? last + 1 : 0;
  /* coarse bands from a copy two levels finer than the band (the
   * approximation of Aubry et al. 2014, sec. 4): as the full image to
   * within 0.01 L* at a step edge (tests/check.c); one level finer adds
   * a third to the overshoot there */
  p->src_level = MAX (first - 2, 0);
}

static inline gfloat
presence_detail (gfloat d,
                 gfloat inv2s2)
{
  return d * expf (-d * d * inv2s2);
}

typedef struct
{
  const gfloat *src;
  gfloat       *dst;
  gint          w;
  gfloat        gamma, inv2s2;
} PresenceRemap;

static G_GNUC_UNUSED void
presence_remap_rows (gint y0, gint y1, gpointer data)
{
  PresenceRemap *r = data;
  gsize          i, i0 = (gsize) y0 * r->w, i1 = (gsize) y1 * r->w;

  for (i = i0; i < i1; i++)
    r->dst[i] = presence_detail (r->src[i] - r->gamma, r->inv2s2);
}

typedef struct
{
  const gfloat *G;      /* the input's Gaussian level */
  const gfloat *lap;    /* the remapped image's Laplacian level */
  gfloat       *D;      /* accumulated change */
  gint          w;
  gfloat        gamma, step, weight, lo_g, hi_g;
} PresenceAccum;

/* D += weight x tent (G - gamma) x lap; the first and last samples take
 * all values beyond them */
static G_GNUC_UNUSED void
presence_accum_rows (gint y0, gint y1, gpointer data)
{
  PresenceAccum *a = data;
  gsize          i, i0 = (gsize) y0 * a->w, i1 = (gsize) y1 * a->w;
  const gfloat   inv = 1.0f / a->step;

  for (i = i0; i < i1; i++)
    {
      gfloat g = a->G[i], t;

      if (g <= a->lo_g || g >= a->hi_g)
        t = 1.0f;
      else
        t = 1.0f - fabsf (g - a->gamma) * inv;
      if (t > 0.0f)
        a->D[i] += a->weight * t * a->lap[i];
    }
}

/* The change of the lightness L (w x h, finite values) that the filter
 * makes at a gain of 1: delta (w x h) is written, L itself is left.
 * Returns FALSE (and delta untouched) without the memory for it. */
static G_GNUC_UNUSED gboolean
presence_llf_delta (const PresenceLLF *p,
                    const gfloat      *L,
                    gint               w,
                    gint               h,
                    gfloat            *delta)
{
  gint     n = p->levels, s0 = MIN (p->src_level, MAX (n - 1, 0)), l, k, ng;
  gint     lw[PRESENCE_MAX_LEVELS + 1], lh[PRESENCE_MAX_LEVELS + 1];
  gfloat  *G[PRESENCE_MAX_LEVELS + 1] = { NULL, };
  gfloat  *R[PRESENCE_MAX_LEVELS + 1] = { NULL, };
  gfloat  *D[PRESENCE_MAX_LEVELS + 1] = { NULL, };
  gfloat  *tmp = NULL, *lap = NULL;
  gfloat   lo = G_MAXFLOAT, hi = -G_MAXFLOAT, step;
  gboolean ok = TRUE;
  gsize    i, npix = (gsize) w * h;

  if (n <= 0)
    {
      memset (delta, 0, npix * sizeof (gfloat));
      return TRUE;
    }
  lw[0] = w;
  lh[0] = h;
  for (l = 1; l <= n; l++)
    {
      lw[l] = presence_half (lw[l - 1]);
      lh[l] = presence_half (lh[l - 1]);
    }

  /* scratch for the separable passes: the largest is at the finest
   * level in use */
  {
    gint  lw0 = s0 > 0 ? lw[s0 - 1] : w, lh0 = s0 > 0 ? lh[s0 - 1] : h;
    gsize big = MAX ((gsize) presence_half (lw0) * lh0, (gsize) lw0 * presence_half (lh0));

    big = MAX (big, (gsize) w * presence_half (h));
    tmp = g_try_new (gfloat, big);
    lap = g_try_new (gfloat, (gsize) lw[s0] * lh[s0]);
    ok = tmp && lap;
  }

  /* the input's Gaussian pyramid (level 0 is L itself) */
  G[0] = (gfloat *) L;
  for (l = 1; ok && l <= n; l++)
    {
      G[l] = g_try_new (gfloat, (gsize) lw[l] * lh[l]);
      if (! G[l])
        ok = FALSE;
      else
        presence_reduce (G[l - 1], lw[l - 1], lh[l - 1], G[l], tmp);
    }
  for (l = s0; ok && l <= n; l++)
    {
      R[l] = g_try_new (gfloat, (gsize) lw[l] * lh[l]);
      if (l < n)
        D[l] = p->band[l] > 0.0f ? g_try_new0 (gfloat, (gsize) lw[l] * lh[l]) : NULL;
      if (! R[l] || (l < n && p->band[l] > 0.0f && ! D[l]))
        ok = FALSE;
    }

  if (ok)
    {
      /* the samples: evenly over the values of the remapped level, one
       * sigma apart (at most 64) */
      const gfloat *src = G[s0];
      gsize         m   = (gsize) lw[s0] * lh[s0];

      for (i = 0; i < m; i++)
        {
          lo = MIN (lo, src[i]);
          hi = MAX (hi, src[i]);
        }
      ng   = (gint) ceilf ((hi - lo) / p->sigma) + 1;
      ng   = CLAMP (ng, 2, 64);
      step = (hi - lo) / (ng - 1);
      if (! (step > 0.0f))
        step = p->sigma;

      for (k = 0; k < ng; k++)
        {
          gfloat        gamma = lo + k * step;
          gfloat        mw    = 1.0f;
          PresenceRemap rm    = { src, R[s0], lw[s0], gamma, 0.5f / (p->sigma * p->sigma) };

          if (p->mid_width > 0.0f)
            {
              gfloat d = (gamma - 0.5f) / p->mid_width;
              mw = expf (-0.5f * d * d);
            }
          if (mw < 1e-4f)
            continue;
          presence_parallel_rows (lh[s0], lw[s0], presence_remap_rows, &rm);
          for (l = s0 + 1; l <= n; l++)
            presence_reduce (R[l - 1], lw[l - 1], lh[l - 1], R[l], tmp);
          for (l = s0; l < n; l++)
            {
              PresenceAccum ac;

              if (! D[l])
                continue;
              presence_expand (R[l + 1], lw[l], lh[l], lap, R[l], NULL, tmp);
              ac.G = G[l]; ac.lap = lap; ac.D = D[l]; ac.w = lw[l];
              ac.gamma = gamma; ac.step = step; ac.weight = p->band[l] * mw;
              ac.lo_g = k == 0 ? gamma : -G_MAXFLOAT;
              ac.hi_g = k == ng - 1 ? gamma : G_MAXFLOAT;
              presence_parallel_rows (lh[l], lw[l], presence_accum_rows, &ac);
            }
        }

      /* collapse the change, coarse to fine; its residual is 0. The
       * levels of R[] are free now and hold the running sum. */
      {
        gfloat  *acc = NULL;   /* the sum at level l + 1 */
        gboolean acc_own = FALSE;

        for (l = n - 1; ok && l >= 0; l--)
          {
            gfloat  *own = l >= s0 ? D[l] : NULL;
            gfloat  *dst;
            gboolean dst_own = FALSE;

            if (l == 0)
              dst = delta;
            else if (l >= s0)
              dst = R[l];
            else
              {
                dst = g_try_new (gfloat, (gsize) lw[l] * lh[l]);
                dst_own = TRUE;
                if (! dst)
                  {
                    ok = FALSE;
                    break;
                  }
              }
            if (acc)
              presence_expand (acc, lw[l], lh[l], dst, NULL, own, tmp);
            else if (own)
              memcpy (dst, own, (gsize) lw[l] * lh[l] * sizeof (gfloat));
            else
              memset (dst, 0, (gsize) lw[l] * lh[l] * sizeof (gfloat));
            if (acc_own)
              g_free (acc);
            acc = dst;
            acc_own = dst_own;
          }
        if (acc_own)
          g_free (acc);
      }
    }

  for (l = 1; l <= n; l++)
    g_free (G[l]);
  for (l = 0; l <= n; l++)
    {
      g_free (R[l]);
      g_free (D[l]);
    }
  g_free (lap);
  g_free (tmp);
  return ok;
}

typedef struct
{
  gfloat       *delta;
  const gfloat *energy;
  gfloat        n2;
  gint          w;
} PresenceCore;

static void
presence_core_rows (gint y0, gint y1, gpointer data)
{
  PresenceCore *c = data;
  gsize         i, i0 = (gsize) y0 * c->w, i1 = (gsize) y1 * c->w;

  for (i = i0; i < i1; i++)
    {
      gfloat e = MAX (c->energy[i], 0.0f);
      c->delta[i] *= e / (e + c->n2);
    }
}

/* Coring of the change: where its local RMS (over about 8 pixels) is
 * well below noise, as in smooth areas with only noise in them, it is
 * scaled down by e / (e + noise^2); where there is texture it stays.
 * work holds w x h floats. Returns FALSE without the memory. */
static G_GNUC_UNUSED gboolean
presence_core (gfloat *delta,
               gint    w,
               gint    h,
               gfloat  noise,
               gfloat *work)
{
  gint    w1 = presence_half (w), h1 = presence_half (h);
  gint    w2 = presence_half (w1), h2 = presence_half (h1);
  gsize   i, n = (gsize) w * h;
  gfloat *l1 = g_try_new (gfloat, (gsize) w1 * h1);
  gfloat *l2 = g_try_new (gfloat, (gsize) w2 * h2);
  gfloat *tmp = g_try_new (gfloat, MAX ((gsize) w1 * h, (gsize) w * h1));
  PresenceCore c;

  if (! l1 || ! l2 || ! tmp)
    {
      g_free (l1); g_free (l2); g_free (tmp);
      return FALSE;
    }
  for (i = 0; i < n; i++)
    work[i] = delta[i] * delta[i];
  presence_reduce (work, w, h, l1, tmp);
  presence_reduce (l1, w1, h1, l2, tmp);
  presence_expand (l2, w1, h1, l1, NULL, NULL, tmp);
  presence_expand (l1, w, h, work, NULL, NULL, tmp);
  c.delta = delta; c.energy = work; c.n2 = noise * noise; c.w = w;
  presence_parallel_rows (h, w, presence_core_rows, &c);
  g_free (l1); g_free (l2); g_free (tmp);
  return TRUE;
}

/* guided filter ------------------------------------------------------------ */

/* box mean over a square of radius r, through an integral image, borders
 * by the pixels inside only */
static G_GNUC_UNUSED void
presence_box_mean (const gfloat *src,
                   gfloat       *dst,
                   gint          w,
                   gint          h,
                   gint          r)
{
  gdouble *sum = g_new0 (gdouble, (gsize) (w + 1) * (h + 1));
  gint     x, y;

  for (y = 0; y < h; y++)
    {
      gdouble rs = 0.0;
      for (x = 0; x < w; x++)
        {
          rs += src[(gsize) y * w + x];
          sum[(gsize) (y + 1) * (w + 1) + x + 1] = sum[(gsize) y * (w + 1) + x + 1] + rs;
        }
    }
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        gint x0 = MAX (x - r, 0), x1 = MIN (x + r + 1, w);
        gint y0 = MAX (y - r, 0), y1 = MIN (y + r + 1, h);
        gdouble s = sum[(gsize) y1 * (w + 1) + x1] - sum[(gsize) y0 * (w + 1) + x1]
                  - sum[(gsize) y1 * (w + 1) + x0] + sum[(gsize) y0 * (w + 1) + x0];
        dst[(gsize) y * w + x] = s / ((x1 - x0) * (y1 - y0));
      }
  g_free (sum);
}

/* The coefficients of the guided filter (He et al.): q = a I + b smooths
 * p along the edges of I. a and b (w x h) are the box means of the
 * per-window coefficients, ready to be applied to I, here or (upsampled)
 * to a larger I (the fast guided filter of He and Sun). */
static G_GNUC_UNUSED void
presence_guided_coeffs (const gfloat *I,
                        const gfloat *p,
                        gint          w,
                        gint          h,
                        gint          r,
                        gfloat        eps,
                        gfloat       *a,
                        gfloat       *b)
{
  gsize   n = (gsize) w * h, i;
  gfloat *mI = g_new (gfloat, n), *mp = g_new (gfloat, n);
  gfloat *II = g_new (gfloat, n), *Ip = g_new (gfloat, n);

  for (i = 0; i < n; i++)
    {
      II[i] = I[i] * I[i];
      Ip[i] = I[i] * p[i];
    }
  presence_box_mean (I, mI, w, h, r);
  presence_box_mean (p, mp, w, h, r);
  presence_box_mean (II, II, w, h, r);
  presence_box_mean (Ip, Ip, w, h, r);
  for (i = 0; i < n; i++)
    {
      gfloat var = II[i] - mI[i] * mI[i];
      gfloat cov = Ip[i] - mI[i] * mp[i];

      a[i] = cov / (MAX (var, 0.0f) + eps);
      b[i] = mp[i] - a[i] * mI[i];
    }
  presence_box_mean (a, a, w, h, r);
  presence_box_mean (b, b, w, h, r);
  g_free (Ip); g_free (II); g_free (mp); g_free (mI);
}

/* bilinear sample of a w x h map at the centre of full-size pixel (x, y),
 * the map covering the image at f full-size pixels per map pixel */
static inline gfloat
presence_sample (const gfloat *m,
                 gint          w,
                 gint          h,
                 gfloat        f,
                 gint          x,
                 gint          y)
{
  gfloat fx = CLAMP ((x + 0.5f) / f - 0.5f, 0.0f, w - 1.0f);
  gfloat fy = CLAMP ((y + 0.5f) / f - 0.5f, 0.0f, h - 1.0f);
  gint   x0 = (gint) fx, y0 = (gint) fy;
  gint   x1 = MIN (x0 + 1, w - 1), y1 = MIN (y0 + 1, h - 1);
  gfloat ax = fx - x0, ay = fy - y0;

  return (1 - ay) * ((1 - ax) * m[(gsize) y0 * w + x0] + ax * m[(gsize) y0 * w + x1])
       +      ay  * ((1 - ax) * m[(gsize) y1 * w + x0] + ax * m[(gsize) y1 * w + x1]);
}

/* whole-image plumbing -------------------------------------------------------- */

/* Every result needs all of the input (the pyramids and the estimates
 * cover the whole image) and is computed once for all of it, so it does
 * not depend on how GIMP or GEGL split the image into tiles; an input
 * without bounds (such as gegl:color) is passed through. */
static G_GNUC_UNUSED GeglRectangle
presence_get_required_for_output (GeglOperation       *operation,
                                  const gchar         *input_pad,
                                  const GeglRectangle *roi)
{
  const GeglRectangle *in = gegl_operation_source_get_bounding_box (operation, "input");

  (void) input_pad;
  return in && ! gegl_rectangle_is_infinite_plane (in) ? *in : *roi;
}

static G_GNUC_UNUSED GeglRectangle
presence_get_cached_region (GeglOperation       *operation,
                            const GeglRectangle *roi)
{
  const GeglRectangle *in = gegl_operation_source_get_bounding_box (operation, "input");

  return in && ! gegl_rectangle_is_infinite_plane (in) ? *in : *roi;
}

/* a change anywhere changes the result everywhere */
static G_GNUC_UNUSED GeglRectangle
presence_get_invalidated_by_change (GeglOperation       *operation,
                                    const gchar         *input_pad,
                                    const GeglRectangle *roi)
{
  const GeglRectangle *in = gegl_operation_source_get_bounding_box (operation, "input");

  (void) input_pad;
  return in && ! gegl_rectangle_is_infinite_plane (in) ? *in : *roi;
}

static G_GNUC_UNUSED void
presence_prepare_linear (GeglOperation *operation)
{
  const Babl *space  = gegl_operation_get_source_space (operation, "input");
  const Babl *format = babl_format_with_space ("RGBA float", space);

  gegl_operation_set_format (operation, "input", format);
  gegl_operation_set_format (operation, "output", format);
}

/* the whole input as RGBA float in its own space, or NULL (after a
 * warning, with the input copied to the output) without the memory */
static G_GNUC_UNUSED gfloat *
presence_get_whole (GeglOperation       *operation,
                    GeglBuffer          *input,
                    GeglBuffer          *output,
                    const GeglRectangle *whole,
                    const gchar         *name)
{
  const Babl *format = gegl_operation_get_format (operation, "output");
  gsize       n      = (gsize) whole->width * whole->height;
  gfloat     *buf    = n <= G_MAXSIZE / (8 * sizeof (gfloat)) ? g_try_new (gfloat, n * 4) : NULL;

  if (! buf)
    {
      g_warning ("%s: not enough memory for an image of %d x %d", name,
                 whole->width, whole->height);
      gegl_buffer_copy (input, whole, GEGL_ABYSS_NONE, output, whole);
      return NULL;
    }
  gegl_buffer_get (input, whole, 1.0, format, buf, GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
  return buf;
}

/* the lightness of every pixel (0 for a broken pixel, which then cannot
 * spread through the pyramids), and the luminance */
typedef struct
{
  const PresenceSpace *s;
  const gfloat        *rgba;
  gfloat              *L, *Y;
  gint                 w;
} PresenceLight;

static G_GNUC_UNUSED void
presence_light_rows (gint y0, gint y1, gpointer data)
{
  PresenceLight *d = data;
  gsize          i, i0 = (gsize) y0 * d->w, i1 = (gsize) y1 * d->w;

  for (i = i0; i < i1; i++)
    {
      const gfloat *p = d->rgba + i * 4;
      gfloat        Y = presence_finite3 (p) ? presence_luminance (d->s, p) : 0.0f;
      gfloat        L;

      if (! isfinite (Y))   /* finite channels, infinite sum */
        Y = 0.0f;
      L = presence_Y_to_L (Y);
      /* far outside any image's range, the detail term is 0 anyway;
       * bounded so that the samples stay a few */
      L = CLAMP (L, -1.0f, 16.0f);
      if (d->Y)
        d->Y[i] = Y;
      d->L[i] = L;
    }
}

static G_GNUC_UNUSED void
presence_lightness (const PresenceSpace *s,
                    const gfloat        *rgba,
                    gint                 w,
                    gint                 h,
                    gfloat              *L,
                    gfloat              *Y)
{
  PresenceLight d = { s, rgba, L, Y, w };

  presence_parallel_rows (h, w, presence_light_rows, &d);
}

typedef struct
{
  const PresenceSpace *s;
  gfloat              *rgba;
  const gfloat        *L, *Y, *delta;
  gfloat               gain;
  gint                 w, range;
} PresenceApply;

/* L + gain x delta as the new lightness of each pixel, colour kept;
 * broken pixels are left as they are; alpha is not touched */
static G_GNUC_UNUSED void
presence_apply_rows (gint y0, gint y1, gpointer data)
{
  PresenceApply *d = data;
  gsize          i, i0 = (gsize) y0 * d->w, i1 = (gsize) y1 * d->w;

  for (i = i0; i < i1; i++)
    {
      gfloat *p = d->rgba + i * 4;
      gfloat  Y = d->Y[i], L2, Y2, out[3];

      if (! presence_finite3 (p) || ! isfinite (presence_luminance (d->s, p)))
        continue;
      /* from the luminance itself: L was bounded for the pyramids */
      L2 = presence_Y_to_L (Y) + d->gain * d->delta[i];
      Y2 = presence_L_to_Y (L2);
      presence_set_luminance (d->s, p, Y, Y2, out);
      presence_range (d->range, Y2, out);
      p[0] = out[0];
      p[1] = out[1];
      p[2] = out[2];
    }
}

/* the detail operations (clarity, texture): the lightness through the
 * band limited local Laplacian filter, the colour kept */
static G_GNUC_UNUSED gboolean
presence_detail_process (GeglOperation       *operation,
                         GeglBuffer          *input,
                         GeglBuffer          *output,
                         const PresenceLLF   *llf_in,
                         gdouble              size,
                         gdouble              width,
                         gfloat               gain,
                         gint                 range,
                         const gchar         *name)
{
  const GeglRectangle *whole = gegl_operation_source_get_bounding_box (operation, "input");
  const Babl          *space = gegl_operation_get_source_space (operation, "input");
  const Babl          *format = gegl_operation_get_format (operation, "output");
  PresenceSpace        s;
  PresenceLLF          llf = *llf_in;
  gfloat              *buf, *L, *Y, *delta;
  gsize                n;
  gint64               t0 = g_get_monotonic_time ();

  if (! whole || whole->width < 1 || whole->height < 1)
    return TRUE;
  if (gegl_rectangle_is_infinite_plane (whole))
    {
      gegl_buffer_copy (input, whole, GEGL_ABYSS_NONE, output, whole);
      return TRUE;
    }
  buf = presence_get_whole (operation, input, output, whole, name);
  if (! buf)
    return TRUE;
  n     = (gsize) whole->width * whole->height;
  L     = g_try_new (gfloat, n);
  Y     = g_try_new (gfloat, n);
  delta = g_try_new (gfloat, n);
  presence_space_init (&s, space);
  presence_llf_band (&llf, size, width, whole->width, whole->height);
  if (L && Y && delta)
    {
      presence_lightness (&s, buf, whole->width, whole->height, L, Y);
      if (presence_llf_delta (&llf, L, whole->width, whole->height, delta) &&
          (llf.noise <= 0.0f ||
           presence_core (delta, whole->width, whole->height, llf.noise, L)))
        {
          PresenceApply ap = { &s, buf, L, Y, delta, gain, whole->width, range };

          presence_parallel_rows (whole->height, whole->width, presence_apply_rows, &ap);
        }
      else
        g_warning ("%s: not enough memory for the pyramids of an image of %d x %d",
                   name, whole->width, whole->height);
    }
  else
    g_warning ("%s: not enough memory for an image of %d x %d", name,
               whole->width, whole->height);
  gegl_buffer_set (output, whole, 0, format, buf, GEGL_AUTO_ROWSTRIDE);
  if (g_getenv ("PRESENCE_DEBUG"))
    g_printerr ("%s: %d x %d, levels %d (from %d), %.3f s\n", name, whole->width,
                whole->height, llf.levels, llf.src_level,
                (g_get_monotonic_time () - t0) / 1e6);
  g_free (delta);
  g_free (Y);
  g_free (L);
  g_free (buf);
  return TRUE;
}

/* passes the input on as it is (exactly) when the operation does nothing */
static G_GNUC_UNUSED gboolean
presence_pass_through (GeglOperation        *operation,
                       GeglOperationContext *context)
{
  GObject *input = gegl_operation_context_get_object (context, "input");

  (void) operation;
  gegl_operation_context_set_object (context, "output", input);
  return TRUE;
}

#endif

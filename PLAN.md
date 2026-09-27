# Plan

## Done

`presence:clarity`, `presence:texture`, `presence:dehaze` and
`presence:whites-blacks` (README). Tested with tests/check.sh (114 checks
with the photos, 108 also under ASan and UBSan), tests/gimp-check.sh
(GIMP 3.2.6 Flatpak: every precision, grayscale, a layer with offsets,
XCF, the command line), tests/gui/shots.sh (the menus and dialogs on
Broadway, looked at; Dehaze applied with OK and live in the saved XCF),
tests/bench.sh (24 and 26 megapixels). Patents in docs/patents.md.

## Next

1. Speed. On 24 megapixels a change of a slider takes 0.24 s (Whites and
   Blacks) to 2.3 s (Texture) with 24 threads, and GIMP recomputes the
   whole image each time. Ideas, in order of gain: the Texture samples are
   0.06 apart (18 over 0 to 1), only the pixels in a sample's reach need
   its Laplacian (skip rows and tiles without them); the full-size copies
   in and out of GEGL (gegl_buffer_get and _set of 384 MB) are serial;
   GIMP's preview could run on a mipmap level (`level` is ignored now;
   the pyramid would have to scale the size by 2^level) for a fast
   preview and the full result on OK.
2. Dehaze: the noise and JPEG blocks of hazy skies are amplified by up to
   10 times. A base and detail split that limits the gain of the finest
   detail would help but comes close to Adobe's US 9,646,364 claim 1
   (suppressing edges of the radiance gradient); read that claim's
   dependent claims and the prosecution before trying it. Also: the
   estimate in the image's own channels (the result differs a little
   between RGB spaces for the same colours); estimating in a fixed space
   would make it independent.
3. Whites and Blacks: Lightroom's sliders adapt to the image (its Auto
   sets them from the histogram); an "Auto" that sets the ends from the
   image's own white and black points, soft, would be this filter's own.
   Highlights and Shadows were left out: gegl:shadows-highlights covers
   them locally.
4. Read the claims not read (docs/patents.md): Adobe's 2010 tone-mapping
   patents of Eric Chan (US 9,390,484, US 9,230,312, US 9,070,044) and the
   non-Chinese hits for "local laplacian filter" (Irystec, Intel,
   Shinyfields, Dolby). Google Patents stopped answering after many
   queries.
5. Offer the operations to GEGL (operations/common-gpl3+, since the
   detail term is darktable's), as `gegl:clarity`, `gegl:texture`,
   `gegl:dehaze` and `gegl:whites-blacks` without `gimp:menu-path`, with
   reference hashes; GIMP issue for the menu places.
6. A "develop" meta-op bundling the four (photoshop-gaps.md item 6
   suggests it), if people ask; each is already one filter, and GIMP's
   filter stack orders them.

## Found along the way (to report upstream)

- The `gegl` program (GEGL 0.4.72 in the GIMP 3.2.6 Flatpak) warns
  "1 GeglBuffers leaked" after any operation that caches its whole
  result through `get_cached_region`, GEGL's own `gegl:stretch-contrast`
  too; in a program of one's own (tests/check.c) nothing leaks.
- GIMP 3.2.6 Flatpak on Broadway: G'MIC-Qt (the org.gimp.GIMP.Plugin.GMic
  extension) crashes at start in the isolated test profile ("Plug-in
  crashed: gmic_gimp_qt"); the GUI tests load only GIMP's own plug-ins.
  Not investigated further.
- GIMP keeps the filter dialogs' "Merge filter" choice between runs in
  `tool-options/gimp-operation-tool`; a test that clicks it once gets
  merged (destructive) filters from then on. tests/gui/start.sh removes
  the file before each start.
- babl's luminance of the sRGB primaries is D50 adapted (0.2225, 0.7169,
  0.0606), not Rec. 709's (0.2126, 0.7152, 0.0722); these operations
  use babl's.
- `gegl_color_new ("rgb(0.5,0.25,0.125)")` gives linear values 0.5, 0.25,
  0.125, not CSS's sRGB encoding.
- gimp-devtools/docs/photoshop-gaps.md: the Microsoft dark channel patent
  is expired (fees), not "about 2031-06".

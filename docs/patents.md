# Patents checked (2026-09-28)

Claims and legal status read on the Google Patents page of each patent
(the claims text, the "status" and "expiration" fields and the legal
events). Searches through Google Patents' own query interface. Not legal
advice; a record of what was found, so that the next person does not have
to start over. Earlier checks in ../gegl-underwater/docs/patents-transmission.md
(same author) cover the transmission and depth priors of underwater work.

## Bottom line

- **Dark channel prior** (He, Sun, Tang): Microsoft's US 8,340,461 B2
  (priority 2010-02-01) is "Expired - Fee Related" (read again for this
  repository). gimp-devtools/docs/photoshop-gaps.md still says "about
  2031-06 if fees were paid"; the fees were not paid.
- **Guided filter** (He, Sun, Tang 2010; the fast form, He and Sun 2015):
  no patent found. Google Patents lists 12 patents with inventor Kaiming He
  and assignee Microsoft: dark channel haze removal (above), matting (2),
  object detection, pooling, segmentation (3), rotation, completion,
  opacity. None on the guided filter.
- **Local Laplacian filter** (Paris, Hasinoff, Kautz 2011; fast form,
  Aubry et al. 2014): no patent found. A search for "local laplacian"
  with assignee Adobe returns nothing; inventor Sylvain Paris with
  "laplacian" gives three Adobe patents, none on it (gradient-domain
  residuals 2009, stylization 2008, US 8,520,972 B2 "Image decomposition",
  which claims segmenting into image cues with matte contour components).
  A search for "local laplacian filter" over all assignees gives 43
  results, mostly Chinese university filings using it inside other
  methods (fusion, tone mapping with networks); the claims of the few
  non-Chinese ones (Irystec real-time tone mapping, Intel hybrid tone
  mapping, Shinyfields content-aware enhancement, Dolby edge-preserving
  filtering) were **not read**. darktable ships a local Laplacian filter
  (GPL-3.0-or-later) since 2016.
- **Adobe's dehaze patents**, read because Camera Raw's Dehaze is the
  model for this filter; none reads on it:
  - US 9,305,339 B2, "Multi-feature image haze removal" (2014-07-01,
    active to 2034-07-01): transmission from a **trained predictive model**
    on several feature maps. This filter uses no learned model.
  - US 9,508,126 B2, "Image haze removal using fast constrained
    transmission estimation" (2015-02-17, active to 2035-03-14): claim 1
    needs the transmission from a **fractionally scaled variant of each
    pixel** with an image-dependent scaling factor (the bound
    t >= (1 - A/I) / (1 - sA/I), claim 4). This filter takes the
    transmission from the patch-wise dark channel of I / A, as He et al.
    published in 2009, before that filing.
  - US 9,508,129 B1 and US 9,646,364 B1, "Dehazing photos and videos using
    visual artifact suppression" (2015-11-06, active): claim 1 of
    US 9,646,364 needs generating the output while **suppressing edges of
    the radiance gradient** from appearing in it (an artifact layer, an
    edge constraint). This filter has no such step (and a base and detail
    split that limits the gain of fine detail, considered for the noise in
    hazy skies, was left out because it would come close).
  - US 10,692,197 B2 (2016-08-20, active to 2036-10-02): haze correction
    parameters **across video frames** from dark channel, brightness and
    airlight statistics. Single images here.
  - US 9,031,345 B2 (2013-03-11, active to 2033-05-28): dehazing refined by
    **optical flow** between images. Single images here.
- **Others met on the way:** US 9,870,600 B2 (University of California,
  2015, active to 2036): dehazing of **mosaiced raw** sensor data before
  demosaicing; GIMP layers are demosaiced RGB. US 9,639,916 B2 (Nippon
  Systemware, dark channel with a binarised transmittance correction):
  expired for fees. US 2026/0010993 A1 (Samsung, pending): multi-frame
  capture with a backlit segmentation map; not a single image.
- **Morphological opening of the dark channel** (this filter's halo fix):
  a US search for "dark channel" with "morphological opening" finds only
  the Samsung application above, with "opening operation" or "dilation"
  nothing on dehazing that is in force. Chinese filings were not
  searched for this; many Chinese dark channel patents exist (the
  underwater notes list some) and apply in China only.
- **Whites and Blacks** (a tone curve on lightness with fixed midtones):
  nothing searched specifically; tone curves are old and the curve here
  is a plain cubic. Adobe's Eric Chan has tone-mapping patents from 2010
  (US 9,390,484 B2, US 9,230,312 B2, US 9,070,044 B2 "Image adjustment");
  their claims were **not read** (Google Patents stopped answering after
  many queries). Worth reading before an upstream submission.

## What the filters do, against the above

- Clarity, Texture: a local Laplacian filter on CIE lightness with per
  level weights, darktable's detail curve, a coring of the change by its
  local RMS (Texture), colour kept by scaling the channels.
- Dehaze: the dark channel of I / A over a patch, followed by a maximum
  over the same patch (an opening), transmission 1 - that, refined by the
  guided filter on lightness with the coefficients upsampled (fast guided
  filter), airlight from the mean of the brightest 0.1 % of the dark
  channel, J = (I - A) / max (t, 0.1) + A, amount scaling 1 - t; adding
  haze by the same model with t raised to a power.

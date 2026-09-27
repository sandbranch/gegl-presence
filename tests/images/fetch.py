#!/usr/bin/env python3
# Downloads the test photos listed in manifest.json from Wikimedia Commons
# into this folder, as 3000 pixel wide renditions (the one marked
# "full_size" at its full size, for the timing on a real photo of more
# than 24 megapixels). All are CC0 or public domain (manifest.json names
# the author, license and page of each; the license was read on each
# page); they stay out of git to keep the repository small, and this
# script makes the same set anywhere.
#   tests/images/fetch.py            all photos
#   tests/images/fetch.py --width 1500
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
import sys
import time
import urllib.parse
import urllib.request

here = os.path.dirname(os.path.abspath(__file__))
width = int(sys.argv[sys.argv.index('--width') + 1]) if '--width' in sys.argv else 3000
UA = {'User-Agent': 'gegl-presence-test-fetch/0.1 (https://github.com/sandbranch/gegl-presence)'}


def api(params):
    params.update(format='json', action='query')
    url = 'https://commons.wikimedia.org/w/api.php?' + urllib.parse.urlencode(params)
    return json.load(urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60))


for photo in json.load(open(os.path.join(here, 'manifest.json'))):
    path = os.path.join(here, photo['file'])
    if os.path.exists(path):
        continue
    params = dict(titles=photo['title'], prop='imageinfo', iiprop='url')
    if not photo.get('full_size'):
        params['iiurlwidth'] = width
    pages = api(params)['query']['pages']
    info = next(iter(pages.values()))['imageinfo'][0]
    data = urllib.request.urlopen(urllib.request.Request(info.get('thumburl', info['url']), headers=UA),
                                  timeout=300).read()
    with open(path + '.part', 'wb') as f:
        f.write(data)
    os.rename(path + '.part', path)
    print('%-24s %6d kB  %s (%s)' % (photo['file'], len(data) // 1024, photo['author'][:40], photo['license']))
    time.sleep(1)

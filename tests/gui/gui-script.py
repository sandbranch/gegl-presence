# Runs inside GIMP on a Broadway display (tests/gui/start.sh). Opens the
# photo PRESENCE_GUI_PHOTO (a CC0 or public domain photo of tests/images),
# scaled to 1200 pixels across, or without it a generated picture, and
# shows it in a window with its layer selected.
#
# Then waits: when PRESENCE_GUI_OUT/save exists it saves the image as
# PRESENCE_GUI_OUT/gui-<name>.xcf (PRESENCE_GUI_NAME) and writes
# PRESENCE_GUI_OUT/saved; when PRESENCE_GUI_OUT/quit exists (or after 10
# minutes) it quits GIMP.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
import array
import math
import os
import time

import gi
gi.require_version('Gimp', '3.0')
gi.require_version('Gegl', '0.4')
from gi.repository import Gimp, Gegl, Gio  # noqa: E402

out = os.environ['PRESENCE_GUI_OUT']
name = os.environ.get('PRESENCE_GUI_NAME', 'gui')
photo = os.environ.get('PRESENCE_GUI_PHOTO', '')


def picture(W, H):
    """hazy hills: lighter and bluer with distance, some texture"""
    a = array.array('f')
    for y in range(H):
        for x in range(W):
            u, v = x / (W - 1), y / (H - 1)
            d = 1.0 - v
            haze = 1 - math.exp(-2.5 * d)
            tex = 0.5 + 0.5 * math.sin(x * 0.3) * math.sin(y * 0.23)
            base = (0.25 + 0.2 * tex, 0.35 + 0.15 * tex, 0.15)
            c = [b * (1 - haze) + 0.8 * haze for b in base]
            a.extend((c[0], c[1], c[2], 1.0))
    return a


if photo and os.path.exists(photo):
    image = Gimp.file_load(Gimp.RunMode.NONINTERACTIVE, Gio.File.new_for_path(photo))
    w, h = image.get_width(), image.get_height()
    if w > 1200:
        image.scale(1200, round(h * 1200 / w))
    layer = image.get_layers()[0]
else:
    W, H = 900, 600
    image = Gimp.Image.new(W, H, Gimp.ImageBaseType.RGB)
    layer = Gimp.Layer.new(image, 'picture', W, H, Gimp.ImageType.RGBA_IMAGE, 100,
                           Gimp.LayerMode.NORMAL)
    image.insert_layer(layer, None, 0)
    buf = layer.get_buffer()
    buf.set(Gegl.Rectangle.new(0, 0, W, H), "R'G'B'A float", picture(W, H).tobytes())
    buf.flush()
    layer.update(0, 0, W, H)
image.set_selected_layers([layer])
image.clean_all()
display = Gimp.Display.new(image)
Gimp.displays_flush()
with open(os.path.join(out, 'ready'), 'w') as f:
    f.write('ready\n')

t0 = time.time()
while not os.path.exists(os.path.join(out, 'quit')) and time.time() - t0 < 600:
    if os.path.exists(os.path.join(out, 'save')):
        os.remove(os.path.join(out, 'save'))
        xcf = os.path.join(out, 'gui-%s.xcf' % name)
        Gimp.file_save(Gimp.RunMode.NONINTERACTIVE, image, Gio.File.new_for_path(xcf), None)
        with open(os.path.join(out, 'saved'), 'w') as f:
            f.write(xcf + '\n')
    time.sleep(0.3)

pdb = Gimp.get_pdb()
quit_proc = pdb.lookup_procedure('gimp-quit')
quit_config = quit_proc.create_config()
quit_config.set_property('force', True)
quit_proc.run(quit_config)

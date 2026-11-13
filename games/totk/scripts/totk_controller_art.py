"""Our controller pictures on Tears of the Kingdom 1.4.3's pause menu page "Basic controls" and ability guides (make_button_icons.py
--our-art): the three pictures Controller_Console^w, Controller_Grip^w and Controller_Pro^w (580x340, ASTC 4x4 in the game) and
their three blurred shadows ControllerSh_*^s (BC4, 314x360, the left half of a mirrored silhouette). The art is the Breath of the
Wild one (botw_controller_art.py) with its buttons moved onto the places the game's own layouts (PaSystemController_00,
PaActionGuide_00) point at: the Console is the handheld drawing, the Grip and the Pro are a pad body of ours. The route is BC5 in
place: BC5 is 16 bytes a 4x4 block like ASTC, so the picture data, the format word and the channel selector change and the BNTX,
the archive and the resource size table keep their sizes. The grey is stored linear (BC5 is UNORM, the game's picture was sRGB),
the alpha in G, and the selector 02 02 02 03 reads RGB = R, A = G. The face buttons are labelled like the prompts: the button at
each Switch button's place shows the controller button that drives it (controls.txt applies). The tilted pictures
Controller_*1^w of the ability guides, their shadows and the 12 R/ZR masks of the ability guides (bands where the game highlights R
and ZR) are drawn too, and the d-pad shortcut icon, and the 15 popup letters of DoCommand_00 (the Switch's A B X Y become the
letters or symbols of the controller button that drives each), both the pane's own text and the message the game fills it from
(LayoutMsg/DoCommand_00.msbt of every Mals language archive). All are in place. Checked by games/totk/scripts/dev/picture_sheet.py.
"""
import math
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'botw' / 'scripts'))
import botw_controller_art as art   # noqa: E402
import botw_formats as bf           # noqa: E402
import botw_prompts as bp           # noqa: E402
from totk_prompt_art import ART     # noqa: E402  {'xbox': 'xbox', 'playstation': 'playstation', 'playstation-label': 'playstation'}

SIZE = (580, 340)
PICTURES = {'console': ('Controller_Console^w', 'ControllerSh_Console^s'),
            'grip': ('Controller_Grip^w', 'ControllerSh_Grip^s'),
            'pro': ('Controller_Pro^w', 'ControllerSh_Pro^s')}
ASTC_4X4_SRGB, BC5_UNORM, BC4_UNORM = 0x2d06, 0x1e01, 0x1d01
RGBA_SELECTOR, GREY_ALPHA = bytes([2, 3, 4, 5]), bytes([2, 2, 2, 3])
PICTURE_FACTS = (ASTC_4X4_SRGB, 580, 340, 8, 303104)    # format, width, height, block height, image size
SHADOW_FACTS = (BC4_UNORM, 314, 360, 16, 81920)
SHADOW_OFFSET, SHADOW_RADIUS, SHADOW_PASSES = (24, 10), 7, 3
CONSOLE_SHIFT = (2, 20)        # BOTW's front-view body, lights, logo and touchpad move by this on the Console picture
ANCHORS = {
    'console': dict(face=(520.0, 114.7), dx=17.8, dy=17.2, lstick=(60.5, 115.6), rstick=(521.6, 174.1), dpad=(60.5, 178.0), reach=17.8,
                    minus=(83.3, 80.0), plus=(496.7, 80.0), zl=(72.0, 46.5), l=(48.0, 56.5), zr=(507.0, 46.5), r=(531.0, 55.5)),
    'grip': dict(face=(393.9, 98.8), dx=22.8, dy=22.2, lstick=(185.6, 99.5), rstick=(396.9, 176.9), dpad=(184.7, 181.2), reach=22.8,
                 minus=(214.4, 53.3), plus=(364.4, 53.3), zl=(195.0, 13.5), l=(165.0, 28.5), zr=(384.0, 13.5), r=(414.0, 28.5)),
    'pro': dict(face=(403.6, 113.9), dx=33.35, dy=29.45, lstick=(171.6, 113.8), rstick=(349.7, 173.0), dpad=(223.6, 174.1), reach=20.8,
                minus=(236.7, 82.2), plus=(342.2, 82.2), zl=(195.0, 16.5), l=(165.0, 28.5), zr=(384.0, 15.5), r=(414.0, 28.5)),
}
# Grip and Pro bodies (ours, symmetric about x = 290, the shadow's mirror axis)
PAD_BODY = art.both_halves(art.outline((290, 44), ((262, 44), (240, 41), (222, 39)), ((202, 36), (188, 31), (160, 31)),
    ((122, 31), (96, 50), (86, 86)), ((78, 116), (72, 170), (74, 222)), ((76, 280), (100, 320), (140, 320)),
    ((176, 320), (194, 296), (210, 268)), ((226, 242), (254, 238), (290, 238))), 580)
PAD_BODY_PS = art.both_halves(art.outline((290, 34), (196, 34), ((156, 34), (128, 30), (108, 42)),
    ((88, 56), (78, 96), (76, 140)), ((74, 200), (76, 280), (96, 312)), ((112, 334), (150, 332), (170, 316)),
    ((190, 298), (204, 268), (218, 248)), ((234, 230), (258, 226), (290, 226))), 580)
PAD_TOUCHPAD = art.outline((256, 40), (324, 40), ((330, 40), (334, 44), (333, 50)), (325, 112), ((324, 117), (320, 120), (314, 120)),
    (266, 120), ((260, 120), (256, 117), (255, 112)), (247, 50), ((246, 44), (250, 40), (256, 40)))
PAD_LIGHTS = ([(243.5, 50), (247, 50), (255, 112), (251.5, 112)], [(336.5, 50), (333, 50), (325, 112), (328.5, 112)])
LINEAR = [round(255 * (v / 255 / 12.92 if v <= 10 else ((v / 255 + 0.055) / 1.055) ** 2.4)) for v in range(256)]

TILT_PICTURES = {'console': ('Controller_Console1^w', 'ControllerSh_Console1^s'),
                 'grip': ('Controller_Grip1^w', 'ControllerSh_Grip1^s'),
                 'pro': ('Controller_Pro1^w', 'ControllerSh_Pro1^s')}
TEXEL = (580 / 570 / 0.9, 1 / 0.9)          # picture pixels per mask texel
MASK_SELECTOR = bytes([1, 1, 1, 2])
MASKS = {   # kind -> shoulder -> (texture stem, (width, height, block height, image size), top-left in picture px, fill ink box in texels)
    'console': {'r': ('ConConsR', (76, 41, 2, 3072), (505.94, 47.22), (8, 7, 63, 33)),
                'zr': ('ConConsZR', (59, 30, 1, 1024), (505.38, 35.56), (9, 7, 50, 23))},
    'grip': {'r': ('ConGripR', (92, 48, 2, 3072), (363.50, 27.78), (9, 8, 81, 40)),
             'zr': ('ConGripZR', (74, 36, 1, 3072), (364.62, 13.33), (9, 7, 62, 27))},
    'pro': {'r': ('ConProR', (102, 44, 2, 4096), (349.93, 37.78), (6, 6, 96, 38)),
            'zr': ('ConProZR', (74, 49, 2, 3072), (377.06, 18.33), (8, 7, 66, 34))},
}
BAND_EDGE, BAND_TOP, BAND_STEPS = 0.15, 0.38, 24     # band thickness: of the width at its bottom-right end, of the height at its top-left end
LINE_IN, LINE_OUT = 2.0, 5.0                         # a line mask texel lies this far (texels) outside the fill
TILT = {   # the tilted pictures' anchors (picture pixels)
    'console': dict(body=(1.03, 0.6875, 29.7), by=0.798, face=(532.5, 114.82), dx=19.07, dy=13.72,
                    lstick=(44.2, 118.28), rstick=(531.4, 167.82), dpad=(49.2, 162.2)),
    'grip': dict(body=(1.0, 0.765, 26.3), by=0.767, face=(400.98, 114.09), dx=24.72, dy=17.02,
                 lstick=(176.25, 120.05), rstick=(400.15, 180.75), dpad=(178.1, 176.7)),
    'pro': dict(body=(1.0, 0.84, 48.0), by=0.880, face=(404.39, 146.28), dx=33.71, dy=25.92,
                lstick=(171.7, 151.95), rstick=(347.6, 203.8), dpad=(225.6, 197.8)),
}
TILT_SHADOW = {'console': (41, 4), 'grip': (35, 2), 'pro': (26, 2)}   # (oy, dx): shadow texel (sx, sy) shows picture pixel (round((sx - 29) * 580 / 570) + dx, sy - oy)
SHADOW_TAPER = 16             # a tilted shadow's last 16 rows fade to 0 (the Pro PlayStation grips reach the texture's bottom)
DPAD_MASKS = ('ButtonIconL46NN_00^s', 'ButtonIconL46NN_01^s')
DPAD_FACTS, DPAD_BOX, GLOW = (BC4_UNORM, 64, 64, 2, 2048), (12, 12, 52, 52), (3, 2, 0.99)   # glow: radius, passes, gain
POPUP = 'blyt/DoCommand_00.bflyt'
POPUP_MESSAGES = 'LayoutMsg/DoCommand_00.msbt'   # in every Mals/<language>.Product.*.sarc.zs
POPUP_LETTER = re.compile(r'T_([ABXY])_0[0-3]')
PS_SYMBOL = {'Cross': '\u00d7', 'Circle': '\u25cb', 'Square': '\u25a1', 'Triangle': '\u25b3'}


def spots(a):
    """The four face button places by Switch button: X north, A east, B south, Y west."""
    cx, cy = a['face']
    return {'x': (cx, cy - a['dy']), 'a': (cx + a['dx'], cy), 'b': (cx, cy + a['dy']), 'y': (cx - a['dx'], cy)}


def sizes(a):
    """(k, rf): the scale of the sticks and the d-pad (BOTW's d-pad arms end 17.5 px out) and the radius of a face button."""
    return a['reach'] / 17.5, min(12.5, 10 * min(a['dx'], a['dy']) / 17.5)


def shifted(points):
    return [(x + CONSOLE_SHIFT[0], y + CONSOLE_SHIFT[1]) for x, y in points]


def body_of(kind, ps):
    if kind == 'console':
        return shifted(art.FRONT_BODY_PS if ps else art.FRONT_BODY)
    return PAD_BODY_PS if ps else PAD_BODY


def buttons(kind):
    """name -> (centre, radius) of the round buttons of a picture."""
    a = ANCHORS[kind]
    k, rf = sizes(a)
    out = {'lstick': (a['lstick'], 27 * k), 'rstick': (a['rstick'], 27 * k), 'dpad': (a['dpad'], 31 * k),
           'minus': (a['minus'], 8.5), 'plus': (a['plus'], 8.5)}
    for name, centre in spots(a).items():
        out[name] = (centre, rf)
    return out


def face_labels(controls, style):
    """slot -> the name of the controller button that drives that Switch button (None: no name in this style)."""
    text = bp.PLAYSTATION_TEXT if ART[style] == 'playstation' else bp.XBOX_TEXT
    return {slot: text.get(controls.get(slot)) for slot in 'abxy'}


def decor(cv, kind, ps, face):
    """The guide button, touchpad, lights, share and mute buttons of a picture's body."""
    def ps_button(x, y):
        cv.paint([art.circle(x, y, 10.5)], art.LIGHT)
        cv.paint([art.circle(x, y, 9.3)], art.EDGE)
        cv.paint([art.circle(x, y, 7.8)], art.DARK)

    def mute(x0, y0, x1, y1):
        cv.paint([art.rounded_rect(x0, y0, x1, y1, 3.5)], art.LIGHT)
        cv.paint([art.rounded_rect(x0 + 1.3, y0 + 1.3, x1 - 1.3, y1 - 1.3, 2.2)], art.DARK)

    def share(x0, y0, x1, y1):
        cv.paint([art.rounded_rect(x0, y0, x1, y1, 4)], art.EDGE)
        cv.paint([art.rounded_rect(x0 + 1.7, y0 + 1.7, x1 - 1.7, y1 - 1.7, 3)], art.DARK)

    if kind == 'console' and ps:
        cv.paint([shifted(art.PANEL)], art.RIM, clip=[face])
        cv.paint([art.inset(shifted(art.PANEL), 1.5)], 118, clip=[face])
        cv.paint([shifted(art.TOUCHPAD)], art.RIM)
        cv.paint([art.inset(shifted(art.TOUCHPAD), 2)], 214)
        for side in art.CONSOLE_LIGHTS:      # BOTW's, before the shift
            cv.paint([shifted(side)], 236)
        ps_button(290, 172)
        mute(280, 190, 300, 197)
    elif kind == 'console':
        art.xbox_logo(cv, 290, 90, 14.5)
        share(282, 121, 298, 131)
    elif ps:
        cv.paint([PAD_TOUCHPAD], art.RIM)
        cv.paint([art.inset(PAD_TOUCHPAD, 2)], 214)
        for side in PAD_LIGHTS:
            cv.paint([side], 236)
        ps_button(290, 142)
        mute(280, 160, 300, 167)
    else:
        art.xbox_logo(cv, 290, 72, 13)
        share(282, 99, 298, 109)


class Placed(art.Canvas):
    """A Canvas whose shapes first go through the affine map self.m = (sx, sy, ox, oy): (x, y) -> (ox + sx x, oy + sy y)."""
    def __init__(self, w, h):
        super().__init__(w, h)
        self.m = (1.0, 1.0, 0.0, 0.0)

    def place(self, shapes):
        sx, sy, ox, oy = self.m
        return [[(ox + sx * x, oy + sy * y) for x, y in s] for s in shapes]


def draw(kind, style, controls, letters):
    """((grey rows, alpha rows), notes) of one picture, 580x340."""
    a = ANCHORS[kind]
    look = ART[style]
    ps = look == 'playstation'
    text = bp.PLAYSTATION_TEXT if ps else bp.XBOX_TEXT
    k, rf = sizes(a)
    body = body_of(kind, ps)
    cv = art.Canvas(*SIZE)
    notes = []
    for s in ('zl', 'zr'):                                                          # 1. triggers
        x, y = a[s]
        t = art.rounded_rect(x - 20 * k, y - 3, x + 20 * k, y + 19, 8)
        cv.paint([t], art.EDGE)
        cv.paint([art.inset(t, 1.7)], art.DARK)
    lift, reach = (7, (174, 100)) if (kind == 'console' and ps) else (9, (188, 116)) if kind == 'console' else (9, (232, 100))
    edge, dark = art.bumper(body, lift, reach, SIZE[0])                              # 2. bumpers
    cv.paint(edge, art.EDGE)
    cv.paint(dark, art.DARK)
    cv.paint([body], art.RIM)                                                       # 3. body
    face = art.inset(body, 3.2)
    cv.paint([face], art.BODY)
    decor(cv, kind, ps, face)                                                       # 4. decor
    art.stick(cv, *a['lstick'], k)                                                  # 5. sticks
    art.stick(cv, *a['rstick'], k)
    (art.arrow_pad if ps else art.dpad)(cv, *a['dpad'], k)                          # 6. d-pad
    labels = face_labels(controls, style)
    for slot, (x, y) in spots(a).items():                                           # 7. face buttons
        art.dark_button(cv, x, y, rf, letters, labels[slot])
        if labels[slot] is None:
            notes.append(f'Switch {slot.upper()}: {controls.get(slot) or "nothing"} has no {look} name; its picture button has no label')
    for slot in ('minus', 'plus'):                                                  # 8. menus
        art.center_button(cv, *a[slot], controls.get(slot), letters, text, 1.0, look)
    return cv.rows(), notes


def picture_bc5(grey, alpha, original, block_height):
    """The BC5 image of a picture (grey rows, alpha rows): R = the grey in linear light, G = the alpha."""
    out = bytearray(original)
    w, h = SIZE
    rim = LINEAR[art.RIM]
    for by in range(h // 4):
        for bx in range(w // 4):
            r, g = [], []
            for y in range(by * 4, by * 4 + 4):
                grow, arow = grey[y], alpha[y]
                for x in range(bx * 4, bx * 4 + 4):
                    g.append(arow[x])
                    r.append(LINEAR[grow[x]] if arow[x] > 0 else rim)
            at = bf.gob_address(bx, by, w // 4, 16, block_height)
            out[at:at + 8] = bf.bc4_encode(r)
            out[at + 8:at + 16] = bf.bc4_encode(g)
    return bytes(out)


def _box_pass(rows, radius=SHADOW_RADIUS):
    """Each value is the sum of the window of 2 * radius + 1 values round it (clamped at the edge) over that count."""
    n, r, size = len(rows[0]), radius, 2 * radius + 1
    out = []
    for row in rows:
        total = [0.0]
        for v in row:
            total.append(total[-1] + v)
        out.append([(total[min(n, x + r + 1)] - total[max(0, x - r)]) / size for x in range(n)])
    return out


def _blur(grid, radius, passes):
    """`passes` box passes along the rows, then `passes` along the columns."""
    for _ in range(passes):
        grid = _box_pass(grid, radius)
    for _ in range(passes):
        columns = _box_pass([list(c) for c in zip(*grid)], radius)
        grid = [list(r) for r in zip(*columns)]
    return grid


def shadow_rows(alpha):
    """The shadow's rows (bytes, 314 values): the silhouette moved by SHADOW_OFFSET on a 628x360 grid and blurred."""
    gw, gh = 628, 360
    ox, oy = SHADOW_OFFSET
    grid = [[0.0] * gw for _ in range(gh)]
    for y, row in enumerate(alpha):
        if 0 <= y + oy < gh:
            for x, v in enumerate(row):
                if 0 <= x + ox < gw:
                    grid[y + oy][x + ox] = float(v)
    grid = _blur(grid, SHADOW_RADIUS, SHADOW_PASSES)
    return [bytes(min(255, round(v)) for v in row[:314]) for row in grid]


def shadow_bc4(rows, original, block_height):
    return bc4_texture(rows, SHADOW_FACTS[1], SHADOW_FACTS[2], block_height, original)


def _differs(bntx, brti, t, facts, selector):
    got = (t.format, t.width, t.height, t.block_height, t.image_size)
    if got != facts:
        return f'format/width/height/block height/image size {got} where the game has {facts}'
    if (t.mips, t.layers, t.depth, t.tile_mode) != (1, 1, 1, 0):
        return f'mips/layers/depth/tile mode {(t.mips, t.layers, t.depth, t.tile_mode)}'
    if selector and bytes(bntx[brti + 0x58:brti + 0x5c]) != selector:
        return f'channel selector {bytes(bntx[brti + 0x58:brti + 0x5c]).hex()}'
    return None


def redraw_pictures(bntx, controls, style, letters):
    """(the BNTX bytes, same length, with the controller pictures redrawn; the picture names drawn; notes). A kind whose
    textures differ from the facts above keeps the Switch picture and gets a note."""
    textures = bf.bntx_textures(bntx)
    at = {name: t.brti for name, t in textures.items()}
    out, drawn, notes = bytearray(bntx), [], []
    for kind, (picture, shadow) in PICTURES.items():
        problem = None
        for name, facts, selector in ((picture, PICTURE_FACTS, RGBA_SELECTOR), (shadow, SHADOW_FACTS, None)):
            if name not in textures:
                problem = f'{name} is not in the layout archive'
            else:
                found = _differs(bntx, at[name], textures[name], facts, selector)
                problem = f'{name}: {found}' if found else None
            if problem:
                break
        if problem:
            notes.append(f'{kind}: {problem}; the Switch picture stays')
            continue
        (grey, alpha), more = draw(kind, style, controls, letters)
        notes += more
        t, s = textures[picture], textures[shadow]
        out[t.offset:t.offset + t.image_size] = picture_bc5(grey, alpha, bntx[t.offset:t.offset + t.image_size], t.block_height)
        struct.pack_into('<I', out, at[picture] + 0x1C, BC5_UNORM)
        out[at[picture] + 0x58:at[picture] + 0x5c] = GREY_ALPHA
        out[s.offset:s.offset + s.image_size] = shadow_bc4(shadow_rows(alpha), bntx[s.offset:s.offset + s.image_size], s.block_height)
        drawn.append(picture)
    assert len(out) == len(bntx)
    return bytes(out), drawn, list(dict.fromkeys(notes))


def _inside(polygon, x, y):
    c = False
    for i in range(len(polygon)):
        (x0, y0), (x1, y1) = polygon[i - 1], polygon[i]
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            c = not c
    return c


def layout_problems(kind, style, alpha):
    """Strings for what is wrong with a picture's layout (an empty list is ok): a button not inside the body, a shoulder anchor
    with no paint."""
    import math
    a = ANCHORS[kind]
    inner = art.inset(body_of(kind, ART[style] == 'playstation'), 3.2)
    margin = {'lstick': 1, 'rstick': 1, 'dpad': 1, 'x': 2, 'a': 2, 'b': 2, 'y': 2}
    problems = []
    for name, ((cx, cy), radius) in buttons(kind).items():
        r = 10.5 if name in ('minus', 'plus') else radius + margin[name]
        if not all(_inside(inner, cx + r * math.cos(t * math.pi / 16), cy + r * math.sin(t * math.pi / 16)) for t in range(32)):
            problems.append(f'{name} is not inside the body')
    for name in ('zl', 'l', 'zr', 'r'):
        x, y = int(a[name][0]), int(a[name][1])
        if max(alpha[y + dy][x + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1)) < 200:
            problems.append(f'{name} anchor has no paint')
    return problems


# ---- the tilted pictures of the ability guides ----
def band(box):
    """The shoulder button inside a mask box (u0, v0, u1, v1; y down): a quarter of an elliptic ring round the box's bottom-left corner."""
    u0, v0, u1, v1 = box
    a, b = u1 - u0, v1 - v0
    outer = [(u0 + a * math.cos(math.radians(90 - 90 * i / BAND_STEPS)), v1 - b * math.sin(math.radians(90 - 90 * i / BAND_STEPS)))
             for i in range(BAND_STEPS + 1)]
    inner = [(u0 + (1 - BAND_EDGE) * a * math.cos(math.radians(90 * i / BAND_STEPS)), v1 - (1 - BAND_TOP) * b * math.sin(math.radians(90 * i / BAND_STEPS)))
             for i in range(BAND_STEPS + 1)]
    return outer + inner


def shoulder(kind, s):
    """(right, left) band contours of shoulder s ('r' or 'zr') in picture pixels."""
    _, _, (ox, oy), box = MASKS[kind][s]
    right = [(ox + u * TEXEL[0], oy + v * TEXEL[1]) for u, v in band(box)]
    return right, art.mirror(right, 580)


def shoulder_mid(kind, s):
    """The middle of the right band in picture pixels (the left one is (580 - x, y))."""
    _, _, (ox, oy), (u0, v0, u1, v1) = MASKS[kind][s]
    c = math.cos(math.radians(45))
    return (ox + (u0 + (1 - BAND_EDGE / 2) * (u1 - u0) * c) * TEXEL[0], oy + (v1 - (1 - BAND_TOP / 2) * (v1 - v0) * c) * TEXEL[1])


def tilt_point(kind, p):
    kx, ky, ty = TILT[kind]['body']
    return 290 + kx * (p[0] - 290), ty + ky * p[1]


def tilt_spots(kind):
    t = TILT[kind]
    cx, cy = t['face']
    return {'x': (cx, cy - t['dy']), 'a': (cx + t['dx'], cy), 'b': (cx, cy + t['dy']), 'y': (cx - t['dx'], cy)}


def tilt_buttons(kind):
    """name -> (centre, radius) of the round buttons of a tilted picture (the radius is across; y is squashed by TILT's by)."""
    a, t = ANCHORS[kind], TILT[kind]
    k, rf = sizes(a)
    out = {'lstick': (t['lstick'], 27 * k), 'rstick': (t['rstick'], 27 * k), 'dpad': (t['dpad'], 31 * k)}
    for slot in ('minus', 'plus'):
        out[slot] = (tilt_point(kind, a[slot]), 8.5)
    for name, centre in tilt_spots(kind).items():
        out[name] = (centre, rf)
    return out


def draw_tilted(kind, style, controls, letters):
    """((grey rows, alpha rows), notes) of a tilted picture, 580x340."""
    a, t = ANCHORS[kind], TILT[kind]
    look = ART[style]
    ps = look == 'playstation'
    text = bp.PLAYSTATION_TEXT if ps else bp.XBOX_TEXT
    k, rf = sizes(a)
    kx, ky, ty = t['body']
    by = t['by']
    front = body_of(kind, ps)
    body = [tilt_point(kind, p) for p in front]
    cv = Placed(*SIZE)
    notes = []
    cv.paint([body], art.RIM)                                                       # 1. body
    cv.paint([art.inset(body, 3.2)], art.BODY)
    cv.m = (kx, ky, 290 * (1 - kx), ty)                                             # 2. decor
    decor(cv, kind, ps, art.inset(front, 3.2))
    cv.m = (1.0, 1.0, 0.0, 0.0)
    for s in ('zr', 'r'):                                                           # 3. shoulders
        right, left = shoulder(kind, s)
        cv.paint([right, left], art.EDGE)
        cv.paint([art.inset(right, 1.7), art.inset(left, 1.7)], art.DARK)
    for s in ('lstick', 'rstick'):                                                  # 4. sticks
        cv.m = (1.0, by, *t[s])
        art.stick(cv, 0, 0, k)
    cv.m = (1.0, by, *t['dpad'])                                                    # 5. d-pad
    (art.arrow_pad if ps else art.dpad)(cv, 0, 0, k)
    labels = face_labels(controls, style)
    for slot, (x, y) in tilt_spots(kind).items():                                   # 6. face buttons
        cv.m = (1.0, by, x, y)
        art.dark_button(cv, 0, 0, rf, letters, labels[slot])
        if labels[slot] is None:
            notes.append(f'Switch {slot.upper()}: {controls.get(slot) or "nothing"} has no {look} name; its picture button has no label')
    for slot in ('minus', 'plus'):                                                  # 7. menus
        cv.m = (1.0, by, *tilt_point(kind, a[slot]))
        art.center_button(cv, 0, 0, controls.get(slot), letters, text, 1.0, look)
    return cv.rows(), notes                                                         # 8.


def tilt_problems(kind, style, alpha):
    """Strings for what is wrong with a tilted picture (an empty list is ok): a button outside the body, a band middle with no
    paint, alpha on the picture's outer pixel border."""
    inner = art.inset([tilt_point(kind, p) for p in body_of(kind, ART[style] == 'playstation')], 3.2)
    by = TILT[kind]['by']
    margin = {'lstick': 1, 'rstick': 1, 'dpad': 1, 'x': 2, 'a': 2, 'b': 2, 'y': 2}
    problems = []
    for name, ((cx, cy), radius) in tilt_buttons(kind).items():
        r = 10.5 if name in ('minus', 'plus') else radius + margin[name]
        if not all(_inside(inner, cx + r * math.cos(i * math.pi / 16), cy + by * r * math.sin(i * math.pi / 16)) for i in range(32)):
            problems.append(f'{name} is not inside the body')
    for s in ('zr', 'r'):
        x, y = shoulder_mid(kind, s)
        for side, px in (('right', x), ('left', 580 - x)):
            if max(alpha[int(y) + dy][int(px) + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1)) < 200:
                problems.append(f'{s} {side} band has no paint')
    if any(alpha[0]) or any(alpha[339]) or any(row[0] or row[579] for row in alpha):
        problems.append('alpha on the picture\'s outer pixel border')
    return problems


# ---- encoders ----
def bc4_texture(rows, width, height, block_height, original):
    """The BC4 image (bytes of the original's length) of rows (width x height values) in place: texels outside are 0."""
    out = bytearray(original)
    bw, bh = (width + 3) // 4, (height + 3) // 4
    for by in range(bh):
        for bx in range(bw):
            values = [rows[by * 4 + i // 4][bx * 4 + i % 4] if bx * 4 + i % 4 < width and by * 4 + i // 4 < height else 0 for i in range(16)]
            at = bf.gob_address(bx, by, bw, 8, block_height)
            out[at:at + 8] = bf.bc4_encode(values)
    return bytes(out)


def dilate(rows, r):
    """Each texel becomes the maximum over the texels within distance r (inside the grid)."""
    h, w = len(rows), len(rows[0])
    reach = int(r)
    out = [[0] * w for _ in range(h)]
    for dy in range(-reach, reach + 1):
        for dx in range(-reach, reach + 1):
            if dx * dx + dy * dy > r * r:
                continue
            for y in range(max(0, -dy), min(h, h - dy)):
                src, dst = rows[y + dy], out[y]
                for x in range(max(0, -dx), min(w, w - dx)):
                    if src[x + dx] > dst[x]:
                        dst[x] = src[x + dx]
    return out


def mask_rows(kind, s):
    """(fill rows, line rows) of the R/ZR mask pair of a shoulder, the texture's size."""
    _, (w, h, _, _), _, box = MASKS[kind][s]
    fill = bf.coverage_rect([band(box)], w, h)
    outer, inner = dilate(fill, LINE_OUT), dilate(fill, LINE_IN)
    return fill, [bytes(max(0, a - b) for a, b in zip(ro, ri)) for ro, ri in zip(outer, inner)]


def tilted_shadow_rows(alpha, kind):
    """The tilted shadow's rows (bytes, 314 values): the silhouette moved by TILT_SHADOW on a 628x360 grid, blurred, with a taper."""
    oy, dx = TILT_SHADOW[kind]
    cols = [round((sx - 29) * 580 / 570) + dx for sx in range(628)]
    grid = [[float(alpha[sy - oy][c]) if 0 <= sy - oy < 340 and 0 <= c < 580 else 0.0 for c in cols] for sy in range(360)]
    grid = _blur(grid, SHADOW_RADIUS, SHADOW_PASSES)
    for sy in range(360 - SHADOW_TAPER, 360):
        grid[sy] = [v * (359 - sy) / SHADOW_TAPER for v in grid[sy]]
    return [bytes(min(255, round(v)) for v in row[:314]) for row in grid]


def glow(rows):
    """The d-pad icon's glow: a blur of the icon (radius 3, 2 passes) times 0.99."""
    radius, passes, gain = GLOW
    return [bytes(max(0, min(255, round(gain * v))) for v in row) for row in _blur([list(r) for r in rows], radius, passes)]


def redraw_tilted(bntx, controls, style, letters):
    """(the BNTX bytes, same length, with the tilted pictures, their shadows and R/ZR masks redrawn; the picture names drawn;
    notes). A kind whose six textures differ from the facts keeps the Switch art and gets a note."""
    textures = bf.bntx_textures(bntx)
    at = {name: t.brti for name, t in textures.items()}
    out, drawn, notes = bytearray(bntx), [], []
    for kind, (picture, shadow) in TILT_PICTURES.items():
        checks = [(picture, PICTURE_FACTS, RGBA_SELECTOR), (shadow, SHADOW_FACTS, None)]
        for stem, (w, h, bh, size), _, _ in MASKS[kind].values():
            checks += [(f'{stem}_{part}^s', (BC4_UNORM, w, h, bh, size), MASK_SELECTOR) for part in ('00', 'Line')]
        problem = None
        for name, facts, selector in checks:
            if name not in textures:
                problem = f'{name} is not in the layout archive'
            else:
                found = _differs(bntx, at[name], textures[name], facts, selector)
                problem = f'{name}: {found}' if found else None
            if problem:
                break
        if problem:
            notes.append(f'{kind} tilted: {problem}; the Switch picture and masks stay')
            continue
        (grey, alpha), more = draw_tilted(kind, style, controls, letters)
        notes += more
        t, sh = textures[picture], textures[shadow]
        out[t.offset:t.offset + t.image_size] = picture_bc5(grey, alpha, bntx[t.offset:t.offset + t.image_size], t.block_height)
        struct.pack_into('<I', out, at[picture] + 0x1C, BC5_UNORM)
        out[at[picture] + 0x58:at[picture] + 0x5c] = GREY_ALPHA
        out[sh.offset:sh.offset + sh.image_size] = shadow_bc4(tilted_shadow_rows(alpha, kind), bntx[sh.offset:sh.offset + sh.image_size], sh.block_height)
        for s, (stem, (w, h, bh, size), _, _) in MASKS[kind].items():
            for part, rows in zip(('00', 'Line'), mask_rows(kind, s)):
                m = textures[f'{stem}_{part}^s']
                out[m.offset:m.offset + size] = bc4_texture(rows, w, h, bh, bntx[m.offset:m.offset + size])
        drawn.append(picture)
    assert len(out) == len(bntx)
    return bytes(out), drawn, list(dict.fromkeys(notes))


def dpad_masks(bntx, style, letters, drawer=None):
    """(the BNTX bytes, same length, with the d-pad shortcut icon ButtonIconL46NN_00^s and its glow _01^s redrawn; notes)."""
    textures = bf.bntx_textures(bntx)
    at = {name: t.brti for name, t in textures.items()}
    for name in DPAD_MASKS:
        found = f'{name} is not in the layout archive' if name not in textures else _differs(bntx, at[name], textures[name], DPAD_FACTS, MASK_SELECTOR)
        if found:
            return bntx, [f'd-pad shortcut icon: {found}; the Switch icon stays']
    drawer = drawer or bp.drawers_of(ART[style], letters)[0xE049]
    icon = bp.render(drawer, 64, DPAD_BOX)
    out = bytearray(bntx)
    for name, rows in zip(DPAD_MASKS, (icon, glow(icon))):
        t = textures[name]
        out[t.offset:t.offset + t.image_size] = bc4_texture(rows, 64, 64, t.block_height, bntx[t.offset:t.offset + t.image_size])
    assert len(out) == len(bntx)
    return bytes(out), []


# ---- the action popup letters ----
def popup_plan(controls, style):
    """({Switch slot: the one character of the controller button that drives it}, notes)."""
    look = ART[style]
    plan, notes = {}, []
    for slot, label in face_labels(controls, style).items():
        char = PS_SYMBOL.get(label) if look == 'playstation' else label if label and len(label) == 1 else None
        if char is None:
            notes.append(f'action popup: Switch {slot.upper()} ({controls.get(slot) or "nothing"}) has no one-character {look} name; its letter is blank')
        plan[slot] = char or ' '
    return plan, notes


def popup_letters(blyt, controls, style):
    """(the DoCommand_00 bytes, same length, with the 15 letter panes T_<A|B|X|Y>_0<0-3> rewritten; notes)."""
    plan, notes = popup_plan(controls, style)
    head, count = struct.unpack_from('<H', blyt, 6)[0], struct.unpack_from('<H', blyt, 0x10)[0]
    pos, found, problem = head, [], None
    for _ in range(count):
        magic, size = bytes(blyt[pos:pos + 4]), struct.unpack_from('<I', blyt, pos + 4)[0]
        if magic == b'txt1':
            name = bytes(blyt[pos + 12:pos + 36]).split(b'\0')[0].decode()
            m = POPUP_LETTER.fullmatch(name)
            if m:
                buffer, string, at = struct.unpack_from('<HH', blyt, pos + 84) + (pos + struct.unpack_from('<I', blyt, pos + 100)[0],)
                own = m.group(1).encode('utf-16-le') + b'\0\0'
                if (buffer, string) != (4, 4) or bytes(blyt[at:at + 4]) != own:
                    problem = f'{name} is not a 4 byte buffer holding its own letter'
                found.append((m.group(1).lower(), at))
        pos += size
    if len(found) != 15 or problem:
        return bytes(blyt), [f'action popup: {problem or f"{len(found)} letter panes, not 15"}; the Switch letters stay']
    out = bytearray(blyt)
    for letter, at in found:
        out[at:at + 2] = plan[letter].encode('utf-16-le')
    assert len(out) == len(blyt)
    return bytes(out), notes


def popup_messages(msbt, controls, style):
    """(the Mals/<language> LayoutMsg/DoCommand_00.msbt bytes, same length, with the 15 letter texts T_<A|B|X|Y>_0<0-3> rewritten; notes).
    The game fills each text pane of the popup from this message (label = pane name), so the pane letters of popup_letters are not enough."""
    plan, notes = popup_plan(controls, style)

    def keep(why):
        return bytes(msbt), [f'action popup messages: {why}; the Switch letters stay']
    try:
        if bytes(msbt[:8]) != b'MsgStdBn' or bytes(msbt[8:10]) != b'\xff\xfe':
            return keep('not a little-endian MsgStdBn file')
        pos, sections = 0x20, {}
        for _ in range(struct.unpack_from('<H', msbt, 0x0e)[0]):
            size = struct.unpack_from('<I', msbt, pos + 4)[0]
            sections[bytes(msbt[pos:pos + 4])] = (pos + 16, size)
            pos = (pos + 16 + size + 15) & ~15
        if b'LBL1' not in sections or b'TXT2' not in sections:
            return keep('LBL1 or TXT2 is missing')
        lbl, (txt, txt_size) = sections[b'LBL1'][0], sections[b'TXT2']
        count = struct.unpack_from('<I', msbt, txt)[0]
        offsets = list(struct.unpack_from(f'<{count}I', msbt, txt + 4)) + [txt_size]
        found = []
        for slot in range(struct.unpack_from('<I', msbt, lbl)[0]):
            labels, at = struct.unpack_from('<II', msbt, lbl + 4 + 8 * slot)
            at += lbl
            for _ in range(labels):
                length = msbt[at]
                name = bytes(msbt[at + 1:at + 1 + length]).decode('ascii')
                index = struct.unpack_from('<I', msbt, at + 1 + length)[0]
                at += 5 + length
                m = POPUP_LETTER.fullmatch(name)
                if m:
                    start = txt + offsets[index] if index < count else 0
                    if index >= count or offsets[index + 1] - offsets[index] != 4 or bytes(msbt[start:start + 4]) != m.group(1).encode('utf-16-le') + b'\0\0':
                        return keep(f'{name} is not a 4 byte text holding its own letter')
                    found.append((m.group(1).lower(), start))
    except (struct.error, IndexError, UnicodeDecodeError):
        return keep('the file is cut short or malformed')
    if len(found) != 15:
        return keep(f'{len(found)} letter texts, not 15')
    out = bytearray(msbt)
    for letter, start in found:
        out[start:start + 2] = plan[letter].encode('utf-16-le')
    assert len(out) == len(msbt)
    return bytes(out), notes

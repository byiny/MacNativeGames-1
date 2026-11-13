#!/usr/bin/env python3
"""Check the controller pictures of an icon set made with make_button_icons.py --our-art against the game's own layouts, and draw a sheet.

usage: picture_sheet.py --romfs R --mod SET --style xbox|playstation|playstation-label [--face-buttons position|label]
                        [--controls controls.txt] [--firmware DIR] [--before SET] --out sheet.png

Reads the original and the set's Common layout archive, and prints ok or FAIL for each item (exit 1 on any FAIL): the archive and
its BNTX keep their lengths; the three pictures Controller_{Console,Grip,Pro}^w are BC5 with the selector 02 02 02 03 and nothing
else of their BRTI changed; their shadows are unchanged BRTIs; no other BNTX byte changed (but the picture and shadow images and
the eight button guide masks of prompt_sheet.py); the decoded pictures and shadows match a new run of totk_controller_art.py;
layout_problems() is empty; and the game's own layouts agree with the art: on the "Basic controls" page (PaSystemController_00,
frames 0 1 2) the shoulder, menu and stick dots, the two diamonds, and in the ability guide (PaActionGuide_00, PatternA B C) the
24 rings of the frames that show Pa_Controller_00. The PNG shows the decoded pictures with the dots (red), the diamonds (red)
and the rings (cyan). Never prints a font key. Game data stays in the folder you name.
"""
import argparse
import io
import math
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import prompt_sheet as ps                                                           # noqa: E402
import make_button_icons as mbi                                                    # noqa: E402
import totk_prompt_art as tpa                                                      # noqa: E402
import totk_controller_art as tca                                                  # noqa: E402
import botw_formats as bf                                                          # noqa: E402
from fontTools.ttLib import TTFont                                                 # noqa: E402
from make_mod_manifest import read_index                                           # noqa: E402
from prompt_layout import prompt_controls                                # noqa: E402

KINDS = ('console', 'grip', 'pro')
CELL, LABEL = (600, 430), ('Console (handheld)', 'Grip (not reached)', 'Pro (docked)')
MASK_PANE = re.compile(r'P_Guide([123])(Cons|Grip|Pro)_(ZR|R)(Line)?_00')
MAGENTA = (230, 40, 200)
# Round-trip limits (max, mean) of a decoded mask against mask_rows(). BC4 holds 8 levels per 4x4 block and a line mask's soft tail
# has about 60 values, so its mean error is 0.6 to 0.9 (the fill's is 0.25 to 0.33): the line's mean limit is 1.0, the fill's 0.5.
MASK_LIMITS = {'fill': (18, 0.5), 'line': (18, 1.0)}
SYMBOLS = (0x00d7, 0x25cb, 0x25a1, 0x25b3)
PANES = (b'pan1', b'pic1', b'txt1', b'wnd1', b'bnd1', b'prt1', b'ali1', b'scr1')
RING = re.compile(r'P_Guide[123]LCircle_0[0-3](_NoIn)?')
RED, CYAN, WHITE = (230, 40, 40), (40, 200, 230), (255, 255, 255)
report = ps.report


# ---- the game's layouts (bflyt panes, bflan keys) ----
def panes(blob):
    """The panes of a bflyt in file order as dicts (name, visible flag, origin byte, translate, rotate z, scale, size, parent index)."""
    head, count = struct.unpack_from('<H', blob, 6)[0], struct.unpack_from('<H', blob, 0x10)[0]
    pos, stack, last, out = head, [], None, []
    for _ in range(count):
        magic, size = bytes(blob[pos:pos + 4]), struct.unpack_from('<I', blob, pos + 4)[0]
        if magic in PANES:
            rz, sx, sy, w, h = struct.unpack_from('<fffff', blob, pos + 64)
            out.append(dict(name=bytes(blob[pos + 12:pos + 36]).split(b'\0')[0].decode(), vis=bool(blob[pos + 8] & 1), origin=blob[pos + 9],
                            t=list(struct.unpack_from('<ff', blob, pos + 44)), rz=rz, s=[sx, sy], size=[w, h], parent=stack[-1] if stack else None))
            last = len(out) - 1
        elif magic == b'pas1':
            stack.append(last)
        elif magic == b'pae1' and stack:
            stack.pop()
        pos += size
    return out


def anim(blob):
    """{pane name: {(tag, target): keys}} of a bflan, keys as (frame, value)."""
    p = blob.index(b'pai1')
    count, table = struct.unpack_from('<HI', blob, p + 14)
    out = {}
    for offset in struct.unpack_from(f'<{count}I', blob, p + table):
        c = p + offset
        name = bytes(blob[c:c + 28]).split(b'\0')[0].decode()
        for info in struct.unpack_from(f'<{blob[c + 28]}I', blob, c + 32):
            i = c + info
            for target in struct.unpack_from(f'<{blob[i + 4]}I', blob, i + 8):
                t = i + target
                curve, n, keys = blob[t + 2], struct.unpack_from('<H', blob, t + 4)[0], t + struct.unpack_from('<I', blob, t + 8)[0]
                values = [struct.unpack_from('<ff', blob, keys + 12 * k) for k in range(n)] if curve == 2 \
                    else [struct.unpack_from('<fH', blob, keys + 8 * k) for k in range(n)]
                out.setdefault(name, {})[(bytes(blob[i:i + 4]).decode(), blob[t + 1])] = values
    return out


def mul(m, n):      # affine (a, b, c, d, e, f): x' = a x + c y + e, y' = b x + d y + f
    return (m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3], m[1] * n[2] + m[3] * n[3],
            m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5])


def invert(m):
    det = m[0] * m[3] - m[1] * m[2]
    return (m[3] / det, -m[1] / det, -m[2] / det, m[0] / det, (m[2] * m[5] - m[3] * m[4]) / det, (m[1] * m[4] - m[0] * m[5]) / det)


def apply(m, p):
    return m[0] * p[0] + m[2] * p[1] + m[4], m[1] * p[0] + m[3] * p[1] + m[5]


def rect(p):
    """(left, top, w, h) of a pane in its own frame (y up)."""
    w, h = p['size']
    return (-w / 2, 0, -w)[p['origin'] & 3], (h / 2, 0, h)[(p['origin'] >> 2) & 3], w, h


def evaluate(layout, keys, frame):
    """The panes at a whole frame with 'm' (matrix to the layout's frame), 'visible' (own and ancestors') and 'centre', 'corners'."""
    done = []
    for p in layout:
        q = dict(p, t=list(p['t']), s=list(p['s']), size=list(p['size']))
        for (tag, target), values in keys.get(p['name'], {}).items():
            v = next((v for f, v in reversed(values) if f <= frame), values[0][1])
            if tag == 'FLPA' and target in (0, 1):
                q['t'][target] = v
            elif tag == 'FLPA' and target == 5:
                q['rz'] = v
            elif tag == 'FLPA' and target in (6, 7):
                q['s'][target - 6] = v
            elif tag == 'FLPA' and target in (8, 9):
                q['size'][target - 8] = v
            elif tag == 'FLVI' and target == 0:
                q['vis'] = bool(v)
        parent = done[q['parent']] if q['parent'] is not None else None
        ox = oy = 0.0
        if parent:
            left, top, w, h = rect(parent)
            ox, oy = (0, left, left + w)[(q['origin'] >> 4) & 3], (0, top, top - h)[(q['origin'] >> 6) & 3]
        c, s = math.cos(math.radians(q['rz'])), math.sin(math.radians(q['rz']))
        local = (c * q['s'][0], s * q['s'][0], -s * q['s'][1], c * q['s'][1], q['t'][0] + ox, q['t'][1] + oy)
        q['m'] = mul(parent['m'], local) if parent else local
        q['visible'] = q['vis'] and (parent['visible'] if parent else True)
        left, top, w, h = rect(q)
        q['centre'] = apply(q['m'], (left + w / 2, top - h / 2))
        q['corners'] = [apply(q['m'], p) for p in ((left, top), (left + w, top), (left + w, top - h), (left, top - h))]
        done.append(q)
    return {q['name']: q for q in done}


def pixel(controller, p):
    """The picture pixel of layout point p, for the pane Pa_Controller_00 (the picture is 580x340 at (0, -20) in its frame)."""
    x, y = apply(invert(controller['m']), p)
    return x + 290, 150 - y


# ---- the picture data of the set ----
def bc5_read(bntx, t):
    """(R rows, G rows) of a BC5 picture."""
    w, h = t.width, t.height
    r, g = [bytearray(w) for _ in range(h)], [bytearray(w) for _ in range(h)]
    for by in range(h // 4):
        for bx in range(w // 4):
            at = t.offset + bf.gob_address(bx, by, w // 4, 16, t.block_height)
            for rows, part in ((r, bntx[at:at + 8]), (g, bntx[at + 8:at + 16])):
                for i, v in enumerate(bf.bc4_decode(part)):
                    rows[by * 4 + i // 4][bx * 4 + i % 4] = v
    return r, g


def bc4_read_wide(bntx, t):
    """The rows of a BC4 texture whose width or height is not a multiple of 4 (the shadows, 314 wide; the masks)."""
    w, h, blocks, block_rows = t.width, t.height, (t.width + 3) // 4, (t.height + 3) // 4
    rows = [bytearray(blocks * 4) for _ in range(block_rows * 4)]
    for by in range(block_rows):
        for bx in range(blocks):
            at = t.offset + bf.gob_address(bx, by, blocks, 8, t.block_height)
            for i, v in enumerate(bf.bc4_decode(bntx[at:at + 8])):
                rows[by * 4 + i // 4][bx * 4 + i % 4] = v
    return [row[:w] for row in rows[:h]]


def ink_box(rows, level=128):
    """(x0, y0, x1, y1) of the texels at or above level, the far edges excluded."""
    pts = [(x, y) for y, row in enumerate(rows) for x, v in enumerate(row) if v >= level]
    return (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts) + 1, max(p[1] for p in pts) + 1) if pts else None


def tilt_pixel(pane, p):
    """The picture pixel of layout point p, for a pane P_<Kind>1_00 (the tilted picture is 570x340 in its pane, 580 wide as a texture)."""
    x, y = apply(invert(pane['m']), p)
    return (x + 285) * 580 / 570, 170 - y


def popup_panes(blob):
    """(fnl1's first font name, [(pane name, text offset, buffer, string, the 4 text bytes)] of the T_[ABXY]_0[0-3] panes) of DoCommand_00."""
    head, count = struct.unpack_from('<H', blob, 6)[0], struct.unpack_from('<H', blob, 0x10)[0]
    pos, font, out = head, None, []
    for _ in range(count):
        magic, size = bytes(blob[pos:pos + 4]), struct.unpack_from('<I', blob, pos + 4)[0]
        if magic == b'fnl1':
            font = re.search(rb'[\w.-]+\.fcpx', bytes(blob[pos:pos + size])).group().decode()
        elif magic == b'txt1':
            name = bytes(blob[pos + 12:pos + 36]).split(b'\0')[0].decode()
            if tca.POPUP_LETTER.fullmatch(name):
                at = pos + struct.unpack_from('<I', blob, pos + 100)[0]
                out.append((name, at, *struct.unpack_from('<HH', blob, pos + 84), bytes(blob[at:at + 4])))
        pos += size
    return font, out


def errors(a, b):
    diffs = [abs(x - y) for ra, rb in zip(a, b) for x, y in zip(ra, rb)]
    return max(diffs), sum(diffs) / len(diffs)


# ---- the sheet ----
def disc(canvas, cx, cy, r, colour):
    for y in range(math.floor(cy - r), math.ceil(cy + r) + 1):
        for x in range(math.floor(cx - r), math.ceil(cx + r) + 1):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                canvas.blend(x, y, colour, 1)


def line(canvas, a, b, colour):
    n = max(1, round(max(abs(b[0] - a[0]), abs(b[1] - a[1]))))
    for i in range(n + 1):
        canvas.blend(round(a[0] + (b[0] - a[0]) * i / n), round(a[1] + (b[1] - a[1]) * i / n), colour, 1)


def ring(canvas, cx, cy, r, colour):
    for i in range(72):
        canvas.blend(round(cx + r * math.cos(i * math.pi / 36)), round(cy + r * math.sin(i * math.pi / 36)), colour, 1)


def text(canvas, letters, words, cx, cy, x0, y0, width):
    words = ''.join(ch if 32 <= ord(ch) < 127 else '?' for ch in words)
    contours = letters.text(words, 20, cx - x0, cy - y0, width)
    canvas.cover(bf.coverage_rect(contours, CELL[0], 40, 4), x0, y0, WHITE, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--romfs', type=Path, required=True)
    parser.add_argument('--mod', type=Path, required=True)
    parser.add_argument('--style', choices=sorted(tpa.ART), required=True)
    parser.add_argument('--face-buttons', choices=('position', 'label'))
    parser.add_argument('--controls', type=Path)
    parser.add_argument('--firmware', type=Path)
    parser.add_argument('--before', type=Path, help="W1f's set of the same style and controls")
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    face = args.face_buttons or ('label' if args.style == 'playstation-label' else 'position')
    owner = args.controls is not None
    source = args.controls.read_text() if owner else ''
    controls = prompt_controls(source, face)
    brand = 'PlayStation' if args.style.startswith('playstation') else 'Xbox'
    firmware = args.firmware or args.romfs.parent.parent / 'firmware'
    letters = tpa.letters(firmware)

    index = read_index(args.romfs / 'runtime-index.bin')
    with (args.romfs / 'romfs.bin').open('rb') as image:
        def read(path):
            offset, size = index[path]; image.seek(offset); return image.read(size)
        pack = ps.zstd.decompress(read('Pack/ZsDic.pack.zs'))
        dictionaries = {}
        for lo, hi in mbi.sarc_members(pack).values():
            d = ps.zstd.ZstdDict(pack[lo:hi]); dictionaries[d.dict_id] = d
        layout = next(p for p in index if p.startswith(mbi.LAYOUT_PREFIX) and p.endswith('.blarc.zs'))
        old_arc = ps.decode(read(layout), dictionaries)
        font_archives = {p: ps.decode(read(p), dictionaries) for p in sorted(index) if p.startswith('Font/') and p.endswith('.bfarc.zs')}
    new_arc = ps.decode((args.mod / layout).read_bytes(), dictionaries)
    before_arc = ps.decode((args.before / layout).read_bytes(), dictionaries) if args.before else None
    old, new = (a[slice(*mbi.sarc_members(a)[mbi.BNTX])] for a in (old_arc, new_arc))
    members = mbi.sarc_members(old_arc)
    new_t = bf.bntx_textures(new)
    brti = {bf.Texture(old, m.start()).name: m.start() for m in re.finditer(b'BRTI', old)}

    report(len(new_arc) == len(old_arc) and len(new) == len(old), f'Common archive {len(new_arc)} and its BNTX {len(new)} bytes keep their lengths ({len(old_arc)}, {len(old)})')
    pictures = {k: tca.PICTURES[k] for k in KINDS}
    for kind, (name, shadow) in pictures.items():
        at, t = brti[name], new_t[name]
        fields = (struct.unpack_from('<I', new, at + 0x1C)[0], bytes(new[at + 0x58:at + 0x5c]))
        same = all(old[at + i] == new[at + i] for i in range(0x100) if not 0x1C <= i < 0x20 and not 0x58 <= i < 0x5c)
        report(fields == (tca.BC5_UNORM, tca.GREY_ALPHA) and same and (t.format, t.width, t.height) == (tca.BC5_UNORM, 580, 340),
               f'{name} BRTI: format 0x{fields[0]:x}, selector {fields[1].hex(" ")}, the other bytes unchanged')
        report(old[brti[shadow]:brti[shadow] + 0x100] == new[brti[shadow]:brti[shadow] + 0x100], f'{shadow} BRTI unchanged')
    changed = [(new_t[n].offset, new_t[n].offset + new_t[n].image_size) for pair in pictures.values() for n in pair]
    masks = {t.name.split('^')[0]: t for t in new_t.values()}
    changed += [(masks[n].offset, masks[n].offset + masks[n].image_size) for n in ps.MASKS]
    fields = {at + i for kind, (name, _) in pictures.items() for at in (brti[name],) for i in (*range(0x1C, 0x20), *range(0x58, 0x5c))}
    tilted = tca.TILT_PICTURES
    mask_names = {k: [f'{stem}_{part}^s' for stem, _, _, _ in tca.MASKS[k].values() for part in ('00', 'Line')] for k in KINDS}
    w1g = [(new_t[n].offset, new_t[n].offset + new_t[n].image_size) for k in KINDS for n in (*tilted[k], *mask_names[k])]
    w1g += [(new_t[n].offset, new_t[n].offset + new_t[n].image_size) for n in tca.DPAD_MASKS]
    w1g_fields = {at + i for k in KINDS for at in (brti[tilted[k][0]],) for i in (*range(0x1C, 0x20), *range(0x58, 0x5c))}
    skip = bytearray(len(new))
    for lo, hi in changed + w1g:
        skip[lo:hi] = b'\1' * (hi - lo)
    for i in fields | w1g_fields:
        skip[i] = 1
    stray = [i for i in range(len(new)) if not skip[i] and old[i] != new[i]]
    report(not stray, f'no other BNTX byte changed ({len(stray)} stray)')

    # 11: W1f's output is unchanged
    popup_old, popup_new = (a[slice(*members[tca.POPUP])] for a in (old_arc, new_arc))
    font, popup_at = popup_panes(popup_old)
    bntx_at, popup_start = members[mbi.BNTX][0], members[tca.POPUP][0]
    if before_arc is not None:
        allowed = bytearray(len(new_arc))
        for lo, hi in w1g:
            allowed[bntx_at + lo:bntx_at + hi] = b'\1' * (hi - lo)
        for i in w1g_fields:
            allowed[bntx_at + i] = 1
        for _, at, *_ in popup_at:
            allowed[popup_start + at:popup_start + at + 2] = b'\1\1'
        outside = [i for i in range(len(new_arc)) if not allowed[i] and before_arc[i] != new_arc[i]] if len(before_arc) == len(new_arc) else [-1]
        report(not outside, f'Common archive equals the before set outside the W1g ranges, fields and popup letters ({len(outside)} stray)')

    # 12: the BRTIs
    for k in KINDS:
        name, shadow = tilted[k]
        at = brti[name]
        got = (struct.unpack_from('<I', new, at + 0x1C)[0], bytes(new[at + 0x58:at + 0x5c]))
        same = all(old[at + i] == new[at + i] for i in range(0x100) if not 0x1C <= i < 0x20 and not 0x58 <= i < 0x5c)
        untouched = [n for n in (shadow, *mask_names[k]) if old[brti[n]:brti[n] + 0x100] != new[brti[n]:brti[n] + 0x100]]
        report(got == (tca.BC5_UNORM, tca.GREY_ALPHA) and same and not untouched,
               f'{k} tilted BRTIs: picture format 0x{got[0]:x} selector {got[1].hex(" ")}, the other bytes, the shadow and the 4 masks original' + (f' (changed: {untouched})' if untouched else ''))
    report(all(old[brti[n]:brti[n] + 0x100] == new[brti[n]:brti[n] + 0x100] for n in tca.DPAD_MASKS), 'd-pad shortcut icon BRTIs original')

    system, system_panes = anim(mbi_member(new_arc, members, 'anim/PaSystemController_00_Pattern.bflan')), panes(mbi_member(new_arc, members, 'blyt/PaSystemController_00.bflyt'))
    guide_panes = panes(mbi_member(new_arc, members, 'blyt/PaActionGuide_00.bflyt'))

    canvas = ps.Canvas(CELL[0] * 3, 1100)
    labels = tca.face_labels(controls, args.style)
    inverse = {tca.LINEAR[v]: v for v in range(255, -1, -1)}
    levels = sorted(inverse)
    for n, kind in enumerate(KINDS):
        x0, a = n * CELL[0], tca.ANCHORS[kind]
        name, shadow = pictures[kind]
        t = new_t[name]
        r, g = bc5_read(new, t)
        (want_grey, want_alpha), _ = tca.draw(kind, args.style, controls, letters)
        peak, mean = errors(g, want_alpha)
        report(peak <= 18 and mean <= 0.25, f'{kind} alpha vs a new draw: max error {peak}, mean {mean:.3f}')
        peak = max(abs(r[y][x] - tca.LINEAR[want_grey[y][x] if want_alpha[y][x] else tca.art.RIM]) for y in range(340) for x in range(580))
        report(peak <= 18, f'{kind} grey (linear) vs a new draw: max error {peak}')
        sh = bc4_read_wide(new, new_t[shadow])
        peak, _ = errors(sh, tca.shadow_rows(g))
        corners = max(sh[y][x] for y in (0, 359) for x in (0, 313))
        report(peak <= 18 and corners <= 8, f'{kind} shadow vs shadow_rows(alpha): max error {peak}, corner texels <= {corners}')
        problems = tca.layout_problems(kind, args.style, g)
        report(not problems, f'{kind} layout_problems: {problems or "none"}')

        # the "Basic controls" page at frame n
        page = evaluate(system_panes, system, n)
        px = lambda p: pixel(page['Pa_Controller_00'], p)      # noqa: E731
        dots = {s: px(page[f'P_Circle{w}_00']['centre']) for s, w in (('zl', 'ZL'), ('zr', 'ZR'), ('l', 'L'), ('r', 'R'), ('minus', 'Minus'),
                                                                       ('plus', 'Plus'), ('lstick', 'LStick'), ('rstick', 'RStick'))}
        shoulder_ok = all(math.dist(dots[s], a[s]) <= 1 and max(g[int(round(dots[s][1])) + dy][int(round(dots[s][0])) + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1)) >= 200
                          for s in ('zl', 'zr', 'l', 'r'))
        report(shoulder_ok, f'{kind} page dots ZL L ZR R within 1 px of the anchors, painted: ' + ' '.join(f'{s}({dots[s][0]:.1f},{dots[s][1]:.1f})/({a[s][0]:.1f},{a[s][1]:.1f})' for s in ('zl', 'l', 'zr', 'r')))
        buttons = tca.buttons(kind)
        report(all(math.dist(dots[s], buttons[s][0]) <= buttons[s][1] + 6 for s in ('minus', 'plus', 'lstick', 'rstick')),
               f'{kind} page dots - + LS RS within a radius + 6 of their buttons: ' + ' '.join(f'{s}({dots[s][0]:.1f},{dots[s][1]:.1f})/({buttons[s][0][0]:.1f},{buttons[s][0][1]:.1f})' for s in ('minus', 'plus', 'lstick', 'rstick')))
        diamonds = {s: (px(page[w]['centre']), [px(c) for c in page[w]['corners']]) for s, w in (('face', 'W_CircleABXY_00'), ('dpad', 'W_CircleCrossKey_00'))}
        report(all(math.dist(diamonds[s][0], a[s]) <= 2 for s in diamonds), f'{kind} diamond centres within 2 px of the anchors: '
               + ' '.join(f'{s}({diamonds[s][0][0]:.1f},{diamonds[s][0][1]:.1f})/({a[s][0]:.1f},{a[s][1]:.1f})' for s in diamonds))
        (fc, fcorners), (dc, dcorners) = diamonds['face'], diamonds['dpad']
        fhalf, dhalf = math.dist(fc, fcorners[0]), math.dist(dc, dcorners[0])
        k, rf = tca.sizes(a)
        faces_in = all(abs(cx + rf * math.cos(i * math.pi / 16) - fc[0]) + abs(cy + rf * math.sin(i * math.pi / 16) - fc[1]) <= fhalf + 3
                       for cx, cy in tca.spots(a).values() for i in range(32))
        arms_in = all(abs(sx * w) + abs(sy * h) <= dhalf + 4 for w, h in ((27.5 * k, 9.5 * k), (9.5 * k, 27.5 * k)) for sx in (-1, 1) for sy in (-1, 1))
        report(faces_in and arms_in, f'{kind} face discs inside the ABXY diamond +3 (half diagonal {fhalf:.1f}), d-pad arms inside its diamond +4 ({dhalf:.1f})')

        # the ability guide, Pattern A B C
        values = anim(mbi_member(new_arc, members, f'anim/PaActionGuide_00_Pattern{"ABC"[n]}.bflan'))
        rings = []
        for frame in range(32):
            guide = evaluate(guide_panes, values, frame)
            if guide['Pa_Controller_00']['visible']:
                rings += [pixel(guide['Pa_Controller_00'], q['centre']) for q in guide.values() if RING.fullmatch(q['name']) and q['visible']]
        inside = [p for p in rings if any(math.dist(p, c) <= r for c, r in buttons.values())]
        report(len(rings) == 24 and len(inside) == 24, f'{kind} guide rings: {len(rings)} visible, {len(inside)} inside a button')

        # the tilted pictures (checks 13 to 18)
        tname, tshadow = tilted[kind]
        tr, tg = bc5_read(new, new_t[tname])
        (want_grey, want_alpha), _ = tca.draw_tilted(kind, args.style, controls, letters)
        peak, mean = errors(tg, want_alpha)
        report(peak <= 18 and mean <= 0.25, f'{kind} tilted alpha vs a new draw: max error {peak}, mean {mean:.3f}')
        peak = max(abs(tr[y][x] - tca.LINEAR[want_grey[y][x] if want_alpha[y][x] else tca.art.RIM]) for y in range(340) for x in range(580))
        report(peak <= 18, f'{kind} tilted grey (linear) vs a new draw: max error {peak}')
        tsh = bc4_read_wide(new, new_t[tshadow])
        peak, _ = errors(tsh, tca.tilted_shadow_rows(tg, kind))
        corners = max([tsh[y][x] for y in (0, 359) for x in (0, 313)] + list(tsh[359]))
        report(peak <= 18 and corners <= 8, f'{kind} tilted shadow vs tilted_shadow_rows(alpha): max error {peak}, corner texels and bottom row <= {corners}')
        problems = tca.tilt_problems(kind, args.style, tg)
        report(not problems, f'{kind} tilt_problems: {problems or "none"}')
        tvalues = anim(mbi_member(new_arc, members, f'anim/PaActionGuide_00_Pattern{"ABC"[n]}.bflan'))
        name_of = f'P_{kind.capitalize()}1_00'
        tbuttons, by = tca.tilt_buttons(kind), tca.TILT[kind]['by']
        trings, wrong, line_panes, centres = [], [], 0, []
        frames = []
        for frame in range(32):
            guide = evaluate(guide_panes, tvalues, frame)
            if guide['Pa_Controller_00']['visible'] or not guide[name_of]['visible']:
                continue
            frames.append(frame)
            pane = guide[name_of]
            trings += [tilt_pixel(pane, q['centre']) for q in guide.values() if RING.fullmatch(q['name']) and q['visible']]
            visible = [q for q in guide.values() if MASK_PANE.fullmatch(q['name']) and q['visible']]
            lines = [q for q in visible if MASK_PANE.fullmatch(q['name']).group(4)]
            expect_lines = 0 if (kind == 'grip' and frame == 30) else 1
            if len(lines) != expect_lines or len(visible) != len(lines) or any(MASK_PANE.fullmatch(q['name']).group(2) != ('Cons', 'Grip', 'Pro')[n] for q in visible):
                wrong.append(f'frame {frame}: {[q["name"] for q in visible]}')
            for q in lines:
                s_ = MASK_PANE.fullmatch(q['name']).group(3).lower()
                _, (mw, mh, _, _), (ox, oy), _ = tca.MASKS[kind][s_]
                want = (ox + mw / 2 * tca.TEXEL[0], oy + mh / 2 * tca.TEXEL[1])
                mirrored = q['m'][0] < 0
                if mirrored:
                    want = (580 - want[0], want[1])
                got = tilt_pixel(pane, q['centre'])
                if math.dist(got, want) > (2.5 if mirrored else 1.5):
                    wrong.append(f'frame {frame} {q["name"]}: centre {got[0]:.1f},{got[1]:.1f} want {want[0]:.1f},{want[1]:.1f}')
                line_panes += 1
        def fraction(p):
            return min(math.sqrt(((p[0] - c[0]) / r) ** 2 + ((p[1] - c[1]) / (r * by)) ** 2) for c, r in tbuttons.values())
        worst = max((fraction(p) for p in trings), default=9)
        report(len(trings) == 18 and worst <= 1, f'{kind} tilted rings: {len(trings)} visible in {len(frames)} frames, each inside a button, worst {worst:.2f} of a radius')
        report(not wrong and line_panes == 15, f'{kind} tilted frames: one line pane each ({line_panes} in {len(frames)} frames), no fill pane, no other kind, centres on the MASKS boxes' + (f': {wrong}' if wrong else ''))
        mask_ok, parts = True, []
        for s_, (stem, (mw, mh, _, _), _, box) in tca.MASKS[kind].items():
            olds = [bc4_read_wide(old, bf.Texture(old, brti[f'{stem}_{part}^s'])) for part in ('00', 'Line')]
            news = [bc4_read_wide(new, new_t[f'{stem}_{part}^s']) for part in ('00', 'Line')]
            want = tca.mask_rows(kind, s_)
            grown = (box[0] - 5, box[1] - 5, box[2] + 5, box[3] + 5)
            original_ok = ink_box(olds[0]) == box and ink_box(olds[1]) == grown
            errs = [errors(a_, b_) for a_, b_ in zip(news, want)]
            same_ink = [ink_box(a_) == ink_box(b_) for a_, b_ in zip(news, olds)]
            overlap = any(f_ >= 128 and l_ >= 128 for fr, lr in zip(*news) for f_, l_ in zip(fr, lr))
            ok = original_ok and all(e[0] <= lim[0] and e[1] <= lim[1] for e, lim in zip(errs, MASK_LIMITS.values())) and all(same_ink) and not overlap
            mask_ok &= ok
            parts.append(f'{s_} original ink {ink_box(olds[0])}/{ink_box(olds[1])} box {box}, max error {errs[0][0]}/{errs[1][0]} mean {errs[0][1]:.2f}/{errs[1][1]:.2f}, ink {"same" if all(same_ink) else "DIFFERENT"}, overlap {overlap}')
        report(mask_ok, f'{kind} masks: ' + '; '.join(parts))
        tx = x0 + 10
        for y in range(340):
            for x in range(580):
                if tg[y][x]:
                    v = tr[y][x]
                    s_ = inverse[v] if v in inverse else inverse[min(levels, key=lambda c: abs(c - v))]
                    canvas.blend(tx + x, 440 + y, (s_, s_, s_), tg[y][x] / 255)
        for p in trings:
            ring(canvas, tx + p[0], 440 + p[1], 15.6, CYAN)
        for s_, (stem, (mw, mh, _, _), (ox, oy), _) in tca.MASKS[kind].items():
            line_rows = bc4_read_wide(new, new_t[f'{stem}_Line^s'])
            for v in range(mh):
                for u in range(mw):
                    if line_rows[v][u]:
                        for px in range(round(ox + u * tca.TEXEL[0]), round(ox + (u + 1) * tca.TEXEL[0])):
                            for py in range(round(oy + v * tca.TEXEL[1]), round(oy + (v + 1) * tca.TEXEL[1])):
                                canvas.blend(tx + px, 440 + py, MAGENTA, line_rows[v][u] / 255)
                                canvas.blend(tx + 579 - px, 440 + py, MAGENTA, line_rows[v][u] / 255)
        text(canvas, letters, f'{kind.capitalize()} tilted (ability guides)', x0 + CELL[0] / 2, 812, x0, 792, CELL[0] - 20)

        # the cell
        for y in range(340):
            for x in range(580):
                if g[y][x]:
                    v = r[y][x]
                    s = inverse[v] if v in inverse else inverse[min(levels, key=lambda c: abs(c - v))]
                    canvas.blend(x0 + 10 + x, 10 + y, (s, s, s), g[y][x] / 255)
        for s, p in {**dots, 'face': diamonds['face'][0], 'dpad': diamonds['dpad'][0]}.items():
            disc(canvas, x0 + 10 + p[0], 10 + p[1], 3, RED)
        for centre, corners_ in diamonds.values():
            for i in range(4):
                line(canvas, (x0 + 10 + corners_[i][0], 10 + corners_[i][1]), (x0 + 10 + corners_[(i + 1) % 4][0], 10 + corners_[(i + 1) % 4][1]), RED)
        for p in rings:
            ring(canvas, x0 + 10 + p[0], 10 + p[1], 15.6, CYAN)
        text(canvas, letters, f'{LABEL[n]} {args.style} {"owner" if owner else "default"}', x0 + CELL[0] / 2, 372, x0, 352, CELL[0] - 20)
        text(canvas, letters, 'N {x}  E {a}  S {b}  W {y}'.format(**{s: labels[s] or '-' for s in 'xaby'}), x0 + CELL[0] / 2, 404, x0, 384, CELL[0] - 20)
        print(f'     {kind} anchors found vs ANCHORS: ' + ' '.join(f'{s} {p[0]:.1f},{p[1]:.1f}/{a[s][0]:.1f},{a[s][1]:.1f}' for s, p in
                                                             {**dots, 'face': diamonds['face'][0], 'dpad': diamonds['dpad'][0]}.items()))
    # 19: the d-pad shortcut icon
    pad = [bc4_read_wide(old, bf.Texture(old, brti[name])) for name in tca.DPAD_MASKS], [bc4_read_wide(new, new_t[name]) for name in tca.DPAD_MASKS]
    box = tuple(round(v) for v in tca.bp.disc_box(pad[0][0]))
    icon = tca.bp.render(tca.bp.drawers_of(tca.ART[args.style], letters)[0xE049], 64, tca.DPAD_BOX)
    peak0, _ = errors(pad[1][0], icon)
    peak1, _ = errors(pad[1][1], tca.glow(pad[1][0]))
    report(box == tca.DPAD_BOX and peak0 <= 18 and peak1 <= 18, f'd-pad icon: original disc box {box} (want {tca.DPAD_BOX}), _00 vs the E049 drawing max error {peak0}, _01 vs glow(_00) max error {peak1}')

    # 20: the popup letters
    plan, _ = tca.popup_plan(controls, args.style)
    font_new, panes_new = popup_panes(popup_new)
    ok = font == font_new == 'Normal_00.fcpx' and len(panes_new) == 15 and [p_[:2] for p_ in popup_at] == [p_[:2] for p_ in panes_new]
    allowed_at = {at_ + i for _, at_, *_ in panes_new for i in range(2)}
    for name, at_, *_, text_ in panes_new:
        ok &= text_[:2] == plan[name[2].lower()].encode('utf-16-le') and text_[2:] == b'\0\0'
    ok &= len(popup_new) == len(popup_old) and all(popup_old[i] == popup_new[i] for i in range(len(popup_old)) if i not in allowed_at)
    report(ok, f'popup: fnl1 first name {font_new}, 15 panes hold the plan, every other DoCommand_00 byte original')
    print('     popup: ' + ' '.join(f'{place} {plan[slot]} (U+{ord(plan[slot]):04X})' for slot, place in (('a', 'east'), ('b', 'south'), ('x', 'north'), ('y', 'west'))))

    # 21: the fonts of the popup
    for path, archive in font_archives.items():
        fmembers = mbi.sarc_members(archive)
        chain = re.findall(rb'[\w.-]+\.bf(?:otf|ttf)', archive[slice(*fmembers['fcpx/Normal_00.bfcpx'])])
        chain = list(dict.fromkeys(c.decode() for c in chain))
        union, parts, missing = set(), [], []
        for fname in chain:
            if f'scft/{fname}' in fmembers:
                raw = archive[slice(*fmembers[f'scft/{fname}'])]
                data = mbi.bfotf_decode(raw)[0] if fname.endswith('.bfotf') else tca.bp.bfttf_decode(raw)
            elif fname == 'nintendo_udsg-r_std_003.bfttf':
                data = tca.bp.bfttf_decode((firmware / tpa.SYSTEM_FONT).read_bytes())
            else:
                missing.append(fname)
                continue
            cmap = set(TTFont(io.BytesIO(data)).getBestCmap())
            union |= cmap
            held = [c for c in SYMBOLS if c in cmap]
            parts.append(f'{fname} ({"all four" if len(held) == 4 else "none" if not held else " ".join(f"U+{c:04X}" for c in held)})')
        report(not missing and all(c in union for c in SYMBOLS), f'{path} Normal_00 chain: ' + ', '.join(parts) + (f'; not found: {missing}' if missing else ''))

    # row 3: the masks at 2x
    x = 10
    for kind in KINDS:
        for s_, (stem, (mw, mh, _, _), _, _) in tca.MASKS[kind].items():
            for row, part in ((866, '00'), (968, 'Line')):
                canvas.cover(bc4_read_wide(new, new_t[f'{stem}_{part}^s']), x, row, WHITE, 1, 2)
            x += 2 * mw + 14
    x += 20
    for name in tca.DPAD_MASKS:
        canvas.cover(bc4_read_wide(new, new_t[name]), x, 866, WHITE, 1, 2)
        x += 142
    text(canvas, letters, 'masks: fill, line; d-pad icon, glow', 300, 1086, 10, 1066, 580)

    for slot, place in (('x', 'north'), ('a', 'east'), ('b', 'south'), ('y', 'west')):
        print(f'     {place} (Switch {slot.upper()}) <- {brand} {labels[slot]}')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(args.out)
    print(f'{args.out}: 3 cells, 3 tilted cells and the masks, {ps.failures} FAIL')
    return 1 if ps.failures else 0


def mbi_member(archive, members, name):
    return archive[slice(*members[name])]


if __name__ == '__main__':
    sys.exit(main())
